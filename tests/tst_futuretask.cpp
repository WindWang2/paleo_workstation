// tst_futuretask — futuretask.h（paleo::tasks::track）专测（#479 测试盲区）：
// promise 型后台作业经统一池/注册表/取消通道挂账的契约——成功/错误串回填、
// 任务侧取消传导到 QFuture、拥有方 watcher 先亡的终态闭环、非可取消路线的
// task 自持观察者、pool() 的选池语义。
#include <QtTest>
#include <QElapsedTimer>
#include <atomic>
#include <memory>
#include <QFutureWatcher>
#include <QThread>
#include <QThreadPool>
#include <QtConcurrent>

#include "../src/services/futuretask.h"
#include "perfbudget_relax.h"

class TestFutureTask : public QObject
{
  Q_OBJECT
private slots:
  void trackSucceedsAndReportsTask();
  void errorFunctorBecomesFailureText();
  void taskCancelCancelsTrackedFuture();
  void watcherDestroyedFinalizesCancellableTask();
  void nonCancellableTrackSurvivesWatcherDeath();
  void poolSelectsServicePool();
};

void TestFutureTask::trackSucceedsAndReportsTask()
{
  PaleoTaskService svc;
  auto *watcher = new QFutureWatcher<int>;
  const QFuture<int> future =
      QtConcurrent::run(paleo::tasks::pool(&svc), [] { return 42; });
  PaleoTask *task = paleo::tasks::track(&svc, watcher, future,
                                        QStringLiteral("answer"));
  QVERIFY(task);
  QCOMPARE(task->title(), QStringLiteral("answer"));
  QVERIFY(task->cancellable());
  QVERIFY(svc.tasks().contains(task)); // 挂进注册表，任务页可见
  watcher->setFuture(future);
  QSignalSpy fin(task, &PaleoTask::finished);
  QVERIFY(fin.wait(5000));
  QCOMPARE(task->state(), PaleoTask::State::Succeeded);
  QCOMPARE(task->errorText(), QString());
  QCOMPARE(watcher->result(), 42);
  delete watcher;
}

void TestFutureTask::errorFunctorBecomesFailureText()
{
  PaleoTaskService svc;
  auto *watcher = new QFutureWatcher<int>;
  const QFuture<int> future =
      QtConcurrent::run(paleo::tasks::pool(&svc), [] { return -1; });
  PaleoTask *task = paleo::tasks::track(
      &svc, watcher, future, QStringLiteral("boom"), true,
      [] { return QStringLiteral("load failed: boom.txt"); });
  watcher->setFuture(future);
  QSignalSpy fin(task, &PaleoTask::finished);
  QVERIFY(fin.wait(5000));
  QCOMPARE(task->state(), PaleoTask::State::Failed);
  QCOMPARE(task->errorText(), QStringLiteral("load failed: boom.txt"));
  delete watcher;
}

void TestFutureTask::taskCancelCancelsTrackedFuture()
{
  PaleoTaskService svc;
  auto *watcher = new QFutureWatcher<int>;
  const QFuture<int> future = QtConcurrent::run(paleo::tasks::pool(&svc), [] {
    QThread::msleep(600); // 保证取消先于自然完成到达
    return 1;
  });
  PaleoTask *task = paleo::tasks::track(&svc, watcher, future,
                                        QStringLiteral("long"));
  watcher->setFuture(future);
  QSignalSpy fin(task, &PaleoTask::finished);
  task->requestCancel(); // cancelAsked → future.cancel() → 终态 Cancelled
  QVERIFY(fin.wait(5000));
  QCOMPARE(task->state(), PaleoTask::State::Cancelled);
  delete watcher;
}

void TestFutureTask::watcherDestroyedFinalizesCancellableTask()
{
  PaleoTaskService svc;
  auto *watcher = new QFutureWatcher<int>;
  auto workerDone = std::make_shared<std::atomic_bool>(false);
  const QFuture<int> future =
      QtConcurrent::run(paleo::tasks::pool(&svc), [workerDone] {
        QThread::msleep(400);
        workerDone->store(true);
        return 7;
      });
  bool errorCalled = false;
  PaleoTask *task = paleo::tasks::track(
      &svc, watcher, future, QStringLiteral("orphan"), true,
      [&errorCalled] { errorCalled = true; return QStringLiteral("x"); });
  watcher->setFuture(future);
  QSignalSpy fin(task, &PaleoTask::finished);
  delete watcher; // 拥有方先走：只请求取消，worker 未退出前不得终态（review M2）
  QVERIFY(task->cancelRequested());
  QCoreApplication::processEvents();
  QVERIFY2(fin.isEmpty() && task->running(),
           "terminal state reported while the worker was still running");
  QVERIFY(fin.wait(5000));
  QVERIFY(workerDone->load()); // 终态严格晚于 worker 真正结束
  QCOMPARE(task->state(), PaleoTask::State::Cancelled);
  QVERIFY2(!errorCalled, "error functor may capture the dead owner");
}

void TestFutureTask::nonCancellableTrackSurvivesWatcherDeath()
{
  PaleoTaskService svc;
  auto *watcher = new QFutureWatcher<int>;
  const QFuture<int> future =
      QtConcurrent::run(paleo::tasks::pool(&svc), [] { return 5; });
  PaleoTask *task = paleo::tasks::track(&svc, watcher, future,
                                        QStringLiteral("purge"), true, {}, false);
  QVERIFY(!task->cancellable());
  watcher->setFuture(future);
  delete watcher; // 非可取消：观察者由 task 自持，purge 继续跑完并 Succeeded
  QSignalSpy fin(task, &PaleoTask::finished);
  QVERIFY(fin.wait(5000));
  QCOMPARE(task->state(), PaleoTask::State::Succeeded);
}

void TestFutureTask::poolSelectsServicePool()
{
  QCOMPARE(paleo::tasks::pool(nullptr), QThreadPool::globalInstance());
  PaleoTaskService svc;
  QCOMPARE(paleo::tasks::pool(&svc), svc.workerPool());
}

QTEST_GUILESS_MAIN(TestFutureTask)
#include "tst_futuretask.moc"
