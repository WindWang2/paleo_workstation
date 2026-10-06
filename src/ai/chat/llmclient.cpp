// 层：功能
#include "llmclient.h"

#include <QDir>
#include <QFile>
#include <QFileInfo>
#include <QHostAddress>
#include <QJsonDocument>
#include <QJsonValue>
#include <QNetworkAccessManager>
#include <QNetworkReply>
#include <QNetworkRequest>
#include <QSaveFile>
#include <QStandardPaths>
#include <QTimer>

// ai/chat — LLM 客户端实现（方向51）。
//
// 协议依据：chat completions 请求体 { model, messages, stream, max_tokens }，
// 应答 choices[0].message.content（非流式）/ choices[0].delta.content（流式），
// 鉴权头 "Authorization: Bearer <key>"。全程 QtNetwork，无第三方 SDK。

namespace {
const char kEnvEndpoint[] = "PALEO_LLM_ENDPOINT";
const char kEnvModel[] = "PALEO_LLM_MODEL";
const char kEnvKey[] = "PALEO_LLM_API_KEY";
// 响应体上限（字节）：流式与非流式共用，超出即判失败——防故障/恶意服务耗尽内存
// （与 WellFaciesConfig 的同款纪律，只是阈值按对话场景下调到 8MiB）。
constexpr qint64 kMaxResponseBytes = 8ll * 1024 * 1024;
} // namespace

// ---------------------------------------------------------------------------
// LlmConfig
// ---------------------------------------------------------------------------
QString LlmConfig::path() {
  return QStandardPaths::writableLocation(
           QStandardPaths::GenericConfigLocation) +
         QStringLiteral("/paleo/llm-chat.json");
}

bool LlmConfig::isLoopbackHost(const QString &host) {
  const QString h = host.trimmed().toLower();
  if (h == QLatin1String("localhost"))
    return true;
  QHostAddress address;
  if (!address.setAddress(h))
    return false;
  return address.isLoopback();
}

QString LlmConfig::validate() const {
  if (endpoint.isEmpty())
    return QObject::tr("尚未配置大模型服务地址——助手处于禁用态");
  if (!endpoint.isValid() || endpoint.host().isEmpty() ||
      (endpoint.scheme() != QLatin1String("http") &&
       endpoint.scheme() != QLatin1String("https")) ||
      !endpoint.userInfo().isEmpty() || endpoint.hasQuery() ||
      endpoint.hasFragment())
    return QObject::tr("大模型服务地址无效（须 http(s)://host[:port][/v1]，不带查询串）");
  if (endpoint.scheme() == QLatin1String("http") && !allowInsecureHttp &&
      !isLoopbackHost(endpoint.host()))
    return QObject::tr("大模型服务必须使用 https://（http 会明文传输 API 密钥与井数据）");
  if (model.trimmed().isEmpty())
    return QObject::tr("尚未配置模型名称——助手处于禁用态");
  if (apiKey.isEmpty())
    return QObject::tr("尚未配置 API 密钥——助手处于禁用态");
  return QString();
}

QUrl LlmConfig::completionsUrl() const {
  QUrl url = endpoint;
  QString path = url.path();
  while (path.endsWith(QLatin1Char('/')))
    path.chop(1);
  path += QStringLiteral("/chat/completions");
  url.setPath(path);
  return url;
}

LlmConfig LlmConfig::fromParts(const QUrl &endpoint, const QString &model,
                               const QByteArray &apiKey, bool stream) {
  LlmConfig config;
  config.endpoint = endpoint;
  config.model = model;
  config.stream = stream;
  config.apiKey = apiKey;
  return config;
}

LlmConfig LlmConfig::load() {
  LlmConfig config;
  QFile file(path());
  if (file.open(QIODevice::ReadOnly)) {
    const QJsonDocument document = QJsonDocument::fromJson(file.readAll());
    if (document.isObject()) {
      const QJsonObject object = document.object();
      config.endpoint =
        QUrl(object.value(QStringLiteral("endpoint")).toString().trimmed());
      config.model = object.value(QStringLiteral("model")).toString();
      config.stream = object.value(QStringLiteral("stream")).toBool(true);
      config.timeoutMs = object.value(QStringLiteral("timeout_ms")).toInt(30000);
      config.maxTokens = object.value(QStringLiteral("max_tokens")).toInt(1024);
      config.allowInsecureHttp =
        object.value(QStringLiteral("allow_insecure_http")).toBool();
    }
  }
  const QString endpointEnv = QString::fromUtf8(qgetenv(kEnvEndpoint)).trimmed();
  if (!endpointEnv.isEmpty())
    config.endpoint = QUrl(endpointEnv);
  const QString modelEnv = QString::fromUtf8(qgetenv(kEnvModel)).trimmed();
  if (!modelEnv.isEmpty())
    config.model = modelEnv;
  // 环境变量里的密钥只在内存中流转（密钥从不进 JSON——UI/测试注入口）。
  const QString keyEnv = QString::fromUtf8(qgetenv(kEnvKey)).trimmed();
  if (!keyEnv.isEmpty())
    config.apiKey = keyEnv.toUtf8();
  return config;
}

bool LlmConfig::save(QString *error) const {
  QJsonObject object;
  // 密钥不在落盘面：由 LlmKeyStore 走系统钥匙串（缺席时降级为内存态）。
  object.insert(QStringLiteral("endpoint"), endpoint.toString());
  object.insert(QStringLiteral("model"), model);
  object.insert(QStringLiteral("stream"), stream);
  object.insert(QStringLiteral("timeout_ms"), timeoutMs);
  object.insert(QStringLiteral("max_tokens"), maxTokens);
  object.insert(QStringLiteral("allow_insecure_http"), allowInsecureHttp);
  const QString path = this->path();
  QSaveFile file(path);
  QDir().mkpath(QFileInfo(path).absolutePath());
  const QByteArray bytes =
    QJsonDocument(object).toJson(QJsonDocument::Compact);
  if (!file.open(QIODevice::WriteOnly) || file.write(bytes) != bytes.size() ||
      !file.commit()) {
    if (error)
      *error = file.errorString();
    return false;
  }
  return true;
}

QString llmErrorLabel(LlmErrorKind kind) {
  switch (kind) {
  case LlmErrorKind::None:
    return QString();
  case LlmErrorKind::NotConfigured:
    return QObject::tr("未配置");
  case LlmErrorKind::Unauthorized:
    return QObject::tr("密钥无效或无访问权限");
  case LlmErrorKind::RateLimited:
    return QObject::tr("服务端限流");
  case LlmErrorKind::ServerError:
    return QObject::tr("服务端错误");
  case LlmErrorKind::BadRequest:
    return QObject::tr("请求被拒绝");
  case LlmErrorKind::Network:
    return QObject::tr("网络错误");
  case LlmErrorKind::Timeout:
    return QObject::tr("请求超时");
  case LlmErrorKind::Malformed:
    return QObject::tr("应答格式非法");
  case LlmErrorKind::TooLarge:
    return QObject::tr("应答超出上限");
  case LlmErrorKind::Cancelled:
    return QObject::tr("已取消");
  }
  return QObject::tr("未知错误");
}

// ---------------------------------------------------------------------------
// LlmStreamParser
// ---------------------------------------------------------------------------
void LlmStreamParser::feed(const QByteArray &chunk, QStringList *deltas,
                           bool *done, QString *error) {
  if (deltas)
    deltas->clear();
  m_buffer.append(chunk);
  // SSE 事件以空行分隔；最后一个不完整的事件留在缓冲里等下一块。
  int boundary;
  while ((boundary = m_buffer.indexOf("\n\n")) >= 0) {
    const QByteArray event = m_buffer.left(boundary);
    m_buffer.remove(0, boundary + 2);
    QString payload;
    for (const QByteArray &rawLine : event.split('\n')) {
      const QByteArray line = rawLine.trimmed();
      if (line.isEmpty() || line.startsWith(':'))
        continue; // 注释/心跳行：SSE 允许，不参与解析
      if (!line.startsWith("data:")) {
        continue; // event:/id:/retry: 等字段：本方向不使用，忽略而非报错
      }
      payload += QString::fromUtf8(line.mid(5).trimmed());
    }
    if (payload.trimmed() == QLatin1String("[DONE]")) {
      if (done)
        *done = true;
      continue;
    }
    if (payload.trimmed().isEmpty())
      continue;
    QJsonParseError parseError;
    const QJsonDocument document =
      QJsonDocument::fromJson(payload.toUtf8(), &parseError);
    if (parseError.error != QJsonParseError::NoError || !document.isObject()) {
      if (error)
        *error = QObject::tr("流式事件不是合法 JSON：%1").arg(parseError.errorString());
      continue;
    }
    const QJsonObject object = document.object();
    const QJsonObject usage = object.value(QStringLiteral("usage")).toObject();
    if (!usage.isEmpty()) {
      // usage 只在收尾事件（或全部事件）里出现，取最后一次即可。
      promptTokens = usage.value(QStringLiteral("prompt_tokens")).toInt(promptTokens);
      completionTokens =
        usage.value(QStringLiteral("completion_tokens")).toInt(completionTokens);
    }
    const QJsonArray choices = object.value(QStringLiteral("choices")).toArray();
    for (const QJsonValue &choiceValue : choices) {
      const QJsonObject choice = choiceValue.toObject();
      const QJsonObject delta = choice.value(QStringLiteral("delta")).toObject();
      const QString text = delta.value(QStringLiteral("content")).toString();
      if (!text.isEmpty() && deltas)
        deltas->append(text);
      const QJsonArray calls = delta.value(QStringLiteral("tool_calls")).toArray();
      for (const QJsonValue &callValue : calls) {
        const QJsonObject callObject = callValue.toObject();
        const int index = callObject.value(QStringLiteral("index")).toInt(-1);
        if (index < 0)
          continue; // 无 index 的增量无法归段——如实丢弃（不猜下标）
        LlmToolDraft draft = m_drafts.value(index);
        draft.index = index;
        const QJsonObject function =
          callObject.value(QStringLiteral("function")).toObject();
        const QString id = callObject.value(QStringLiteral("id")).toString();
        if (!id.isEmpty())
          draft.id = id;
        const QString name = function.value(QStringLiteral("name")).toString();
        if (!name.isEmpty())
          draft.name = name;
        draft.arguments += function.value(QStringLiteral("arguments")).toString();
        m_drafts.insert(index, draft);
      }
    }
  }
}

QVector<ChatToolCall> LlmStreamParser::takeCompleteCalls() {
  QVector<ChatToolCall> out;
  for (auto it = m_drafts.begin(); it != m_drafts.end();) {
    const LlmToolDraft &draft = it.value();
    ChatToolCall call;
    call.id = draft.id;
    call.name = draft.name;
    call.argumentsJson = draft.arguments;
    if (draft.hasIdentity() && call.hasValidArguments()) {
      out.append(call);
      it = m_drafts.erase(it);
    } else {
      ++it;
    }
  }
  return out;
}

QVector<ChatToolCall> LlmStreamParser::flush() {
  QVector<ChatToolCall> out;
  // 按下标升序：模型并发下发多段时破坏顺序会让人看不懂。
  QList<int> indices = m_drafts.keys();
  std::sort(indices.begin(), indices.end());
  for (int index : indices) {
    const LlmToolDraft draft = m_drafts.value(index);
    if (!draft.hasIdentity())
      continue;
    ChatToolCall call;
    call.id = draft.id;
    call.name = draft.name;
    call.argumentsJson = draft.arguments;
    out.append(call);
  }
  m_drafts.clear();
  return out;
}

// ---------------------------------------------------------------------------
// LlmClient
// ---------------------------------------------------------------------------
LlmClient::LlmClient(QObject *parent)
  : QObject(parent)
  , m_nam(new QNetworkAccessManager(this))
  , m_timer(new QTimer(this))
{
  m_timer->setSingleShot(true);
  connect(m_timer, &QTimer::timeout, this, [this]() {
    fail(LlmErrorKind::Timeout,
         QObject::tr("大模型请求空闲超时（>%1 ms）").arg(m_config.timeoutMs));
  });
}

LlmClient::~LlmClient() {
  ++m_generation; // 作废在飞回调（析构期不会再有事件循环跑它们）
  m_timer->stop();
  if (m_reply) {
    m_reply->abort();
    m_reply.clear(); // 析构期不做 deleteLater：事件循环已不在，无人消费
  }
}

void LlmClient::setConfig(const LlmConfig &config) { m_config = config; }

QNetworkReply *LlmClient::issueRequest(const QVector<ChatMessage> &messages) {
  QJsonArray array;
  for (const ChatMessage &message : messages)
    array.append(message.toProtocolJson()); // 只发协议字段（见 chatmessage.h）
  QJsonObject body;
  body.insert(QStringLiteral("model"), m_config.model);
  body.insert(QStringLiteral("messages"), array);
  body.insert(QStringLiteral("stream"), m_config.stream);
  body.insert(QStringLiteral("max_tokens"), m_config.maxTokens);

  QNetworkRequest request(m_config.completionsUrl());
  request.setHeader(QNetworkRequest::ContentTypeHeader,
                    QStringLiteral("application/json"));
  request.setRawHeader(
    "Accept", m_config.stream ? "text/event-stream" : "application/json");
  if (!m_config.apiKey.isEmpty())
    request.setRawHeader("Authorization", "Bearer " + m_config.apiKey);
  request.setTransferTimeout(0); // 超时由本端定时器统一判（避免双重超时语义）
  return m_nam->post(request, QJsonDocument(body).toJson(QJsonDocument::Compact));
}

void LlmClient::send(const QVector<ChatMessage> &messages) {
  const QString invalid = m_config.validate();
  if (!invalid.isEmpty()) {
    // 诚实禁用态：不给假结果，也不静默吞掉请求。
    emit errorOccurred(LlmErrorKind::NotConfigured, invalid);
    return;
  }
  cancel(); // 中止上一轮（若有）并提世代
  const int generation = ++m_generation;
  m_body.clear();
  m_received = 0;
  m_parser = LlmStreamParser();
  m_promptTokens = 0;
  m_completionTokens = 0;

  QNetworkReply *reply = issueRequest(messages);
  m_reply = reply;
  m_timer->start(m_config.timeoutMs);

  connect(reply, &QNetworkReply::readyRead, this, [this, reply, generation]() {
    if (generation != m_generation)
      return;
    m_timer->start(m_config.timeoutMs); // 空闲计时：收到数据即重置
    const QByteArray chunk = reply->readAll();
    // 上限按累计到货字节判（流式与非流式同一把尺子——不能因为流式的载荷
    // 不进 m_body 就让它绕过内存上限）。
    if (m_received + qint64(chunk.size()) > kMaxResponseBytes) {
      fail(LlmErrorKind::TooLarge, QObject::tr("应答超过 %1 字节上限")
                                     .arg(kMaxResponseBytes));
      return;
    }
    m_received += chunk.size();
    if (!m_config.stream) {
      m_body.append(chunk);
      return;
    }
    QStringList deltas;
    bool done = false;
    QString parseError;
    m_parser.feed(chunk, &deltas, &done, &parseError);
    for (const QString &text : deltas)
      emit deltaReceived(text);
    for (const ChatToolCall &call : m_parser.takeCompleteCalls())
      emit toolCallReceived(call);
    if (!parseError.isEmpty()) {
      fail(LlmErrorKind::Malformed, parseError);
      return;
    }
    if (done) {
      for (const ChatToolCall &call : m_parser.flush())
        emit toolCallReceived(call);
      finishUp(QStringLiteral("stop"), m_parser.promptTokens,
               m_parser.completionTokens);
    }
  });

  connect(reply, &QNetworkReply::finished, this, [this, reply, generation]() {
    if (generation != m_generation) { // 已被超时/取消/新一轮作废
      reply->deleteLater();
      return;
    }
    m_timer->stop();
    ++m_generation;
    m_reply = nullptr;
    const bool ok = reply->error() == QNetworkReply::NoError;
    const int status =
      reply->attribute(QNetworkRequest::HttpStatusCodeAttribute).toInt();
    const QByteArray tail = reply->readAll();
    const QString detail = reply->errorString();
    reply->deleteLater();
    // 分类顺序：先看 HTTP 状态码，再看传输层错误——Qt 会把 401/403/404/5xx
    // 同时报成 error()（AuthenticationRequiredError 等），若先判 ok 就会把
    // 「密钥无效」误报成「网络错误」（实测 401 就踩过）。
    if (status == 401 || status == 403) {
      emit errorOccurred(
        LlmErrorKind::Unauthorized,
        QObject::tr("密钥无效或无访问权限（HTTP %1）").arg(status));
      return;
    }
    if (status == 429) {
      emit errorOccurred(LlmErrorKind::RateLimited,
                         QObject::tr("服务端限流（HTTP %1）").arg(status));
      return;
    }
    if (status >= 500) {
      emit errorOccurred(LlmErrorKind::ServerError,
                         QObject::tr("服务端错误（HTTP %1）").arg(status));
      return;
    }
    if (status >= 400) {
      emit errorOccurred(LlmErrorKind::BadRequest,
                         QObject::tr("请求被拒绝（HTTP %1）").arg(status));
      return;
    }
    if (!ok) {
      emit errorOccurred(LlmErrorKind::Network,
                         QObject::tr("网络错误：%1").arg(detail));
      return;
    }
    if (m_config.stream) {
      // [DONE] 缺失但连接已正常关闭：把缓冲里最后一个事件兜底 flush。
      if (!tail.isEmpty()) {
        QStringList deltas;
        bool done = false;
        QString parseError;
        m_parser.feed(tail + QByteArrayLiteral("\n\n"), &deltas, &done,
                      &parseError);
        for (const QString &text : deltas)
          emit deltaReceived(text);
        for (const ChatToolCall &call : m_parser.takeCompleteCalls())
          emit toolCallReceived(call);
      }
      for (const ChatToolCall &call : m_parser.flush())
        emit toolCallReceived(call);
      finishUp(QStringLiteral("stop"), m_parser.promptTokens,
               m_parser.completionTokens);
      return;
    }
    handleNonStreamReply(m_body);
  });
}

void LlmClient::handleNonStreamReply(const QByteArray &payload) {
  QJsonParseError parseError;
  const QJsonDocument document =
    QJsonDocument::fromJson(payload, &parseError);
  if (parseError.error != QJsonParseError::NoError || !document.isObject()) {
    emit errorOccurred(LlmErrorKind::Malformed,
                       QObject::tr("应答不是合法 JSON：%1")
                         .arg(parseError.errorString()));
    return;
  }
  const QJsonObject object = document.object();
  const QJsonArray choices = object.value(QStringLiteral("choices")).toArray();
  if (choices.isEmpty()) {
    emit errorOccurred(LlmErrorKind::Malformed,
                       QObject::tr("应答缺少 choices 字段"));
    return;
  }
  const QJsonObject choice = choices.at(0).toObject();
  const QJsonObject message = choice.value(QStringLiteral("message")).toObject();
  const QString content = message.value(QStringLiteral("content")).toString();
  if (!content.isEmpty())
    emit deltaReceived(content);
  const QJsonArray calls = message.value(QStringLiteral("tool_calls")).toArray();
  for (const QJsonValue &value : calls)
    emit toolCallReceived(ChatToolCall::fromJson(value.toObject()));
  const QJsonObject usage = object.value(QStringLiteral("usage")).toObject();
  finishUp(choice.value(QStringLiteral("finish_reason")).toString(),
           usage.value(QStringLiteral("prompt_tokens")).toInt(),
           usage.value(QStringLiteral("completion_tokens")).toInt());
}

void LlmClient::cancel() {
  ++m_generation; // 作废在飞的那一轮（其回调一律不再回信号）
  m_timer->stop();
  if (m_reply) {
    m_reply->abort();
    m_reply->deleteLater();
    m_reply = nullptr;
  }
}

void LlmClient::fail(LlmErrorKind kind, const QString &message) {
  ++m_generation;
  m_timer->stop();
  if (m_reply) {
    m_reply->abort();
    m_reply->deleteLater();
    m_reply = nullptr;
  }
  emit errorOccurred(kind, message);
}

void LlmClient::finishUp(const QString &reason, int promptTokens,
                         int completionTokens) {
  ++m_generation;
  m_timer->stop();
  // 收尾即放掉 reply：连接可能还开着（[DONE] 到了不等于 socket 关了），
  // 留着裸指针等析构时解引用就是悬垂。
  if (m_reply) {
    m_reply->deleteLater();
    m_reply.clear();
  }
  emit finished(reason, promptTokens, completionTokens);
}
