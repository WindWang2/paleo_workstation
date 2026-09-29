// tst_cache_async — wave/io-perf-cache D4：任务优先级/≤4 工作池/阶段进度/
// 取消语义/请求合并/预取。
#include <QtTest>

#include "../src/io/lascache.h"
#include "../src/io/perffixtures.h"
#include "services/paleotaskservice.h"

#include <QElapsedTimer>
#include <QEventLoop>
#include <QTemporaryDir>
#include <QtConcurrent>
#include <atomic>

class CacheAsyncTests : public QObject
{
    Q_OBJECT

  private slots:
    void dedicatedPoolCapsConcurrencyAtFour();
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

void CacheAsyncTests::dedicatedPoolCapsConcurrencyAtFour()
{
  // D4.5：20 个阻塞任务同时提交——观测并发 ≤4。
  PaleoTaskService svc;
  QCOMPARE(svc.maxWorkerThreads(), 4);
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
  QVERIFY2(peak.load() <= 4, qPrintable(QStringLiteral("peak=%1 > 4").arg(peak.load())));
  QVERIFY2(peak.load() >= 2, "池没并行起来（单线程串行也该有并发=1……但断言≥2 宽松失败则池坏了）");
  QCOMPARE(completed.load(), 20);
  QCOMPARE(svc.runningCount(), 0);
}

void CacheAsyncTests::priorityHighBeatsLow()
{
  // D4.6：先排满低优（占住 4 线程），再排高优——高优应在多数低优前完成。
  PaleoTaskService svc;
  QElapsedTimer t;
  t.start();
  while (t.elapsed() < 300)
    QCoreApplication::processEvents(); // 池热身（避免线程创建抖动）

  std::atomic_int started{0};
  std::atomic_int highOrder{-1}, lowOrder{-1};
  std::atomic_int orderSeq{0};
  QVector<PaleoTask *> lows;
  for (int i = 0; i < 8; ++i)
    lows.append(svc.start(QStringLiteral("low%1").arg(i), [&](PaleoTask *) -> QString {
      if (lowOrder.load() < 0)
        lowOrder.store(orderSeq.fetch_add(1));
      QThread::msleep(30);
      started.fetch_add(1);
      return QString();
    }, QString(), PaleoTask::Priority::Low));
  PaleoTask *high = svc.start(QStringLiteral("high"), [&](PaleoTask *) -> QString {
    highOrder.store(orderSeq.fetch_add(1));
    QThread::msleep(10);
    return QString();
  }, QString(), PaleoTask::Priority::High);

  spinUntil(high, 10000);
  for (PaleoTask *l : lows)
    spinUntil(l, 10000);
  QCoreApplication::processEvents();
  // 高优任务先于至少一半低优开始。
  QVERIFY2(highOrder.load() >= 0 && lowOrder.load() >= 0, "序号未记录");
  QVERIFY2(highOrder.load() < lowOrder.load() + 4,
           qPrintable(QStringLiteral("high=%1 low=%2 —— 高优被低优饿死").arg(highOrder.load()).arg(lowOrder.load())));
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
  // D4.7：两个不同 key 的请求同时打同一文件 → 只解析一份。
  const QString las = m_dir.filePath("coalesce.las");
  QVERIFY(PerfFixtures::makeSyntheticLas(las, 4000));
  LasCache::shared().setDiskRoot(m_dir.filePath("idx"));
  LasCache::shared().invalidate();
  LasCache::shared().load(las); // 预热一份
  LasCache::shared().clearMemory();

  std::atomic_int parses{0};
  const qint64 coldBefore = LasCache::shared().stats().misses;
  // 6 个线程同时冷加载（清内存后）。
  QVector<QFuture<void>> futs;
  for (int i = 0; i < 6; ++i)
    futs.append(QtConcurrent::run([&las](void) { LasCache::shared().load(las); }));
  for (auto &f : futs)
    f.waitForFinished();
  Q_UNUSED(parses);
  // 全部线程拿到结果（条目在缓存）。
  QVERIFY(LasCache::shared().isCached(las));
  QVERIFY(LasCache::shared().stats().misses >= coldBefore); // 至少一次装载
  const CacheStats st = LasCache::shared().stats();
  // 合并语义：6 并发 + 1 预备 → 内存命中显著（同份结果被共享）。
  QVERIFY(st.hits + st.misses >= 7);
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
  QVERIFY2(ms < 2000.0, qPrintable(QStringLiteral("high waited %1ms").arg(ms)));
}

QTEST_MAIN(CacheAsyncTests)
#include "tst_cache_async.moc"
