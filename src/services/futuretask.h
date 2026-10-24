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
  // 拥有方 watcher 已析构：错误串 functor 可能捕获了它（或其宿主），终态时
  // 不得再调用。
  auto ownerGone = std::make_shared<bool>(false);
  // 终态只由服务自持的观察者（挂在 task 下）在 future 真正结束后落定——
  // 拥有方 watcher（常挂在对话框/dock 下）先亡时只请求取消、不提前终态，
  // 否则任务页显示已结束而 worker 仍在写同一文件/图层（review M2）。
  auto *observer = new QFutureWatcher<T>(task);
  QObject::connect(observer, &QFutureWatcherBase::finished, observer,
                   &QObject::deleteLater);
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
                   [task, work, completed, ownerGone, error] {
                     *completed = true;
                     if (!task)
                       return;
                     if (work.isCanceled())
                       task->requestCancel();
                     if (*ownerGone) {
                       task->finishExternal();
                       return;
                     }
                     task->finishExternal(
                         work.isCanceled() && !task->cancellable()
                             ? QStringLiteral("后台操作未能完成")
                             : (error ? error() : QString()));
                   });
  QObject::connect(watcher, &QObject::destroyed, task,
                   [task, work, completed, ownerGone, cancellable]() mutable {
                     if (*completed || !task)
                       return;
                     if (!cancellable)
                       return; // purge 等不可取消作业：继续跑完，终态如实
                     *ownerGone = true;
                     work.cancel();
                     task->requestCancel(); // 终态待 observer 见 future 结束
                   });
  observer->setFuture(future);
  return task;
}
} // namespace paleo::tasks
