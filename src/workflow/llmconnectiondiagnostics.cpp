// 层：功能
#include "llmconnectiondiagnostics.h"
#include "llmdiagnosticreport.h"
#include "../ai/chat/llmcredentialguard.h"

#include <QJsonDocument>
#include <QJsonObject>
#include <QNetworkAccessManager>
#include <QUuid>

// H6 实现：显式启动的隔离诊断生命周期。本地规则（W1）先于任何网络；
// 探测经生产 LlmClient（同序列化/传输分类/错误终态）；工具往返是内存
// 协议探测（固定 nonce → 固定内存 echo 结果 → 一次后续请求），不经任何
// 执行面、不落任何存储。

namespace paleo::chat {

LlmConnectionDiagnostics::LlmConnectionDiagnostics(QObject *parent)
  : QObject(parent) {
  createNetworkObjects();
}

LlmConnectionDiagnostics::~LlmConnectionDiagnostics() {
  // 专属 client/inventory 析构各自 abort 在途请求（不触碰任何业务对象）。
}

void LlmConnectionDiagnostics::setNetworkAccessManagerForTesting(
  QNetworkAccessManager *nam) {
  m_testNam = nam;
  createNetworkObjects(); // 重建专属网络对象（测试须在首次 runAction 前调用）
}

void LlmConnectionDiagnostics::createNetworkObjects() {
  delete m_inventory;
  delete m_textClient;
  delete m_streamClient;
  delete m_toolClient;
  m_inventory = new LlmModelInventory(LlmModelInventory::Limits{}, m_testNam, this);
  m_textClient = m_testNam ? new LlmClient(m_testNam, this) : new LlmClient(this);
  m_streamClient = m_testNam ? new LlmClient(m_testNam, this) : new LlmClient(this);
  m_toolClient = m_testNam ? new LlmClient(m_testNam, this) : new LlmClient(this);
  // 目录收口
  connect(m_inventory, &LlmModelInventory::finished, this,
          [this](const LlmCatalogObservation &observation) {
    if (!m_busy || m_action != LlmDiagnosticsAction::Catalog)
      return; // 非本代次/已收口：迟到结果不覆盖新候选
    m_report.catalog = observation;
    emit modelCandidatesChanged(observation.models); // 空列表 = 清空候选
    completeAction(LlmDiagnosticsAction::Catalog);
  });
  // 文本/流式收口（客户端专属：一个 client 一种探测）
  connect(m_textClient, &LlmClient::deltaReceived, this, [this] {
    if (m_busy && m_action == LlmDiagnosticsAction::Text)
      ++m_report.text.deltaCount;
  });
  connect(m_streamClient, &LlmClient::deltaReceived, this, [this] {
    if (m_busy && m_action == LlmDiagnosticsAction::Stream)
      ++m_report.stream.deltaCount;
  });
  connect(m_textClient, &LlmClient::finished, this, [this](const QString &reason,
                                                           int prompt, int completion) {
    onProbeFinished(LlmProbeObservation::Kind::Text, reason, prompt, completion);
  });
  connect(m_textClient, &LlmClient::errorOccurred, this,
          [this](LlmErrorKind kind, const QString &message) {
    onProbeError(LlmProbeObservation::Kind::Text, kind, message);
  });
  connect(m_streamClient, &LlmClient::finished, this, [this](const QString &reason,
                                                             int prompt, int completion) {
    onProbeFinished(LlmProbeObservation::Kind::Stream, reason, prompt, completion);
  });
  connect(m_streamClient, &LlmClient::errorOccurred, this,
          [this](LlmErrorKind kind, const QString &message) {
    onProbeError(LlmProbeObservation::Kind::Stream, kind, message);
  });
  // 工具往返收口
  connect(m_toolClient, &LlmClient::deltaReceived, this, [this] {
    if (m_busy && m_action == LlmDiagnosticsAction::Tool)
      ++m_report.toolRoundTrip.deltaCount;
  });
  connect(m_toolClient, &LlmClient::toolCallReceived, this,
          &LlmConnectionDiagnostics::onToolFrame);
  connect(m_toolClient, &LlmClient::finished, this,
          &LlmConnectionDiagnostics::onToolFinished);
  connect(m_toolClient, &LlmClient::errorOccurred, this,
          &LlmConnectionDiagnostics::onToolError);
}

// ---------------------------------------------------------------------------
// 本地规则（W1）
// ---------------------------------------------------------------------------

QString LlmConnectionDiagnostics::structuralProblem(
  const LlmDiagnosticCandidate &candidate) const {
  // 借生产 LlmConfig::validate 的端点规则：占位模型/密钥只存在于本临时
  // 校验对象（绝不成为实际请求头或请求体——本函数返回即弃）。
  LlmConfig probe;
  probe.endpoint = candidate.endpoint;
  probe.allowInsecureHttp = candidate.allowInsecureHttp;
  probe.model = QStringLiteral("structural-placeholder");
  probe.apiKey = QByteArrayLiteral("structural-placeholder");
  return probe.validate();
}

QString LlmConnectionDiagnostics::missingCredentialReason(
  const LlmDiagnosticCandidate &candidate) const {
  if (candidate.keySource == LlmDiagnosticCandidate::KeySource::InMemoryCurrent)
    return QString();
  if (candidate.keySource == LlmDiagnosticCandidate::KeySource::ExplicitInput)
    return QString();
  // None：区分「端点变更未给目标凭据」与「当前本就无密钥」两类原因。
  return QObject::tr(
           "诊断需要本次可用凭据：沿用当前密钥要求端点身份完全一致（含 base "
           "path），端点已变更时请在密钥框输入新目标的密钥（仅用于本次诊断，"
           "不写入钥匙串）");
}

LlmConfig LlmConnectionDiagnostics::probeConfig(
  const LlmDiagnosticCandidate &candidate, bool stream) const {
  LlmConfig config;
  config.endpoint = candidate.endpoint;
  config.model = candidate.model;
  config.stream = stream;
  config.timeoutMs = candidate.timeoutMs;
  config.totalTimeoutMs = candidate.totalTimeoutMs;
  // 诊断输出上限不高于候选允许值；实际值进报告（不把测试结论泛化为全部
  // token 参数可用）。
  config.maxTokens = qMin(kProbeMaxTokens, candidate.maxTokens);
  config.allowInsecureHttp = candidate.allowInsecureHttp;
  config.apiKey = candidate.key;
  return config;
}

ChatMessage LlmConnectionDiagnostics::syntheticUserMessage() const {
  // 固定合成消息：无用户历史、无工程摘要、无真实数据。
  ChatMessage message;
  message.role = ChatRole::User;
  message.content =
    QObject::tr("连接诊断合成消息：请原样回复「诊断正常」。");
  return message;
}

ChatMessage LlmConnectionDiagnostics::toolPromptMessage() const {
  ChatMessage message;
  message.role = ChatRole::User;
  message.content = QObject::tr(
    "工具往返诊断：请调用 %1 工具，nonce 参数必须精确传 \"%2\"；不要调用"
    "其他工具，也不要执行任何实际操作。")
                      .arg(llmDiagnosticEchoToolName(), m_tool.nonce);
  return message;
}

AiToolSpec LlmConnectionDiagnostics::echoToolSpec() const {
  // 任务私有无副作用 echo：不进全局 registry，不经任何执行面。
  AiToolSpec spec;
  spec.name = llmDiagnosticEchoToolName();
  spec.title = QObject::tr("诊断回声");
  spec.description = QObject::tr(
    "连接诊断专用：原样回显宿主提供的 nonce。无任何业务副作用。");
  AiToolParamSpec nonce;
  nonce.name = QStringLiteral("nonce");
  nonce.type = QStringLiteral("string");
  nonce.description = QObject::tr("宿主生成的诊断一次性数值");
  nonce.required = true;
  spec.parameters.append(nonce);
  return spec;
}

// ---------------------------------------------------------------------------
// 显式启动（W2）
// ---------------------------------------------------------------------------

void LlmConnectionDiagnostics::runAction(LlmDiagnosticsAction action,
                                         const LlmDiagnosticCandidate &candidate) {
  if (m_busy) {
    // 单飞：在途再启动 = 本次点击的单次终态（零网络）。
    emit candidateRejected(QObject::tr("已有诊断在途——请等待完成或先取消"));
    return;
  }
  const QString structural = structuralProblem(candidate);
  if (!structural.isEmpty()) {
    emit candidateRejected(structural);
    return;
  }
  if (candidate.key.isEmpty()) {
    emit candidateRejected(missingCredentialReason(candidate));
    return;
  }
  if (action != LlmDiagnosticsAction::Catalog &&
      candidate.model.trimmed().isEmpty()) {
    emit candidateRejected(QObject::tr("尚未填写模型名称——文本/流式/工具探测"
                                       "需要真实选定模型（目录查询不需要）"));
    return;
  }
  m_busy = true;
  emit busyChanged(true);
  ++m_generation;
  m_action = action;
  m_diagnosticKey = candidate.key;
  // 冻结快照：实际测试值（不含任何秘密）+ 分项初始状态。
  m_report = LlmDiagnosticReport();
  m_report.generation = m_generation;
  m_report.endpoint = candidate.endpoint;
  m_report.model = candidate.model;
  m_report.draftStream = candidate.stream;
  m_report.timeoutMs = candidate.timeoutMs;
  m_report.totalTimeoutMs = candidate.totalTimeoutMs;
  m_report.candidateMaxTokens = candidate.maxTokens;
  m_report.probeMaxTokens = qMin(kProbeMaxTokens, candidate.maxTokens);
  m_report.keySource = candidate.keySource;
  m_report.startedAt = QDateTime::currentDateTime();
  switch (action) {
  case LlmDiagnosticsAction::Catalog:
    startCatalog(candidate);
    break;
  case LlmDiagnosticsAction::Text:
    startTextProbe(candidate);
    break;
  case LlmDiagnosticsAction::Stream:
    startStreamProbe(candidate);
    break;
  case LlmDiagnosticsAction::Tool:
    startToolProbe(candidate);
    break;
  }
  publishSummary();
}

void LlmConnectionDiagnostics::startCatalog(
  const LlmDiagnosticCandidate &candidate) {
  m_report.catalog.status = LlmDiagnosticStatus::Running;
  // 目录请求无凭据要求之外的重试/降级：单次 GET，失败如实分项。
  LlmDiagnosticCandidate request = candidate;
  request.key = m_diagnosticKey;
  m_inventory->probe(request);
}

void LlmConnectionDiagnostics::startTextProbe(
  const LlmDiagnosticCandidate &candidate) {
  m_report.text.kind = LlmProbeObservation::Kind::Text;
  m_report.text.status = LlmDiagnosticStatus::Running;
  // 计数先于 send（attempt 口径）：send 配置无效会同步收口，事后补计会漏。
  ++m_report.text.httpRequests;
  m_textClient->setConfig(probeConfig(candidate, /*stream=*/false));
  m_textClient->send({syntheticUserMessage()});
}

void LlmConnectionDiagnostics::startStreamProbe(
  const LlmDiagnosticCandidate &candidate) {
  m_report.stream.kind = LlmProbeObservation::Kind::Stream;
  m_report.stream.status = LlmDiagnosticStatus::Running;
  // 动作语义：「测试流式请求」恒以 stream=true 探测（与草稿当前开关无关，
  // 草稿值只进报告上下文）；结论仍只到「被当前客户端接受」。
  ++m_report.stream.httpRequests; // 计数先于 send（attempt 口径）
  m_streamClient->setConfig(probeConfig(candidate, /*stream=*/true));
  m_streamClient->send({syntheticUserMessage()});
}

void LlmConnectionDiagnostics::startToolProbe(
  const LlmDiagnosticCandidate &candidate) {
  m_report.toolRoundTrip.kind = LlmProbeObservation::Kind::ToolRoundTrip;
  m_report.toolRoundTrip.status = LlmDiagnosticStatus::Running;
  m_tool = ToolProbe();
  m_tool.phase = ToolProbe::Phase::AwaitingFrame;
  m_tool.nonce = QUuid::createUuid().toString(QUuid::Id128).left(24);
  // 计数先于 send（attempt 口径）；工具往返沿用草稿 stream 开关（报告头
  // draftStream 为该值——分项 detail 在终局再次点明实际模式）。
  ++m_report.toolRoundTrip.httpRequests;
  m_toolClient->setConfig(probeConfig(candidate, candidate.stream));
  m_toolClient->send({toolPromptMessage()}, {echoToolSpec()});
}

void LlmConnectionDiagnostics::cancel() {
  if (!m_busy)
    return;
  // 只取消诊断自身网络对象；Running 分项落 Cancelled（已终态分项不动）。
  m_inventory->cancel();
  m_textClient->cancel();
  m_streamClient->cancel();
  m_toolClient->cancel();
  for (LlmProbeObservation *observation :
       {&m_report.text, &m_report.stream, &m_report.toolRoundTrip}) {
    if (observation->status == LlmDiagnosticStatus::Running)
      observation->status = LlmDiagnosticStatus::Cancelled;
  }
  if (m_report.catalog.status == LlmDiagnosticStatus::Running)
    m_report.catalog.status = LlmDiagnosticStatus::Cancelled;
  m_tool = ToolProbe();
  completeAction(m_action);
}

// ---------------------------------------------------------------------------
// 探测收口
// ---------------------------------------------------------------------------

LlmProbeObservation &
LlmConnectionDiagnostics::observationFor(LlmProbeObservation::Kind kind) {
  return kind == LlmProbeObservation::Kind::Text
           ? m_report.text
           : kind == LlmProbeObservation::Kind::Stream ? m_report.stream
                                                       : m_report.toolRoundTrip;
}

void LlmConnectionDiagnostics::failObservation(
  LlmProbeObservation &observation, LlmDiagnosticStatus status, QString detail) {
  observation.status = status;
  observation.detail = redactCredential(detail, m_diagnosticKey);
}

void LlmConnectionDiagnostics::onProbeFinished(LlmProbeObservation::Kind kind,
                                               const QString &reason,
                                               int promptTokens,
                                               int completionTokens) {
  if (!m_busy ||
      (kind == LlmProbeObservation::Kind::Text &&
       m_action != LlmDiagnosticsAction::Text) ||
      (kind == LlmProbeObservation::Kind::Stream &&
       m_action != LlmDiagnosticsAction::Stream))
    return;
  LlmProbeObservation &observation = observationFor(kind);
  if (observation.status != LlmDiagnosticStatus::Running)
    return;
  observation.finishReason = reason;
  // usage 是服务端报告值：零值亦可能表示缺省——报告按「报告值」呈现，
  // 不宣称精确零费用/零 token。
  observation.usageReported = promptTokens > 0 || completionTokens > 0;
  observation.promptTokens = promptTokens;
  observation.completionTokens = completionTokens;
  observation.status = LlmDiagnosticStatus::Passed;
  observation.detail =
    kind == LlmProbeObservation::Kind::Text
      ? QObject::tr("文本请求可用（finish=%1）").arg(reason)
      : QObject::tr("stream=true 请求被当前客户端接受（真实 SSE 未独立确认）");
  completeAction(kind == LlmProbeObservation::Kind::Text
                   ? LlmDiagnosticsAction::Text
                   : LlmDiagnosticsAction::Stream);
}

void LlmConnectionDiagnostics::onProbeError(LlmProbeObservation::Kind kind,
                                            LlmErrorKind kindError,
                                            const QString &message) {
  if (!m_busy ||
      (kind == LlmProbeObservation::Kind::Text &&
       m_action != LlmDiagnosticsAction::Text) ||
      (kind == LlmProbeObservation::Kind::Stream &&
       m_action != LlmDiagnosticsAction::Stream))
    return;
  LlmProbeObservation &observation = observationFor(kind);
  if (observation.status != LlmDiagnosticStatus::Running)
    return;
  LlmDiagnosticStatus status;
  switch (kindError) {
  case LlmErrorKind::None:
  case LlmErrorKind::NotConfigured:
    status = LlmDiagnosticStatus::LocalRejected;
    break;
  case LlmErrorKind::Unauthorized:
    status = LlmDiagnosticStatus::Unauthorized;
    break;
  case LlmErrorKind::RateLimited:
    status = LlmDiagnosticStatus::RateLimited;
    break;
  case LlmErrorKind::ServerError:
    status = LlmDiagnosticStatus::ServerError;
    break;
  case LlmErrorKind::BadRequest:
    status = LlmDiagnosticStatus::Rejected;
    break;
  case LlmErrorKind::Network:
    status = LlmDiagnosticStatus::Network;
    break;
  case LlmErrorKind::Timeout:
    status = LlmDiagnosticStatus::Timeout;
    break;
  case LlmErrorKind::Malformed:
    status = LlmDiagnosticStatus::Malformed;
    break;
  case LlmErrorKind::TooLarge:
    status = LlmDiagnosticStatus::TooLarge;
    break;
  case LlmErrorKind::Cancelled:
    status = LlmDiagnosticStatus::Cancelled;
    break;
  }
  failObservation(observation, status, message);
  completeAction(kind == LlmProbeObservation::Kind::Text
                   ? LlmDiagnosticsAction::Text
                   : LlmDiagnosticsAction::Stream);
}

// ---------------------------------------------------------------------------
// 工具往返（W5）——内存协议探测，零业务副作用
// ---------------------------------------------------------------------------

bool LlmConnectionDiagnostics::frameStrictlyMatches(const ChatToolCall &call,
                                                    QString *why) const {
  if (call.name != llmDiagnosticEchoToolName()) {
    if (why)
      *why = QObject::tr("工具名不匹配（收到 %1）")
               .arg(redactCredential(call.name, m_diagnosticKey));
    return false;
  }
  if (call.id.trimmed().isEmpty()) {
    if (why)
      *why = QObject::tr("调用 ID 为空");
    return false;
  }
  const QJsonDocument arguments =
    QJsonDocument::fromJson(call.argumentsJson.toUtf8());
  if (!arguments.isObject()) {
    if (why)
      *why = QObject::tr("实参不是 JSON 对象");
    return false;
  }
  const QJsonObject object = arguments.object();
  if (object.size() != 1 || object.value(QStringLiteral("nonce")).toString() !=
                              m_tool.nonce) {
    if (why)
      *why = QObject::tr("实参不是恰好一个与宿主 nonce 精确一致的 "
                         "{\"nonce\":…} 对象");
    return false;
  }
  return true;
}

void LlmConnectionDiagnostics::onToolFrame(const ChatToolCall &call) {
  if (!m_busy || m_action != LlmDiagnosticsAction::Tool ||
      m_report.toolRoundTrip.status != LlmDiagnosticStatus::Running)
    return;
  ++m_tool.callCount;
  if (m_tool.phase == ToolProbe::Phase::ResultSent) {
    // 后续请求又要求工具：零第三轮，如实记录（终局按 Indeterminate 呈现）。
    m_report.toolRoundTrip.secondToolCallSeen = true;
    return;
  }
  if (m_tool.callCount > 1) {
    failObservation(m_report.toolRoundTrip, LlmDiagnosticStatus::Malformed,
                    QObject::tr("工具往返收到 %1 个调用帧（要求恰好一个）")
                      .arg(m_tool.callCount));
    completeAction(LlmDiagnosticsAction::Tool);
    return;
  }
  QString why;
  if (!frameStrictlyMatches(call, &why)) {
    failObservation(m_report.toolRoundTrip, LlmDiagnosticStatus::Malformed, why);
    completeAction(LlmDiagnosticsAction::Tool);
    return;
  }
  m_tool.observed = true;
  m_tool.frame = call; // 原样保存：回放用真实帧（含服务端原样的实参原文）
  m_report.toolRoundTrip.toolFrameObserved = true;
}

void LlmConnectionDiagnostics::sendToolFollowUp() {
  // 固定内存 echo 结果按原 call ID 构造 assistant/tool 协议消息（回放真实
  // 帧，不重建）；只属于本诊断对象，不写会话、不执行模型提供的任何内容。
  m_tool.phase = ToolProbe::Phase::ResultSent;
  m_report.toolRoundTrip.toolResultSentBack = true;
  ChatMessage assistant;
  assistant.role = ChatRole::Assistant;
  assistant.toolCalls.append(m_tool.frame);
  ChatMessage toolReply;
  toolReply.role = ChatRole::Tool;
  toolReply.toolCallId = m_tool.frame.id;
  toolReply.content =
    QStringLiteral("{\"echo\":\"%1\"}").arg(m_tool.nonce);
  m_toolClient->send(
    {toolPromptMessage(), assistant, toolReply},
    QVector<AiToolSpec>()); // 第二请求不带 tools：纯文本终局
  ++m_report.toolRoundTrip.httpRequests;
}

void LlmConnectionDiagnostics::onToolFinished(const QString &reason,
                                              int promptTokens,
                                              int completionTokens) {
  if (!m_busy || m_action != LlmDiagnosticsAction::Tool ||
      m_report.toolRoundTrip.status != LlmDiagnosticStatus::Running)
    return;
  m_report.toolRoundTrip.finishReason = reason;
  m_report.toolRoundTrip.usageReported = promptTokens > 0 || completionTokens > 0;
  m_report.toolRoundTrip.promptTokens = promptTokens;
  m_report.toolRoundTrip.completionTokens = completionTokens;
  if (m_tool.phase == ToolProbe::Phase::AwaitingFrame) {
    if (m_tool.observed && m_tool.callCount == 1) {
      // 首请求完整成功且恰好一个严格匹配帧：最多一次后续请求（回放真实帧）。
      sendToolFollowUp();
      return;
    }
    if (m_tool.observed)
      return; // 多帧已 fail（单终态已落）
    // auto 允许模型直接作答：未调用 ≠ 工具能力不支持。
    m_report.toolRoundTrip.status = LlmDiagnosticStatus::Indeterminate;
    m_report.toolRoundTrip.detail = QObject::tr(
      "模型未调用诊断工具（tool_choice=auto 允许直接作答；文本声称会调用"
      "不算工具帧）——不能据此判定工具能力");
    completeAction(LlmDiagnosticsAction::Tool);
    return;
  }
  // 第二请求终局
  if (m_report.toolRoundTrip.secondToolCallSeen) {
    m_report.toolRoundTrip.status = LlmDiagnosticStatus::Indeterminate;
    m_report.toolRoundTrip.detail = QObject::tr(
      "后续请求再次要求工具（已忽略；零第三轮）——不冒充完整通过");
  } else {
    m_report.toolRoundTrip.status = LlmDiagnosticStatus::Passed;
    m_report.toolRoundTrip.followUpTextCompleted = true;
    m_report.toolRoundTrip.detail = QObject::tr(
      "观察到正确工具帧 → 固定内存结果已按原 call ID 回送 → 后续文本完成"
      "（模式：stream=%1）")
      .arg(m_report.draftStream ? QStringLiteral("开") : QStringLiteral("关"));
  }
  completeAction(LlmDiagnosticsAction::Tool);
}

void LlmConnectionDiagnostics::onToolError(LlmErrorKind kind,
                                           const QString &message) {
  if (!m_busy || m_action != LlmDiagnosticsAction::Tool ||
      m_report.toolRoundTrip.status != LlmDiagnosticStatus::Running)
    return;
  // 工具帧已出现但随后整体 malformed/错误：不给完整通过结论（错误终态）。
  // 两枚举数值不对齐：显式映射，禁止 static_cast。
  LlmDiagnosticStatus status = LlmDiagnosticStatus::Network;
  switch (kind) {
  case LlmErrorKind::Unauthorized:
    status = LlmDiagnosticStatus::Unauthorized;
    break;
  case LlmErrorKind::RateLimited:
    status = LlmDiagnosticStatus::RateLimited;
    break;
  case LlmErrorKind::ServerError:
    status = LlmDiagnosticStatus::ServerError;
    break;
  case LlmErrorKind::BadRequest:
    status = LlmDiagnosticStatus::Rejected;
    break;
  case LlmErrorKind::Network:
    status = LlmDiagnosticStatus::Network;
    break;
  case LlmErrorKind::Timeout:
    status = LlmDiagnosticStatus::Timeout;
    break;
  case LlmErrorKind::TooLarge:
    status = LlmDiagnosticStatus::TooLarge;
    break;
  case LlmErrorKind::Malformed:
    status = LlmDiagnosticStatus::Malformed;
    break;
  case LlmErrorKind::None:
  case LlmErrorKind::NotConfigured:
    status = LlmDiagnosticStatus::LocalRejected;
    break;
  case LlmErrorKind::Cancelled:
    status = LlmDiagnosticStatus::Cancelled;
    break;
  }
  failObservation(m_report.toolRoundTrip, status, message);
  completeAction(LlmDiagnosticsAction::Tool);
}

// ---------------------------------------------------------------------------
// 收口与发布
// ---------------------------------------------------------------------------

void LlmConnectionDiagnostics::completeAction(LlmDiagnosticsAction action) {
  if (!m_busy)
    return;
  m_busy = false;
  m_tool = ToolProbe();
  m_diagnosticKey.clear(); // 本次凭据随代次结束出内存
  emit busyChanged(false);
  emit actionFinished(action);
  publishSummary();
}

void LlmConnectionDiagnostics::publishSummary() {
  emit summaryChanged(renderLlmDiagnosticSummary(m_report));
}

} // namespace paleo::chat
