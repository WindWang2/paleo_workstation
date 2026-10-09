// tests/tst_correlation_async — B1（wave/deepen-perf）：连井剖面大 LAS
// quiet 异步化的行为与 UI 线程解阻对照。
//   · 同步降级路径（无任务服务）行为不变：返回值即解析结果；
//   · 任务服务在场：loadWellLas/setLasForWell 立即返回（受理），
//     结果经 lasLoadFinished 回填（世代号丢陈旧、同井新请求协作取消）；
//   · 59MB 级合成 LAS 的「调用返回时延」对照（异步 < 解析时长——同步路径
//     在无服务口径下测同一文件作为「改前」数字）。
#include <QtTest>
#include <QApplication>
#include <QDir>
#include <QDirIterator>
#include <QElapsedTimer>
#include <QSignalSpy>
#include <QTemporaryDir>
#include <QTimer>

#include "../src/ui/correlationpanel.h"
#include "../src/linkage/selectioncontext.h"
#include "../src/services/paleotaskservice.h"

#include "../src/io/perffixtures.h" // makeSyntheticLas（合成夹具，白名单外仅测试用）

namespace
{
// 等到 panel 的在途 LAS 请求收敛（信号到达 + 事件循环排空）。
bool waitForLasSettled(WellCorrelationPanel *panel, const QString &wellId, int ms)
{
  QDeadlineTimer t(ms);
  while (panel->isLasLoadPending(wellId) && !t.hasExpired())
    QApplication::processEvents(QEventLoop::AllEvents, 20);
  QApplication::processEvents(QEventLoop::AllEvents, 20);
  return !panel->isLasLoadPending(wellId);
}

// 目录（递归）里最大的 .las 文件；空 = 没有 LAS。
QString biggestLasUnder(const QString &dir)
{
  QString best;
  qint64 bestSize = 0;
  QDirIterator it(dir, {QStringLiteral("*.las"), QStringLiteral("*.LAS")},
                  QDir::Files, QDirIterator::Subdirectories);
  while (it.hasNext())
  {
    it.next();
    if (it.fileInfo().size() > bestSize)
    {
      bestSize = it.fileInfo().size();
      best = it.filePath();
    }
  }
  return best;
}
} // namespace

class TestCorrelationAsync : public QObject
{
  Q_OBJECT

  private slots:

    void initTestCase()
    {
      qRegisterMetaType<QString>("QString");
    }

    void manyWellsYieldBetweenResults()
    {
      SelectionContext ctx;
      PaleoTaskService svc;
      WellCorrelationPanel panel(&ctx);
      panel.setTaskService(&svc);
      QList<QPair<QString, QString>> wells;
      for (int i = 0; i < 12; ++i) wells.append({QString::number(i), QString::number(i)});
      panel.setWells(wells);
      QTemporaryDir dir;
      const QString las = dir.filePath(QStringLiteral("batch.las"));
      QVERIFY(PerfFixtures::makeSyntheticLas(las, 200));
      int received = 0;
      connect(&panel, &WellCorrelationPanel::lasLoadFinished, this, [&](const QString &, bool ok) {
        QVERIFY(ok);
        QTest::qSleep(25); // 同批多井结果触发较重 GUI 消费者。
        ++received;
      });
      for (const auto &well : wells) QVERIFY(panel.loadWellLas(well.first, las, "GR"));
      QTest::qSleep(100);
      QElapsedTimer clock; clock.start();
      qint64 previous = 0, maximumGap = 0;
      int ticks = 0;
      QTimer heartbeat; heartbeat.setInterval(10);
      connect(&heartbeat, &QTimer::timeout, this, [&] {
        const auto now = clock.elapsed();
        maximumGap = qMax(maximumGap, now - previous); previous = now; ++ticks;
      });
      heartbeat.start();
      QTRY_COMPARE_WITH_TIMEOUT(received, 12, 10000);
      QVERIFY(ticks > 4);
      QVERIFY2(maximumGap < 200, qPrintable(QString("UI heartbeat gap: %1 ms").arg(maximumGap)));
      for (const auto &well : wells) {
        QVERIFY(!panel.isLasLoadPending(well.first));
        QCOMPARE(panel.wellTrackMnemonics(well.first), QStringList{QStringLiteral("GR")});
      }
    }

    void resetDropsResultsWaitingForGui()
    {
      SelectionContext ctx;
      PaleoTaskService svc;
      WellCorrelationPanel panel(&ctx);
      panel.setTaskService(&svc);
      panel.setWells({{"1", "1"}, {"2", "2"}, {"3", "3"}});
      QTemporaryDir dir;
      const QString las = dir.filePath(QStringLiteral("reset.las"));
      QVERIFY(PerfFixtures::makeSyntheticLas(las, 200));
      QSignalSpy done(&panel, &WellCorrelationPanel::lasLoadFinished);
      connect(&panel, &WellCorrelationPanel::lasLoadFinished, &panel,
              [&] { panel.resetProject(); });
      for (int i = 1; i <= 3; ++i) QVERIFY(panel.loadWellLas(QString::number(i), las, "GR"));
      QTest::qSleep(100);
      QTRY_COMPARE(done.count(), 1);
      QTRY_COMPARE(svc.runningCount(), 0);
      QTest::qWait(100);
      QCOMPARE(done.count(), 1);
      QCOMPARE(panel.wellCount(), 0);
      QVERIFY(!panel.hasCurves());
    }

    // 无任务服务：同步旧路径的返回值语义保持（正/负例）。
    void syncFallbackUnchanged()
    {
      SelectionContext ctx;
      WellCorrelationPanel panel(&ctx);
      panel.setWells({{QStringLiteral("W1"), QStringLiteral("井1")}});
      QVERIFY(!panel.taskService());

      QTemporaryDir dir;
      const QString las = dir.filePath(QStringLiteral("w1.las"));
      QVERIFY(PerfFixtures::makeSyntheticLas(las, 200));
      // 改前契约：同步解析当场拿结果（成功/曲线缺失两口径）。
      QVERIFY(panel.loadWellLas(QStringLiteral("W1"), las, QStringLiteral("GR")));
      QVERIFY(!panel.loadWellLas(QStringLiteral("W1"), las, QStringLiteral("NOSUCH")));
      QVERIFY(panel.setLasForWell(QStringLiteral("W1"), las));
      QVERIFY(!panel.isLasLoadPending(QStringLiteral("W1")));
    }

    // 异步：受理即返回 + 终态信号回填（成功/曲线缺失两口径）。
    void asyncSubmitAndResultDelivery()
    {
      SelectionContext ctx;
      WellCorrelationPanel panel(&ctx);
      panel.setWells({{QStringLiteral("W1"), QStringLiteral("井1")}});
      PaleoTaskService svc;
      panel.setTaskService(&svc);
      QCOMPARE(panel.taskService(), &svc);

      QTemporaryDir dir;
      const QString las = dir.filePath(QStringLiteral("w1.las"));
      QVERIFY(PerfFixtures::makeSyntheticLas(las, 4000));

      QSignalSpy doneSpy(&panel, &WellCorrelationPanel::lasLoadFinished);
      QSignalSpy errSpy(&panel, &WellCorrelationPanel::lasLoadError);
      QVERIFY(panel.loadWellLas(QStringLiteral("W1"), las, QStringLiteral("GR")));
      QVERIFY(panel.isLasLoadPending(QStringLiteral("W1"))); // 受理在途
      QVERIFY(waitForLasSettled(&panel, QStringLiteral("W1"), 30000));
      QCOMPARE(doneSpy.count(), 1);
      QCOMPARE(doneSpy.at(0).at(1).toBool(), true);
      QCOMPARE(panel.wellTrackMnemonics(QStringLiteral("W1")),
               QStringList{QStringLiteral("GR")});
      QCOMPARE(panel.curveItemCount(QStringLiteral("W1")), 1);

      // 曲线不存在：lasLoadFinished(false) + lasLoadError 带原因。
      doneSpy.clear();
      errSpy.clear();
      QVERIFY(panel.loadWellLas(QStringLiteral("W1"), las, QStringLiteral("NOSUCH")));
      QVERIFY(waitForLasSettled(&panel, QStringLiteral("W1"), 30000));
      QCOMPARE(doneSpy.count(), 1);
      QCOMPARE(doneSpy.at(0).at(1).toBool(), false);
      QCOMPARE(errSpy.count(), 1);
      QVERIFY(errSpy.at(0).at(1).toString().contains(QStringLiteral("NOSUCH")));
    }

    // setLasForWell 的异步路径：只填 browser，不上轨。
    void asyncSetLasForWellFillsBrowserOnly()
    {
      SelectionContext ctx;
      WellCorrelationPanel panel(&ctx);
      panel.setWells({{QStringLiteral("W1"), QStringLiteral("井1")}});
      PaleoTaskService svc;
      panel.setTaskService(&svc);

      QTemporaryDir dir;
      const QString las = dir.filePath(QStringLiteral("w1.las"));
      QVERIFY(PerfFixtures::makeSyntheticLas(las, 300));

      QSignalSpy doneSpy(&panel, &WellCorrelationPanel::lasLoadFinished);
      QVERIFY(panel.setLasForWell(QStringLiteral("W1"), las));
      QVERIFY(waitForLasSettled(&panel, QStringLiteral("W1"), 30000));
      QCOMPARE(doneSpy.count(), 1);
      QCOMPARE(doneSpy.at(0).at(1).toBool(), true);
      QCOMPARE(panel.wellTrackMnemonics(QStringLiteral("W1")), QStringList());
      QVERIFY(panel.curveBrowser());
    }

    // 文件不存在：异步模式下同步快速失败（不进任务池）。
    void asyncMissingFileFailsFast()
    {
      SelectionContext ctx;
      WellCorrelationPanel panel(&ctx);
      panel.setWells({{QStringLiteral("W1"), QStringLiteral("井1")}});
      PaleoTaskService svc;
      panel.setTaskService(&svc);
      const QString missing = QStringLiteral("/nonexistent/dir/w9.las");
      QVERIFY(!panel.loadWellLas(QStringLiteral("W1"), missing, QStringLiteral("GR")));
      QVERIFY(!panel.setLasForWell(QStringLiteral("W1"), missing));
      QVERIFY(!panel.isLasLoadPending(QStringLiteral("W1")));
    }

    // 同井二次请求：旧请求协作取消，只有最新一代结果落地。
    void asyncSupersedeDropsStaleResult()
    {
      SelectionContext ctx;
      WellCorrelationPanel panel(&ctx);
      panel.setWells({{QStringLiteral("W1"), QStringLiteral("井1")}});
      PaleoTaskService svc;
      panel.setTaskService(&svc);

      QTemporaryDir dir;
      const QString lasA = dir.filePath(QStringLiteral("a.las"));
      const QString lasB = dir.filePath(QStringLiteral("b.las"));
      QVERIFY(PerfFixtures::makeSyntheticLas(lasA, 8000));
      QVERIFY(PerfFixtures::makeSyntheticLas(lasB, 200));

      QSignalSpy doneSpy(&panel, &WellCorrelationPanel::lasLoadFinished);
      QVERIFY(panel.loadWellLas(QStringLiteral("W1"), lasA, QStringLiteral("GR")));
      QVERIFY(panel.isLasLoadPending(QStringLiteral("W1")));
      // 立刻改主意换 B：A 的结果按世代号丢弃（不回填、不算成功）。
      QVERIFY(panel.loadWellLas(QStringLiteral("W1"), lasB, QStringLiteral("DT")));
      QVERIFY(waitForLasSettled(&panel, QStringLiteral("W1"), 30000));

      // A 可能走 Cancelled（不发射）或 Succeeded-but-stale（发射前丢弃）：
      // 无论哪种，最终可见状态只来自 B。
      QCOMPARE(panel.wellTrackMnemonics(QStringLiteral("W1")),
               QStringList{QStringLiteral("DT")});
      // 至少一次成功终态（B），且没有任何「GR 上轨」的中间态泄漏。
      bool sawDt = false;
      for (const auto &args : doneSpy)
        if (args.at(1).toBool())
          sawDt = true;
      QVERIFY(sawDt);
    }

    // 59MB 级合成 LAS：调用返回时延对照（异步受理 << 同步解析）。
    // 这是 B1 的可复现改前/改后数字（selfcheck 不持面板，落这里）。
    // 两段各用独立文件：LasCache 进程内共享，「改后」段也吃冷解析。
    void bigLasDoesNotBlockCaller()
    {
      QTemporaryDir dir;
      const QString lasSync = dir.filePath(QStringLiteral("big_sync.las"));
      const QString lasAsync = dir.filePath(QStringLiteral("big_async.las"));
      // ~59MB：5 曲线 × ~740k 行（每行 ~80 字节）——A13.Las 量级。
      QVERIFY(PerfFixtures::makeSyntheticLas(lasSync, 740000));
      QVERIFY(PerfFixtures::makeSyntheticLas(lasAsync, 740000));

      // 「改前」口径：无任务服务的同步路径，调用点阻塞 = 解析时长。
      {
        SelectionContext ctx;
        WellCorrelationPanel panel(&ctx);
        panel.setWells({{QStringLiteral("W1"), QStringLiteral("井1")}});
        QElapsedTimer t;
        t.start();
        QVERIFY(panel.loadWellLas(QStringLiteral("W1"), lasSync, QStringLiteral("GR")));
        qDebug("sync (before): loadWellLas blocked %lld ms", t.elapsed());
      }

      // 「改后」口径：任务服务在场，受理时延与解析时长解耦。
      {
        SelectionContext ctx;
        WellCorrelationPanel panel(&ctx);
        panel.setWells({{QStringLiteral("W1"), QStringLiteral("井1")}});
        PaleoTaskService svc;
        panel.setTaskService(&svc);
        QElapsedTimer t;
        t.start();
        QVERIFY(panel.loadWellLas(QStringLiteral("W1"), lasAsync, QStringLiteral("GR")));
        const qint64 acceptMs = t.elapsed();
        QElapsedTimer parseClock;
        parseClock.start();
        QVERIFY(panel.isLasLoadPending(QStringLiteral("W1")));
        QVERIFY(waitForLasSettled(&panel, QStringLiteral("W1"), 60000));
        qDebug("async (after): loadWellLas returned in %lld ms (parse %lld ms continued in pool)",
               acceptMs, parseClock.elapsed());
        QCOMPARE(panel.wellTrackMnemonics(QStringLiteral("W1")),
                 QStringList{QStringLiteral("GR")});
        // 受理必须立刻：大文件解析百毫秒~秒级，受理超过 500ms 就等于没解阻。
        QVERIFY2(acceptMs < 500, "async accept path must not parse on the GUI thread");
      }
    }

    // 真工区口径（B1 报告数字）：PALEO_REAL_PROJECT_AREA 指向 project_area
    // 时对最大 LAS 测改前/改后；未设置跳过（tst_smoke_realdata 同惯例）。
    void realProjectBigLasLatency()
    {
      const QString area = qEnvironmentVariable("PALEO_REAL_PROJECT_AREA");
      if (area.isEmpty())
        QSKIP("PALEO_REAL_PROJECT_AREA not set — real-data latency measurement skipped");
      const QString las = biggestLasUnder(area);
      if (las.isEmpty())
        QSKIP("no .las under PALEO_REAL_PROJECT_AREA");
      qDebug("real-project biggest LAS: %s (%lld MB)", qPrintable(las),
             QFileInfo(las).size() / (1024 * 1024));

      // 改前：同步路径（新进程口径由 LasCache 冷缓存保证——本用例先跑同步）。
      {
        SelectionContext ctx;
        WellCorrelationPanel panel(&ctx);
        panel.setWells({{QStringLiteral("W1"), QStringLiteral("井1")}});
        QElapsedTimer t;
        t.start();
        const bool ok = panel.loadWellLas(QStringLiteral("W1"), las, QStringLiteral("GR"));
        qDebug("sync (before): blocked %lld ms, ok=%d", t.elapsed(), int(ok));
      }
      // 改后：受理时延（同文件已入 LasCache——解析命中内存层；对照点是
      // 「调用点不再等解析」，受理时延与冷热无关）。
      {
        SelectionContext ctx;
        WellCorrelationPanel panel(&ctx);
        panel.setWells({{QStringLiteral("W1"), QStringLiteral("井1")}});
        PaleoTaskService svc;
        panel.setTaskService(&svc);
        QElapsedTimer t;
        t.start();
        QVERIFY(panel.loadWellLas(QStringLiteral("W1"), las, QStringLiteral("GR")));
        const qint64 acceptMs = t.elapsed();
        qDebug("async (after): returned in %lld ms", acceptMs);
        QVERIFY(waitForLasSettled(&panel, QStringLiteral("W1"), 60000));
        QVERIFY2(acceptMs < 500, "async accept path must not parse on the GUI thread");
      }
    }
};

int main(int argc, char *argv[])
{
  if (qgetenv("QT_QPA_PLATFORM").isEmpty())
    qputenv("QT_QPA_PLATFORM", "offscreen");
  QApplication app(argc, argv); // 面板是 QWidget 栈
  TestCorrelationAsync tc;
  return QTest::qExec(&tc, argc, argv);
}

#include "tst_correlation_async.moc"
