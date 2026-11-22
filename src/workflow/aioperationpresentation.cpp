// 层：功能
#include "aioperationpresentation.h"
#include "aichatcontroller.h"
#include "capabilityhost.h"
#include "capabilityrecovery.h"
#include "documentsession.h"
#include "services/hostcallcontext.h"
#include <QJsonDocument>
#include <QSet>

namespace paleo::chat {
namespace {
// 会话 id 的展示上限只作用于日志/忙碌键拼接；服务自身仍拿原值。
QString busyKey(const QString &action, const QString &operationId) {
  return action + QLatin1Char(':') + operationId;
}
} // namespace

AiOperationPresentation::AiOperationPresentation(const Deps &deps, QObject *parent)
  : QObject(parent), m_deps(deps) {
  // 重绑/换工程只递增展示代次并通知刷新；动作受理时永远现读 live 值。
  // 重绑/换会话只发通知；在途回调的丢弃由 guard（会话 id）与 per-动作代次承担。
  if (m_deps.document)
    connect(m_deps.document, &paleo::document::DocumentSession::sessionChanged, this,
            &AiOperationPresentation::contextChanged);
  if (m_deps.chat)
    connect(m_deps.chat, &AiChatController::sessionChanged, this,
            &AiOperationPresentation::contextChanged);
  if (m_deps.host)
    connect(m_deps.host, &QObject::destroyed, this, [this] { emit contextChanged(); });
}

AiOperationPresentation::HostContextView AiOperationPresentation::currentContext() const {
  HostContextView view;
  if (!m_deps.chat) {
    view.unavailableCode = QStringLiteral("chat_controller_unavailable");
    return view;
  }
  view.chatSessionId = m_deps.chat->session().id;
  view.hostTurnId = m_deps.chat->hostTurnId();
  view.turnContext = m_deps.chat->turnContext();
  if (!m_deps.host) {
    view.unavailableCode = QStringLiteral("operation_host_or_session_unavailable");
    return view;
  }
  const auto context = m_deps.host->context();
  view.open = context.open;
  view.readOnly = context.readOnly;
  view.documentId = context.documentId;
  view.epoch = context.epoch;
  view.horizon = context.horizon;
  view.unitId = context.unitId;
  view.selectionCount = int(context.selectionIds.size());
  if (!context.open) {
    view.unavailableCode = QStringLiteral("document_unavailable");
    return view;
  }
  view.available = true;
  return view;
}

bool AiOperationPresentation::busy(const QString &operationId, const QString &action) const {
  return m_busy.contains(busyKey(action, operationId));
}

bool AiOperationPresentation::checkGuard(const Guard &guard) const {
  // owner 线程同步校验：依赖存活 + 会话未变。宿主重绑/换工程由 scopeFor 的
  // checkContext(live, captured) 拦截；异步回调另加 per-动作代次。
  return guard.valid() && m_deps.chat && m_deps.host &&
         m_deps.chat->session().id == guard.chatSessionId;
}

QString AiOperationPresentation::scopeFor(const Guard &guard, QString *reason) const {
  if (!checkGuard(guard)) {
    if (reason)
      *reason = QStringLiteral("chat_context_changed");
    return {};
  }
  const auto live = m_deps.host->context();
  if (!live.open) {
    if (reason)
      *reason = QStringLiteral("document_unavailable");
    return {};
  }
  // 外层 Read 门：captured 与 live 的差异（重绑/换工程/epoch 变化）在此拒绝。
  const auto gate = paleo::host::checkContext(live, guard.capturedContext, paleo::host::Read);
  if (!gate.isEmpty()) {
    if (reason)
      *reason = gate;
    return {};
  }
  return m_deps.host->callerScope(paleo::host::Caller::Agent, guard.chatSessionId);
}

bool AiOperationPresentation::beginAction(const QString &operationId, const QString &action,
                                          Guard *guard, QString *reason) {
  if (!m_deps.chat || !m_deps.host || !m_deps.host->recovery()) {
    if (reason)
      *reason = QStringLiteral("operation_host_or_session_unavailable");
    return false;
  }
  const auto sessionId = m_deps.chat->session().id;
  if (sessionId.isEmpty()) {
    if (reason)
      *reason = QStringLiteral("chat_session_unavailable");
    return false;
  }
  const QString key = busyKey(action, operationId);
  if (m_busy.contains(key)) {
    if (reason)
      *reason = QStringLiteral("request_already_busy");
    return false;
  }
  guard->chatSessionId = sessionId;
  guard->capturedContext = m_deps.host->context();
  m_busy.insert(key);
  emit busyChanged(operationId, action, true);
  return true;
}

void AiOperationPresentation::releaseBusy(const QString &operationId, const QString &action) {
  if (m_busy.remove(busyKey(action, operationId)))
    emit busyChanged(operationId, action, false);
}

bool AiOperationPresentation::requestRefresh(const QString &operationId, QString *reason) {
  Guard guard;
  if (!beginAction(operationId, QStringLiteral("refresh"), &guard, reason))
    return false;
  QString gate;
  const auto scope = scopeFor(guard, &gate);
  if (scope.isEmpty()) {
    releaseBusy(operationId, QStringLiteral("refresh"));
    if (reason)
      *reason = gate;
    return false;
  }
  const QPointer<AiOperationPresentation> self(this);
  QJsonObject response = m_deps.host->recovery()->get(scope, operationId);
  // 同步可重入调用后再校验 owner 与绑定：期间销毁/换会话则结果不发。
  if (!self || !checkGuard(guard)) {
    releaseBusy(operationId, QStringLiteral("refresh"));
    if (reason)
      *reason = QStringLiteral("chat_context_changed");
    return false;
  }
  releaseBusy(operationId, QStringLiteral("refresh"));
  emit operationStateChanged(operationId, response);
  return true;
}

bool AiOperationPresentation::requestEvents(const QString &operationId, quint64 afterRevision,
                                            QString *reason) {
  Guard guard;
  if (!beginAction(operationId, QStringLiteral("events"), &guard, reason))
    return false;
  QString gate;
  const auto scope = scopeFor(guard, &gate);
  if (scope.isEmpty()) {
    releaseBusy(operationId, QStringLiteral("events"));
    if (reason)
      *reason = gate;
    return false;
  }
  const QPointer<AiOperationPresentation> self(this);
  QJsonObject response =
    m_deps.host->recovery()->events(scope, operationId, afterRevision);
  if (!self || !checkGuard(guard)) {
    releaseBusy(operationId, QStringLiteral("events"));
    if (reason)
      *reason = QStringLiteral("chat_context_changed");
    return false;
  }
  releaseBusy(operationId, QStringLiteral("events"));
  emit operationEventsArrived(operationId, response);
  return true;
}

bool AiOperationPresentation::requestResults(const QString &operationId, QString *reason) {
  Guard guard;
  if (!beginAction(operationId, QStringLiteral("results"), &guard, reason))
    return false;
  QString gate;
  const auto scope = scopeFor(guard, &gate);
  if (scope.isEmpty()) {
    releaseBusy(operationId, QStringLiteral("results"));
    if (reason)
      *reason = gate;
    return false;
  }
  const QPointer<AiOperationPresentation> self(this);
  QJsonObject response = m_deps.host->recovery()->results(scope, operationId);
  if (!self || !checkGuard(guard)) {
    releaseBusy(operationId, QStringLiteral("results"));
    if (reason)
      *reason = QStringLiteral("chat_context_changed");
    return false;
  }
  releaseBusy(operationId, QStringLiteral("results"));
  emit operationResultsArrived(operationId, response);
  return true;
}

bool AiOperationPresentation::requestPortPage(const QString &operationId, const QString &portId,
                                              qint64 offset, int maxBytes, QString *reason) {
  Guard guard;
  if (!beginAction(operationId, QStringLiteral("read"), &guard, reason))
    return false;
  QString gate;
  const auto scope = scopeFor(guard, &gate);
  if (scope.isEmpty()) {
    releaseBusy(operationId, QStringLiteral("read"));
    if (reason)
      *reason = gate;
    return false;
  }
  if (offset < 0 || maxBytes < 1 || maxBytes > 24 * 1024) {
    releaseBusy(operationId, QStringLiteral("read"));
    if (reason)
      *reason = QStringLiteral("invalid_read_budget");
    return false;
  }
  // 新读页请求顶替同操作旧读回调（按动作隔离：不误伤 resume 等其他动作）。
  const quint64 sequence = ++m_actionSequences[busyKey(QStringLiteral("read"), operationId)];
  const QPointer<AiOperationPresentation> self(this);
  const auto finished = m_deps.host->recovery()->read(
    scope, operationId, portId, offset, maxBytes, this,
    [this, self, sequence, guard, operationId, portId](QJsonObject page) {
      if (!self)
        return; // 适配器已销毁：回调属过期代次，静默丢弃（busy 随对象消亡）
      releaseBusy(operationId, QStringLiteral("read"));
      if (sequence != m_actionSequences.value(busyKey(QStringLiteral("read"), operationId)) ||
          !checkGuard(guard))
        return; // 被更新读请求/会话切换顶替：不发UI
      emit operationPortPage(operationId, portId, page);
    }, &gate);
  if (!finished) {
    // read 同步拒绝（task 服务缺席/operation 不存在/上下文门）：错误码即事实。
    releaseBusy(operationId, QStringLiteral("read"));
    QJsonObject rejection{{QStringLiteral("contract"),
                           QStringLiteral("paleo.operation.v2")},
                          {QStringLiteral("ok"), false},
                          {QStringLiteral("code"), gate.isEmpty()
                              ? QStringLiteral("result_unavailable") : gate}};
    emit operationPortPage(operationId, portId, rejection);
    return false;
  }
  return true;
}

bool AiOperationPresentation::requestCancel(const QString &operationId, QString *reason) {
  Guard guard;
  if (!beginAction(operationId, QStringLiteral("cancel"), &guard, reason))
    return false;
  QString gate;
  const auto scope = scopeFor(guard, &gate);
  if (scope.isEmpty()) {
    releaseBusy(operationId, QStringLiteral("cancel"));
    if (reason)
      *reason = gate;
    return false;
  }
  const QPointer<AiOperationPresentation> self(this);
  QJsonObject response = m_deps.host->recovery()->cancel(scope, operationId);
  if (!self || !checkGuard(guard)) {
    releaseBusy(operationId, QStringLiteral("cancel"));
    if (reason)
      *reason = QStringLiteral("chat_context_changed");
    return false;
  }
  releaseBusy(operationId, QStringLiteral("cancel"));
  emit operationActionFinished(operationId, QStringLiteral("cancel"), response);
  return true;
}

bool AiOperationPresentation::requestResume(const QString &operationId, QString *reason) {
  Guard guard;
  if (!beginAction(operationId, QStringLiteral("resume"), &guard, reason))
    return false;
  QString gate;
  const auto scope = scopeFor(guard, &gate);
  if (scope.isEmpty()) {
    releaseBusy(operationId, QStringLiteral("resume"));
    if (reason)
      *reason = gate;
    return false;
  }
  const quint64 sequence =
    ++m_actionSequences[busyKey(QStringLiteral("resume"), operationId)];
  const QPointer<AiOperationPresentation> self(this);
  auto *task = m_deps.host->recovery()->start(
    scope, operationId, this, QStringLiteral("Resume operation"),
    [this, self, sequence, guard, operationId](paleo::capability::ExecuteResult result) {
      if (!self)
        return;
      releaseBusy(operationId, QStringLiteral("resume"));
      if (sequence != m_actionSequences.value(busyKey(QStringLiteral("resume"), operationId)) ||
          !checkGuard(guard))
        return;
      QJsonObject response = QJsonDocument::fromJson(result.responseJson).object();
      if (response.isEmpty())
        response = QJsonObject{{QStringLiteral("contract"),
                                QStringLiteral("paleo.operation.v2")},
                               {QStringLiteral("ok"), false},
                               {QStringLiteral("code"), result.code}};
      emit operationActionFinished(operationId, QStringLiteral("resume"), response);
    }, true, &gate);
  if (!task) {
    releaseBusy(operationId, QStringLiteral("resume"));
    QJsonObject rejection{{QStringLiteral("contract"),
                           QStringLiteral("paleo.operation.v2")},
                          {QStringLiteral("ok"), false},
                          {QStringLiteral("code"), gate.isEmpty()
                              ? QStringLiteral("operation_resume_unavailable") : gate}};
    emit operationActionFinished(operationId, QStringLiteral("resume"), rejection);
    if (reason)
      *reason = gate.isEmpty() ? QStringLiteral("operation_resume_unavailable") : gate;
    return false;
  }
  return true;
}

} // namespace paleo::chat
