// 层：功能
#include "aichatcontroller.h"
#include "chatinvocationbinding.h"
#include <QCryptographicHash>
#include <QJsonDocument>
#include <QPointer>
#include <QSignalBlocker>
#include <QUuid>

AiChatController::~AiChatController() {
  // Disconnect before member destruction: cancellation can synchronously emit,
  // while transcript/session members must still be alive for normal callbacks.
  const QSignalBlocker notifications(this);
  stopCurrentRound(false);
  disconnect(&m_client, nullptr, this, nullptr); disconnect(&m_runner, nullptr, this, nullptr);
  disconnect(&m_piBridge, nullptr, this, nullptr);
  m_piBridge.abort(false); m_client.cancel(); m_runner.interrupt();
}
void AiChatController::setPiRuntime(const paleo::pi::RuntimeConfig &configuration) {
  if (configuration.enabled == m_piConfiguration.enabled && configuration.root == m_piConfiguration.root &&
      configuration.manifestSha256 == m_piConfiguration.manifestSha256) return;
  const QPointer<AiChatController> guard(this);
  if (m_streaming) interrupt();
  if (guard) m_piConfiguration = configuration;
}
ChatToolCall AiChatController::hostAdmittedCall(const ChatToolCall &source) const {
  auto call = source;
  const auto ordinal = !m_session.messages.isEmpty() && m_session.messages.last().role == ChatRole::Assistant ?
      m_session.messages.last().toolCalls.size() + 1 : 1;
  // Provider IDs are freshly generated on a model retry. Durable identity is
  // host-owned position; changed content at that position conflicts before run.
  const QJsonObject identity{{"session", m_session.id}, {"turn", m_hostTurn}, {"round", QString::number(m_modelRound)},
    {"admittedOrdinal", QString::number(ordinal)}};
  call.id = "host_" + QString::fromLatin1(QCryptographicHash::hash(
      QJsonDocument(identity).toJson(QJsonDocument::Compact), QCryptographicHash::Sha256).toHex().left(40));
  return call;
}
void AiChatController::startEngineTurn() {
  m_modelRound = 0; m_hostTurn.clear(); m_turnContext = m_runner.hostContextIdentity();
  // G2-W1：定格本轮执行归属——模型配置与协商工具表在本入口快照，原生续轮
  // 与 Pi 桥内快照同源；回合中途的 setConfig/工具表变化不影响在途请求。
  m_turnConfig = m_config;
  m_turnTools = tools();
  m_turnFrozen = true;
  const auto code = paleo::chat::validateInvocationBindings(m_session);
  if (!code.isEmpty()) { piFailure(code); return; }
  for (qsizetype index = m_session.messages.size(); index > 0; --index) {
    auto &message = m_session.messages[index - 1];
    if (message.role != ChatRole::User) continue;
    if (message.hostTurnId.isEmpty()) message.hostTurnId = QUuid::createUuid().toString(QUuid::Id128);
    m_hostTurn = message.hostTurnId; break;
  }
  if (!m_piConfiguration.enabled) { sendRound(); return; }
  const QPointer<AiChatController> guard(this); const auto sessionId = m_session.id, turn = m_hostTurn;
  QString error;
  // Reuse the existing atomic private ChatSessionStore; this is not a second
  // operation journal. A missing durable host identity must precede no effects.
  if (!saveCurrentSession(&error)) { if (guard) piFailure("pi_session_save_failed"); return; }
  if (!guard || !m_streaming || sessionId != m_session.id || turn != m_hostTurn) return;
  const auto context = m_turnContext;
  const auto started = m_piBridge.start(m_piConfiguration, m_runner.taskService(), sessionId, turn, m_turnTools,
    [guard, context, sessionId, turn] {
      return guard && guard->m_session.id == sessionId && guard->m_hostTurn == turn && guard->m_runner.hostContextIdentity() == context;
    });
  if (guard && !started.isEmpty()) piFailure(started);
}
void AiChatController::piFailure(const QString &code) {
  if (!m_streaming) return;
  const QPointer<AiChatController> guard(this);
  const auto sequence = m_turnSequence + 1;
  stopCurrentRound(false);
  if (guard && sequence == m_turnSequence) emit errorOccurred(code);
}
