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
QString busyKey(const QString &action, const QString &operationId) {
  return action + QLatin1Char(':') + operationId;
}
QJsonObject operationError(const QString &code) {
  return {{"contract", "paleo.operation.v2"}, {"ok", false}, {"code", code}};
}
} // namespace

AiOperationPresentation::AiOperationPresentation(const Deps &deps, QObject *parent)
  : QObject(parent), m_deps(deps) {
  // A generation also catches switching away from a session and then back to it.
  const auto invalidate = [this] {
    ++m_generation;
    emit contextChanged();
  };
  if (m_deps.document)
    connect(m_deps.document, &paleo::document::DocumentSession::sessionChanged, this, invalidate);
  if (m_deps.chat)
    connect(m_deps.chat, &AiChatController::sessionChanged, this, invalidate);
  if (m_deps.host)
    connect(m_deps.host, &QObject::destroyed, this, invalidate);

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

QString AiOperationPresentation::guardError(const Guard &guard) const {
  if (!m_deps.chat || !m_deps.host || guard.generation != m_generation ||
      m_deps.chat->session().id != guard.chatSessionId)
    return QStringLiteral("chat_context_changed");
  return paleo::host::checkContext(m_deps.host->context(), guard.capturedContext,
                                   paleo::host::Read);
}

QString AiOperationPresentation::beginAction(const QString &operationId, const QString &action,
                                             Guard &guard, QString *reason) {
  if (!m_deps.chat || !m_deps.host || !m_deps.host->recovery()) {
    if (reason)
      *reason = QStringLiteral("operation_host_or_session_unavailable");
    return {};
  }
  guard.chatSessionId = m_deps.chat->session().id;
  if (guard.chatSessionId.isEmpty() || busy(operationId, action)) {
    if (reason)
      *reason = guard.chatSessionId.isEmpty() ? QStringLiteral("chat_session_unavailable")
                                             : QStringLiteral("request_already_busy");
    return {};
  }
  guard.generation = m_generation;
  guard.capturedContext = m_deps.host->context();
  const QPointer<AiOperationPresentation> self(this);
  m_busy.insert(busyKey(action, operationId));
  emit busyChanged(operationId, action, true);

  if (!self)
    return {};
  const auto gate = guardError(guard);
  if (!gate.isEmpty()) {
    if (reason)
      *reason = gate;
    releaseBusy(operationId, action);
    return {};
  }
  return m_deps.host->callerScope(paleo::host::Caller::Agent, guard.chatSessionId);
}

void AiOperationPresentation::releaseBusy(const QString &operationId, const QString &action) {
  if (m_busy.remove(busyKey(action, operationId)))
    emit busyChanged(operationId, action, false);
}

bool AiOperationPresentation::finishAction(const QString &operationId, const QString &action,
                                           const Guard &guard, const QJsonObject &response,
                                           const Reporter &report, QString *reason) {
  const QPointer<AiOperationPresentation> self(this);
  const auto gate = guardError(guard);
  if (gate.isEmpty())
    report(response); // Keep this action busy until synchronous observers get its result.
  else if (reason)
    *reason = gate;
  if (self)
    releaseBusy(operationId, action);
  return gate.isEmpty();
}

bool AiOperationPresentation::runAction(const QString &operationId, const QString &action,
                                        const Operation &operation, const Reporter &report,
                                        QString *reason) {
  Guard guard;
  const auto scope = beginAction(operationId, action, guard, reason);
  if (scope.isEmpty())
    return false;
  const QPointer<AiOperationPresentation> self(this);
  const auto response = operation(*m_deps.host->recovery(), scope);
  return self && finishAction(operationId, action, guard, response, report, reason);
}

bool AiOperationPresentation::requestRefresh(const QString &operationId, QString *reason) {
  return runAction(operationId, QStringLiteral("refresh"),
    [&operationId](auto &service, const auto &scope) { return service.get(scope, operationId); },
    [this, &operationId](const auto &response) { emit operationStateChanged(operationId, response); },
    reason);
}

bool AiOperationPresentation::requestEvents(const QString &operationId, quint64 afterRevision,
                                            QString *reason) {
  return runAction(operationId, QStringLiteral("events"),
    [&operationId, afterRevision](auto &service, const auto &scope) {
      return service.events(scope, operationId, afterRevision);
    },
    [this, &operationId](const auto &response) { emit operationEventsArrived(operationId, response); },
    reason);
}

bool AiOperationPresentation::requestResults(const QString &operationId, QString *reason) {
  return runAction(operationId, QStringLiteral("results"),
    [&operationId](auto &service, const auto &scope) { return service.results(scope, operationId); },
    [this, &operationId](const auto &response) { emit operationResultsArrived(operationId, response); },
    reason);
}

bool AiOperationPresentation::requestPortPage(const QString &operationId, const QString &portId,
                                              qint64 offset, int maxBytes, QString *reason) {
  if (offset < 0 || maxBytes < 1 || maxBytes > 24 * 1024) {
    if (reason)
      *reason = QStringLiteral("invalid_read_budget");
    return false;
  }
  const auto action = QStringLiteral("read");
  Guard guard;
  const auto scope = beginAction(operationId, action, guard, reason);
  if (scope.isEmpty())
    return false;
  const QPointer<AiOperationPresentation> self(this);
  const auto report = [this, operationId, portId](const QJsonObject &page) {
    emit operationPortPage(operationId, portId, page);
  };
  QString gate;
  const bool started = m_deps.host->recovery()->read(
    scope, operationId, portId, offset, maxBytes, this,
    [self, guard, action, operationId, report](QJsonObject page) {
      if (self)
        self->finishAction(operationId, action, guard, page, report);
    }, &gate);
  if (!started && self) {
    if (gate.isEmpty())
      gate = QStringLiteral("result_unavailable");
    if (reason)
      *reason = gate;
    finishAction(operationId, action, guard, operationError(gate), report);
  }
  return started;
}

bool AiOperationPresentation::requestCancel(const QString &operationId, QString *reason) {
  const auto action = QStringLiteral("cancel");
  return runAction(operationId, action,
    [&operationId](auto &service, const auto &scope) { return service.cancel(scope, operationId); },
    [this, &operationId, &action](const auto &response) {
      emit operationActionFinished(operationId, action, response);
    }, reason);
}

bool AiOperationPresentation::requestResume(const QString &operationId, QString *reason) {
  const auto action = QStringLiteral("resume");
  Guard guard;
  const auto scope = beginAction(operationId, action, guard, reason);
  if (scope.isEmpty())
    return false;
  const QPointer<AiOperationPresentation> self(this);
  const auto report = [this, operationId, action](const QJsonObject &response) {
    emit operationActionFinished(operationId, action, response);
  };
  QString gate;
  auto *task = m_deps.host->recovery()->start(
    scope, operationId, this, QStringLiteral("Resume operation"),
    [self, guard, action, operationId, report](paleo::capability::ExecuteResult result) {
      if (!self)
        return;
      const auto response = QJsonDocument::fromJson(result.responseJson).object();
      self->finishAction(operationId, action, guard,
                         response.isEmpty() ? operationError(result.code) : response, report);
    }, true, &gate);
  if (!task && self) {
    if (gate.isEmpty())
      gate = QStringLiteral("operation_resume_unavailable");
    if (reason)
      *reason = gate;
    finishAction(operationId, action, guard, operationError(gate), report);
  }
  return task != nullptr;
}

} // namespace paleo::chat
