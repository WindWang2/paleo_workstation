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
#include "../ai/tileinference.h"
#include "../ai/wellfaciesservice.h"
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

// PW-013：层位追踪参数上限（radius/种子来自 LLM，O(seeds·(2r+1)²)）。
constexpr int kMaxSuggestRadius = 256;
constexpr int kMaxSuggestSeeds = 64;
constexpr int kMaxSuggestWindow = 4096;
constexpr int kMaxLineNumberAbs = 10000000; // 线号/样点号合理域，防 ±radius 溢出

} // namespace

AiChatToolRunner::AiChatToolRunner(QObject *parent)
  : QObject(parent), m_facies(new WellFaciesService(this)), m_query(std::make_unique<paleo::query::ProjectQueryCapability>()) {}

AiChatToolRunner::~AiChatToolRunner() {
  ++m_generation; // 作废在途回调
  m_queue.clear();
  if (m_activeTask)
    m_activeTask->requestCancel();
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
  const bool changed = context.catalog != m_context.catalog || context.documentId != m_context.documentId ||
      context.projectDir != m_context.projectDir || context.documentEpoch != m_context.documentEpoch ||
      context.capabilityHost != m_context.capabilityHost || context.requireCapabilityHost != m_context.requireCapabilityHost;
  if (changed)
    invalidateContext();
  m_context = context;
  if (changed) rebindQueryScope();
}

void AiChatToolRunner::setTaskService(PaleoTaskService *service) {
  if (m_taskService == service)
    return;
  invalidateContext();
  m_taskService = service;
}

void AiChatToolRunner::invalidateContext() {
  ++m_contextGeneration;
  ++m_resultEpoch;
  rebindQueryScope();
  if (m_activeTask)
    m_activeTask->requestCancel();
}

quint64 AiChatToolRunner::taskSession() const {
  return m_taskService ? m_taskService->session() : 0;
}
void AiChatToolRunner::rebindQueryScope() {
  m_query->bind(m_context.catalog, {m_context.documentId, m_context.documentEpoch, m_contextGeneration, m_querySessionId});
}

bool AiChatToolRunner::activeContextIsCurrent() const {
  return m_activeContextGeneration == m_contextGeneration && m_activeTaskSession == taskSession();
}

void AiChatToolRunner::run(const ChatToolCall &call) {
  m_queue.append({call, m_contextGeneration, taskSession(),m_faciesEpoch});
  startNext();
}

void AiChatToolRunner::cancel() {
  // 取消 = 作废（口径见头文件）：在途结果不再 emit；tile 任务协作取消
  // （不写产品）；层位任务算完但丢弃；测井相 HTTP 中止。
  ++m_generation;
  m_queue.clear();
  if (m_activeTask)
    m_activeTask->requestCancel();
  if (m_activeIsFacies) {
    m_facies->disconnect(this);
    m_facies->cancel();
  }
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
  m_active = true;
  m_activeIsFacies = false;
  const int generation = m_generation;
  emit toolStarted(m_activeCall);
  if (generation != m_generation)
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
  const QJsonDocument document =
    QJsonDocument::fromJson(m_activeCall.argumentsJson.toUtf8(), &parseError);
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
      auto expected = live;
      expected.documentId = m_context.documentId; expected.epoch = m_context.documentEpoch;
      if (live.documentId != expected.documentId || live.epoch != expected.epoch)
        code = QStringLiteral("stale_session");
      else if (m_activeCall.name != QLatin1String("paleo.read_result")) {
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
  const auto runtime=readiness(m_activeCall.name,args);
  if(!runtime.ready) {finishActive(false,QJsonObject{{"code",runtime.code}});return;}
  if (m_activeCall.name == QStringLiteral("paleo.read_result")) {
    refreshResultScope();
    const auto result = m_results.read(args);
    finishActive(result.value("code").toString() == QLatin1String("ok"), result);
  } else if(m_activeCall.name.startsWith(QLatin1String("paleo.operation_")))
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
  if (generation != m_generation) {
    if (task)
      task->requestCancel();
    return;
  }
  m_activeTask = task;
  // taskAdded can rebind the host synchronously before the task is assigned.
  if (task && !activeContextIsCurrent())
    task->requestCancel();
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
      emit toolFinished(call, current && ok,
                        QString::fromUtf8(QJsonDocument(result).toJson(QJsonDocument::Compact)));
      startNext();
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
  const QString model = args.value(QStringLiteral("model")).toString();
  const int windowSamples = intArg(args, "window_samples", 0);
  const int radius = intArg(args, "radius", 0);
  const QJsonArray seeds = args.value(QStringLiteral("seeds")).toArray();
  // PW-013：radius/种子数/种子坐标设上限，超限拒绝（不进入 O(seeds·r²) 扩张）。
  if (radius > kMaxSuggestRadius || windowSamples > kMaxSuggestWindow ||
      seeds.size() > kMaxSuggestSeeds) {
    finishActive(false,
                 errorPayload(tr("层位建议参数超限：radius ≤%1、窗长 ≤%2、种子数 ≤%3")
                                .arg(kMaxSuggestRadius).arg(kMaxSuggestWindow)
                                .arg(kMaxSuggestSeeds)));
    return;
  }
  QVector<TrackingSeed> parsed;
  for (const QJsonValue &value : seeds) {
    const QJsonObject seed = value.toObject();
    TrackingSeed s;
    s.inlineNo = intArg(seed, "inline", kMaxLineNumberAbs + 1);
    s.xlineNo = intArg(seed, "xline", kMaxLineNumberAbs + 1);
    s.sampleIndex = intArg(seed, "sample_index", -1);
    if (s.inlineNo < -kMaxLineNumberAbs || s.inlineNo > kMaxLineNumberAbs ||
        s.xlineNo < -kMaxLineNumberAbs || s.xlineNo > kMaxLineNumberAbs || s.sampleIndex < 0 ||
        s.sampleIndex > kMaxLineNumberAbs) {
      finishActive(false, errorPayload(tr("种子点坐标缺失或越界（须为整数线号与非负样点号）")));
      return;
    }
    parsed.append(s);
  }
  if (model.isEmpty() || windowSamples <= 0 || radius <= 0 || parsed.isEmpty()) {
    finishActive(false,
                 errorPayload(tr("模型名、正数窗长/半径与至少一个种子点为必填")));
    return;
  }
  const qint64 diameter = qint64(radius) * 2 + 1;
  const qint64 expanded = qint64(seeds.size()) * diameter * diameter;
  if (expanded > 20000 || expanded * windowSamples > 8'000'000) {
    finishActive(false, QJsonObject{{QStringLiteral("code"), QStringLiteral("tracking_expansion_budget")}});
    return;
  }
  QString error;
  auto cancelled = std::make_shared<std::atomic_bool>(false);
  const int generation = m_generation;
  const QString callId = m_activeCall.id;
  TrackingInput input; input.fetch = m_context.traceFetch;
  if (m_context.trackingInput && !m_context.trackingInput(
      [cancelled] { return cancelled->load(); }, &input, &error)) {
    finishActive(false, QJsonObject{{QStringLiteral("code"), QStringLiteral("tracking_context_unavailable")},
                                  {QStringLiteral("error"), error}});
    return;
  }
  if (generation != m_generation || !m_active || m_activeCall.id != callId) return;
  PaleoTask *task = m_workflow->startSuggestion(
    m_context.horizon, model, parsed, windowSamples, radius,
    input.fetch, &error, input.ownerValidation);
  if (!task) {
    finishActive(false, errorPayload(error));
    return;
  }
  connect(task, &PaleoTask::cancelAsked, task, [cancelled] { cancelled->store(true); });
  if (task->cancelRequested()) cancelled->store(true);
  if (generation != m_generation || !m_active || m_activeCall.id != callId || !activeContextIsCurrent()) {
    task->requestCancel(); return;
  }
  m_activeTask = task;
  connect(task, &PaleoTask::finished, this, [this, generation, callId, identity = input.identity]() {
    if (generation != m_generation || !m_active || m_activeCall.id != callId)
      return;
    const PaleoTask::State state = m_activeTask->state();
    const QString inputError = m_activeTask->property("paleoTrackingInputError").toString();
    if (!inputError.isEmpty()) {
      finishActive(false, QJsonObject{{QStringLiteral("code"), inputError}}); return;
    }
    if (state == PaleoTask::State::Succeeded) {
      // 建议已入裁决队列（startSuggestion 收尾）：数量从队列侧取——
      // 非种子未裁决 + 已接受（含种子锚点）= 总数。
      const int pending = m_workflow->pendingCount(m_context.horizon);
      const int accepted = m_workflow->acceptedCount(m_context.horizon);
      int seeds = 0;
      for (const TrackingSuggestion &s :
           m_workflow->acceptedSuggestions(m_context.horizon))
        if (s.isSeed)
          ++seeds;
      QJsonObject payload;
      payload.insert(QStringLiteral("tool"),
                     QStringLiteral("paleo.horizon_tracking_suggestion"));
      payload.insert(QStringLiteral("horizon"), m_context.horizon);
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
