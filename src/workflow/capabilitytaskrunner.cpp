// 层：功能
#include "capabilitytaskrunner.h"

#include "../services/paleotaskservice.h"
#include <QPointer>
#include <QThread>

#include <memory>
#include <utility>

namespace paleo::capability {
namespace {

QString taskRejection(PaleoTaskService *tasks, QObject *owner, const ExecuteRequest &request) {
  if (!tasks || !owner || tasks->thread() != QThread::currentThread() ||
      owner->thread() != tasks->thread())
    return QStringLiteral("task_service_unavailable");
  const QString &intent = request.publishIntent;
  if (intent == QLatin1String("publish"))
    return QStringLiteral("publish_not_allowed");
  if (intent == QLatin1String("prepare")) {
    const QString id = CapabilityRegistry::resolveAlias(request.capabilityId, nullptr);
    if (id != QLatin1String("constraint.idw") &&
        !(id == QLatin1String("seismic.attributes.compute") && request.seismicTrace))
      return QStringLiteral("prepare_not_supported");
  }
  if (!intent.isEmpty() && intent != QLatin1String("prepare") && intent != QLatin1String("compute-only"))
    return QStringLiteral("unsupported_intent");
  return {};
}

ExecuteResult terminalWithoutResult(const ExecuteResult &previous, const QString &code,
                                    const QString &execution) {
  ExecuteResult result;
  result.operationId = previous.operationId;
  result.code = code;
  result.execution = previous.ok && !previous.execution.isEmpty() ? previous.execution : execution;
  result.commitState = previous.commitState.isEmpty() ? QStringLiteral("not_requested") : previous.commitState;
  return result;
}

} // namespace

PaleoTask *startCapabilityTask(PaleoTaskService *tasks, QObject *completionContext,
                              const QString &title, ExecuteRequest request,
                              std::function<void(ExecuteResult)> finished, QString *rejectionCode) {
  const QString rejected = taskRejection(tasks, completionContext, request);
  if (rejectionCode)
    *rejectionCode = rejected;
  if (!rejected.isEmpty())
    return nullptr;

  request.catalog = nullptr;
  request.onRunning = {};
  request.cancelled = {};
  const quint64 session = tasks->session();
  const QPointer<PaleoTaskService> service(tasks);
  const auto output = std::make_shared<ExecuteResult>();
  PaleoTask *task = tasks->start(title, [request = std::move(request), output](PaleoTask *running) mutable {
    request.cancelled = [running] { return running->cancelRequested(); };
    *output = execute(request);
    return output->ok ? QString() : output->code;
  });
  if (!task) {
    if (rejectionCode)
      *rejectionCode = QStringLiteral("task_service_unavailable");
    return nullptr;
  }
  QObject::connect(task, &PaleoTask::finished, completionContext,
                   [service, task, session, output, finished = std::move(finished)] {
    if (!finished)
      return;
    if (!service || session != service->session()) {
      finished(terminalWithoutResult(*output, QStringLiteral("stale_session"), QStringLiteral("cancelled")));
      return;
    }
    // A late cancellation can arrive after the gateway recorded an outcome.
    if (task->state() == PaleoTask::State::Cancelled && output->code.isEmpty()) {
      finished(terminalWithoutResult(*output, QStringLiteral("cancelled"), QStringLiteral("cancelled")));
      return;
    }
    if (task->state() == PaleoTask::State::Failed && output->code.isEmpty()) {
      finished(terminalWithoutResult(*output, QStringLiteral("task_failed"), QStringLiteral("failed")));
      return;
    }
    finished(std::move(*output));
  });
  return task;
}

} // namespace paleo::capability
