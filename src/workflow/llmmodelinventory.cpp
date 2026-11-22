// 层：功能
#include "llmmodelinventory.h"
#include "../ai/chat/llmcredentialguard.h"

#include <QHostAddress>
#include <QJsonArray>
#include <QJsonDocument>
#include <QJsonObject>
#include <QJsonValue>
#include <QNetworkAccessManager>
#include <QNetworkReply>
#include <QNetworkRequest>
#include <QTimer>

// H6-W3 实现：单次 GET、双时限、字节预算、有限形状解析、凭据防回显。
// 分类与 LlmClient 的统一裁决同序（HTTP 优先于传输层错误）。

namespace paleo::chat {

LlmModelInventory::LlmModelInventory(Limits limits,
                                     QNetworkAccessManager *namForTesting,
                                     QObject *parent)
  : QObject(parent)
  , m_limits(limits)
  , m_nam(namForTesting)
  , m_ownedNam(namForTesting == nullptr)
  , m_idleTimer(new QTimer(this))
  , m_totalTimer(new QTimer(this)) {
  if (m_ownedNam)
    m_nam = new QNetworkAccessManager(this);
  m_idleTimer->setSingleShot(true);
  connect(m_idleTimer, &QTimer::timeout, this, [this] {
    if (!m_reply)
      return;
    LlmCatalogObservation observation;
    observation.status = LlmDiagnosticStatus::Timeout;
    observation.detail = QObject::tr("目录请求空闲超时（>%1 ms）")
                           .arg(m_limits.idleTimeoutMs);
    cancel();
    finishWith(observation);
  });
  m_totalTimer->setSingleShot(true);
  connect(m_totalTimer, &QTimer::timeout, this, [this] {
    if (!m_reply)
      return;
    LlmCatalogObservation observation;
    observation.status = LlmDiagnosticStatus::Timeout;
    observation.detail = QObject::tr("目录请求超过总时限（%1 ms）")
                           .arg(m_limits.totalTimeoutMs);
    cancel();
    finishWith(observation);
  });
}

LlmModelInventory::~LlmModelInventory() {
  if (m_reply) {
    m_reply->abort();
    m_reply.clear();
  }
}

QUrl LlmModelInventory::modelsUrl(const QUrl &endpoint) {
  QUrl url = endpoint;
  QString path = url.path();
  while (path.endsWith(QLatin1Char('/')))
    path.chop(1);
  path += QStringLiteral("/models");
  url.setPath(path);
  return url;
}

void LlmModelInventory::probe(const LlmDiagnosticCandidate &candidate) {
  if (m_reply)
    return; // 在途：单次终态纪律（重复启动忽略，不发第二请求）
  cancel();
  m_finished = false;
  m_body.clear();
  m_received = 0;
  m_key = candidate.key;

  QNetworkRequest request(modelsUrl(candidate.endpoint));
  request.setHeader(QNetworkRequest::ContentTypeHeader,
                    QStringLiteral("application/json"));
  request.setAttribute(QNetworkRequest::RedirectPolicyAttribute,
                       QNetworkRequest::ManualRedirectPolicy);
  request.setRawHeader("Accept", "application/json");
  if (!m_key.isEmpty())
    request.setRawHeader("Authorization", "Bearer " + m_key);
  request.setTransferTimeout(0); // 超时由本端双计时器统一判

  m_reply = m_nam->get(request);
  m_idleTimer->start(m_limits.idleTimeoutMs);
  m_totalTimer->start(m_limits.totalTimeoutMs);

  const QPointer<LlmModelInventory> self(this);
  connect(m_reply, &QNetworkReply::readyRead, this, [this, self] {
    if (!self || !m_reply)
      return;
    m_idleTimer->start(m_limits.idleTimeoutMs);
    const QByteArray chunk = m_reply->readAll();
    if (!countBytes(chunk.size()))
      return; // 超预算：countBytes 内已收口（finishWith）
    m_body.append(chunk);
  });

  connect(m_reply, &QNetworkReply::finished, this, [this, self] {
    // cancel()/abort 路径：m_reply 已先断引用——终态由 cancel 调用方落
    // （超预算/超时在 countBytes/计时器内收口；用户取消由协调器标记），
    // 此处静默退出，绝不与先到终态二次结算。
    if (!self || !m_reply)
      return;
    QNetworkReply *reply = m_reply.data();
    m_reply.clear();
    m_idleTimer->stop();
    m_totalTimer->stop();
    if (!reply)
      return;
    reply->deleteLater();
    const int status =
      reply->attribute(QNetworkRequest::HttpStatusCodeAttribute).toInt();
    // tail 与 readyRead 同一把尺子、每字节恰好一次（超限在 countBytes 内收口）。
    const QByteArray tail = reply->readAll();
    if (!countBytes(qint64(tail.size())))
      return;
    m_body.append(tail);
    if (status >= 300 && status < 400) {
      const QString location = redactCredential(
        reply->header(QNetworkRequest::LocationHeader).toString(), m_key);
      LlmCatalogObservation observation;
      observation.status = LlmDiagnosticStatus::Network;
      observation.detail =
        QObject::tr("目录请求要求重定向（HTTP %1 → %2）：未跟随（避免凭据跨源外泄）")
          .arg(QString::number(status),
               location.isEmpty() ? QStringLiteral("?") : location);
      finishWith(observation);
      return;
    }
    if (status == 404 || status == 405) {
      LlmCatalogObservation observation;
      observation.status = LlmDiagnosticStatus::Unsupported;
      observation.detail =
        QObject::tr("服务未实现模型目录（HTTP %1）：目录缺席不代表文本不可用，"
                    "可手填模型继续")
          .arg(status);
      finishWith(observation);
      return;
    }
    if (status == 401 || status == 403) {
      LlmCatalogObservation observation;
      observation.status = LlmDiagnosticStatus::Unauthorized;
      observation.detail =
        QObject::tr("目录无访问权限（HTTP %1）").arg(status);
      finishWith(observation);
      return;
    }
    if (status == 429) {
      LlmCatalogObservation observation;
      observation.status = LlmDiagnosticStatus::RateLimited;
      observation.detail = QObject::tr("目录请求被限流（HTTP 429）");
      finishWith(observation);
      return;
    }
    if (status >= 500) {
      LlmCatalogObservation observation;
      observation.status = LlmDiagnosticStatus::ServerError;
      observation.detail =
        QObject::tr("目录服务端错误（HTTP %1）").arg(status);
      finishWith(observation);
      return;
    }
    if (status >= 400) {
      LlmCatalogObservation observation;
      observation.status = LlmDiagnosticStatus::Rejected;
      observation.detail =
        QObject::tr("目录请求被拒绝（HTTP %1）").arg(status);
      finishWith(observation);
      return;
    }
    if (reply->error() != QNetworkReply::NoError) {
      LlmCatalogObservation observation;
      observation.status = LlmDiagnosticStatus::Network;
      observation.detail = redactCredential(
        QObject::tr("目录网络错误：%1").arg(reply->errorString()), m_key);
      finishWith(observation);
      return;
    }
    finishWith(parseDocument(m_body, m_key));
  });
}

void LlmModelInventory::cancel() {
  m_idleTimer->stop();
  m_totalTimer->stop();
  if (QNetworkReply *reply = m_reply.data()) {
    m_reply.clear(); // 先断引用：abort 触发的同步 finished 不再走解析路径
    reply->abort();  // （LlmClient 同款纪律：先作废 attempt 再 releaseReply）
    reply->deleteLater();
  }
}

bool LlmModelInventory::countBytes(qint64 bytes) {
  if (bytes > m_limits.maxBytes - m_received) {
    LlmCatalogObservation observation;
    observation.status = LlmDiagnosticStatus::TooLarge;
    observation.detail = QObject::tr("目录应答超过 %1 字节预算")
                           .arg(m_limits.maxBytes);
    cancel();
    finishWith(observation);
    return false;
  }
  m_received += bytes;
  return true;
}

void LlmModelInventory::finishWith(LlmCatalogObservation observation) {
  if (m_finished)
    return; // 单次终态：TooLarge/取消等先到者胜，迟到收口静默丢弃
  m_finished = true;
  emit finished(observation);
}

LlmCatalogObservation
LlmModelInventory::parseDocument(const QByteArray &body,
                                 const QByteArray &apiKey) const {
  LlmCatalogObservation observation;
  const QJsonDocument document = QJsonDocument::fromJson(body);
  if (!document.isObject()) {
    observation.status = LlmDiagnosticStatus::UnknownShape;
    observation.detail = QObject::tr("目录应答不是已知形状（须形如 "
                                     "{\"data\":[{\"id\":…}]} 的 JSON 对象）");
    return observation;
  }
  const QJsonObject object = document.object();
  const QJsonValue data = object.value(QStringLiteral("data"));
  if (!data.isArray()) {
    observation.status = LlmDiagnosticStatus::UnknownShape;
    observation.detail = QObject::tr("目录应答缺少 data 数组（未知形状）");
    return observation;
  }
  const QJsonArray entries = data.toArray();
  QStringList seen;
  for (const QJsonValue &entry : entries) {
    if (seen.size() >= m_limits.maxEntries) {
      observation.truncated = true;
      break;
    }
    if (!entry.isObject()) {
      ++observation.malformedEntries;
      continue;
    }
    // 凭据防回显：条目任何字符串值/键回显本次凭据 → 整条剔除（不展示）。
    if (jsonDisclosesCredential(entry, apiKey)) {
      observation.credentialFiltered = true;
      continue;
    }
    const QJsonValue id = entry.toObject().value(QStringLiteral("id"));
    if (!id.isString())
      continue;
    const QString modelId = id.toString();
    // 有界 + 形状严格：空串/超长/带首尾空白的 ID 都按形状非法计
    // （不截半截当模型名，也不静默 trim 改变服务端给出的精确 ID）。
    if (modelId.isEmpty() || modelId.size() > m_limits.maxIdChars ||
        modelId != modelId.trimmed()) {
      ++observation.malformedEntries;
      continue;
    }
    // 精确去重（不 trim、不大小写折叠成另一模型）。
    if (seen.contains(modelId)) {
      ++observation.duplicatesFiltered;
      continue;
    }
    seen.append(modelId);
  }
  if (seen.isEmpty() && observation.malformedEntries == 0 &&
      entries.isEmpty()) {
    observation.status = LlmDiagnosticStatus::Empty;
    observation.detail = QObject::tr("目录成功但未列出任何模型（空目录）");
    return observation;
  }
  observation.status = LlmDiagnosticStatus::Passed;
  observation.models = seen;
  if (observation.malformedEntries > 0)
    observation.detail = QObject::tr("目录含 %1 个形状非法条目（已跳过）")
                           .arg(observation.malformedEntries);
  if (observation.truncated && observation.detail.isEmpty())
    observation.detail =
      QObject::tr("目录条目超过 %1 项上限：已截断（部分目录）")
        .arg(m_limits.maxEntries);
  return observation;
}

} // namespace paleo::chat
