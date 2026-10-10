// 层：功能
#include "remotepredictrouter.h"

#include <QJsonArray>
#include <QJsonDocument>
#include <QJsonObject>
#include <QNetworkAccessManager>
#include <QNetworkReply>
#include <QNetworkRequest>
#include <QTimer>

#include <cmath>

// ai/ — 远端推理路由实现（方向51：全异步，零事件循环嵌套）。
//
// 轮次号（runId）契约：router 每次 start 领一个只增不复用的 runId，随
// healthCheck/predict 传下去，传输层**原样带回**。router 收到与当前不符的
// runId 就静默丢弃——取消 / 重入之后迟到的应答因此没有机会被写成新结论
// （旧实现的"取消"只是置个 flag，迟到的完成照样回来）。
// 传输层自己另有内部 generation，用来作废自己那侧的 reply（超时先 bump 再
// abort，避免 abort 触发的 finished 重复回包）。

LocalPredictor::~LocalPredictor() = default;

RemoteTransport::RemoteTransport(QObject *parent) : QObject(parent) {}
RemoteTransport::~RemoteTransport() = default;

// ---------------------------------------------------------------------------
// HttpRemoteTransport
// ---------------------------------------------------------------------------
HttpRemoteTransport::HttpRemoteTransport(const QUrl &base, QObject *parent)
  : RemoteTransport(parent)
  , m_base(base)
  , m_nam(new QNetworkAccessManager(this))
  , m_timer(new QTimer(this))
{
  m_timer->setSingleShot(true);
  // 只连一次：多次 healthCheck/predict 不累积定时器连接（反复 connect 会让
  // 一次超时发多次信号——历史上的 secondary emit 源）。
  connect(m_timer, &QTimer::timeout, this, [this]() {
    const Op op = m_op;
    if (op == Op::None)
      return;
    const int timeoutMs = m_opTimeoutMs;
    const quint64 runId = m_runId;
    // 先提世代再 abort：abort 触发的 finished 判为过期，不会重复回包。
    ++m_generation;
    m_op = Op::None;
    m_runId = 0;
    m_timer->stop();
    QPointer<QNetworkReply> victim = m_reply;
    m_reply.clear();
    if (victim) {
      victim->abort();
      victim->deleteLater();
    }
    const QString reason = QObject::tr("远端请求超时（>%1ms）: %2")
                             .arg(timeoutMs)
                             .arg(m_base.toString());
    if (op == Op::Health)
      emit healthDone(runId, false, reason);
    else
      emit predictDone(runId, {}, reason);
  });
}

HttpRemoteTransport::~HttpRemoteTransport() = default;

void HttpRemoteTransport::abort()
{
  ++m_generation;
  m_op = Op::None;
  m_timer->stop();
  if (m_reply) {
    m_reply->abort();
    m_reply->deleteLater();
    m_reply.clear();
  }
}

void HttpRemoteTransport::begin(int timeoutMs, Op op, QNetworkReply *reply,
                                quint64 runId)
{
  abort();
  const int generation = ++m_generation;
  m_op = op;
  m_runId = runId;
  m_opTimeoutMs = timeoutMs;
  m_reply = reply;
  m_timer->start(timeoutMs);
  connect(reply, &QNetworkReply::finished, this,
          [this, generation, op, reply, runId]() {
    if (generation != m_generation) { // 已被超时/取消/新一轮作废
      reply->deleteLater();
      return;
    }
    m_timer->stop();
    m_op = Op::None;
    ++m_generation; // 本轮已 settle，后续任何迟到信号判过期
    m_runId = 0;
    m_reply.clear();
    const bool ok = reply->error() == QNetworkReply::NoError;
    const int status =
      reply->attribute(QNetworkRequest::HttpStatusCodeAttribute).toInt();
    const QByteArray payload = reply->readAll();
    const QString detail = reply->errorString();
    reply->deleteLater();
    if (op == Op::Health) {
      if (!ok || status != 200) {
        emit healthDone(runId, false, QObject::tr("健康检查失败: HTTP %1 (%2)")
                                 .arg(status)
                                 .arg(detail));
        return;
      }
      if (!payload.contains("ok")) {
        emit healthDone(runId, false, QObject::tr("健康检查应答异常: %1")
                                 .arg(QString::fromUtf8(payload)));
        return;
      }
      emit healthDone(runId, true, {});
      return;
    }
    if (!ok || status != 200) {
      emit predictDone(runId, {}, QObject::tr("远端预测失败: HTTP %1 (%2)")
                              .arg(status)
                              .arg(detail));
      return;
    }
    const QJsonObject doc = QJsonDocument::fromJson(payload).object();
    if (m_wellsRequest) {
      const auto points = doc.value(QStringLiteral("points"));
      if (!points.isArray() || points.toArray().isEmpty()) {
        emit predictDone(runId, {}, QObject::tr("测井预测应答缺少井点和相井段"));
        return;
      }
      QVariantList rows;
      for (const auto &v : points.toArray()) {
        auto row = v.toObject().toVariantMap();
        if (v.toObject().value("facies_intervals").isArray())
          row.insert("facies_intervals", QString::fromUtf8(QJsonDocument(v.toObject().value("facies_intervals").toArray()).toJson(QJsonDocument::Compact)));
        rows << row;
      }
      emit wellPredictDone(runId, rows);
      return;
    }
    const QJsonArray got = doc.value(QStringLiteral("cells")).toArray();
    if (got.size() != m_gridColumns * m_gridRows) {
      emit predictDone(runId, {},
                       QObject::tr("远端预测应答格数 %1 与请求网格 %2×%3 不符")
                              .arg(got.size())
                              .arg(m_gridColumns)
                              .arg(m_gridRows));
      return;
    }
    QVector<int> cells;
    cells.reserve(got.size());
    for (const QJsonValue &v : got)
      cells.append(v.toInt());
    emit predictDone(runId, cells, {});
  });
}

void HttpRemoteTransport::healthCheck(int timeoutMs, quint64 runId)
{
  if (!m_nam)
    return;
  QNetworkRequest req(m_base.resolved(QUrl(QStringLiteral("/health"))));
  req.setTransferTimeout(0); // 超时统一由本端定时器判
  begin(timeoutMs, Op::Health, m_nam->get(req), runId);
}

void HttpRemoteTransport::predict(const RemotePredictionRequest &request,
                                  int timeoutMs, quint64 runId)
{
  if (!m_nam)
    return;
  m_wellsRequest = request.kind == QLatin1String("wells");
  m_gridColumns = request.columns;
  m_gridRows = request.rows;
  QJsonObject body;
  body.insert(QStringLiteral("id"), request.id);
  body.insert(QStringLiteral("horizon"), request.horizon);
  body.insert(QStringLiteral("kind"), request.kind);
  body.insert(QStringLiteral("columns"), request.columns);
  body.insert(QStringLiteral("rows"), request.rows);
  QJsonArray samples;
  for (float v : request.samples)
    samples.append(std::isnan(v) ? QJsonValue() : QJsonValue(double(v)));
  body.insert(QStringLiteral("samples"), samples);
  if (m_wellsRequest) {
    body.insert(QStringLiteral("wells"), QJsonArray::fromVariantList(request.wells));
    body.insert(QStringLiteral("facies"), QJsonArray::fromVariantList(request.facies));
  }

  QNetworkRequest req(m_base.resolved(QUrl(QStringLiteral("/predict"))));
  req.setHeader(QNetworkRequest::ContentTypeHeader,
                QStringLiteral("application/json"));
  req.setTransferTimeout(0);
  QNetworkReply *reply =
    m_nam->post(req, QJsonDocument(body).toJson(QJsonDocument::Compact));
  begin(timeoutMs, Op::Predict, reply, runId);
}

// ---------------------------------------------------------------------------
// RemotePredictionRouter
// ---------------------------------------------------------------------------
RemotePredictionRouter::RemotePredictionRouter(RemoteTransport *transport,
                                               QObject *parent)
  : RemotePredictionService(parent)
  , m_transport(transport)
{
  if (!m_transport)
    return;
  connect(m_transport, &RemoteTransport::healthDone, this,
          [this](quint64 runId, bool ok, const QString &error) {
            onHealth(runId, ok, error);
          });
  connect(m_transport, &RemoteTransport::wellPredictDone, this,
          [this](quint64 runId, const QVariantList &points) {
            if (runId != m_runId || m_cancelled || !m_running || m_request.kind != QLatin1String("wells")) return;
            RemotePredictionResult result;
            result.request = m_request; result.points = points;
            result.method = QStringLiteral("remote"); result.mock = false;
            m_lastRoute = result.method; m_running = false;
            emit progress(m_request.id, 100); emit completed(result);
          });
  connect(m_transport, &RemoteTransport::predictDone, this,
          [this](quint64 runId, const QVector<int> &cells,
                 const QString &error) {
            onPredict(runId, cells, error);
          });
}

RemotePredictionRouter::~RemotePredictionRouter() = default;

void RemotePredictionRouter::setLocalPredictor(LocalPredictor *predictor)
{
  m_local = predictor;
}
void RemotePredictionRouter::setFallbackEnabled(bool enabled)
{
  m_fallbackEnabled = enabled;
}

void RemotePredictionRouter::start(const RemotePredictionRequest &request)
{
  m_cancelled = false;
  m_lastRoute.clear();
  m_request = request;
  // 领新轮次号：上一轮的任何回包（哪怕没有 runId 校验的实现）在新号下都被判
  // 过期。m_nextRunId 只增不复用——复用的号会让旧回包冒充新一轮。
  m_runId = ++m_nextRunId;
  m_running = true;
  // 契约：start 立即返回；链在下一轮事件循环才起跑（对齐旧行为，取消先到
  // 即得「已取消」）。轮次号按值捕获快照——触发时读 m_runId 会把旧 request
  // 配上新一号的 runId，绕过 beginChain 的过期防线（0ms FIFO 多数情况掩盖
  // 了它，cancel→立即 start 是现实触发路径）。
  const quint64 runId = m_runId;
  QTimer::singleShot(
      0, this, [this, runId, request]() { beginChain(runId, request); });
}

void RemotePredictionRouter::cancel()
{
  // 在飞的那一轮要有个终态：老语义（同步链时代）就是「取消即 failed(已取消)」，
  // 这里照旧报一次——否则调用方（MappingWorkbench）会一直停在 busy。
  const bool wasRunning = m_running;
  m_cancelled = true;
  if (m_transport)
    m_transport->abort();
  // 轮次号置 0（= 无有效在飞轮次）+ m_running=false：在飞回包一律判过期，
  // 取消之后不会再补一个 completed（幽灵结果）。
  m_runId = 0;
  m_running = false;
  if (wasRunning)
    emit failed(m_request.id, QObject::tr("已取消"));
}

void RemotePredictionRouter::beginChain(quint64 runId,
                                        const RemotePredictionRequest &request)
{
  if (runId != m_runId)
    return; // 期间又来了一轮新的 start（或已取消）
  if (m_cancelled)
    return settleFail(QObject::tr("已取消"));
  if (!m_transport)
    return settleFail(QObject::tr("未绑定远端传输"));
  emit progress(request.id, 5);
  m_transport->healthCheck(m_healthTimeoutMs, runId);
}

void RemotePredictionRouter::onHealth(quint64 runId, bool ok, const QString &error)
{
  if (runId != m_runId)
    return;
  if (m_cancelled)
    return settleFail(QObject::tr("已取消"));
  if (!ok)
    return degradeOrDie(runId, QObject::tr("健康检查"), error);
  emit progress(m_request.id, 30);
  m_transport->predict(m_request, m_predictTimeoutMs, runId);
}

void RemotePredictionRouter::onPredict(quint64 runId, const QVector<int> &cells,
                                       const QString &error)
{
  if (runId != m_runId)
    return;
  if (m_cancelled)
    return settleFail(QObject::tr("已取消"));
  if (!error.isEmpty())
    return degradeOrDie(runId, QObject::tr("远端预测"), error);

  RemotePredictionResult result;
  result.request = m_request;
  result.cells = cells;
  result.method = QStringLiteral("remote");
  result.mock = false;
  m_lastRoute = result.method;
  emit progress(m_request.id, 100);
  m_running = false;
  emit completed(result);
}

void RemotePredictionRouter::degradeOrDie(quint64 runId, const QString &stageName,
                                          const QString &stageError)
{
  if (runId != m_runId)
    return;
  LocalPredictor *local = m_local;
  const bool usable = m_request.kind != QLatin1String("wells") && local && m_fallbackEnabled && local->isConfigured();
  if (!usable) {
    settleFail(QObject::tr("远端不可用且降级未配置（%1）：%2")
                 .arg(stageName, stageError));
    return;
  }
  emit progress(m_request.id, 50);
  QVector<int> cells;
  QString localError;
  if (!local->predict(m_request, cells, &localError)) {
    settleFail(QObject::tr("远端失败 (%1)，本地降级失败 (%2)")
                 .arg(stageError, localError));
    return;
  }
  RemotePredictionResult result;
  result.request = m_request;
  result.cells = cells;
  result.method = local->engineId();
  result.mock = false;
  m_lastRoute = result.method;
  emit progress(m_request.id, 100);
  m_running = false;
  emit completed(result);
}

void RemotePredictionRouter::settleFail(const QString &reason)
{
  m_running = false;
  emit failed(m_request.id, reason);
}
