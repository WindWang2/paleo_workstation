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
  flushImportReceipts();
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
void AiChatController::startEngineTurn(const QJsonObject &context) {
  const QPointer<AiChatController> guard(this);
  const auto sequence = m_turnSequence;
  m_modelRound = 0;
  m_roundRejected = false;
  m_toolRound = 0;
  m_turnCalls = 0;
  m_pendingToolResults = 0;
  m_unansweredCalls.clear();
  m_hostTurn.clear();
  m_turnContext = context;
  // One snapshot drives the provider schema, prompt, budgets and Pi bridge.
  const auto config = m_config;
  const auto brief = m_projectBrief;
  const auto declared = tools();
  if (!guard || sequence != m_turnSequence) return;
  m_turnConfig = config;
  m_turnTools = declared;
  m_turnPrompt = buildSystemPrompt(declared, brief);
  m_toolTokens = estimateTokens(m_turnPrompt) + qMin(config.maxTokens, kHistoryTokenBudget / 2);
  m_turnFrozen = true;
  const auto code = paleo::chat::validateInvocationBindings(m_session);
  for (qsizetype index = m_session.messages.size(); index > 0; --index) {
    auto &message = m_session.messages[index - 1];
    if (message.role != ChatRole::User) continue;
    if (message.hostTurnId.isEmpty()) message.hostTurnId = QUuid::createUuid().toString(QUuid::Id128);
    m_hostTurn = message.hostTurnId;
    m_toolTokens += estimateTokens(message.content);
    break;
  }
  // Synchronous UI observers must see the same ownership as the engine.
  setStreaming(true);
  if (!guard || sequence != m_turnSequence || !ensureTurnContext()) return;
  if (!code.isEmpty()) { failTurn(code); return; }
  if (!m_piConfiguration.enabled) { sendRound(); return; }
  const auto sessionId = m_session.id, turn = m_hostTurn;
  QString error;
  // Reuse the existing atomic private ChatSessionStore; this is not a second
  // operation journal. A missing durable host identity must precede no effects.
  if (!saveCurrentSession(&error)) { if (guard) failTurn("pi_session_save_failed"); return; }
  if (!guard || sequence != m_turnSequence || !ensureTurnContext()) return;
  const auto turnContext = m_turnContext;
  const auto started = m_piBridge.start(m_piConfiguration, m_runner.taskService(), sessionId, turn, m_turnTools,
    [guard, turnContext, sequence] {
      return guard && guard->m_turnSequence == sequence && guard->m_runner.hostContextIdentity() == turnContext;
    });
  if (guard && sequence == m_turnSequence && !started.isEmpty()) failTurn(started);
}
bool AiChatController::ensureTurnContext() {
  if (!m_streaming) return false;
  if (m_runner.hostContextIdentity() == m_turnContext) return true;
  failTurn(QStringLiteral("interrupted"));
  return false;
}

void AiChatController::failTurn(const QString &code) {
  if (!m_streaming) return;
  const QPointer<AiChatController> guard(this);
  const auto sequence = m_turnSequence + 1;
  stopCurrentRound(false);
  if (guard && sequence == m_turnSequence) emit errorOccurred(code);
}
