// 层：功能
#include "aichattoolrunner.h"

#include <QJsonArray>
#include <QJsonDocument>
#include <QJsonValue>
#include <QPointer>

#include "../ai/chat/domaintools.h"
#include "../ai/wellfaciesservice.h"
#include "../domain/welllogfacies.h"
#include "../services/paleotaskservice.h"

// workflow/ — 聊天工具执行器实现（方向61）。
//
// 结果 JSON 契约：成功 = {"tool": <名>, ...摘要}；失败 = {"tool": <名>,
// "error": <人可读原因>}。两种都按 role=tool 回灌——协议要求每个
// tool_call_id 有应答，错误也让模型如实转述，不在本地吞掉。

namespace {
// 聊天面的 tile 分类低置信阈值：工具 schema 不暴露该参数（表 shape 与方向51
// 一致），执行面取 0.5（掩膜产品语义见 aiassistworkflow.h）。想调参走
// 工作流面板，不走聊天。
constexpr double kChatLowConfidenceThreshold = 0.5;

int intArg(const QJsonObject &args, const char *key, int fallback) {
  const QJsonValue value = args.value(QLatin1String(key));
  if (value.isDouble())
    return qRound(value.toDouble());
  if (value.isString()) // 表口径允许 "12" 文本整数（jsonTypeMatches）
    return value.toString().trimmed().toInt();
  return fallback;
}

double doubleArg(const QJsonObject &args, const char *key) {
  const QJsonValue value = args.value(QLatin1String(key));
  if (value.isDouble())
    return value.toDouble();
  if (value.isString()) {
    bool ok = false;
    const double parsed = value.toString().trimmed().toDouble(&ok);
    if (ok)
      return parsed;
  }
  return qQNaN(); // 未给
}
} // namespace

AiChatToolRunner::AiChatToolRunner(QObject *parent)
  : QObject(parent), m_facies(new WellFaciesService(this)) {}

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
  m_faciesConfig = config;
  m_facies->configure(config);
}

void AiChatToolRunner::setContext(const Context &context) {
  m_context = context;
}

void AiChatToolRunner::run(const ChatToolCall &call) {
  m_queue.append(call);
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
  m_activeCall = m_queue.takeFirst();
  m_active = true;
  m_activeIsFacies = false;
  emit toolStarted(m_activeCall);

  QJsonParseError parseError;
  const QJsonDocument document =
    QJsonDocument::fromJson(m_activeCall.argumentsJson.toUtf8(), &parseError);
  const QJsonObject args = document.object();
  const AiToolDispatch dispatch = dispatchAiTool(m_activeCall.name, args);
  // 表驱动诚实态：未登记 / 本构建不可用 / 实参不合格 → 错误结果回灌
  // （不冒充成功，也不静默丢帧）。
  if (parseError.error != QJsonParseError::NoError && !document.isNull()) {
    finishActive(false, errorPayload(tr("实参不是合法 JSON：%1")
                                       .arg(parseError.errorString())));
    return;
  }
  if (dispatch.status != AiToolDispatchStatus::Routed) {
    const QString text = aiToolDispatchLabel(dispatch.status) +
                         (dispatch.note.isEmpty()
                            ? QString()
                            : QStringLiteral("：%1").arg(dispatch.note));
    finishActive(false, errorPayload(text));
    return;
  }
  if (m_activeCall.name == QStringLiteral("paleo.tile_classification"))
    runTile(m_activeCall, args);
  else if (m_activeCall.name ==
           QStringLiteral("paleo.horizon_tracking_suggestion"))
    runHorizon(m_activeCall, args);
  else if (m_activeCall.name == QStringLiteral("paleo.well_facies_prediction"))
    runFacies(m_activeCall, args);
  else
    finishActive(false, errorPayload(tr("未登记的工具 %1").arg(m_activeCall.name)));
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
  m_active = false;
  m_activeIsFacies = false;
  m_activeTask.clear();
  if (wasFacies)
    m_facies->disconnect(this); // 单发钩子：用完即拆（串行执行下防堆积）
  emit toolFinished(call, ok,
                    QString::fromUtf8(QJsonDocument(payload).toJson(
                      QJsonDocument::Compact)));
  startNext();
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
  const int tileRows = qMax(1, intArg(args, "tile_rows", 128));
  const int tileCols = qMax(1, intArg(args, "tile_columns", 128));
  const int halo = qMax(0, intArg(args, "halo", 8));
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
  if (!m_context.traceFetch) {
    finishActive(false,
                 errorPayload(tr("层位建议上下文未绑定（须提供道窗取数）")));
    return;
  }
  const QString model = args.value(QStringLiteral("model")).toString();
  const int windowSamples = intArg(args, "window_samples", 0);
  const int radius = intArg(args, "radius", 0);
  const QJsonArray seeds = args.value(QStringLiteral("seeds")).toArray();
  QVector<TrackingSeed> parsed;
  for (const QJsonValue &value : seeds) {
    const QJsonObject seed = value.toObject();
    TrackingSeed s;
    s.inlineNo = intArg(seed, "inline", 0);
    s.xlineNo = intArg(seed, "xline", 0);
    s.sampleIndex = intArg(seed, "sample_index", 0);
    parsed.append(s);
  }
  if (model.isEmpty() || windowSamples <= 0 || radius <= 0 || parsed.isEmpty()) {
    finishActive(false,
                 errorPayload(tr("模型名、正数窗长/半径与至少一个种子点为必填")));
    return;
  }
  QString error;
  PaleoTask *task = m_workflow->startSuggestion(
    m_context.horizon, model, parsed, windowSamples, radius,
    m_context.traceFetch, &error);
  if (!task) {
    finishActive(false, errorPayload(error));
    return;
  }
  m_activeTask = task;
  const int generation = m_generation;
  const QString callId = m_activeCall.id;
  connect(task, &PaleoTask::finished, this, [this, generation, callId]() {
    if (generation != m_generation || !m_active || m_activeCall.id != callId)
      return;
    const PaleoTask::State state = m_activeTask->state();
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

void AiChatToolRunner::runFacies(const ChatToolCall &call,
                                 const QJsonObject &args) {
  Q_UNUSED(call)
  const QString endpointError =
    WellFaciesConfig::validateUrl(m_faciesConfig.baseUrl,
                                  m_faciesConfig.allowInsecureHttp);
  if (!endpointError.isEmpty() || m_faciesConfig.apiKey.isEmpty()) {
    finishActive(false,
                 errorPayload(tr("测井相服务未配置（端点与密钥须显式设置）")));
    return;
  }
  if (!m_context.faciesInput) {
    finishActive(false,
                 errorPayload(tr("未绑定井曲线数据上下文（须打开工程并加载井数据）")));
    return;
  }
  const QString modelId = args.value(QStringLiteral("model")).toString();
  const QString well = args.value(QStringLiteral("well_name")).toString();
  const double depthFrom = doubleArg(args, "depth_from");
  const double depthTo = doubleArg(args, "depth_to");
  if (modelId.isEmpty() || well.isEmpty()) {
    finishActive(false, errorPayload(tr("模型 id 与井名为必填")));
    return;
  }
  WellFaciesInput input;
  QString inputError;
  if (!m_context.faciesInput(well, depthFrom, depthTo, &input, &inputError)) {
    finishActive(false, errorPayload(inputError));
    return;
  }
  if (!input.ready()) {
    finishActive(false, errorPayload(input.reason));
    return;
  }
  WellFaciesModel model;
  if (m_context.faciesModel)
    model = m_context.faciesModel(modelId);
  model.id = modelId;
  m_activeIsFacies = true;
  const int generation = m_generation;
  const QString callId = m_activeCall.id;
  connect(m_facies, &WellFaciesService::completed, this,
          [this, generation, callId, well](const WellFaciesResult &result) {
            if (generation != m_generation || !m_active ||
                m_activeCall.id != callId)
              return;
            QJsonObject payload;
            payload.insert(QStringLiteral("tool"),
                           QStringLiteral("paleo.well_facies_prediction"));
            payload.insert(QStringLiteral("well"), well);
            payload.insert(QStringLiteral("model"), result.modelName);
            payload.insert(QStringLiteral("intervals"),
                           int(result.intervals.size()));
            QJsonArray sample;
            const int kSample = 20; // 摘要上限：全文给模型只会烧上下文
            for (int i = 0; i < result.intervals.size() && i < kSample; ++i) {
              const auto &interval = result.intervals[i];
              QJsonObject entry;
              entry.insert(QStringLiteral("top"), double(interval.topDepth));
              entry.insert(QStringLiteral("bottom"),
                           double(interval.bottomDepth));
              entry.insert(QStringLiteral("category"), interval.category);
              sample.append(entry);
            }
            payload.insert(QStringLiteral("intervals_sample"), sample);
            if (result.intervals.size() > kSample)
              payload.insert(
                QStringLiteral("truncated"),
                tr("仅前 %1 段（共 %2 段）").arg(kSample).arg(result.intervals.size()));
            finishActive(true, payload);
          });
  connect(m_facies, &WellFaciesService::failed, this,
          [this, generation, callId](const QString &reason) {
            if (generation != m_generation || !m_active ||
                m_activeCall.id != callId)
              return;
            finishActive(false, errorPayload(reason));
          });
  m_facies->predict(model, well, input);
}
