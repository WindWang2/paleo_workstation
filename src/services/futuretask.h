// 层：数据
#pragma once
#include "paleotaskservice.h"
#include <QFutureWatcher>
#include <QPointer>
#include <QThreadPool>
#include <memory>

// Existing promise-based jobs use the same bounded pool, task registry and
// session cancellation as start(). The watcher remains the result owner.
namespace paleo::tasks {
inline QThreadPool *pool(PaleoTaskService *service) {
  return service ? service->workerPool() : QThreadPool::globalInstance();
}
template <class T>
PaleoTask *track(PaleoTaskService *service, QFutureWatcher<T> *watcher,
                 const QFuture<T> &future, const QString &title,
                 bool quiet = false, std::function<QString()> error = {},
                 bool cancellable = true) {
  if (!service)
    return nullptr;
  QPointer<PaleoTask> task = service->beginExternal(title, {}, quiet);
  task->setCancellable(cancellable);
  QFuture<T> work = future;
  auto completed = std::make_shared<bool>(false);
  // Purge continues after a dialog/controller closes. A service-owned watcher
  // keeps the task's terminal state truthful without retaining the UI owner.
  auto *observer = cancellable ? watcher : new QFutureWatcher<T>(task);
  if (!cancellable) {
    QObject::connect(observer, &QFutureWatcherBase::finished, observer,
                     &QObject::deleteLater);
  }
  QObject::connect(task, &PaleoTask::cancelAsked, watcher,
                   [work]() mutable { work.cancel(); });
  QObject::connect(observer, &QFutureWatcherBase::progressValueChanged, task,
                   [task, observer](int done) {
                     if (!task)
                       return;
                     task->reportBytes(done, observer->progressMaximum());
                     task->reportDetail(observer->progressText());
                   });
  QObject::connect(observer, &QFutureWatcherBase::finished, task,
                   [task, work, completed, error] {
                     *completed = true;
                     if (!task)
                       return;
                     if (work.isCanceled())
                       task->requestCancel();
                     task->finishExternal(
                         work.isCanceled() && !task->cancellable()
                             ? QStringLiteral("后台操作未能完成")
                             : (error ? error() : QString()));
                   });
  QObject::connect(watcher, &QObject::destroyed, task,
                   [task, work, completed, cancellable]() mutable {
                     if (*completed || !task || !cancellable)
                       return;
                     work.cancel();
                     task->requestCancel();
                     task->finishExternal();
                   });
  if (!cancellable)
    observer->setFuture(future);
  return task;
}
} // namespace paleo::tasks
