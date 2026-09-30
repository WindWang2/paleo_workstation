// tests/tst_importqueue_progress — B2（wave/deepen-perf）：文件夹导入真进度条。
//   · 队列 ETA 纯函数（无样本不编数字）；
//   · Running 态取消经 cancelHook 协作取消底层任务（取消 ≠ 失败，不重试）；
//   · 面板整体进度/ETA/全部取消面；
//   · FolderImportQueueAdapter：串行驱动、完成回填、失败自动重试、取消全链。
#include <QtTest>
#include <QApplication>
#include <QAtomicInt>
#include <QProgressBar>
#include <QPushButton>
#include <QSignalSpy>
#include <QThread>

#include "../src/ui/pages/dataopsimportui.h"
#include "../src/ui/pages/dataops/dataopsimportqueue.h"
#include "../src/services/paleotaskservice.h"

using namespace paleo::dataops;

namespace
{
// 等到队列收敛（无 pending）或超时。
bool waitForSettled(ImportQueuePanel *panel, int ms)
{
  QDeadlineTimer t(ms);
  while (panel->queue().hasPending() && !t.hasExpired())
    QApplication::processEvents(QEventLoop::AllEvents, 20);
  QApplication::processEvents(QEventLoop::AllEvents, 20);
  return !panel->queue().hasPending();
}
} // namespace

class TestImportQueueProgress : public QObject
{
  Q_OBJECT

  private slots:

    // ---- ETA 纯函数：诚实口径 -----------------------------------------------
    void etaHelpersDoNotInventNumbers()
    {
      QCOMPARE(estimateRemainingMs(0, 5, 1000), qint64(-1)); // 无完成样本
      QCOMPARE(estimateRemainingMs(2, 0, 1000), qint64(-1)); // 无剩余
      QCOMPARE(estimateRemainingMs(2, 0, 0), qint64(-1));
      // 2 项 1s → 均值 500ms/项 → 剩 3 项 ≈ 1500ms。
      QCOMPARE(estimateRemainingMs(2, 3, 1000), qint64(1500));
      QCOMPARE(etaDisplayText(-1), QStringLiteral("--"));
      QVERIFY(etaDisplayText(45'000).contains(QStringLiteral("45")));
      QVERIFY(etaDisplayText(120'000).contains(QStringLiteral("2"))); // 整分
    }

    // ---- 取消钩子：Running 态协作取消 ----------------------------------------
    void cancelRunningInvokesHook()
    {
      ImportRetryQueue q;
      ImportQueueItem it;
      it.path = QStringLiteral("/tmp/a.las");
      it.state = ImportItemState::Running;
      q.enqueue(it);
      int hookCalls = 0;
      q.at(0)->cancelHook = [&hookCalls] { ++hookCalls; };
      QVERIFY(q.cancelItem(0));
      QCOMPARE(hookCalls, 1);
      QCOMPARE(q.items().at(0).state, ImportItemState::Canceled);
      // Done 态不可取消；重复取消不可再触发钩子。
      q.at(0)->state = ImportItemState::Done;
      QVERIFY(!q.cancelItem(0));
    }

    // ---- 面板：整体进度 + ETA + 全部取消可见性 -------------------------------
    void overallProgressAndEtaSurfaced()
    {
      ImportQueuePanel panel;
      // 同步 runner：当场完成（小环境行为）。
      panel.setRunner([](int index, ImportQueueItem &, ImportRetryQueue *queue) {
        queue->markRunning(index);
        queue->markDone(index, QStringLiteral("asset-%1").arg(index));
      });
      panel.enqueuePaths({QStringLiteral("/tmp/1.las"), QStringLiteral("/tmp/2.las"),
                          QStringLiteral("/tmp/3.las"), QStringLiteral("/tmp/4.las")},
                         {});
      auto *overall = panel.findChild<QProgressBar *>(QStringLiteral("importQueueOverall"));
      auto *cancelAll = panel.findChild<QPushButton *>(QStringLiteral("importQueueCancelAll"));
      auto *eta = panel.findChild<QLabel *>(QStringLiteral("importQueueEta"));
      QVERIFY(overall && cancelAll && eta);
      QCOMPARE(overall->value(), 100);
      QVERIFY(cancelAll->isHidden()); // 无 pending——全部取消收起
      QVERIFY(eta->text().contains(QStringLiteral("已完成 4")));

      // 半程态：2 完成 2 待处理 → 50% + 取消按钮在场 + ETA 无样本「--」。
      ImportQueuePanel mid;
      int done = 0;
      mid.setRunner([&done](int index, ImportQueueItem &, ImportRetryQueue *queue) {
        if (done < 2)
        {
          ++done;
          queue->markRunning(index);
          queue->markDone(index, QStringLiteral("a"));
        }
      });
      mid.enqueuePaths({QStringLiteral("/tmp/1.las"), QStringLiteral("/tmp/2.las"),
                        QStringLiteral("/tmp/3.las"), QStringLiteral("/tmp/4.las")},
                       {});
      auto *overallMid = mid.findChild<QProgressBar *>(QStringLiteral("importQueueOverall"));
      auto *cancelMid = mid.findChild<QPushButton *>(QStringLiteral("importQueueCancelAll"));
      auto *etaMid = mid.findChild<QLabel *>(QStringLiteral("importQueueEta"));
      QCOMPARE(overallMid->value(), 50);
      QVERIFY(!cancelMid->isHidden());
      QVERIFY(etaMid->text().contains(QStringLiteral("剩余 2")));
      // 完成样本已有（2 项当场同步完成）——ETA 是数值估计或 0ms 时的
      // 「--」，两者都合法；断言只锚定格式与计数，不对墙钟抖动过敏。
      QVERIFY(etaMid->text().contains(QStringLiteral("--")) ||
              etaMid->text().contains(QStringLiteral("约")));
    }

    // ---- 适配器：串行驱动 + 完成回填 ------------------------------------------
    void adapterDrivesItemsSerially()
    {
      PaleoTaskService tasks;
      FolderImportQueueAdapter adapter(nullptr, &tasks);
      QAtomicInt active{0}, maxActive{0};
      QStringList started;
      adapter.setImportFn([&active, &maxActive, &started](const QString &kind,
                                                          const QString &path,
                                                          QString *) -> QString {
        const int n = active.fetchAndAddRelaxed(1) + 1;
        int m = maxActive.loadRelaxed();
        while (n > m && !maxActive.testAndSetRelaxed(m, n))
          m = maxActive.loadRelaxed();
        started.append(path);
        QThread::msleep(40); // 模拟导入耗时（池线程）
        active.fetchAndAddRelaxed(-1);
        return QStringLiteral("id-") + QFileInfo(path).fileName() +
               QStringLiteral("-") + kind;
      });
      ImportQueuePanel panel;
      adapter.attach(&panel);
      panel.enqueuePaths({QStringLiteral("/tmp/w1.las"), QStringLiteral("/tmp/w2.las"),
                          QStringLiteral("/tmp/w3.las")},
                         {{QStringLiteral("las"), QStringLiteral("well_log")}});

      QVERIFY(waitForSettled(&panel, 30'000));
      const QVector<ImportQueueItem> items = panel.queue().items();
      QCOMPARE(items.size(), 3);
      for (const ImportQueueItem &it : items)
      {
        QCOMPARE(it.state, ImportItemState::Done);
        QVERIFY(it.resultingAssetId.startsWith(QStringLiteral("id-")));
        QCOMPARE(it.progressPercent, 100);
      }
      QCOMPARE(maxActive.loadRelaxed(), 1); // 串行：一次一项
      QCOMPARE(started.size(), 3);          // 启动序 = 队列序（无遗漏）
    }

    // ---- 适配器：失败 → 自动重试 → 成功 --------------------------------------
    void adapterFailureAutoRetries()
    {
      PaleoTaskService tasks;
      FolderImportQueueAdapter adapter(nullptr, &tasks);
      int attempts = 0;
      adapter.setImportFn([&attempts](const QString &, const QString &path,
                                      QString *error) -> QString {
        if (attempts++ == 0)
        {
          if (error)
            *error = QStringLiteral("模拟首次失败");
          return QString();
        }
        return QStringLiteral("id-retry");
      });
      ImportQueuePanel panel;
      adapter.attach(&panel);
      panel.enqueuePaths({QStringLiteral("/tmp/flaky.las")}, {});
      // RetryWait→Queued 的驱动拍是 1.5s 周期；预算 15s。
      QVERIFY(waitForSettled(&panel, 15'000));
      const QVector<ImportQueueItem> items = panel.queue().items();
      QCOMPARE(items.size(), 1);
      QCOMPARE(items.at(0).state, ImportItemState::Done);
      QCOMPARE(items.at(0).resultingAssetId, QStringLiteral("id-retry"));
      QCOMPARE(items.at(0).retryCount, 1);
    }

    // ---- 适配器 + 面板：全部取消（进行中协作取消 + 排队直接取消）---------------
    void cancelAllStopsRunningAndQueued()
    {
      PaleoTaskService tasks;
      FolderImportQueueAdapter adapter(nullptr, &tasks);
      QAtomicInt begun{0};
      adapter.setImportFn([&begun](const QString &, const QString &, QString *) -> QString {
        begun.fetchAndAddRelaxed(1);
        QThread::msleep(250); // 短耗时：取消后终态很快回来
        return QStringLiteral("id-late");
      });
      ImportQueuePanel panel;
      adapter.attach(&panel);
      panel.enqueuePaths({QStringLiteral("/tmp/x1.las"), QStringLiteral("/tmp/x2.las"),
                          QStringLiteral("/tmp/x3.las")},
                         {});
      // 等第一项进入 Running（串行：只会有一项在跑）。
      QDeadlineTimer t(5000);
      while (begun.loadRelaxed() == 0 && !t.hasExpired())
        QApplication::processEvents(QEventLoop::AllEvents, 10);
      QVERIFY(begun.loadRelaxed() >= 1);

      panel.cancelAll();
      const QVector<ImportQueueItem> snapshot = panel.queue().items();
      for (const ImportQueueItem &it : snapshot)
        QCOMPARE(it.state, ImportItemState::Canceled);

      // 收敛后取消态保持（取消 ≠ 失败：不重试、不翻 Done——即使任务实际完成）。
      QVERIFY(waitForSettled(&panel, 10'000));
      const QVector<ImportQueueItem> after = panel.queue().items();
      QCOMPARE(after.size(), 3);
      for (const ImportQueueItem &it : after)
        QCOMPARE(it.state, ImportItemState::Canceled);
      QCOMPARE(begun.loadRelaxed(), 1); // 排队项被取消后不再启动
    }
};

int main(int argc, char *argv[])
{
  if (qgetenv("QT_QPA_PLATFORM").isEmpty())
    qputenv("QT_QPA_PLATFORM", "offscreen");
  QApplication app(argc, argv);
  TestImportQueueProgress tc;
  return QTest::qExec(&tc, argc, argv);
}

#include "tst_importqueue_progress.moc"
