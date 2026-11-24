// 层：功能
#include "mappingexplanationservice.h"
#include "../services/paleotaskservice.h"

#include <QDateTime>
#include <QJsonDocument>
#include <QJsonObject>
#include <QMetaObject>
#include <QRegularExpression>
#include <QUuid>
#include <cmath>

namespace {
constexpr int kMaxQueuedRequests = 32;
constexpr int kMaxCombinedLength = 8500;
constexpr int kBackgroundIdleTimeoutMs = 25000;
constexpr int kBackgroundTotalTimeoutMs = 45000;
constexpr int kBackgroundMaxTokens = 512;

QString operationLabel(const QString &kind) {
  if (kind == QLatin1String("paint"))
    return MappingExplanationService::tr("绘制/覆盖相面");
  if (kind == QLatin1String("split"))
    return MappingExplanationService::tr("分割相面");
  if (kind == QLatin1String("merge"))
    return MappingExplanationService::tr("合并相面");
  if (kind == QLatin1String("geometry"))
    return MappingExplanationService::tr("调整相面边界几何");
  if (kind == QLatin1String("classification"))
    return MappingExplanationService::tr("调整相归属分类");
  if (kind == QLatin1String("save"))
    return MappingExplanationService::tr("保存编图修订");
  if (kind == QLatin1String("manual_request"))
    return MappingExplanationService::tr("编图现状说明");
  return MappingExplanationService::tr("相面编辑");
}

QString formatArea(double areaSqm) {
  if (!std::isfinite(areaSqm) || areaSqm <= 0.0)
    return QString();
  if (areaSqm >= 100000.0)
    return MappingExplanationService::tr("约 %1 km²")
        .arg(areaSqm / 1000000.0, 0, 'f', 3);
  return MappingExplanationService::tr("约 %1 m²").arg(areaSqm, 0, 'f', 0);
}

QString formatWellsSummary(const QVariantList &wells) {
  if (wells.isEmpty())
    return QString();
  QStringList parts;
  const int limit = qMin(6, int(wells.size()));
  for (int i = 0; i < limit; ++i) {
    const auto row = wells.at(i).toMap();
    const QString name = row.value(QStringLiteral("well_name")).toString().trimmed();
    if (name.isEmpty())
      continue;
    const QString dominant = row.value(QStringLiteral("formation_dominant")).toString().trimmed();
    parts << (dominant.isEmpty() ? name : QStringLiteral("%1（%2）").arg(name, dominant));
  }
  if (parts.isEmpty())
    return QString();
  QString joined = parts.join(QStringLiteral("、"));
  if (wells.size() > limit)
    joined += MappingExplanationService::tr("等 %1 口井").arg(wells.size());
  return joined;
}

void clampExplanationText(QString *text, QString *reason) {
  if (reason)
    *reason = reason->trimmed().left(300);
  if (text) {
    const int limit = qMax(200, kMaxCombinedLength - (reason ? int(reason->size()) : 0));
    *text = text->trimmed().left(limit);
  }
}

QString extractJsonObjectSlice(const QString &raw) {
  QString trimmed = raw.trimmed();
  const QRegularExpression fence(QStringLiteral("^```(?:json)?\\s*\\n([\\s\\S]*?)\\n```$"));
  const auto match = fence.match(trimmed);
  if (match.hasMatch())
    trimmed = match.captured(1).trimmed();
  const int open = trimmed.indexOf(QLatin1Char('{'));
  const int close = trimmed.lastIndexOf(QLatin1Char('}'));
  if (open >= 0 && close > open)
    return trimmed.mid(open, close - open + 1);
  return trimmed;
}

} // namespace

QVariantMap MappingExplanationResult::toVariantMap() const {
  return {
      {QStringLiteral("request_id"), requestId},
      {QStringLiteral("horizon"), horizon},
      {QStringLiteral("layer_id"), layerId},
      {QStringLiteral("version_id"), versionId},
      {QStringLiteral("feature_uid"), featureUid},
      {QStringLiteral("geometry_hash"), geometryHash},
      {QStringLiteral("facies_code"), faciesCode},
      {QStringLiteral("text"), text},
      {QStringLiteral("reason"), reason},
      {QStringLiteral("generator"), generator},
      {QStringLiteral("model"), model},
      {QStringLiteral("fallback_reason"), fallbackReason},
      {QStringLiteral("generated_at"), generatedAt},
      {QStringLiteral("source_versions"), referenceVersionIds},
      {QStringLiteral("apply_to_edit_basis"), applyToEditBasis},
      {QStringLiteral("ai_generated"), generator == QLatin1String("ai")},
      {QStringLiteral("auto_generated"), true},
      {QStringLiteral("worker_thread_id"), static_cast<qulonglong>(workerThreadId)},
  };
}

MappingExplanationResult MappingExplanationResult::fromVariantMap(const QVariantMap &map) {
  MappingExplanationResult out;
  out.requestId = map.value(QStringLiteral("request_id")).toString();
  out.horizon = map.value(QStringLiteral("horizon")).toString();
  out.layerId = map.value(QStringLiteral("layer_id")).toString();
  out.versionId = map.value(QStringLiteral("version_id")).toString();
  out.featureUid = map.value(QStringLiteral("feature_uid")).toString();
  out.geometryHash = map.value(QStringLiteral("geometry_hash")).toString();
  out.faciesCode = map.value(QStringLiteral("facies_code")).toInt();
  out.text = map.value(QStringLiteral("text")).toString();
  out.reason = map.value(QStringLiteral("reason")).toString();
  out.generator = map.value(QStringLiteral("generator")).toString();
  out.model = map.value(QStringLiteral("model")).toString();
  out.fallbackReason = map.value(QStringLiteral("fallback_reason")).toString();
  out.generatedAt = map.value(QStringLiteral("generated_at")).toString();
  out.referenceVersionIds = map.value(QStringLiteral("source_versions")).toStringList();
  out.applyToEditBasis = map.value(QStringLiteral("apply_to_edit_basis")).toBool();
  out.workerThreadId = static_cast<quintptr>(map.value(QStringLiteral("worker_thread_id")).toULongLong());
  return out;
}

class MappingExplanationService::Worker : public QObject {
  Q_OBJECT
public:
  explicit Worker(std::shared_ptr<std::atomic<quint64>> generation)
      : m_generation(std::move(generation)) {}

public slots:
  void enqueue(const MappingExplanationRequest &request) {
    if (m_shuttingDown || request.generation != m_generation->load())
      return;
    m_highWaterSeq = qMax(m_highWaterSeq, request.sequence);
    const auto key = coalesceKey(request);
    for (auto it = m_queue.begin(); it != m_queue.end(); ++it) {
      if (coalesceKey(*it) == key) {
        *it = request;
        if (!m_running)
          processNext();
        return;
      }
    }
    while (int(m_queue.size()) >= kMaxQueuedRequests)
      m_queue.pop_front();
    m_queue.push_back(request);
    if (!m_running)
      processNext();
  }

  void cancelAll() {
    m_queue.clear();
    if (m_client)
      m_client->cancel();
    m_running = false;
    m_buffer.clear();
    m_highWaterSeq = 0;
  }

  void shutdownOnWorkerThread() {
    m_shuttingDown = true;
    cancelAll();
    delete m_client;
    m_client = nullptr;
  }

signals:
  void completed(const MappingExplanationResult &result, int remaining,
                 quint64 highWaterSeq);

private:
  static QString coalesceKey(const MappingExplanationRequest &req) {
    return req.layerId + QChar(0x1f) + req.featureUid + QChar(0x1f) +
           (req.applyToEditBasis ? QStringLiteral("basis") : QStringLiteral("feature"));
  }

  void ensureClient() {
    if (m_client)
      return;
    m_client = new LlmClient(this);
    connect(m_client, &LlmClient::deltaReceived, this, [this](const QString &delta) {
      if (m_running)
        m_buffer += delta;
    });
    connect(m_client, &LlmClient::finished, this, [this](const QString &, int, int) {
      if (!m_running)
        return;
      finishActiveAi(QString());
    });
    connect(m_client, &LlmClient::errorOccurred, this,
            [this](LlmErrorKind kind, const QString &message) {
              if (!m_running)
                return;
              if (kind == LlmErrorKind::Cancelled) {
                m_running = false;
                m_buffer.clear();
                processNext();
                return;
              }
              finishActiveAi(message.isEmpty() ? llmErrorLabel(kind) : message);
            });
  }

  MappingExplanationResult baseResult(const MappingExplanationRequest &req) const {
    MappingExplanationResult result;
    result.generation = req.generation;
    result.sequence = req.sequence;
    result.requestId = req.requestId;
    result.projectDir = req.projectDir;
    result.horizon = req.horizon;
    result.layerId = req.layerId;
    result.versionId = req.versionId;
    result.featureUid = req.featureUid;
    result.geometryHash = req.geometryHash;
    result.faciesCode = req.faciesCode;
    result.referenceVersionIds = req.referenceVersionIds;
    result.applyToEditBasis = req.applyToEditBasis;
    result.generatedAt = QDateTime::currentDateTimeUtc().toString(Qt::ISODateWithMs);
    result.workerThreadId = reinterpret_cast<quintptr>(QThread::currentThreadId());
    return result;
  }

  void deliverAndContinue(const MappingExplanationResult &result) {
    m_running = false;
    m_buffer.clear();
    if (!m_shuttingDown && result.generation == m_generation->load())
      emit completed(result, int(m_queue.size()), m_highWaterSeq);
    processNext();
  }

  void finishActiveAi(const QString &aiError) {
    const auto req = m_active;
    auto result = baseResult(req);
    if (req.generation != m_generation->load() || m_shuttingDown) {
      m_running = false;
      m_buffer.clear();
      processNext();
      return;
    }
    if (aiError.isEmpty() &&
        MappingExplanationService::parseAiResponse(m_buffer, req, &result.text, &result.reason)) {
      result.generator = QStringLiteral("ai");
      result.model = req.llmConfig.model;
    } else {
      MappingExplanationService::buildLocalFactualExplanation(req, &result.text, &result.reason);
      result.generator = QStringLiteral("local_factual");
      result.fallbackReason = aiError.isEmpty()
                                  ? MappingExplanationService::tr("AI 返回内容为空或无法解析，已自动生成本地事实摘要")
                                  : MappingExplanationService::tr("AI 生成未完成（%1），已自动生成本地事实摘要")
                                        .arg(aiError);
    }
    deliverAndContinue(result);
  }

  void processNext() {
    if (m_shuttingDown || m_running)
      return;
    while (!m_queue.empty()) {
      const auto req = m_queue.front();
      m_queue.pop_front();
      if (req.generation != m_generation->load())
        continue;
      if (req.llmConfig.enabled()) {
        m_active = req;
        m_running = true;
        m_buffer.clear();
        ensureClient();
        LlmConfig cfg = req.llmConfig;
        cfg.timeoutMs = qMin(cfg.timeoutMs, kBackgroundIdleTimeoutMs);
        cfg.totalTimeoutMs = qMin(cfg.totalTimeoutMs, kBackgroundTotalTimeoutMs);
        cfg.maxTokens = qMin(cfg.maxTokens, kBackgroundMaxTokens);
        m_client->setConfig(cfg);
        m_client->send(MappingExplanationService::buildPromptMessages(req));
        return;
      }
      auto result = baseResult(req);
      MappingExplanationService::buildLocalFactualExplanation(req, &result.text, &result.reason);
      result.generator = QStringLiteral("local_factual");
      deliverAndContinue(result);
      return;
    }
  }

  std::shared_ptr<std::atomic<quint64>> m_generation;
  std::deque<MappingExplanationRequest> m_queue;
  MappingExplanationRequest m_active;
  LlmClient *m_client = nullptr;
  QString m_buffer;
  quint64 m_highWaterSeq = 0;
  bool m_running = false;
  bool m_shuttingDown = false;
};

MappingExplanationService::MappingExplanationService(QObject *parent)
    : QObject(parent), m_generation(std::make_shared<std::atomic<quint64>>(1)) {
  qRegisterMetaType<MappingExplanationRequest>("MappingExplanationRequest");
  qRegisterMetaType<MappingExplanationResult>("MappingExplanationResult");
}

MappingExplanationService::~MappingExplanationService() {
  m_generation->fetch_add(1);
  if (m_worker && m_thread.isRunning()) {
    QMetaObject::invokeMethod(
        m_worker,
        [worker = m_worker] {
          worker->shutdownOnWorkerThread();
          QThread::currentThread()->quit();
        },
        Qt::QueuedConnection);
    if (!m_thread.wait(5000)) {
      m_thread.quit();
      m_thread.wait(2000);
    }
  }
  delete m_worker;
  m_worker = nullptr;
  finishTaskIfIdle();
}

void MappingExplanationService::setTaskService(PaleoTaskService *tasks) {
  m_taskSvc = tasks;
}

void MappingExplanationService::setLlmConfig(const LlmConfig &config) {
  m_llmConfig = config;
  emit statusChanged();
}

void MappingExplanationService::setEnabled(bool enabled) {
  if (m_enabled == enabled)
    return;
  m_enabled = enabled;
  if (!enabled)
    cancelAll();
  emit statusChanged();
}

QString MappingExplanationService::statusText() const {
  if (m_busy)
    return m_activeDetail.isEmpty() ? tr("正在后台线程生成编图解释…") : m_activeDetail;
  if (!m_enabled)
    return tr("后台自动生成解释已暂停（编图仍可直接进行，可随时手动生成）");
  if (m_llmConfig.enabled())
    return tr("后台自动生成已开启（AI 模型：%1；独立线程运行，不阻塞编图）")
        .arg(m_llmConfig.model);
  return tr("后台自动生成已开启（当前未配置 AI，将自动生成本地事实摘要；配置 AI 后自动启用 AI 生成）");
}

void MappingExplanationService::ensureWorker() {
  if (m_worker)
    return;
  m_thread.setObjectName(QStringLiteral("PaleoMappingExplain"));
  m_worker = new Worker(m_generation);
  m_worker->moveToThread(&m_thread);
  connect(m_worker, &Worker::completed, this,
          &MappingExplanationService::onWorkerFinished, Qt::QueuedConnection);
  m_thread.start();
}

void MappingExplanationService::beginTaskIfNeeded(const QString &layerId, bool useAi) {
  if (!m_taskSvc || (m_task && m_task->running()))
    return;
  m_task = m_taskSvc->beginExternal(
      useAi ? tr("后台生成编图解释（AI）") : tr("后台生成编图解释摘要"),
      layerId, /*quiet=*/true);
  if (m_task) {
    m_task->reportDetail(useAi ? tr("AI 正在后台线程整理编图解释草稿…")
                               : tr("正在后台线程整理编图事实摘要…"));
    connect(m_task, &PaleoTask::cancelAsked, this,
            &MappingExplanationService::cancelAll);
  }
}

void MappingExplanationService::finishTaskIfIdle(const QString &error) {
  if (m_task && m_task->running())
    m_task->finishExternal(error);
  m_task.clear();
}

QString MappingExplanationService::enqueue(MappingExplanationRequest request) {
  ensureWorker();
  if (request.requestId.isEmpty())
    request.requestId = QUuid::createUuid().toString(QUuid::WithoutBraces);
  request.generation = m_generation->load();
  request.sequence = ++m_lastEnqueuedSeq;
  if (!request.llmConfig.enabled() && m_llmConfig.enabled())
    request.llmConfig = m_llmConfig;
  const bool useAi = request.llmConfig.enabled();
  m_activeDetail = useAi
                       ? tr("正在后台线程调用 AI（%1）生成解释草稿…").arg(request.llmConfig.model)
                       : tr("正在后台线程生成编图事实摘要…");
  if (!m_busy) {
    m_busy = true;
    emit busyChanged(true, m_activeDetail);
  }
  emit statusChanged();
  beginTaskIfNeeded(request.layerId, useAi);
  QMetaObject::invokeMethod(
      m_worker,
      [worker = m_worker, request] { worker->enqueue(request); },
      Qt::QueuedConnection);
  return request.requestId;
}

void MappingExplanationService::cancelAll() {
  m_generation->fetch_add(1);
  if (m_worker && m_thread.isRunning()) {
    QMetaObject::invokeMethod(
        m_worker,
        [worker = m_worker] { worker->cancelAll(); },
        Qt::QueuedConnection);
  }
  const bool wasBusy = m_busy;
  m_busy = false;
  m_activeDetail.clear();
  finishTaskIfIdle(tr("已取消后台解释生成"));
  if (wasBusy)
    emit busyChanged(false, QString());
  emit statusChanged();
}

void MappingExplanationService::onWorkerFinished(const MappingExplanationResult &result,
                                                 int remaining,
                                                 quint64 highWaterSeq) {
  if (result.generation != m_generation->load())
    return;
  if (remaining <= 0 && highWaterSeq >= m_lastEnqueuedSeq) {
    m_busy = false;
    m_activeDetail.clear();
    finishTaskIfIdle();
    emit busyChanged(false, QString());
  }
  emit explanationReady(result);
  emit statusChanged();
}

QVector<ChatMessage> MappingExplanationService::buildPromptMessages(
    const MappingExplanationRequest &req) {
  ChatMessage system;
  system.role = ChatRole::System;
  system.content = tr(
      "你是石油地质古地理编图助手。请仅根据用户提供的当前工区与相面编辑事实，"
      "生成一段简明、专业、客观的中文编图解释说明草稿和一句简短修改目的。\n"
      "严格要求：\n"
      "1. 只使用给定事实，严禁编造任何未提供的井名、深度、物性数值、地震反射特征或岩心结论。\n"
      "2. 事实有限时如实指出需结合测井/地震或单因素资料复核，不得冒充已确认解释。\n"
      "3. 只输出一个 JSON 对象，格式为 {\"text\": \"2~4句简明地质解释草稿\", \"reason\": \"1句修改目的\"}，"
      "不要输出多余解释或 Markdown 标题。");

  QStringList facts;
  const QString horizonLabel = req.unitDisplayName.isEmpty() ? req.horizon : req.unitDisplayName;
  facts << tr("层位/单元：%1").arg(horizonLabel.isEmpty() ? tr("未指定") : horizonLabel);
  facts << tr("编图操作：%1").arg(operationLabel(req.operationKind));
  if (!req.faciesPath.isEmpty())
    facts << tr("目标相归属：%1（编码 %2）").arg(req.faciesPath).arg(req.faciesCode);
  if (!req.beforeFaciesPath.isEmpty() && req.beforeFaciesPath != req.faciesPath)
    facts << tr("修改前相归属：%1（编码 %2）").arg(req.beforeFaciesPath).arg(req.beforeFaciesCode);
  const QString area = formatArea(req.areaSqm);
  if (!area.isEmpty())
    facts << tr("图斑面积：%1").arg(area);
  if (req.featureCount > 0)
    facts << tr("当前图件相面总数：%1").arg(req.featureCount);
  const QString wells = formatWellsSummary(req.wellsInFeature);
  if (!wells.isEmpty())
    facts << tr("图斑内井资料：%1").arg(wells);
  if (!req.attributionSummary.trimmed().isEmpty())
    facts << tr("上游计算来源：%1").arg(req.attributionSummary.trimmed());
  if (!req.referenceTitles.isEmpty())
    facts << tr("可见/关联参考图件：%1").arg(req.referenceTitles.mid(0, 8).join(QStringLiteral("、")));

  ChatMessage user;
  user.role = ChatRole::User;
  user.content = tr("请根据以下编图事实生成解释草稿 JSON：\n%1").arg(facts.join(QLatin1Char('\n')));
  return {system, user};
}

bool MappingExplanationService::parseAiResponse(const QString &raw,
                                                const MappingExplanationRequest &req,
                                                QString *text, QString *reason) {
  const QString cleaned = raw.trimmed();
  if (cleaned.isEmpty())
    return false;
  QString parsedText;
  QString parsedReason;
  const QString slice = extractJsonObjectSlice(cleaned);
  QJsonParseError err;
  const QJsonDocument doc = QJsonDocument::fromJson(slice.toUtf8(), &err);
  if (err.error == QJsonParseError::NoError && doc.isObject()) {
    const QJsonObject obj = doc.object();
    parsedText = obj.value(QStringLiteral("text")).toString().trimmed();
    if (parsedText.isEmpty())
      parsedText = obj.value(QStringLiteral("explanation")).toString().trimmed();
    parsedReason = obj.value(QStringLiteral("reason")).toString().trimmed();
    if (parsedReason.isEmpty())
      parsedReason = obj.value(QStringLiteral("purpose")).toString().trimmed();
  }
  if (parsedText.isEmpty()) {
    // 模型未严格输出 JSON 时，直接收容非空正文，不丢失有效 AI 回答。
    parsedText = cleaned;
  }
  if (parsedReason.isEmpty()) {
    QString fallbackText;
    buildLocalFactualExplanation(req, &fallbackText, &parsedReason);
  }
  clampExplanationText(&parsedText, &parsedReason);
  if (parsedText.isEmpty())
    return false;
  if (text)
    *text = parsedText;
  if (reason)
    *reason = parsedReason;
  return true;
}

void MappingExplanationService::buildLocalFactualExplanation(
    const MappingExplanationRequest &req, QString *text, QString *reason) {
  const QString horizonLabel = req.unitDisplayName.isEmpty() ? req.horizon : req.unitDisplayName;
  const QString op = operationLabel(req.operationKind);
  QStringList clauses;
  if (!req.faciesPath.isEmpty()) {
    if (!req.beforeFaciesPath.isEmpty() && req.beforeFaciesPath != req.faciesPath) {
      clauses << tr("%1层位经%2将相归属由「%3」调整为「%4」（编码 %5）")
                     .arg(horizonLabel, op, req.beforeFaciesPath, req.faciesPath)
                     .arg(req.faciesCode);
    } else {
      clauses << tr("%1层位经%2形成「%3」（编码 %4）相面")
                     .arg(horizonLabel, op, req.faciesPath)
                     .arg(req.faciesCode);
    }
  } else {
    clauses << tr("%1层位执行%2（当前相面数 %3）")
                   .arg(horizonLabel, op)
                   .arg(qMax(0, req.featureCount));
  }
  const QString area = formatArea(req.areaSqm);
  if (!area.isEmpty())
    clauses << tr("面积%1").arg(area);
  const QString wells = formatWellsSummary(req.wellsInFeature);
  if (!wells.isEmpty())
    clauses << tr("图斑内包含井：%1").arg(wells);
  if (!req.attributionSummary.trimmed().isEmpty())
    clauses << tr("来源依据：%1").arg(req.attributionSummary.trimmed());
  if (!req.referenceTitles.isEmpty())
    clauses << tr("参考图件：%1").arg(req.referenceTitles.mid(0, 6).join(QStringLiteral("、")));

  QString builtText = clauses.join(QStringLiteral("；")) +
                      tr("。本说明由后台自动汇总当前图件事实生成，待地质复核。");
  QString builtReason = !req.faciesPath.isEmpty()
                            ? tr("后台自动记录：%1（%2）").arg(op, req.faciesPath)
                            : tr("后台自动记录：%1%2").arg(horizonLabel, op);
  clampExplanationText(&builtText, &builtReason);
  if (text)
    *text = builtText;
  if (reason)
    *reason = builtReason;
}

#include "mappingexplanationservice.moc"
