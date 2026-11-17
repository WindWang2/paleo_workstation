// 层：功能
#include "pichatbridge.h"
#include "services/paleotaskservice.h"
#include <QCoreApplication>
#include <QJsonDocument>
#include <QSet>
#include <QUuid>
#include <atomic>
#include <limits>

namespace {
bool keys(const QJsonObject &value, QStringList expected) { expected.sort(); return value.keys() == expected; }
qsizetype textBytes(const QString &text) {
  return QJsonDocument(QJsonObject{{"text", text}}).toJson(QJsonDocument::Compact).size();
}
struct Result { paleo::pi::ConversationResult value; std::atomic_bool ready{false}; };
} // namespace

PiChatBridge::PiChatBridge(QObject *parent) : QObject(parent), m_timer(this) {
  m_timer.setInterval(25); connect(&m_timer, &QTimer::timeout, this, &PiChatBridge::controlTick);
}
PiChatBridge::~PiChatBridge() { if (m_channel && m_active) m_channel->requestStop(false); }
QString PiChatBridge::start(const paleo::pi::RuntimeConfig &configuration, PaleoTaskService *service,
    const QString &session, const QString &turn, const QVector<AiToolSpec> &tools, std::function<bool()> contextCurrent) {
  if (m_active) return "pi_engine_busy";
  if (!configuration.enabled) return "pi_disabled";
  if (!service || service->maxWorkerThreads() < 2) return "pi_worker_capacity_unavailable";
  if (m_generation == std::numeric_limits<quint64>::max()) return "pi_generation_budget";
  paleo::pi::ConversationIdentity identity{session, QString::number(++m_generation), turn, QUuid::createUuid().toString(QUuid::Id128)};
  if (!identity.valid() || tools.size() > 128 || !contextCurrent) return "pi_conversation_request_invalid";
  qsizetype bytes = 0; QSet<QString> names;
  for (const auto &tool : tools) {
    const auto schema = QJsonDocument(tool.toJsonSchema()).toJson(QJsonDocument::Compact);
    bytes += schema.size() + tool.description.toUtf8().size() + tool.name.size();
    if (names.contains(tool.name) || schema.size() > 16 * 1024 || bytes > 512 * 1024) return "pi_configuration_budget";
    names.insert(tool.name);
  }
  m_channel = std::make_shared<paleo::pi::ConversationChannel>(identity);
  m_tools = tools; m_calls.clear(); m_completed.clear(); m_contextCurrent = std::move(contextCurrent);
  m_failure.clear(); m_round = 0; m_nextCall = 0; m_definition = 0; m_phase = Phase::Opening;
  m_pendingText.clear(); m_pendingTextBytes = 0; m_modelEnding = false; m_sentCalls = 0;
  m_active = true; m_service = service; m_controlAge.start();
  const QPointer<PiChatBridge> guard(this);
  if (!send("open")) { if (guard) m_active = false; return "pi_outgoing_budget"; }
  const auto state = std::make_shared<Result>(); const auto channel = m_channel;
  const auto generation = m_generation;
  // The application outlives its TaskService workers. Inspect QPointer only on
  // the owner thread; the worker posts one coalesced notification to this root.
  auto *application = QCoreApplication::instance();
  auto *task = service->start(tr("Pi 对话编排"), [configuration, channel, state, guard, application](PaleoTask *task) {
    const auto stopped = [task, channel] { return task->cancelRequested() || channel->stopReason() != 0; };
    const auto inspection = paleo::pi::inspectRuntime(configuration, stopped);
    if (!inspection.ok()) state->value.code = inspection.code;
    else {
      paleo::pi::ConversationRequest request; request.enabled = configuration.enabled; request.runtime = inspection.runtime;
      request.channel = channel; request.cancelled = [task] { return task->cancelRequested(); };
      request.incoming = [guard, application] {
        if (application) QMetaObject::invokeMethod(application, [guard] { if (guard) guard->drain(); }, Qt::QueuedConnection);
      };
      state->value = paleo::pi::runConversation(request);
    }
    state->ready.store(true, std::memory_order_release);
    return state->value.ok() ? QString() : state->value.code;
  }, QString(), PaleoTask::Priority::High, true);
  if (!guard || generation != m_generation) { channel->requestStop(false); return "interrupted"; }
  if (!task) { m_active = false; return "pi_task_unavailable"; }
  m_task = task; m_timer.start();
  connect(task, &PaleoTask::finished, this, [this, guard, generation, state, channel] {
    if (!guard || generation != m_generation) return;
    drain(); if (!guard) return;
    QString code = state->ready.load(std::memory_order_acquire) ? state->value.code : QStringLiteral("interrupted");
    if (channel->stopReason()) code = channel->stopReason() == 1 ? "cancelled" : "interrupted";
    else if (!m_failure.isEmpty()) code = m_failure;
    else if (code == "ok" && m_phase != Phase::Closed) code = "pi_conversation_incomplete";
    m_timer.stop(); m_task.clear(); m_active = false; emit finished(code);
  }, Qt::SingleShotConnection);
  return {};
}
bool PiChatBridge::send(const QString &type, const QJsonObject &payload) {
  if (m_channel && m_channel->send(type, payload)) return true;
  fail("pi_outgoing_budget"); return false;
}
void PiChatBridge::fail(const QString &code) {
  const bool first = m_failure.isEmpty(); if (first) m_failure = code;
  if (m_channel) m_channel->requestStop(false);
  m_pendingText.clear(); m_pendingTextBytes = 0; m_modelEnding = false;
  if (first) emit failed(code);
}
void PiChatBridge::abort(bool explicitUser) {
  if (m_active && m_channel) m_channel->requestStop(explicitUser);
  m_pendingText.clear(); m_pendingTextBytes = 0; m_modelEnding = false;
}
void PiChatBridge::controlTick() {
  if (!m_active) return;
  const QPointer<PiChatBridge> guard(this);
  // A service shutdown may delete a queued task without delivering its queued
  // finished signal. Do not leave the selected engine permanently busy.
  if (!m_service || !m_task) {
    fail("pi_task_unavailable"); if (!guard) return;
    m_timer.stop(); m_task.clear(); m_active = false; emit finished("interrupted"); return;
  }
  const bool current = m_contextCurrent && m_contextCurrent();
  if (!guard) return;
  if (!current) fail("interrupted");
  if (!guard) return;
  if (!m_channel->stopReason() && (m_phase != Phase::Model || m_modelEnding) && m_phase != Phase::Tool && m_phase != Phase::Closed && m_controlAge.elapsed() > 15000)
    fail("pi_control_timeout");
  if (!guard) return;
  flushModel();
  if (!guard) return;
  drain();
}
void PiChatBridge::configureNext() {
  m_controlAge.restart();
  if (m_definition == m_tools.size()) { m_phase = Phase::AwaitModel; send("start"); return; }
  const auto &tool = m_tools[m_definition];
  send("tool_definition", {{"name", tool.name}, {"description", tool.description},
    {"parametersJson", QString::fromUtf8(QJsonDocument(tool.toJsonSchema()).toJson(QJsonDocument::Compact))}});
}
void PiChatBridge::drain() {
  if (!m_channel) return;
  QVector<QJsonObject> frames;
  if (!m_channel->takePeer(frames)) {
    if (!m_drainScheduled) { m_drainScheduled = true; QTimer::singleShot(0, this, [this] { m_drainScheduled = false; drain(); }); }
    return;
  }
  const QPointer<PiChatBridge> guard(this);
  for (const auto &value : frames) { frame(value); if (!guard) return; }
}
void PiChatBridge::frame(const QJsonObject &value) {
  if (!m_active) return;
  const auto type = value.value("type").toString(); const auto payload = value.value("payload").toObject();
  if (m_channel->stopReason() && type != "idle" && type != "closed") return;
  if (!m_channel->stopReason()) {
    const QPointer<PiChatBridge> guard(this); const bool current = m_contextCurrent && m_contextCurrent();
    if (!guard) return;
    if (!current) { fail("interrupted"); return; }
  }
  m_controlAge.restart();
  if (type == "opened" && m_phase == Phase::Opening) {
    if (!keys(payload, {"upstreamCommit", "coreVersion", "nodeVersion", "toolExecution", "modelTransport", "sandboxed"}) ||
        payload.value("toolExecution") != "sequential" || payload.value("modelTransport") != "host-only" || payload.value("sandboxed") != false) {
      fail("pi_conversation_identity"); return;
    }
    m_phase = Phase::Configuring; configureNext(); return;
  }
  if (type == "configured" && m_phase == Phase::Configuring && m_definition < m_tools.size() &&
      keys(payload, {"name"}) && payload.value("name") == m_tools[m_definition].name) { ++m_definition; configureNext(); return; }
  if (type == "model_request" && m_phase == Phase::AwaitModel && keys(payload, {"round"}) &&
      payload.value("round").toString() == QString::number(m_round + 1) && m_round < 6) {
    ++m_round; m_calls.clear(); m_nextCall = 0; m_phase = Phase::Model; emit modelRequested(); return;
  }
  if (type == "tool_request" && keys(payload, {"round", "callId"})) {
    const auto id = payload.value("callId").toString();
    if (m_completed.contains(id)) { if (payload.value("round") == m_completed[id].value("round")) send("tool_result", m_completed[id]); else fail("pi_tool_state"); return; }
    if (m_nextCall < m_calls.size() && payload.value("round").toString() == QString::number(m_round) && id == m_calls[m_nextCall].id) {
      if (m_phase == Phase::Tool) return; // In-flight retransmission never dispatches twice.
      if (m_phase == Phase::AwaitTool) { m_phase = Phase::Tool; emit toolRequested(m_calls[m_nextCall]); return; }
    }
    fail("pi_tool_state"); return;
  }
  if (type == "idle" && keys(payload, {"code"}) &&
      (m_channel->stopReason() || m_phase == Phase::AwaitIdle || (m_phase == Phase::AwaitModel && m_nextCall == m_calls.size()))) {
    const auto code = payload.value("code").toString();
    if (!QStringList{"ok", "error", "cancelled", "interrupted"}.contains(code)) { fail("pi_idle_state"); return; }
    if (!m_channel->stopReason() && code != "ok" && m_failure.isEmpty()) m_failure = "pi_agent_failed";
    m_phase = Phase::Closing; send("close"); return;
  }
  if (type == "closed" && m_phase == Phase::Closing && payload.isEmpty()) { m_phase = Phase::Closed; return; }
  fail("pi_conversation_state");
}
bool PiChatBridge::modelDelta(const QString &text) {
  if (!m_active || m_phase != Phase::Model || m_channel->stopReason() || m_modelEnding) return false;
  if (text.isEmpty()) return true;
  // At most sixteen <=4096-character chunks and their complete JSON framing.
  // The second 128-KiB / 16-frame partition belongs exclusively to the ring.
  const auto bytes = textBytes(text);
  if (m_pendingText.size() + text.size() > 16 * 4096 ||
      bytes > paleo::pi::ConversationOutgoingBytes - 16 * 1024 - m_pendingTextBytes) { fail("pi_outgoing_budget"); return false; }
  m_pendingText += text; m_pendingTextBytes += bytes;
  return true;
}
bool PiChatBridge::modelFinished(const QVector<ChatToolCall> &calls, const QString &reason) {
  if (!m_active || m_phase != Phase::Model || m_channel->stopReason() || calls.size() > 8 || m_modelEnding) return false;
  m_calls = calls; m_nextCall = 0; m_sentCalls = 0; m_modelReason = reason; m_modelEnding = true; m_controlAge.restart();
  const QPointer<PiChatBridge> guard(this); flushModel(); return guard && m_failure.isEmpty();
}
void PiChatBridge::flushModel() {
  if (!m_active || m_phase != Phase::Model || m_channel->stopReason()) return;
  const auto queue = [&](const QString &type, const QJsonObject &payload) {
    if (m_channel->trySend(type, payload)) return true;
    const auto code = m_channel->error();
    if (!code.isEmpty() && code != "pi_conversation_busy") fail(code);
    return false;
  };
  while (!m_pendingText.isEmpty()) {
    qsizetype count = qMin<qsizetype>(4096, m_pendingText.size());
    if (count < m_pendingText.size() && m_pendingText[count - 1].isHighSurrogate() && m_pendingText[count].isLowSurrogate()) --count;
    const auto text = m_pendingText.left(count);
    if (!queue("model_delta", {{"round", QString::number(m_round)}, {"text", text}})) return;
    m_pendingText.remove(0, count); m_pendingTextBytes = qMax<qsizetype>(0, m_pendingTextBytes - textBytes(text));
  }
  m_pendingTextBytes = 0;
  if (!m_modelEnding) return;
  while (m_sentCalls < m_calls.size()) {
    const auto &call = m_calls[m_sentCalls];
    if (!queue("model_call", {{"round", QString::number(m_round)}, {"callId", call.id}, {"name", call.name}, {"argumentsJson", call.argumentsJson}})) return;
    ++m_sentCalls;
  }
  if (!queue("model_end", {{"round", QString::number(m_round)}, {"reason", m_modelReason}})) return;
  m_modelEnding = false; m_phase = m_calls.isEmpty() ? Phase::AwaitIdle : Phase::AwaitTool; m_controlAge.restart();
}
void PiChatBridge::modelFailed(const QString &code) {
  if (!m_active || m_phase != Phase::Model) { fail(code); return; }
  m_failure = code; m_phase = Phase::AwaitIdle; m_controlAge.restart();
  m_pendingText.clear(); m_pendingTextBytes = 0; m_modelEnding = false;
  send("model_error", {{"round", QString::number(m_round)}, {"code", code}});
}
bool PiChatBridge::toolFinished(const ChatToolCall &call, bool ok, const QString &content) {
  if (!m_active || m_phase != Phase::Tool || m_channel->stopReason() || m_nextCall >= m_calls.size() || call != m_calls[m_nextCall]) return false;
  const QJsonObject result{{"round", QString::number(m_round)}, {"callId", call.id}, {"ok", ok}, {"content", content}};
  m_completed.insert(call.id, result); ++m_nextCall;
  m_phase = m_nextCall < m_calls.size() ? Phase::AwaitTool : Phase::AwaitModel; m_controlAge.restart();
  return send("tool_result", result);
}
