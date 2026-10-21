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
  // #469：响应体上限必须在接收途中生效。Content-Length 声明超限（metaDataChanged）
  // 或已缓冲字节超限（readyRead；本端在 finished 前从不读走数据，bytesAvailable
  // 即累计已收量）即先提世代再 abort——abort 同步触发的 finished 判为过期，不重复
  // 回包；内存峰值被钳在 ~上限 + 一个网络读块。不依赖 downloadProgress：Qt 6.8
  // loopback 实测在挂住的连接上不发该信号（readyRead 照常发）。
  const auto guardOversize = [this, generation, op, reply, runId](qint64 received, qint64 declared) {
    if (generation != m_generation)
      return;
    const qint64 limit = m_maxResponseBytes;
    if (received <= limit && declared <= limit)
      return;
    m_timer->stop();
    m_op = Op::None;
    ++m_generation;
    m_runId = 0;
    m_reply.clear();
    reply->abort();
    reply->deleteLater();
    const QString limitMsg = QObject::tr("远端预测响应超出上限（%1 字节，已中止接收）").arg(limit);
    if (op == Op::Health)
      emit healthDone(runId, false, limitMsg);
    else
      emit predictDone(runId, {}, limitMsg);
  };
  connect(reply, &QNetworkReply::metaDataChanged, this, [reply, guardOversize]() {
    bool ok = false;
    const qint64 declared = reply->header(QNetworkRequest::ContentLengthHeader).toLongLong(&ok);
    guardOversize(0, ok ? declared : -1);
  });
  connect(reply, &QNetworkReply::readyRead, this, [reply, guardOversize]() {
    guardOversize(reply->bytesAvailable(), -1);
  });
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
    const qint64 limit = m_maxResponseBytes;
    const QByteArray payload = reply->read(limit + 1); // 兜底：未报进度的后端
    const QString detail = reply->errorString();
    reply->deleteLater();
    if (payload.size() > limit) {
      const QString limitMsg = QObject::tr("远端预测响应超出上限（%1 字节）").arg(limit);
      if (op == Op::Health)
        emit healthDone(runId, false, limitMsg);
      else
        emit predictDone(runId, {}, limitMsg);
      return;
    }
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
    for (const QJsonValue &v : got) {
      if (v.isNull() || !v.isDouble())
        cells.append(-9999);
      else
        cells.append(v.toInt());
    }
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
    samples.append(!std::isfinite(v) ? QJsonValue() : QJsonValue(double(v)));
  body.insert(QStringLiteral("samples"), samples);
  body.insert(QStringLiteral("facies"), QJsonArray::fromVariantList(request.facies));
  body.insert(QStringLiteral("predictionLevel"), QStringLiteral("micro_facies"));
  body.insert(QStringLiteral("faciesSchema"), QStringLiteral("paleo_facies_hierarchy_v1"));
  if (m_wellsRequest)
    body.insert(QStringLiteral("wells"), QJsonArray::fromVariantList(request.wells));

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
  // #479：本地降级重活（SHA256 全读+会话构建+warmup+全网格推理）不得在 GUI
  // 线程槽内同步执行——专用单线程池跑（降级是罕见路径，1 线程即够，且不与
  // 任务池/全局池抢资源）。
  m_fallbackPool.setMaxThreadCount(1);
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

RemotePredictionRouter::~RemotePredictionRouter()
{
  // 排空降级 worker：worker 内对 this 的排队回调随 QObject 析构清队，
  // 不会在析构后触达；m_ownedLocal/m_retiredLocals 在成员析构序中晚于
  // 池排空（声明序在池之前），安全。
  m_fallbackPool.clear();
  m_fallbackPool.waitForDone();
  m_retiredLocals.clear();
}

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
  if (runId == 0 || runId != m_runId || m_cancelled)
    return;
  if (!ok)
    return degradeOrDie(runId, QObject::tr("健康检查"), error);
  emit progress(m_request.id, 30);
  m_transport->predict(m_request, m_predictTimeoutMs, runId);
}

void RemotePredictionRouter::onPredict(quint64 runId, const QVector<int> &cells,
                                       const QString &error)
{
  if (runId == 0 || runId != m_runId || m_cancelled)
    return;
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
  if (runId == 0 || runId != m_runId || m_cancelled)
    return;
  LocalPredictor *local = m_local;
  const bool usable = m_request.kind != QLatin1String("wells") && local && m_fallbackEnabled && local->isConfigured();
  if (!usable) {
    settleFail(QObject::tr("远端不可用且降级未配置（%1）：%2")
                 .arg(stageName, stageError));
    return;
  }
  emit progress(m_request.id, 50);
  // #479：本地 ORT 降级是秒~分钟级重活（模型全读 SHA256 + 会话构建 + warmup
  // + 全网格推理），不得在 GUI 线程槽内同步执行——移入专用池，结果按轮次号
  // 排队回本线程结算（runId 守卫与远端迟到回包同一道防线）。
  runLocalFallback(runId, m_request, stageError);
}

void RemotePredictionRouter::runLocalFallback(quint64 runId,
                                              const RemotePredictionRequest &request,
                                              const QString &stageError)
{
  LocalPredictor *local = m_local; // 快照：setLocalFallback 可重绑，旧引擎由
                                   // m_ownedLocal 持有到 router 析构，指针稳定。
  QPointer<RemotePredictionRouter> self(this);
  m_fallbackPool.start([this, self, runId, request, stageError, local] {
    // C-15（full-audit-20261020）：关停窗口守卫——池线程执行中 router 可能恰在
    // 析构（quit 场景池等待超时/未等），invokeMethod 的上下文对象不得用裸 this；
    // self 失效即静默放弃本次本地降级结算。
    if (!self)
      return;
    QVector<int> cells;
    QString localError;
    const bool ok = local && local->predict(request, cells, &localError);
    // 排队回 router 线程结算：内层以 QPointer 复核（已入队调用会被析构清掉，
    // 双保险）；self 失效即不再触达 this。
    QMetaObject::invokeMethod(this,
                              [this, self, runId, request, stageError, ok,
                               cells, localError] {
                                if (!self)
                                  return;
                                settleLocalResult(runId, request, stageError,
                                                  ok, cells, localError);
                              },
                              Qt::QueuedConnection);
  });
}

void RemotePredictionRouter::settleLocalResult(quint64 runId,
                                               const RemotePredictionRequest &request,
                                               const QString &stageError, bool ok,
                                               const QVector<int> &cells,
                                               const QString &localError)
{
  // 迟到守卫：取消/重入之后到达的降级结果按过期丢弃——与远端迟到回包同语义。
  if (runId == 0 || runId != m_runId || m_cancelled || !m_running)
    return;
  LocalPredictor *local = m_local;
  if (!ok || !local) {
    settleFail(QObject::tr("远端失败 (%1)，本地降级失败 (%2)")
                 .arg(stageError, localError));
    return;
  }
  RemotePredictionResult result;
  result.request = request;
  result.cells = cells;
  result.method = local->engineId();
  result.mock = false;
  m_lastRoute = result.method;
  emit progress(request.id, 100);
  m_running = false;
  emit completed(result);
}

void RemotePredictionRouter::settleFail(const QString &reason)
{
  m_running = false;
  emit failed(m_request.id, reason);
}
