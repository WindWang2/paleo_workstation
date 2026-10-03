#include <QtTest>
#include <QLabel>
#include <QProgressBar>
#include <QSemaphore>
#include <QThread>
#include <QElapsedTimer>
#include <atomic>
#include <QTreeWidget>

#include "../src/metadata/paleoprojectstore.h"
#include "../src/services/paleotaskservice.h"
#include "../src/ui/taskpanel.h"

// PaleoTaskService (pass-2 D1/D2): worker runs on QThreadPool, byte progress is
// queued back to the GUI thread, cancel is cooperative, a layerId binding marks
// the store busy registry for the task's lifetime; TaskPanel renders rows.
class TestTaskService : public QObject
{
  Q_OBJECT
private slots:

  void runsOnWorkerAndReportsProgress()
  {
    PaleoProjectStore store;
    PaleoTaskService svc(&store);

    QSignalSpy added(&svc, &PaleoTaskService::taskAdded);
    std::atomic_bool workerRan{false};
    QThread *guiThread = QThread::currentThread();
    auto *t = svc.start(QStringLiteral("hash file"), [&](PaleoTask *task) {
      if (QThread::currentThread() == guiThread)
        return QStringLiteral("did not run on the pool");
      workerRan.store(true);
      for (int i = 1; i <= 4; ++i)
      {
        task->reportBytes(i * 250, 1000);
        QThread::msleep(5);
      }
      return QString();
    });

    QCOMPARE(added.count(), 1);
    QSignalSpy fin(t, &PaleoTask::finished);
    QVERIFY(fin.wait(5000));
    QVERIFY(workerRan.load());
    QCOMPARE(t->state(), PaleoTask::State::Succeeded);
    QCOMPARE(t->percent(), 100);
    QCOMPARE(t->bytesDone(), 1000);
    QCOMPARE(t->bytesTotal(), 1000);
    QCOMPARE(t->etaText(), QStringLiteral("--")); // finished → no ETA
  }

  void layerIdMarksBusyForTaskLifetime()
  {
    PaleoProjectStore store;
    PaleoTaskService svc(&store);
    QSemaphore gate;

    auto *t = svc.start(
        QStringLiteral("index"),
        [&gate](PaleoTask *) {
          gate.acquire(); // hold the worker until the test asserts busy
          return QString();
        },
        QStringLiteral("seismic.sgy"));

    QVERIFY(store.layerBusy(QStringLiteral("seismic.sgy")));
    QSignalSpy fin(t, &PaleoTask::finished);
    gate.release();
    QVERIFY(fin.wait(5000));
    QVERIFY(!store.layerBusy(QStringLiteral("seismic.sgy")));
  }

  void cancelIsCooperative()
  {
    PaleoTaskService svc;
    auto *t = svc.start(QStringLiteral("decode"), [](PaleoTask *task) {
      while (!task->cancelRequested())
        QThread::msleep(2);
      return QString(); // returns ok but cancelRequested wins → Cancelled
    });
    QSignalSpy fin(t, &PaleoTask::finished);
    t->requestCancel();
    QVERIFY(fin.wait(5000));
    QCOMPARE(t->state(), PaleoTask::State::Cancelled);
  }

  void workerErrorBecomesFailed()
  {
    PaleoTaskService svc;
    auto *t = svc.start(QStringLiteral("import"), [](PaleoTask *) {
      return QStringLiteral("boom");
    });
    QSignalSpy fin(t, &PaleoTask::finished);
    QVERIFY(fin.wait(5000));
    QCOMPARE(t->state(), PaleoTask::State::Failed);
    QCOMPARE(t->errorText(), QStringLiteral("boom"));
  }

  // H-2：服务析构必须先让 worker 全部退出——闭包捕获的对象（这里是堆上的
  // 计数器，生产中是 DataImportService* 等）在服务析构后立即被调用方释放。
  // 旧实现孤儿化池直接返回：worker 取消后继续访问已释放对象（ASan UAF）。
  void destroyDrainsInFlightWorkers()
  {
    auto *svc = new PaleoTaskService;
    auto *shared = new std::atomic<int>(0);
    std::atomic<int> started{0};
    constexpr int kTasks = 3;
    for (int i = 0; i < kTasks; ++i)
      svc->start(QStringLiteral("long"), [shared, &started](PaleoTask *task) {
        started.fetch_add(1);
        while (!task->cancelRequested())
          QThread::msleep(2);
        QThread::msleep(30); // 取消后仍有收尾工作，期间访问捕获对象
        shared->fetch_add(1);
        return QString();
      });
    QTRY_COMPARE_WITH_TIMEOUT(started.load(), kTasks, 5000);
    delete svc; // 析构 = 取消 + 等 worker 退出
    QCOMPARE(shared->load(), kTasks);
    delete shared; // 此后不得再有 worker 触碰它
  }

  // 主线程排空时泵事件：worker 正 BlockingQueuedConnection 回主线程时，
  // shutdown() 不得互锁到超时。
  void shutdownPumpsEventsForBlockingWorker()
  {
    PaleoTaskService svc;
    QObject mainThreadObj;
    std::atomic<bool> started{false};
    std::atomic<bool> marshalled{false};
    svc.start(QStringLiteral("marshal"), [&](PaleoTask *task) {
      started = true;
      while (!task->cancelRequested())
        QThread::msleep(2);
      QMetaObject::invokeMethod(&mainThreadObj, [&] { marshalled = true; },
                                Qt::BlockingQueuedConnection);
      return QString();
    });
    QTRY_VERIFY_WITH_TIMEOUT(started.load(), 5000);
    QElapsedTimer clock;
    clock.start();
    QVERIFY(svc.shutdown(5000));
    QVERIFY(marshalled.load());
    QVERIFY2(clock.elapsed() < 4000, "shutdown() deadlocked against a BlockingQueued worker");
  }

  void panelRendersTaskRowWithProgress()
  {
    PaleoProjectStore store;
    PaleoTaskService svc(&store);
    TaskPanel panel(&store, &svc);
    panel.show(); // isVisibleTo() semantics need a shown parent

    QSemaphore gate;
    auto *t = svc.start(
        QStringLiteral("bin D61"),
        [&gate](PaleoTask *task) {
          task->reportBytes(512, 1024);
          gate.acquire();
          return QString();
        },
        QStringLiteral("d61.tif"));

    QTRY_VERIFY_WITH_TIMEOUT(
        panel.findChild<QProgressBar *>(QStringLiteral("taskProgress")) !=
            nullptr,
        3000);
    auto *bar =
        panel.findChild<QProgressBar *>(QStringLiteral("taskProgress"));
    QTRY_COMPARE_WITH_TIMEOUT(bar->value(), 50, 3000);

    QSignalSpy fin(t, &PaleoTask::finished);
    gate.release();
    QVERIFY(fin.wait(5000));

    // Finished row keeps the row with 「完成」 in the status column.
    panel.refresh();
    auto *list = panel.findChild<QTreeWidget *>(QStringLiteral("busyList"));
    QVERIFY(list);
    QCOMPARE(list->topLevelItemCount(), 1);
    QCOMPARE(list->topLevelItem(0)->text(4), QStringLiteral("完成"));

    svc.clearFinished();
    panel.refresh();
    QCOMPARE(list->topLevelItemCount(), 0);
  }

  // #153：beginNewSession() 会话号自增、运行中任务被取消、排队未出队的
  // 任务不跑 work（旧工程任务不在新工程里开工），终态均为 Cancelled。
  void beginNewSessionCancelsRunningAndQueued()
  {
    PaleoTaskService svc(nullptr);
    svc.setMaxWorkerThreads(1);
    std::atomic_bool firstStarted{false};
    std::atomic_bool secondRan{false};
    auto *first = svc.start(QStringLiteral("old-1"), [&](PaleoTask *task) {
      firstStarted.store(true);
      QElapsedTimer c;
      c.start();
      while (!task->cancelRequested() && c.elapsed() < 3000)
        QThread::msleep(2);
      return QString();
    });
    auto *second = svc.start(QStringLiteral("old-2"), [&](PaleoTask *) {
      secondRan.store(true);
      return QString();
    });
    QTRY_VERIFY_WITH_TIMEOUT(firstStarted.load(), 3000);
    const quint64 s0 = first->session();
    QCOMPARE(second->session(), s0);
    QCOMPARE(svc.beginNewSession(), s0 + 1);
    QCOMPARE(svc.session(), s0 + 1);
    QTRY_VERIFY_WITH_TIMEOUT(!first->running() && !second->running(), 3000);
    QCOMPARE(first->state(), PaleoTask::State::Cancelled);
    QCOMPARE(second->state(), PaleoTask::State::Cancelled);
    QVERIFY2(!secondRan.load(), "queued task ran its work after the session was reset");
    auto *fresh = svc.start(QStringLiteral("new"), [](PaleoTask *) { return QString(); });
    QCOMPARE(fresh->session(), s0 + 1);
    QSignalSpy fin(fresh, &PaleoTask::finished);
    QVERIFY(fin.wait(3000));
    QCOMPARE(fresh->state(), PaleoTask::State::Succeeded);
  }

  // #164：终态非 quiet 任务按上限保留（删最旧），quiet 任务终态后自动移除；
  // 运行中任务永不删。
  void registryRetentionIsBounded()
  {
    PaleoTaskService svc(nullptr);
    svc.setRetention(/*maxFinished=*/3, /*quietLingerMs=*/0);
    QSemaphore hold;
    auto *running = svc.start(QStringLiteral("long"), [&hold](PaleoTask *) {
      hold.acquire();
      return QString();
    });
    QVector<qint64> ids;
    for (int i = 0; i < 8; ++i)
    {
      auto *t = svc.start(QStringLiteral("t%1").arg(i), [](PaleoTask *) { return QString(); });
      ids << t->id();
      QSignalSpy fin(t, &PaleoTask::finished);
      QVERIFY(fin.wait(3000));
    }
    for (int i = 0; i < 6; ++i)
    {
      auto *q = svc.start(QStringLiteral("slice"), [](PaleoTask *) { return QString(); },
                          QString(), /*quiet=*/true);
      QSignalSpy fin(q, &PaleoTask::finished);
      QVERIFY(fin.wait(3000));
    }
    QTRY_COMPARE_WITH_TIMEOUT(svc.tasks().size(), 4, 3000); // long + 最近 3 条
    QVERIFY(svc.tasks().contains(running));
    QVector<qint64> kept;
    for (PaleoTask *t : svc.tasks())
      if (t != running)
        kept << t->id();
    QCOMPARE(kept, (QVector<qint64>{ids[5], ids[6], ids[7]}));
    hold.release();
    QSignalSpy fin(running, &PaleoTask::finished);
    QVERIFY(fin.wait(3000));
    QTRY_COMPARE_WITH_TIMEOUT(svc.tasks().size(), 3, 3000);
  }
};

QTEST_MAIN(TestTaskService)
#include "tst_taskservice.moc"
