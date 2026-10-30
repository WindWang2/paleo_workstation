// tst_cache_async — wave/io-perf-cache D4：任务优先级/≤4 工作池/阶段进度/
// 取消语义/请求合并/预取。
#include <QtTest>

#include "../src/io/lascache.h"
#include "../src/io/perffixtures.h"
#include "services/paleotaskservice.h"
#include "perfbudget_relax.h"

#include <QElapsedTimer>
#include <QEventLoop>
#include <QTemporaryDir>
#include <QtConcurrent>
#include <array>
#include <atomic>
#include <thread>
#include <vector>

class CacheAsyncTests : public QObject
{
    Q_OBJECT

  private slots:
    void dedicatedPoolCapsConcurrencyByCores();
    void priorityHighBeatsLow();
    void stageProgressEvents();
    void cancelReachesCooperativeWorker();
    void cancelStateAndNoDangling();
    void coalescedConcurrentLasLoad();
    void prefetchPopulatesCache();
    void lowPriorityDoesNotBlockHigh();

  private:
    QTemporaryDir m_dir;
    void spinUntil(PaleoTask *task, int timeoutMs);
};

void CacheAsyncTests::spinUntil(PaleoTask *task, int timeoutMs)
{
  QElapsedTimer t;
  t.start();
  while (task->running() && t.elapsed() < timeoutMs)
    QCoreApplication::processEvents(QEventLoop::AllEvents, 20);
}

void CacheAsyncTests::dedicatedPoolCapsConcurrencyByCores()
{
  // D4.5：20 个阻塞任务同时提交——观测并发 ≤ 池上限。池上限按核数取
  // qBound(4, idealThreadCount, 8)：须落在 [4,8] 且与本机核数关系一致。
  PaleoTaskService svc;
  const int cap = svc.maxWorkerThreads();
  QVERIFY2(cap >= 4 && cap <= 8, qPrintable(QStringLiteral("cap=%1 不在 [4,8]").arg(cap)));
  QCOMPARE(cap, qBound(4, QThread::idealThreadCount(), 8));
  std::atomic_int running{0};
  std::atomic_int peak{0};
  std::atomic_int completed{0};
  QVector<PaleoTask *> tasks;
  for (int i = 0; i < 20; ++i)
  {
    tasks.append(svc.start(QStringLiteral("block%1").arg(i), [&](PaleoTask *) -> QString {
      const int now = running.fetch_add(1) + 1;
      int expect = peak.load();
      while (now > expect && !peak.compare_exchange_weak(expect, now))
      {
      }
      QThread::msleep(40);
      running.fetch_sub(1);
      completed.fetch_add(1);
      return QString();
    }));
  }
  for (PaleoTask *t : tasks)
    spinUntil(t, 10000);
  QCoreApplication::processEvents();
  QVERIFY2(peak.load() <= cap, qPrintable(QStringLiteral("peak=%1 > cap=%2").arg(peak.load()).arg(cap)));
  // 并发下限是主机属性（真单核/受限容器串行执行 peak==1 也是合法），池契约
  // 由上面的 cap 断言钉住——观测到的峰值只断言上界。
  QCOMPARE(completed.load(), 20);
  QCOMPARE(svc.runningCount(), 0);
}

void CacheAsyncTests::priorityHighBeatsLow()
{
  // D4.6：先排满低优（占住 4 线程），再排高优——高优应在多数低优前开始。
  PaleoTaskService svc;
  // 池热身：跑一轮极短任务把工作线程建起来（等终态，不赌墙钟）。
  {
    QVector<PaleoTask *> warm;
    for (int i = 0; i < svc.maxWorkerThreads(); ++i)
      warm.append(svc.start(QStringLiteral("warm%1").arg(i),
                            [](PaleoTask *) -> QString { return QString(); }));
    for (PaleoTask *t : warm)
      spinUntil(t, 5000);
  }

  // 低优数 = 2×池上限：前一半占满全部工作线程，后一半留在队列里等高优插队
  // （池上限随核数在 [4,8]，固定 8 个在 8 线程池上会全部立即开跑、无从插队）。
  constexpr int kMaxLowCount = 16;
  const int kLowCount = 2 * svc.maxWorkerThreads();
  QVERIFY(kLowCount <= kMaxLowCount);
  std::atomic_int started{0};
  std::atomic_int highOrder{-1};
  std::atomic_int orderSeq{0};
  std::atomic_bool releaseLows{false};
  std::array<std::atomic_int, kMaxLowCount> lowOrders;
  for (auto &o : lowOrders)
    o.store(-1);
  QVector<PaleoTask *> lows;
  for (int i = 0; i < kLowCount; ++i)
    lows.append(svc.start(QStringLiteral("low%1").arg(i), [&, i](PaleoTask *) -> QString {
      lowOrders[i].store(orderSeq.fetch_add(1)); // 每个 low 都记录自己的开始序号
      started.fetch_add(1);
      while (!releaseLows.load()) // 占住线程直到高优入队——顺序不再赌墙钟
        QThread::msleep(1);
      return QString();
    }, QString(), PaleoTask::Priority::Low));
  // 等首批低优把全部工作线程占满再投高优：队列里还剩 cap 个低优，
  // 高优必然插到它们前面，lowsAfterHigh 恒为 cap 不再抖动。
  QElapsedTimer gate;
  gate.start();
  while (started.load() < svc.maxWorkerThreads() && gate.elapsed() < paleo::perfbudget::relaxedBudgetMs(5000))
    QThread::msleep(1);
  QVERIFY2(started.load() >= svc.maxWorkerThreads(), "低优未占满线程池");
  PaleoTask *high = svc.start(QStringLiteral("high"), [&](PaleoTask *) -> QString {
    highOrder.store(orderSeq.fetch_add(1));
    QThread::msleep(10);
    return QString();
  }, QString(), PaleoTask::Priority::High);
  releaseLows.store(true);

  spinUntil(high, 10000);
  for (PaleoTask *l : lows)
    spinUntil(l, 10000);
  QCoreApplication::processEvents();
  // 高优任务先于至少一半低优开始。
  QVERIFY2(highOrder.load() >= 0, "高优序号未记录");
  int lowsAfterHigh = 0;
  for (int i = 0; i < kLowCount; ++i)
  {
    const auto &o = lowOrders[i];
    QVERIFY2(o.load() >= 0, "低优序号未记录");
    if (o.load() > highOrder.load())
      ++lowsAfterHigh;
  }
  QVERIFY2(lowsAfterHigh >= kLowCount / 2,
           qPrintable(QStringLiteral("high=%1 只有 %2/%3 个低优在其后开始 —— 高优被低优饿死")
                          .arg(highOrder.load()).arg(lowsAfterHigh).arg(kLowCount)));
}

void CacheAsyncTests::stageProgressEvents()
{
  // D4.3：阶段化进度经队列回到任务对象——stage/stagePercent 可读。
  PaleoTaskService svc;
  PaleoTask *task = svc.start(QStringLiteral("staged"), [](PaleoTask *t) -> QString {
    t->reportStage(QStringLiteral("scan"), 10);
    t->reportStage(QStringLiteral("parse"), 50);
    t->reportStage(QStringLiteral("publish"), 100);
    return QString();
  });
  spinUntil(task, 5000);
  QCoreApplication::processEvents();
  QCOMPARE(task->state(), PaleoTask::State::Succeeded);
  QCOMPARE(task->stage(), QStringLiteral("publish"));
  QCOMPARE(task->stagePercent(), 100);
  QVERIFY(task->isFinished());
}

void CacheAsyncTests::cancelReachesCooperativeWorker()
{
  PaleoTaskService svc;
  std::atomic_bool sawCancel{false};
  PaleoTask *task = svc.start(QStringLiteral("cancellable"), [&](PaleoTask *t) -> QString {
    for (int i = 0; i < 100 && !t->cancelRequested(); ++i)
      QThread::msleep(20);
    sawCancel.store(t->cancelRequested());
    return t->cancelRequested() ? QString() : QStringLiteral("not cancelled");
  });
  QThread::msleep(60);
  task->requestCancel();
  spinUntil(task, 5000);
  QCoreApplication::processEvents();
  QVERIFY(sawCancel.load());
  QCOMPARE(task->state(), PaleoTask::State::Cancelled);
}

void CacheAsyncTests::cancelStateAndNoDangling()
{
  // D4.4：取消后无悬挂——服务任务表仍完整持有、终态可达、可清理。
  PaleoTaskService svc;
  const int before = svc.tasks().size();
  PaleoTask *task = svc.start(QStringLiteral("dangling"), [](PaleoTask *t) -> QString {
    while (!t->cancelRequested())
      QThread::msleep(10);
    return QString();
  });
  QThread::msleep(50);
  task->requestCancel();
  spinUntil(task, 5000);
  QCoreApplication::processEvents();
  QCOMPARE(svc.tasks().size(), before + 1);
  QVERIFY(!task->running());
  svc.clearFinished();
  QCOMPARE(svc.tasks().size(), before);
}

void CacheAsyncTests::coalescedConcurrentLasLoad()
{
  // D4.7：并发冷加载同一文件 → 合并器只跑一份解析。
  const QString las = m_dir.filePath("coalesce.las");
  QVERIFY(PerfFixtures::makeSyntheticLas(las, 4000));
  LasCache::shared().setDiskRoot(QString()); // 纯内存——保证走到冷解析
  LasCache::shared().invalidate();

  std::atomic_int parses{0};
  const int riderBase = LasCache::shared().ridersJoinedForTest();
  // 冷解析钩子等齐其余 5 个提交者搭车再返回：其余线程必然都进在途集，
  // 合并计数与线程池容量/启动时差脱钩（10s 兜底防挂死）。
  LasCache::shared().setColdParseHookForTest([&parses, riderBase] {
    parses.fetch_add(1);
    QElapsedTimer t;
    t.start();
    while (LasCache::shared().ridersJoinedForTest() - riderBase < 5 &&
           t.elapsed() < paleo::perfbudget::relaxedBudgetMs(10000))
      QThread::msleep(5);
  });
  // 6 个真实线程同时冷加载——显式线程不受 QtConcurrent 池上限影响，
  // 小核数 CI runner 上也能保证 5 个搭车者全部在途。
  std::vector<std::thread> threads;
  threads.reserve(6);
  for (int i = 0; i < 6; ++i)
    threads.emplace_back([&las] { LasCache::shared().load(las); });
  for (auto &t : threads)
    t.join();
  LasCache::shared().setColdParseHookForTest(nullptr);
  // 全部线程拿到结果（条目在缓存）。
  QVERIFY(LasCache::shared().isCached(las));
  // 合并语义：6 并发只触发一次真实解析，其余搭车。
  QCOMPARE(parses.load(), 1);
  QVERIFY(LasCache::shared().ridersJoinedForTest() - riderBase >= 5);
}

void CacheAsyncTests::prefetchPopulatesCache()
{
  // D1.3：批量预取把文件装进缓存（同步口径验证缓存态；调度在任务服务测）。
  QStringList paths;
  for (int i = 0; i < 4; ++i)
  {
    const QString p = m_dir.filePath(QStringLiteral("pre%1.las").arg(i));
    QVERIFY(PerfFixtures::makeSyntheticLas(p, 300));
    paths << p;
  }
  LasCache::shared().setDiskRoot(QString()); // 纯内存
  LasCache::shared().invalidate();
  QCOMPARE(LasCache::shared().prefetch(paths), 4);
  for (const QString &p : paths)
    QVERIFY(LasCache::shared().isCached(p));
}

void CacheAsyncTests::lowPriorityDoesNotBlockHigh()
{
  // D4.6 语义面：低优长任务占满池时，高优任务仍能在其之前拿到线程。
  PaleoTaskService svc;
  QVector<PaleoTask *> lows;
  for (int i = 0; i < 4; ++i)
    lows.append(svc.start(QStringLiteral("slowlow%1").arg(i), [](PaleoTask *t) -> QString {
      for (int i = 0; i < 100 && !t->cancelRequested(); ++i)
        QThread::msleep(20);
      return QString();
    }, QString(), PaleoTask::Priority::Low));
  QThread::msleep(80); // 低优已占住 4 线程
  QElapsedTimer highTimer;
  highTimer.start();
  PaleoTask *high = svc.start(QStringLiteral("fasthigh"), [](PaleoTask *) -> QString {
    return QString();
  }, QString(), PaleoTask::Priority::High);
  spinUntil(high, 8000);
  const double ms = highTimer.nsecsElapsed() / 1.0e6;
  for (PaleoTask *l : lows)
    l->requestCancel();
  for (PaleoTask *l : lows)
    spinUntil(l, 5000);
  QCoreApplication::processEvents();
  QCOMPARE(high->state(), PaleoTask::State::Succeeded);
  // Windows 2 核 runner 的线程池调度/唤醒更慢（实测 2.4s）——语义是
  // 「高优先于低优完成」而非绝对墙钟，按平台放缩预算。
  const double budgetMs =
#ifdef Q_OS_WIN
      4000.0;
#else
      2000.0;
#endif
  QVERIFY2(ms < budgetMs, qPrintable(QStringLiteral("high waited %1ms").arg(ms)));
}

QTEST_MAIN(CacheAsyncTests)
#include "tst_cache_async.moc"
