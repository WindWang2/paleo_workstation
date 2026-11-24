// 层：功能
#include "aichattoolrunner.h"
#include "capabilityhost.h"
#include "services/projectquerycapability.h"
#include "capabilitytaskrunner.h"
#include "wellfaciesreplay.h"
#include <QDir>

#include <QJsonArray>
#include <QJsonDocument>
#include <QFileInfo>
#include <QJsonValue>
#include <QMetaObject>
#include <QPointer>

#include <algorithm>
#include <atomic>
#include <cmath>
#include <limits>
#include <memory>
#include <utility>

#include "../ai/chat/domaintools.h"
#include "../ai/chat/mapdisplaytools.h"
#include "../ai/chat/planpreviewtool.h"
#include "../ai/tileinference.h"
#include "../ai/wellfaciesservice.h"
#include "ai/onnxpredictionservice.h" // G3/W6：setContext 比较 QPointer 需完整类型
#include "tileoperationservice.h"     // 同上
#include "../catalog/datacatalog.h"
#include "../services/paleotaskservice.h"

// workflow/ — 聊天工具执行器实现（方向61；方向77 增只读查询/血缘两工具）。
//
// 结果 JSON 契约：成功给摘要/结果引用；失败给稳定 code 或人可读 error。
// 两种都按 role=tool 回灌——协议要求每个
// tool_call_id 有应答，错误也让模型如实转述，不在本地吞掉。

namespace {
// 聊天面的 tile 分类低置信阈值：工具 schema 不暴露该参数（表 shape 与方向51
// 一致），执行面取 0.5（掩膜产品语义见 aiassistworkflow.h）。想调参走
// 工作流面板，不走聊天。
constexpr double kChatLowConfidenceThreshold = 0.5;

// 摘要段硬上限（预算纪律：system prompt 轻注入不喧宾夺主）。
constexpr int kBriefCharCap = 300;
constexpr int kBriefAssetTypes = 6;

// PW-060：严格整数——double 须有限、整值且落在 int 域（qRound 越界是 UB）；
// 文本须整串可解析。其余一律回退 fallback（调用方再按必填/范围拒绝）。
int intArg(const QJsonObject &args, const char *key, int fallback) {
  const QJsonValue value = args.value(QLatin1String(key));
  if (value.isDouble()) {
    const double d = value.toDouble();
    if (!std::isfinite(d) || d != std::floor(d) ||
        d < double(std::numeric_limits<int>::min()) ||
        d > double(std::numeric_limits<int>::max()))
      return fallback;
    return static_cast<int>(d);
  }
  if (value.isString()) { // 表口径允许 "12" 文本整数（jsonTypeMatches）
    bool ok = false;
    const int parsed = value.toString().trimmed().toInt(&ok);
    return ok ? parsed : fallback;
  }
  return fallback;
}

} // namespace

AiChatToolRunner::AiChatToolRunner(QObject *parent)
  : QObject(parent), m_facies(new WellFaciesService(this)), m_query(std::make_unique<paleo::query::ProjectQueryCapability>()) {}

AiChatToolRunner::~AiChatToolRunner() {
  ++m_generation; // 作废在途回调
  m_queue.clear();
  if (m_activeTask)
    m_activeTask->requestInterruption();
  if (m_activeIsFacies)
    m_facies->cancel();
}

void AiChatToolRunner::setWorkflow(AiAssistWorkflow *workflow) {
  m_workflow = workflow;
}

void AiChatToolRunner::setFaciesConfig(const WellFaciesConfig &config) {
  if(config.baseUrl==m_faciesConfig.baseUrl&&config.apiKey==m_faciesConfig.apiKey&&
     config.allowInsecureHttp==m_faciesConfig.allowInsecureHttp)return;
  ++m_faciesEpoch;
  if(m_activeIsFacies&&m_activeTask)m_activeTask->requestCancel();
  m_faciesConfig = config;
  m_facies->configure(config);
}

void AiChatToolRunner::setContext(const Context &context) {
  ++m_bindingRevision;
  const bool replaceSourceResolver=bool(context.sourceResolver)||bool(m_context.sourceResolver);
  // G3/W6：provider/取数回调的接入或撤除（null↔非空）也算绑定变化——
  // 旧判定只看 catalog/文档身份，撤掉 trackingInput/gridFetch 后世代不
  // 前进，资格判断与排队调用仍按旧绑定放行。std::function 同布尔值替换
  // 无法值比较（已知边界，见 aichattoolrunner.h）；指针成员按值比较。
  const bool providerBindingChanged =
      bool(context.trackingInput)!=bool(m_context.trackingInput) ||
      bool(context.traceFetch)!=bool(m_context.traceFetch) ||
      bool(context.gridFetch)!=bool(m_context.gridFetch) ||
      bool(context.frozenFaciesInput)!=bool(m_context.frozenFaciesInput) ||
      bool(context.faciesContext)!=bool(m_context.faciesContext) ||
      bool(context.runtimeReadiness)!=bool(m_context.runtimeReadiness) ||
      bool(context.sourceResolver)!=bool(m_context.sourceResolver) ||
      context.onnx!=m_context.onnx || context.tileOperations!=m_context.tileOperations ||
      context.pythonRuntime!=m_context.pythonRuntime || context.pythonTrusted!=m_context.pythonTrusted ||
      context.pythonSandbox!=m_context.pythonSandbox;
  const bool changed = context.catalog != m_context.catalog || context.documentId != m_context.documentId ||
      context.projectDir != m_context.projectDir || context.documentEpoch != m_context.documentEpoch ||
      context.horizon != m_context.horizon ||
      context.capabilityHost != m_context.capabilityHost || context.requireCapabilityHost != m_context.requireCapabilityHost ||
      providerBindingChanged;
  m_context = context;
  // cancelAsked() can synchronously replace this binding again.
  if (changed) invalidateContext();
  else if (replaceSourceResolver) m_query->setSourceResolver(m_context.sourceResolver);
}

void AiChatToolRunner::setTaskService(PaleoTaskService *service) {
  if (m_taskService == service)
    return;
  m_taskService = service;
  ++m_bindingRevision;
  invalidateContext();
}

void AiChatToolRunner::invalidateContext() {
  ++m_contextGeneration;
  ++m_resultEpoch;
  rebindQueryScope();
  if (m_activeTask)
    m_activeTask->requestInterruption();
}

quint64 AiChatToolRunner::taskSession() const {
  return m_taskService ? m_taskService->session() : 0;
}
QJsonObject AiChatToolRunner::hostContextIdentity() const {
  return {{"documentId", m_context.documentId}, {"epoch", QString::number(m_context.documentEpoch)},
      {"generation", QString::number(m_contextGeneration)}, {"taskSession", QString::number(taskSession())}};
}
void AiChatToolRunner::rebindQueryScope() {
  m_query->setSourceResolver(m_context.sourceResolver);
  m_query->bind(m_context.catalog, {m_context.documentId, m_context.documentEpoch, m_contextGeneration, m_querySessionId});
}

bool AiChatToolRunner::activeContextIsCurrent() const {
  return m_activeContextGeneration == m_contextGeneration && m_activeTaskSession == taskSession();
}

void AiChatToolRunner::run(const ChatToolCall &call) {
  run(call, 0);
}

void AiChatToolRunner::run(const ChatToolCall &call, int quotaBytes) {
  m_queue.append({call, m_contextGeneration, taskSession(), m_faciesEpoch, quotaBytes});
  startNext();
}

void AiChatToolRunner::cancel() {
  stopCurrentRound(true);
}
void AiChatToolRunner::interrupt() {
  stopCurrentRound(false);
}
void AiChatToolRunner::stopCurrentRound(bool explicitRequest) {
  const QPointer<AiChatToolRunner> guard(this);
  // 取消 = 作废（口径见头文件）：在途结果不再 emit；tile 任务协作取消
  // （不写产品）；层位任务算完但丢弃；测井相 HTTP 中止。
  ++m_generation;
  if(explicitRequest)m_explicitCancelGeneration=m_generation;
  m_queue.clear();
  // H7：已生成页的配额记录只活到该调用完成；取消即作废，不进入新上下文。
  m_deliveredReadCallId.clear();
  m_deliveredReadQuota = 0;
  if (m_activeTask) {
    if(explicitRequest)m_activeTask->requestCancel();
    else m_activeTask->requestInterruption();
  }
  if (!guard) return;
  if (m_activeIsFacies) {
    m_facies->disconnect(this);
    m_facies->cancel();
  }
  if (!guard) return;
  m_active = false;
  m_activeIsFacies = false;
  m_activeTask.clear();
}

void AiChatToolRunner::startNext() {
  if (m_active || m_queue.isEmpty())
    return;
  const QueuedCall next = m_queue.takeFirst();
  m_activeCall = next.call;
  m_activeContextGeneration = next.contextGeneration;
  m_activeTaskSession = next.taskSession;
  m_activeFaciesEpoch=next.faciesEpoch;
  m_activeQuotaBytes = next.quotaBytes;
  m_active = true;
  m_activeIsFacies = false;
  const int generation = m_generation;
  const QPointer<AiChatToolRunner> guard(this);
  emit toolStarted(m_activeCall);
  if (!guard || generation != m_generation)
    return;
  if (!activeContextIsCurrent()) {
    finishActive(false, QJsonObject{{QStringLiteral("code"), QStringLiteral("stale_session")}});
    return;
  }
  if (!m_activeCall.argumentsWithinBudget()) {
    finishActive(false, QJsonObject{{QStringLiteral("code"), QStringLiteral("request_budget_exceeded")}});
    return;
  }

  QJsonParseError parseError;
  // G3/W3：顶层必须是 JSON object（空串 = 无参工具，与
  // ChatToolCall::hasValidArguments 口径一致）。此前数组/字符串/null 顶层
  // 解析成功后 .object() 得空对象，静默走「缺必填」甚至无参执行。
  QJsonDocument document;
  if (m_activeCall.argumentsJson.trimmed().isEmpty()) {
    document = QJsonDocument::fromJson(QByteArrayLiteral("{}"));
  } else {
    document = QJsonDocument::fromJson(m_activeCall.argumentsJson.toUtf8(), &parseError);
  }
  const QJsonObject args = document.object();
  // 表驱动诚实态：未登记 / 本构建不可用 / 实参不合格 → 错误结果回灌
  // （不冒充成功，也不静默丢帧）。注：fromJson 失败必返回 null 文档，
  // 只看 parseError 即可（旧实现的 !isNull() 使该分支永不触发，坏 JSON
  // 被误报成「缺少必填参数」）。
  if (parseError.error != QJsonParseError::NoError) {
    finishActive(false, errorPayload(tr("实参不是合法 JSON：%1")
                                       .arg(parseError.errorString())));
    return;
  }
  if (!document.isObject()) {
    finishActive(false, QJsonObject{{QStringLiteral("code"), QStringLiteral("invalid_arguments_type")},
                                    {QStringLiteral("error"), tr("工具实参顶层必须是 JSON object")}});
    return;
  }
  if(m_activeFaciesEpoch!=m_faciesEpoch&&(m_activeCall.name=="paleo.well_facies_prediction"||
      (m_activeCall.name=="paleo.query_project"&&args.value("topic").toString()=="well_facies_models"))) {
    if(m_activeCall.name==QLatin1String("paleo.well_facies_prediction")) {
      QString code;const auto replay=paleo::wellfacies::completedReplay(m_context.capabilityHost,m_resultSession,args,code);
      if(!replay.isEmpty()){finishActive(true,replay);return;}
    }
    finishActive(false,{{"code","facies_config_changed"}});return;
  }
  if (m_context.requireCapabilityHost) {
    QString code;
    if (!m_context.capabilityHost) code = QStringLiteral("host_unavailable");
    else {
      const auto live = m_context.capabilityHost->context();
      if (!guard || generation != m_generation) return;
      if (!activeContextIsCurrent()) {
        finishActive(false, QJsonObject{{"code", "stale_session"}});
        return;
      }
      auto expected = live;
      expected.documentId = m_context.documentId; expected.epoch = m_context.documentEpoch;
      if (live.documentId != expected.documentId || live.epoch != expected.epoch)
        code = QStringLiteral("stale_session");
      else if (m_activeCall.name != QLatin1String("paleo.read_result") &&
               m_activeCall.name != QLatin1String(paleo::chat::kPlanPreviewTool)) {
        unsigned effects = paleo::host::Read;
        if (m_activeCall.name == QLatin1String("paleo.tile_classification"))
          effects |= paleo::host::PrivateStage | paleo::host::CatalogPublish | paleo::host::Presentation;
        code = paleo::host::checkContext(live, expected, effects);
      }
    }
    if (!code.isEmpty()) { finishActive(false, QJsonObject{{QStringLiteral("code"), code}}); return; }
  }
  const AiToolDispatch dispatch = dispatchAiTool(m_activeCall.name, args);
  if (dispatch.status != AiToolDispatchStatus::Routed) {
    if(m_activeCall.name==QLatin1String("paleo.well_facies_prediction") && !dispatch.note.isEmpty()) {
      finishActive(false,{{"code",dispatch.note}});return;
    }
    const QString text = aiToolDispatchLabel(dispatch.status) +
                         (dispatch.note.isEmpty()
                            ? QString()
                            : QStringLiteral("：%1").arg(dispatch.note));
    finishActive(false, errorPayload(text));
    return;
  }
  // G3/W3/W5：严格外部档——本入口只承接新外部模型请求（controller 派发），
  // integer/number 必须是 JSON 原生数值、主题字段/预算域/seeds 元素在此
  // 收紧；失败发生在任何 provider/Task/operation 之前。宿主内部的宽松
  // 兼容路径不经本 runner 执行（validateParameters/dispatchAiTool 直调）。
  {
    QString strictField;
    const QString strictCode = dispatch.spec.validateExternalArguments(args, &strictField);
    if (!strictCode.isEmpty()) {
      QJsonObject payload{{QStringLiteral("code"), strictCode}};
      if (!strictField.isEmpty())
        payload.insert(QStringLiteral("field"), strictField);
      finishActive(false, payload);
      return;
    }
  }
  const auto runtime = readinessForSpec(dispatch.spec, args, false);
  if (!guard || generation != m_generation) return;
  if (!runtime.ready) {finishActive(false,QJsonObject{{"code",runtime.code}});return;}
  if (!activeContextIsCurrent()) {
    finishActive(false, QJsonObject{{"code", "stale_session"}});
    return;
  }
  if (m_activeCall.name == QStringLiteral("paleo.read_result")) {
    refreshResultScope();
    // H7-W3：一次读取准入、一次页面选择。页生成直接以「本次调用实际能发送
    // 的最终 envelope」计价；成功页即本次读取事实，最终化不再查缓存。
    const auto result = m_results.read(args, m_activeQuotaBytes);
    // 记录本次实际生效的 envelope 预算（请求上限与宿主配额的交集），
    // 而不是只记宿主配额：两者可能不同，最终化必须按较小者复核。
    const int envelopeBytes = m_results.lastReadEnvelopeBytes();
    if (result.value("code").toString() == QLatin1String("ok") && envelopeBytes > 0) {
      m_deliveredReadCallId = m_activeCall.id;
      m_deliveredReadQuota = envelopeBytes;
    }
    finishActive(result.value("code").toString() == QLatin1String("ok"), result);
  } else if(m_activeCall.name==QLatin1String("paleo.python_environment"))
    runPythonEnvironment();
  else if(m_activeCall.name==QLatin1String("paleo.workflow_plan_preview"))
    runPlanPreview(args);
  // I3：精确名称集合分派（isMapDisplayTool），不用 paleo.map_ 前缀——
  // I4 的 paleo.map_locate 不得被本族吞掉。
  else if(paleo::agent::mapdisplay::isMapDisplayTool(m_activeCall.name))
    runMapDisplayTool(m_activeCall.name, args);
  else if(m_activeCall.name.startsWith(QLatin1String("paleo.operation_")))
    runPersistentOperation(args);
  else if(m_activeCall.name==QLatin1String("paleo.tile_classification_v2"))
    runTileV2(args);
  else if (m_activeCall.name == QStringLiteral("paleo.tile_classification"))
    runTile(m_activeCall, args);
  else if (m_activeCall.name ==
           QStringLiteral("paleo.horizon_tracking_suggestion"))
    runHorizon(m_activeCall, args);
  else if (m_activeCall.name == QStringLiteral("paleo.well_facies_prediction"))
    runFacies(args);
  else if (m_activeCall.name == QStringLiteral("paleo.query_project"))
    runQuery(m_activeCall, args);
  else if (m_activeCall.name == QStringLiteral("paleo.asset_lineage"))
    runLineage(m_activeCall, args);
  else if (isAiCapabilityTool(m_activeCall.name))
    runCapability(dispatch.spec.title, args);
  else
    finishActive(false, errorPayload(tr("未登记的工具 %1").arg(m_activeCall.name)));
}

void AiChatToolRunner::runCapability(const QString &title, const QJsonObject &args) {
  using namespace paleo::capability;
  if (m_context.requireCapabilityHost && !m_context.capabilityHost) {
    finishActive(false, QJsonObject{{QStringLiteral("code"), QStringLiteral("host_unavailable")}});
    return;
  }
  if (m_context.projectDir.isEmpty() || m_context.documentId.isEmpty()) {
    finishActive(false, errorPayload(tr("能力工具须由宿主绑定工程")));
    return;
  }
  if (!m_context.capabilityHost && m_context.catalog && m_context.catalog->refusesWrites()) {
    finishActive(false, QJsonObject{{QStringLiteral("code"), QStringLiteral("project_read_only")}});
    return;
  }
  ExecuteRequest request;
  const auto rejected = m_context.capabilityHost?prepareHostedChatToolRequest(request,m_activeCall.name,args):prepareChatToolRequest(
      request, m_activeCall.name, args, QStringLiteral("chat-host"), m_context.documentId,
      m_context.projectDir + QStringLiteral("/.paleo/capability-ops"));
  if (!rejected.isEmpty()) {
    finishActive(false, rejected);
    return;
  }
  const int generation = m_generation;
  const QString callId = m_activeCall.id;
  QString rejectionCode;
  const auto finished = [this, generation, callId](ExecuteResult result) {
        if (generation != m_generation || !m_active || m_activeCall.id != callId)
          return;
        finishActive(result.ok, chatToolResponse(result));
      };
  const QPointer<AiChatToolRunner> guard(this);
  PaleoTask *task = nullptr;
  if (m_context.capabilityHost) {
    const auto prepared = m_context.capabilityHost->prepare(request, paleo::host::Caller::Agent, {}, {}, {},m_resultSession);
    if (!prepared.ok) {
      finishActive(false, QJsonObject{{QStringLiteral("code"), prepared.code}});
      return;
    }
    task = m_context.capabilityHost->start(prepared.handle, request, this, title, finished, &rejectionCode);
  } else {
    task = startCapabilityTask(m_taskService, this, title, std::move(request), finished, &rejectionCode);
  }
  if(!guard){if(task)task->requestInterruption();return;}
  if (generation != m_generation) {
    if (task) {
      if(m_explicitCancelGeneration>generation)task->requestCancel();else task->requestInterruption();
    }
    return;
  }
  m_activeTask = task;
  // taskAdded can rebind the host synchronously before the task is assigned.
  if (task && !activeContextIsCurrent())
    task->requestInterruption();
  if (!task)
    finishActive(false, QJsonObject{{QStringLiteral("code"), rejectionCode}});
}

QJsonObject AiChatToolRunner::errorPayload(const QString &message) const {
  QJsonObject payload;
  payload.insert(QStringLiteral("tool"), m_activeCall.name);
  payload.insert(QStringLiteral("error"), message);
  return payload;
}

void AiChatToolRunner::finishActive(bool ok, const QJsonObject &payload) {
  const ChatToolCall call = m_activeCall;
  const bool wasFacies = m_activeIsFacies;
  const int generation = m_generation;
  m_activeIsFacies = false;
  m_activeTask.clear();
  if (wasFacies)
    m_facies->disconnect(this); // 单发钩子：用完即拆（串行执行下防堆积）
  // 统一异步回报（#281）：同步失败分支（实参非法/未路由/上下文未绑定）若
  // 从 run() 的调用栈直接 emit，调用方（aichatcontroller 的派发循环）会在
  // 同一栈里被重入改容器——range-for 迭代器失效 UB。toolFinished 一律经
  // 事件循环再发，成功/失败同一语义；cancel 经世代号把在途报告作废（取消=
  // 作废，口径见头文件），对象析构时 Qt 随 context 丢弃未投递的入队调用。
  QMetaObject::invokeMethod(
    this,
    [this, generation, call, ok, payload]() {
      if (generation != m_generation)
        return; // 已取消/作废：在途结果不再回灌
      const bool current = activeContextIsCurrent();
      const QJsonObject result = current ? payload
          : QJsonObject{{QStringLiteral("code"), QStringLiteral("stale_session")}};
      m_active = false;
      const QPointer<AiChatToolRunner> guard(this);
      emit toolFinished(call, current && ok,
                        QString::fromUtf8(QJsonDocument(result).toJson(QJsonDocument::Compact)));
      if (guard) startNext();
    },
    Qt::QueuedConnection);
}

void AiChatToolRunner::runTile(const ChatToolCall &call,
                               const QJsonObject &args) {
  Q_UNUSED(call)
  if (!m_workflow) {
    finishActive(false, errorPayload(tr("tile 分类执行面未绑定（AiAssistWorkflow）")));
    return;
  }
  if (m_context.horizon.isEmpty() || !m_context.gridFetch) {
    finishActive(false,
                 errorPayload(tr("tile 分类上下文未绑定（须打开工程并提供网格取数）")));
    return;
  }
  const QString model = args.value(QStringLiteral("model")).toString();
  const int rows = intArg(args, "grid_rows", 0);
  const int cols = intArg(args, "grid_columns", 0);
  if (model.isEmpty() || rows <= 0 || cols <= 0) {
    finishActive(false, errorPayload(tr("模型名与正数网格尺寸为必填")));
    return;
  }
  // PW-012：网格尺寸来自模型输出，不可信——超限直接拒绝（不钳制，免得静默
  // 改变产物几何）；tile/halo 钳到安全域。
  if (rows > kMaxTileGridSide || cols > kMaxTileGridSide ||
      qint64(rows) * cols > kMaxTileGridPixels) {
    finishActive(false, errorPayload(tr("网格尺寸 %1×%2 超出上限（单边 ≤%3，总像元 ≤%4）")
                                         .arg(rows).arg(cols).arg(kMaxTileGridSide)
                                         .arg(kMaxTileGridPixels)));
    return;
  }
  const int tileRows = qBound(1, intArg(args, "tile_rows", 128), kMaxTileSide);
  const int tileCols = qBound(1, intArg(args, "tile_columns", 128), kMaxTileSide);
  const int halo = qBound(0, intArg(args, "halo", 8), kMaxTileHalo);
  QString error;
  PaleoTask *task = m_workflow->startClassification(
    m_context.horizon, model, rows, cols, tileRows, tileCols, halo,
    m_context.gridFetch, kChatLowConfidenceThreshold, &error);
  if (!task) {
    finishActive(false, errorPayload(error));
    return;
  }
  m_activeTask = task;
  const int generation = m_generation;
  const QString callId = m_activeCall.id;
  connect(task, &PaleoTask::finished, this, [this, generation, callId]() {
    if (generation != m_generation || !m_active || m_activeCall.id != callId)
      return; // 已被取消/作废或串台：迟到终态不回灌
    const PaleoTask::State state = m_activeTask->state();
    if (state == PaleoTask::State::Succeeded) {
      QJsonObject payload = m_workflow->lastProductStats();
      payload.insert(QStringLiteral("tool"),
                     QStringLiteral("paleo.tile_classification"));
      payload.insert(QStringLiteral("horizon"), m_context.horizon);
      payload.insert(
        QStringLiteral("note"),
        tr("产物已按草稿态登记为 DERIVED 派生资产（03_Predict 组），"
           "未改任何解释数据"));
      finishActive(true, payload);
      return;
    }
    if (state == PaleoTask::State::Cancelled)
      finishActive(false, errorPayload(tr("tile 分类已取消（未写任何产品）")));
    else
      finishActive(false, errorPayload(m_activeTask->errorText()));
  });
}

void AiChatToolRunner::runHorizon(const ChatToolCall &call,
                                  const QJsonObject &args) {
  Q_UNUSED(call)
  if (!m_workflow) {
    finishActive(false, errorPayload(tr("层位建议执行面未绑定（AiAssistWorkflow）")));
    return;
  }
  if (!m_context.traceFetch && !m_context.trackingInput) {
    finishActive(false,
                 errorPayload(tr("层位建议上下文未绑定（须提供道窗取数）")));
    return;
  }
  const QString model = args.value("model").toString();
  const int windowSamples = args.value("window_samples").toInt();
  const int radius = args.value("radius").toInt();
  QVector<TrackingSeed> parsed;
  for (const auto &value : args.value("seeds").toArray()) {
    const auto seed = value.toObject();
    parsed.append(TrackingSeed{seed.value("inline").toInt(), seed.value("xline").toInt(),
                               seed.value("sample_index").toInt()});
  }
  QString error;
  auto cancelled = std::make_shared<std::atomic_bool>(false);
  const int generation = m_generation;
  const quint64 bindingRevision = m_bindingRevision;
  const QString callId = m_activeCall.id;
  const QString horizon = m_context.horizon;
  const QPointer<AiChatToolRunner> guard(this);
  const QPointer<AiAssistWorkflow> workflow(m_workflow);
  const auto invocationCurrent = [&] {
    return guard && generation == m_generation && m_active && m_activeCall.id == callId;
  };
  const auto bindingCurrent = [&] {
    return workflow && workflow == m_workflow && bindingRevision == m_bindingRevision && activeContextIsCurrent();
  };
  const auto provider = m_context.trackingInput;
  TrackingInput input;
  input.fetch = m_context.traceFetch;
  const bool available = !provider || provider([cancelled] { return cancelled->load(); }, &input, &error);
  if (!invocationCurrent()) return;
  if (!bindingCurrent()) {
    finishActive(false, QJsonObject{{"code", "context_rebound"}});
    return;
  }
  if (!available) {
    finishActive(false, QJsonObject{{"code", "tracking_context_unavailable"}, {"error", error}});
    return;
  }
  const QPointer<PaleoTask> task = workflow->startSuggestion(
    horizon, model, parsed, windowSamples, radius, input.fetch, &error, input.ownerValidation);
  if (!invocationCurrent()) {
    if (task) task->requestInterruption();
    return;
  }
  if (!bindingCurrent()) {
    finishActive(false, QJsonObject{{"code", "context_rebound"}});
    if (task) task->requestInterruption();
    return;
  }
  if (!task) {
    finishActive(false, errorPayload(error));
    return;
  }
  connect(task.data(), &PaleoTask::cancelAsked, task.data(), [cancelled] { cancelled->store(true); });
  if (task->cancelRequested()) cancelled->store(true);
  m_activeTask = task;
  connect(task.data(), &PaleoTask::finished, this, [this, generation, callId, workflow, horizon, identity = input.identity]() {
    if (generation != m_generation || !m_active || m_activeCall.id != callId)
      return;
    if (!activeContextIsCurrent() || !workflow) {
      finishActive(false, QJsonObject{{"code", "stale_session"}});
      return;
    }
    const PaleoTask::State state = m_activeTask->state();
    const QString inputError = m_activeTask->property("paleoTrackingInputError").toString();
    if (!inputError.isEmpty()) {
      finishActive(false, QJsonObject{{QStringLiteral("code"), inputError}}); return;
    }
    if (state == PaleoTask::State::Succeeded) {
      // 建议已入裁决队列（startSuggestion 收尾）：数量从队列侧取——
      // 非种子未裁决 + 已接受（含种子锚点）= 总数。
      const int pending = workflow->pendingCount(horizon);
      const int accepted = workflow->acceptedCount(horizon);
      int seeds = 0;
      for (const TrackingSuggestion &s :
           workflow->acceptedSuggestions(horizon))
        if (s.isSeed)
          ++seeds;
      QJsonObject payload;
      payload.insert(QStringLiteral("tool"),
                     QStringLiteral("paleo.horizon_tracking_suggestion"));
      payload.insert(QStringLiteral("horizon"), horizon);
      payload.insert(QStringLiteral("suggestions_total"), pending + accepted);
      payload.insert(QStringLiteral("pending_review"), pending);
      payload.insert(QStringLiteral("input"), identity);
      payload.insert(QStringLiteral("model_sha256"), m_activeTask->property("paleoModelSha256").toString());
      payload.insert(QStringLiteral("seed_count"), seeds);
      payload.insert(
        QStringLiteral("note"),
        tr("建议已进入待裁决队列（解释员逐条接受/否决，显式提交才落库；"
           "未自动写任何解释）"));
      finishActive(true, payload);
      return;
    }
    if (state == PaleoTask::State::Cancelled)
      finishActive(false, errorPayload(tr("层位建议已取消（建议未入队）")));
    else
      finishActive(false, errorPayload(m_activeTask->errorText()));
  });
}

QString AiChatToolRunner::projectBrief(const DataCatalog *catalog,
                                       const QString &projectDir) {
  if (!catalog || !catalog->isOpen())
    return tr("未打开——工程数据查询（query/lineage 工具）会得到如实报错。");
  const QHash<QString, int> entities = catalog->entityCountsByType();
  const int wells = entities.value(QStringLiteral("well"));
  const int planned = entities.value(QStringLiteral("planned"));
  const int surveys = entities.value(QStringLiteral("seismic_survey"));
  const int boundaries = entities.value(QStringLiteral("sequence_boundary"));
  const int auxiliary = entities.value(QStringLiteral("auxiliary"));
  const QHash<QString, int> assetCounts = catalog->assetCountsByType();
  // 计数降序、同数按名稳定排序；超上限归并成「等 N 类」。
  QList<QPair<QString, int>> ranked;
  for (auto it = assetCounts.cbegin(); it != assetCounts.cend(); ++it)
    ranked.append(qMakePair(it.key(), it.value()));
  std::sort(ranked.begin(), ranked.end(),
            [](const QPair<QString, int> &a, const QPair<QString, int> &b) {
              return a.second == b.second ? a.first < b.first
                                          : a.second > b.second;
            });
  QStringList assetParts;
  for (int i = 0; i < ranked.size() && i < kBriefAssetTypes; ++i)
    assetParts.append(tr("%1 %2").arg(ranked[i].first).arg(ranked[i].second));
  if (ranked.size() > kBriefAssetTypes)
    assetParts.append(tr("等 %1 类").arg(ranked.size()));
  QString brief = tr("井 %1 口（其中计划井 %2）、地震测线 %3、层序界面 %4、辅助 %5；"
                     "资产：%6；catalog 修订 %7。")
                    .arg(wells + planned)
                    .arg(planned)
                    .arg(surveys)
                    .arg(boundaries)
                    .arg(auxiliary)
                    .arg(assetParts.isEmpty() ? tr("（无）") : assetParts.join(QStringLiteral("、")))
                    .arg(catalog->catalogRevision());
  if (!projectDir.isEmpty())
    brief = tr("工程 %1：").arg(QFileInfo(projectDir).fileName()) + brief;
  if (brief.size() > kBriefCharCap)
    brief = brief.left(kBriefCharCap - 1) + QStringLiteral("…");
  return brief;
}
