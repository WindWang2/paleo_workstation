// 层：功能
#include "llmclient.h"
#include "llmcredentialguard.h"

#include <QDir>
#include <QFile>
#include <QFileInfo>
#include <QHostAddress>
#include <QJsonArray>
#include <QJsonDocument>
#include <QJsonValue>
#include <QNetworkAccessManager>
#include <QNetworkReply>
#include <QNetworkRequest>
#include <QSaveFile>
#include <QStandardPaths>
#include <QTimer>

// ai/chat — LLM 客户端实现（方向51；G1 重整为 attempt 冻结 + 双时限 +
// 统一终态裁决）。
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
// readyRead 与 finished tail 同一把尺子、每字节只计一次（W6）。
constexpr qint64 kMaxResponseBytes = 8ll * 1024 * 1024;

// 应答是不是普通 chat completions JSON（choices 数组）——「stream=true 却
// 收到非 SSE 应答」的判别式之一（#588）：SSE 解析器对普通 JSON 一个事件都
// 切不出，若按流式收尾就会把零增量当成功 finishUp，空答复冒充成功。
bool isChatCompletionsJson(const QByteArray &body) {
  const QJsonDocument document = QJsonDocument::fromJson(body);
  return document.isObject() &&
         document.object().value(QStringLiteral("choices")).isArray();
}
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
      // PW-067：用户 JSON 里的超时/令牌数钳到合理域（0/负值会导致即时超时或 400）。
      config.timeoutMs =
        qBound(1000, object.value(QStringLiteral("timeout_ms")).toInt(30000), 600000);
      // G1（W2）：旧 JSON 缺 total_timeout_ms → 默认 300000（5 分钟）；
      // 合法域 [1000, 3600000]（1 秒 ~ 1 小时）。
      config.totalTimeoutMs = qBound(
        1000, object.value(QStringLiteral("total_timeout_ms")).toInt(300000),
        3600000);
      config.maxTokens =
        qBound(1, object.value(QStringLiteral("max_tokens")).toInt(1024), 32768);
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
  object.insert(QStringLiteral("total_timeout_ms"), totalTimeoutMs);
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
// LlmStreamParser —— 实现在 llmstreamparser.cpp（G1 拆分，函数体规模闸友好）
// ---------------------------------------------------------------------------

// ---------------------------------------------------------------------------
// LlmClient
// ---------------------------------------------------------------------------
LlmClient::LlmClient(QObject *parent)
  : QObject(parent)
  , m_nam(new QNetworkAccessManager(this))
  , m_timer(new QTimer(this))
  , m_totalTimer(new QTimer(this)) {
  installDeadlineTimers();
}

// 注入构造：NAM 归调用方所有（栈上注入时 reparent 会造成「栈作用域 +
// 父对象」双重析构）；默认构造自持 NAM。
LlmClient::LlmClient(QNetworkAccessManager *networkAccessManagerForTesting,
                     QObject *parent)
  : QObject(parent)
  , m_nam(networkAccessManagerForTesting)
  , m_timer(new QTimer(this))
  , m_totalTimer(new QTimer(this)) {
  installDeadlineTimers();
}

void LlmClient::installDeadlineTimers() {
  m_timer->setSingleShot(true);
  connect(m_timer, &QTimer::timeout, this, [this]() {
    // idle 与 total 共用 LlmErrorKind::Timeout（enum 兼容），文案区分
    // 「空闲超时 / 总时限」（W2）：调用方/测试按稳定子串辨识。
    const int generation = m_attempt.generation;
    if (!alive(generation) || !m_reply)
      return;
    fail(generation, LlmErrorKind::Timeout,
         QObject::tr("大模型请求空闲超时（>%1 ms）")
           .arg(m_attempt.config.timeoutMs));
  });
  m_totalTimer->setSingleShot(true);
  connect(m_totalTimer, &QTimer::timeout, this, [this]() {
    const int generation = m_attempt.generation;
    if (!alive(generation) || !m_reply)
      return;
    fail(generation, LlmErrorKind::Timeout,
         QObject::tr("大模型请求超过总时限（%1 ms；空闲计时不延长它）")
           .arg(m_attempt.config.totalTimeoutMs));
  });
}

LlmClient::~LlmClient() {
  ++m_generation; // 作废在飞回调（析构期不会再有事件循环跑它们）
  m_timer->stop();
  m_totalTimer->stop();
  if (m_reply) {
    m_reply->abort();
    m_reply.clear(); // 析构期不做 deleteLater：事件循环已不在，无人消费
  }
}

void LlmClient::setConfig(const LlmConfig &config) {
  // 只影响后续 send：在途请求冻结在 m_attempt（W1）。
  m_config = config;
}

QNetworkReply *LlmClient::issueRequest(const QVector<ChatMessage> &messages,
                                       const QVector<AiToolSpec> &tools,
                                       const LlmConfig &frozen) {
  QJsonArray array;
  for (const ChatMessage &message : messages)
    array.append(message.toProtocolJson()); // 只发协议字段（见 chatmessage.h）
  QJsonObject body;
  body.insert(QStringLiteral("model"), frozen.model);
  body.insert(QStringLiteral("messages"), array);
  body.insert(QStringLiteral("stream"), frozen.stream);
  body.insert(QStringLiteral("max_tokens"), frozen.maxTokens);
  // 工具上送（方向61）：tools[] 按 OpenAI 口径 = {type:"function",
  // function:{name,description,parameters}}；tool_choice 恒 "auto"——模型自决
  // 是否调用（含不调工具直接作答），不用 "required"/"none" 强制，也不点名
  // 单工具。空表不上送（无工具场景请求体保持方向51 形状）。
  if (!tools.isEmpty()) {
    QJsonArray toolArray;
    for (const AiToolSpec &spec : tools) {
      QJsonObject entry;
      entry.insert(QStringLiteral("type"), QStringLiteral("function"));
      entry.insert(QStringLiteral("function"), spec.toChatFunction());
      toolArray.append(entry);
    }
    body.insert(QStringLiteral("tools"), toolArray);
    body.insert(QStringLiteral("tool_choice"), QStringLiteral("auto"));
  }

  QNetworkRequest request(frozen.completionsUrl());
  request.setHeader(QNetworkRequest::ContentTypeHeader,
                    QStringLiteral("application/json"));
  // 重定向策略显式设为手动（与 wellfaciesservice.cpp 同款纪律）：Qt 默认
  // 会跟随跨源重定向，Bearer 密钥会被带到重定向目标主机上。此处不跟随——
  // 3xx 由 finished 的 status 分支如实报错，调用方自行决定是否重试。
  request.setAttribute(QNetworkRequest::RedirectPolicyAttribute,
                       QNetworkRequest::ManualRedirectPolicy);
  request.setRawHeader(
    "Accept", frozen.stream ? "text/event-stream" : "application/json");
  if (!frozen.apiKey.isEmpty())
    request.setRawHeader("Authorization", "Bearer " + frozen.apiKey);
  request.setTransferTimeout(0); // 超时由本端双计时器统一判（避免双重超时语义）
  return m_nam->post(request, QJsonDocument(body).toJson(QJsonDocument::Compact));
}

void LlmClient::send(const QVector<ChatMessage> &messages,
                     const QVector<AiToolSpec> &tools) {
  const QString invalid = m_config.validate();
  if (!invalid.isEmpty()) {
    // 诚实禁用态：不给假结果，也不静默吞掉请求。invalid send 不作废在途
    // 请求（语义：本次提交无效；上一轮合法请求照常完成，错误归属新提交）。
    emit errorOccurred(LlmErrorKind::NotConfigured, invalid);
    return;
  }
  cancel(); // 中止上一轮（若有）并提世代——有效 send 替换旧请求
  const int generation = ++m_generation;
  // 冻结 attempt（W1）：URL/模型/流式/密钥/双时限取当下快照；期间 setConfig
  // 只影响下一次 send。
  m_attempt = Attempt();
  m_attempt.generation = generation;
  m_attempt.config = m_config;
  m_body.clear();
  m_received = 0;
  m_sawStreamEvent = false;
  m_parser = LlmStreamParser();

  QNetworkReply *reply = issueRequest(messages, tools, m_attempt.config);
  m_reply = reply;
  m_timer->start(m_attempt.config.timeoutMs);
  m_totalTimer->start(m_attempt.config.totalTimeoutMs);

  // 订阅者可能在任一信号的同步槽里再 send/cancel/delete this：每个继续
  // 处理点前重查对象存活（QPointer）与世代号（W1）。
  const QPointer<LlmClient> self(this);
  connect(reply, &QNetworkReply::readyRead, this,
          [this, self, reply, generation]() {
    if (!self || !alive(generation))
      return;
    m_timer->start(m_attempt.config.timeoutMs); // idle：真实到货即重置
    const int status =
      reply->attribute(QNetworkRequest::HttpStatusCodeAttribute).toInt();
    const QByteArray chunk = reply->readAll();
    // 上限按累计到货字节判（流式与非流式同一把尺子；tail 在 finished 另计，
    // 每字节恰好一次——W6）。
    if (!countBytes(generation, qint64(chunk.size())))
      return;
    if (!m_sawStreamEvent)
      m_body.append(chunk); // 非 SSE 判别/非流式解析要整段体（见 W6 释放点）
    if (!m_attempt.config.stream)
      return;
    if (status >= 300) {
      // HTTP 分类优先（W5）：非 2xx 的应答体（哪怕长得像 SSE、带 [DONE]）
      // 不向下游派发任何内容；字节已计数，等 finished 统一裁决。
      return;
    }
    QStringList deltas;
    bool done = false;
    QString parseError;
    m_parser.feed(chunk, &deltas, &done, &parseError);
    if (!parseError.isEmpty() || !deltas.isEmpty())
      m_sawStreamEvent = true; // 见过真 SSE 事件（文本/坏事件）：不走非 SSE 兼容层
    for (const QString &text : deltas) {
      emit deltaReceived(text);
      if (!self || !alive(generation))
        return;
    }
    QString callError;
    const QVector<ChatToolCall> calls =
      m_parser.takeCompleteCalls(&callError);
    if (!calls.isEmpty() || done)
      m_sawStreamEvent = true;
    for (const ChatToolCall &call : calls) {
      emit toolCallReceived(call);
      if (!self || !alive(generation))
        return;
    }
    if (m_sawStreamEvent)
      m_body.clear(); // W6：已确认真流式，原始体不再需要（判别面已过）
    if (!parseError.isEmpty() || !callError.isEmpty()) {
      fail(generation, LlmErrorKind::Malformed,
           parseError.isEmpty() ? callError : parseError);
      return;
    }
    if (done)
      finalizeStream(generation); // [DONE] → 终局校验 + finished（单一收口）
  });

  connect(reply, &QNetworkReply::finished, this,
          [this, self, reply, generation]() {
    reply->deleteLater();
    if (!self || !alive(generation))
      return; // 已被 [DONE] 收尾/超时/取消/新一轮作废：不再二次结算
    const bool transportOk = reply->error() == QNetworkReply::NoError;
    const int status =
      reply->attribute(QNetworkRequest::HttpStatusCodeAttribute).toInt();
    const QString contentType =
      reply->header(QNetworkRequest::ContentTypeHeader).toString();
    const QString detail = reply->errorString();
    const QByteArray tail = reply->readAll();
    m_reply.clear();
    if (!countBytes(generation, qint64(tail.size())))
      return; // tail 越限同样有界失败（W6：readyRead 与 tail 统一口径）
    if (!m_sawStreamEvent)
      m_body.append(tail);
    // 分类顺序（W5 统一裁决）：HTTP 状态码优先于传输层错误——Qt 会把
    // 401/403/404/5xx 同时报成 error()（AuthenticationRequiredError 等），
    // 先判 ok 就会把「密钥无效」误报成「网络错误」。
    if (status >= 300 && status < 400) {
      // 请求不跟随重定向（见 issueRequest）：3xx 必须如实报错；Location
      // 可能携带回显的凭据/查询秘密——先按 attempt 密钥脱敏再进诊断。
      const QString location = paleo::chat::redactCredential(
        reply->header(QNetworkRequest::LocationHeader).toString(),
        m_attempt.config.apiKey);
      fail(generation, LlmErrorKind::Network,
           QObject::tr("服务端要求重定向（HTTP %1 → %2）：未跟随（避免密钥跨源外泄）")
             .arg(QString::number(status),
                  location.isEmpty() ? QStringLiteral("?") : location));
      return;
    }
    if (status == 401 || status == 403) {
      fail(generation, LlmErrorKind::Unauthorized,
           QObject::tr("密钥无效或无访问权限（HTTP %1）").arg(status));
      return;
    }
    if (status == 429) {
      fail(generation, LlmErrorKind::RateLimited,
           QObject::tr("服务端限流（HTTP %1）").arg(status));
      return;
    }
    if (status >= 500) {
      fail(generation, LlmErrorKind::ServerError,
           QObject::tr("服务端错误（HTTP %1）").arg(status));
      return;
    }
    if (status >= 400) {
      fail(generation, LlmErrorKind::BadRequest,
           QObject::tr("请求被拒绝（HTTP %1）").arg(status));
      return;
    }
    if (!transportOk) {
      fail(generation, LlmErrorKind::Network,
           QObject::tr("网络错误：%1").arg(detail));
      return;
    }
    if (!m_attempt.config.stream) {
      handleNonStreamReply(generation, m_body);
      return;
    }
    // 流式应答的 EOF 收尾：**无条件**把尾字节 + 一个事件分隔符喂进解析器
    // （PW-061：最后一个事件可能没有空行结尾——「整条流只有一个缺终止
    // 空行的事件」时 readyRead 一个完整行都切不出，m_sawStreamEvent 仍是
    // false，若把兜底喂给关在「见过事件」门后就会把合法完整 EOF 误判成
    // 「未包含任何事件」）。普通 JSON 应答喂它无害：不含 `data:` 字段名的
    // 行一律忽略，#588 的非 SSE 判别不受影响。
    QStringList deltas;
    bool done = false;
    QString parseError;
    m_parser.feed(tail + QByteArrayLiteral("\n\n"), &deltas, &done, &parseError);
    // 证据门（三类真内容）：坏事件、文本增量、[DONE] 终局、**工具草稿**。
    // 最后一项不可省：整条流只有一个「缺终止空行的工具帧」时，尾喂会把调用
    // 封印进草稿表却没有增量/done/错误——漏掉它就会跳过 finalizeStream，
    // 把已收到的工具帧静默丢弃，还报一句与事实相反的「未包含任何事件」。
    if (!parseError.isEmpty() || !deltas.isEmpty() || done ||
        m_parser.hasDrafts())
      m_sawStreamEvent = true; // 尾字节补出了真 SSE 内容/终局
    for (const QString &text : deltas) {
      emit deltaReceived(text);
      if (!self || !alive(generation))
        return;
    }
    if (!parseError.isEmpty()) {
      // EOF 时的坏事件：残缺 JSON/工具参数不得成功（合法完整 EOF 的容错
      // 范围只到「缺终止空行」，不含畸形载荷）。
      fail(generation, LlmErrorKind::Malformed, parseError);
      return;
    }
    if (m_sawStreamEvent) {
      finalizeStream(generation);
      return;
    }
    // 本轮没见过任何 SSE 事件：判别非 SSE 兼容层（#588）。优先级：内容类型
    // 明示非 event-stream、或 body 确是 chat completions 形状 → 整段转非
    // 流式解析（缺 choices 等字段如实报 Malformed）。
    const bool contentTypeMismatch =
      !contentType.trimmed().isEmpty() &&
      !contentType.contains(QStringLiteral("text/event-stream"));
    if (contentTypeMismatch || isChatCompletionsJson(m_body)) {
      handleNonStreamReply(generation, m_body);
      return;
    }
    // 200 error 信封（无 choices 的 JSON 对象）：如实报错——按流式收尾会把
    // 错误体当零内容成功（空答复冒充成功的又一变体）。
    {
      QJsonParseError envelopeError;
      const QJsonDocument document =
        QJsonDocument::fromJson(m_body, &envelopeError);
      if (envelopeError.error == QJsonParseError::NoError && document.isObject() &&
          document.object().contains(QStringLiteral("error"))) {
        const QJsonValue error = document.object().value(QStringLiteral("error"));
        QString message = error.isObject()
                            ? error.toObject().value(QStringLiteral("message")).toString()
                            : error.toString();
        if (message.isEmpty())
          message = QObject::tr("（无详情）");
        fail(generation, LlmErrorKind::Malformed,
             QObject::tr("服务端返回错误信封：%1").arg(message));
        return;
      }
    }
    // 零事件流（纯心跳/注释流、空应答、非 JSON 垃圾体）：没有可交付的内容，
    // 如实报错，不把零内容当成功。
    fail(generation, LlmErrorKind::Malformed,
         QObject::tr("流式应答未包含任何事件"));
  });
}

void LlmClient::handleNonStreamReply(int generation, const QByteArray &payload) {
  QJsonParseError parseError;
  const QJsonDocument document =
    QJsonDocument::fromJson(payload, &parseError);
  if (parseError.error != QJsonParseError::NoError || !document.isObject()) {
    fail(generation, LlmErrorKind::Malformed,
         QObject::tr("应答不是合法 JSON：%1").arg(parseError.errorString()));
    return;
  }
  const QJsonObject object = document.object();
  if (object.contains(QStringLiteral("error"))) {
    const QJsonValue error = object.value(QStringLiteral("error"));
    QString message = error.isObject()
                        ? error.toObject().value(QStringLiteral("message")).toString()
                        : error.toString();
    if (message.isEmpty())
      message = QObject::tr("（无详情）");
    fail(generation, LlmErrorKind::Malformed,
         QObject::tr("服务端返回错误信封：%1").arg(message));
    return;
  }
  const QJsonArray choices = object.value(QStringLiteral("choices")).toArray();
  if (choices.isEmpty()) {
    fail(generation, LlmErrorKind::Malformed,
         QObject::tr("应答缺少 choices 字段"));
    return;
  }
  const QJsonObject choice = choices.at(0).toObject();
  const QJsonValue messageValue = choice.value(QStringLiteral("message"));
  if (!messageValue.isObject()) {
    fail(generation, LlmErrorKind::Malformed,
         QObject::tr("应答缺少 message 字段"));
    return;
  }
  const QPointer<LlmClient> self(this);
  const QJsonObject message = messageValue.toObject();
  const QString content = message.value(QStringLiteral("content")).toString();
  if (!content.isEmpty()) {
    emit deltaReceived(content);
    if (!self || !alive(generation))
      return;
  }
  const QJsonArray calls = message.value(QStringLiteral("tool_calls")).toArray();
  for (const QJsonValue &value : calls) {
    emit toolCallReceived(ChatToolCall::fromJson(value.toObject()));
    if (!self || !alive(generation))
      return;
  }
  const QJsonObject usage = object.value(QStringLiteral("usage")).toObject();
  // finish_reason 透传（W5）：length/content_filter 等原样给 finished；
  // 旧兼容端点缺该字段时退化为 "stop"（文档化口径，不改写已有原因）。
  QString reason = choice.value(QStringLiteral("finish_reason")).toString();
  if (reason.isEmpty())
    reason = QStringLiteral("stop");
  finishUp(generation, reason,
           usage.value(QStringLiteral("prompt_tokens")).toInt(),
           usage.value(QStringLiteral("completion_tokens")).toInt());
}

void LlmClient::finalizeStream(int generation) {
  // 流终局（[DONE] 或 EOF）唯一收口：终局校验 → 完整工具帧（先于 finished
  // 交付）→ finished。残缺 arguments/缺 name 在此如实失败，绝不改造成 {}。
  if (!alive(generation))
    return;
  const QPointer<LlmClient> self(this);
  QString callError;
  const QVector<ChatToolCall> calls = m_parser.flush(&callError);
  for (const ChatToolCall &call : calls) {
    emit toolCallReceived(call);
    if (!self || !alive(generation))
      return;
  }
  if (!callError.isEmpty()) {
    fail(generation, LlmErrorKind::Malformed, callError);
    return;
  }
  QString reason = m_parser.finishReason();
  if (reason.isEmpty())
    reason = QStringLiteral("stop"); // 旧兼容端点缺 finish_reason 的退化口径
  finishUp(generation, reason, m_parser.promptTokens, m_parser.completionTokens);
}

bool LlmClient::countBytes(int generation, qint64 bytes) {
  if (!alive(generation))
    return false;
  if (bytes > kMaxResponseBytes - m_received) { // 差值比较：加法不溢出
    fail(generation, LlmErrorKind::TooLarge,
         QObject::tr("应答超过 %1 字节上限").arg(kMaxResponseBytes));
    return false;
  }
  m_received += bytes;
  return true;
}

void LlmClient::releaseReply() {
  if (m_reply) {
    m_reply->abort(); // 连接可能还开着（[DONE] ≠ socket 已关）：先断再放
    m_reply->deleteLater();
    m_reply.clear();
  }
}

void LlmClient::cancel() {
  ++m_generation; // 作废在飞的那一轮（其回调一律不再回信号；不伪造成功/失败）
  m_timer->stop();
  m_totalTimer->stop();
  releaseReply();
}

void LlmClient::fail(int generation, LlmErrorKind kind, const QString &message) {
  if (!alive(generation))
    return; // 旧 attempt 的迟到失败：静默丢弃，不得污染新请求
  ++m_generation; // 本 attempt 就此终局：单终态，后续 finished/计时器不再结算
  m_timer->stop();
  m_totalTimer->stop();
  releaseReply();
  // 诊断出网前按 attempt 密钥脱敏（W8）：错误文本/信封消息/Location 都可能
  // 回显 Bearer；密钥身份取自本次请求的冻结快照——配置切换后仍按旧 key 检。
  emit errorOccurred(kind, paleo::chat::redactCredential(message,
                                                         m_attempt.config.apiKey));
}

void LlmClient::finishUp(int generation, const QString &reason, int promptTokens,
                         int completionTokens) {
  if (!alive(generation))
    return;
  ++m_generation;
  m_timer->stop();
  m_totalTimer->stop();
  // 收尾即放掉 reply：连接可能还开着（[DONE] 到了不等于 socket 关了），
  // 留着裸指针等析构时解引用就是悬垂。
  releaseReply();
  emit finished(reason, promptTokens, completionTokens);
}
