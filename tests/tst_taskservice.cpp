#include <QtTest>
#include <QLabel>
#include <QProgressBar>
#include <QSemaphore>
#include <QThread>
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
};

QTEST_MAIN(TestTaskService)
#include "tst_taskservice.moc"
