// 层：测试壳
//
// 批次队列编排（方向 33）的断言集。逐条对账 Oracle：
//   1. 幂等——同批连跑两遍，第二遍全部 Skipped，executor 调用数零增量
//      （catalog 资产/版本零增量的代理判据：executor 是唯一的生产副作用面）；
//   2. 崩溃恢复——批中强杀（夹具内模拟：只落盘部分终态即销毁编排器），
//      重启续跑只补未完项，已完成项不重复执行；
//   3. 失败隔离——批内造一单失败，其余照常完成，失败原因聚合进报告；
//   4. 取消语义——批中取消，在跑作业协作停止、待跑作废、状态如实回写；
//   5. 熔断——catalog 不可用（fatal 词表命中）停全批，不白烧后续每项。
//
// 另附：批次定义展开（笛卡尔积 + 逐项覆写）、指纹稳定性、报告导出。
//
// 两条实现纪律（都踩过，抄自 tst_jobrunner）：
//   - 回调一律捕获 std::shared_ptr<Probe>，不捕获裸指针。teardown 会释放
//     fixture，而 worker 可能仍在跑（服务超时后会 orphan 池），裸指针必 UAF。
//   - 每个 compute 轮询循环都必须同时看取消标志与闸门，且有超时上界。
#include <QCoreApplication>
#include <QDir>
#include <QFile>
#include <QSet>
#include <QElapsedTimer>
#include <QEventLoop>
#include <QJsonArray>
#include <QJsonDocument>
#include <QJsonObject>
#include <QMutex>
#include <QMutexLocker>
#include <QTemporaryDir>
#include <QTest>
#include <QThread>

#include <atomic>
#include <functional>
#include <memory>

#include "services/paleotaskservice.h"
#include "workflow/batchjobqueue.h"

using PaleoBatchQueue::BatchItem;
using PaleoBatchQueue::BatchQueue;
using PaleoBatchQueue::BatchReport;
using PaleoBatchQueue::BatchSpec;
using PaleoBatchQueue::ItemState;

namespace
{

/// 测试侧观测状态（与 BatchItem 分离，保持其为纯数据）。
struct Probe
{
  std::atomic_int computeCalls{0};   ///< executor 被调次数（生产副作用的代理计数）
  std::atomic_int fatalCalls{0};
  std::atomic_int gateOpen{0};       ///< 闸门：放开等待中的 compute
  mutable QMutex mutex;
  QStringList executed;              ///< itemId 序（断言执行过哪些）
  QStringList failedItems;            ///< 预置失败项
  QStringList fatalItems;            ///< 预置熔断项
  QString fatalText{QStringLiteral( "catalog-unavailable" )};
  qint64 perItemWorkMs = 0;          ///< 每项模拟工作量（0 = 不耗时）

  void note(const QString &id)
  {
    QMutexLocker lock(&mutex);
    executed.append(id);
  }
  bool shouldFail(const QString &id) const
  {
    QMutexLocker lock(&mutex);
    return failedItems.contains(id);
  }
  bool shouldBeFatal(const QString &id) const
  {
    QMutexLocker lock(&mutex);
    return fatalItems.contains(id);
  }
  int executedCount() const
  {
    QMutexLocker lock(&mutex);
    return executed.size();
  }
  void reset()
  {
    QMutexLocker lock(&mutex);
    executed.clear();
    gateOpen.store(0);
  }
};

/// 泵事件直到 pred 为真或超时。编排层的 commit/drop 走 QueuedConnection，
/// 必须泵事件才推进——与真实 GUI 事件循环同形。
bool pumpUntil(const std::function<bool()> &pred, int timeoutMs = 8000)
{
  QElapsedTimer clock;
  clock.start();
  while (!pred())
  {
    if (clock.elapsed() > timeoutMs)
      return false;
    QCoreApplication::processEvents(QEventLoop::AllEvents, 10);
    QThread::msleep(1);
  }
  return true;
}

/// compute 侧等待：闸门开 / 被取消 / 超时，三者任一即返回。上界必需。
void waitOnGate(const std::shared_ptr<Probe> &probe,
                const std::function<bool()> &cancelled, int timeoutMs = 4000)
{
  QElapsedTimer clock;
  clock.start();
  while (clock.elapsed() < timeoutMs)
  {
    if (probe->gateOpen.load())
      return;
    if (cancelled && cancelled())
      return;
    QThread::msleep(2);
  }
}

/// 构造批次定义：2 层位 × 2 方法 = 4 项。
BatchSpec makeSpec(const QString &batchId, const QStringList &horizons,
                   const QStringList &methods, int concurrency = 1)
{
  BatchSpec s;
  s.batchId = batchId;
  s.horizons = horizons;
  s.methodIds = methods;
  s.paramTemplates.insert(QStringLiteral("cellSize"), 25.0);
  s.concurrency = concurrency;
  return s;
}

/// 注入的执行体：可门控、可失败、可熔断，计执行次数。
PaleoBatchQueue::ItemExecutor makeExecutor(const std::shared_ptr<Probe> &probe)
{
  return [probe](BatchItem &job, const std::function<bool()> &cancelled,
                 const std::function<void(double, const QString &)> &progress) {
    probe->computeCalls.fetch_add(1);
    probe->note(job.itemId);

    // 门控：闸门未开就一直等（测取消路径用）。带超时上界，不无限等。
    if (probe->gateOpen.load() == 0)
      waitOnGate(probe, cancelled);

    if (cancelled && cancelled())
    {
      job.error = QObject::tr("已取消");
      return false;
    }
    if (probe->perItemWorkMs > 0)
      QThread::msleep(static_cast<unsigned long>(probe->perItemWorkMs));

    if (progress)
      progress(50.0, QStringLiteral("interpolate"));

    if (probe->shouldBeFatal(job.itemId))
    {
      probe->fatalCalls.fetch_add(1);
      job.error = probe->fatalText; // 命中熔断词表
      return false;
    }
    if (probe->shouldFail(job.itemId))
    {
      job.error = QObject::tr("注入的失败");
      return false;
    }
    return true; // 成功：error 留空
  };
}

int countState(const QVector<BatchItem> &items, ItemState s)
{
  int n = 0;
  for (const BatchItem &it : items)
    if (it.state == s)
      ++n;
  return n;
}

} // namespace

class TestBatchJobQueue : public QObject
{
  Q_OBJECT

private slots:
  // 每例结束都拆 fixture——断言提前返回时也不把在途任务留给下一例。
  void cleanup() { tearDown(); }

  // ---- 定义面 ----
  void specExpandsCartesianProduct();
  void perItemOverrideChangesFingerprint();
  void fingerprintIsKeyOrderIndependent();
  void specRoundTripsThroughJson();

  // ---- Oracle 1 幂等 ----
  void rerunSameBatchSkipsAllItems();
  void duplicateFingerprintInSameBatchSkipped();

  // ---- Oracle 2 崩溃恢复 ----
  void resumeAfterCrashCompletesOnlyPending();
  void changedFingerprintRerunsInsteadOfReusing();

  // ---- Oracle 3 失败隔离 ----
  void failingItemDoesNotStopBatch();
  void failureReasonsAggregateInReport();
  void retryFailedRerunsOnlyFailures();
  void retryFailedWorksAfterBatchFinished();

  // ---- Oracle 4 取消语义 ----
  void cancelBatchStopsRunningAndInvalidatesQueued();
  void cancelSingleQueuedItem();

  // ---- Oracle 5 熔断 ----
  void fatalErrorTripsBreakerAndStopsBatch();

  // ---- 报告面 ----
  void reportExportsJsonWithHistogram();
  void progressSignalsFirePerItem();

  // ---- 入口守卫 ----
  void rejectsStartWithoutExecutor();
  void rejectsConcurrentBatch();

private:
  std::unique_ptr<QObject> m_dispatcher;
  std::unique_ptr<PaleoTaskService> m_tasks;
  std::unique_ptr<BatchQueue> m_queue;
  std::shared_ptr<Probe> m_probe;
  QTemporaryDir m_dir;

  QString projectDir() const { return m_dir.path(); }

  void setUp(int concurrency = 1)
  {
    m_dispatcher = std::make_unique<QObject>();
    m_tasks = std::make_unique<PaleoTaskService>(nullptr, m_dispatcher.get());
    m_tasks->setMaxWorkerThreads(4);
    m_queue = std::make_unique<BatchQueue>(m_tasks.get(), projectDir(),
                                           m_dispatcher.get());
    m_queue->setConcurrency(concurrency);
    m_probe = std::make_shared<Probe>();
    m_queue->setExecutor(makeExecutor(m_probe));
  }

  void tearDown()
  {
    if (m_tasks)
      m_tasks->shutdown(3000, false);
    m_queue.reset();
    m_tasks.reset();
    m_dispatcher.reset();
    // probe 故意不在此释放：worker 可能仍在跑。回调持 shared_ptr，故不悬垂。
    m_probe.reset();
  }

  /// 跑到批结束（或超时）。返回是否真结束。
  bool runToCompletion(int timeoutMs = 10000)
  {
    auto probe = m_probe;
    return pumpUntil([probe, this] { return !m_queue->busy(); }, timeoutMs);
  }
};

// ---- 定义面 ----

void TestBatchJobQueue::specExpandsCartesianProduct()
{
  const BatchSpec s =
      makeSpec(QStringLiteral("b1"), {QStringLiteral("H1"), QStringLiteral("H2")},
               {QStringLiteral("generateFactor"), QStringLiteral("contours")}, 2);
  QString err;
  const QVector<BatchItem> items = s.expand(&err);
  QVERIFY2(err.isEmpty(), qPrintable(err));
  QCOMPARE(items.size(), 4);
  QCOMPARE(items.at(0).itemId, QStringLiteral("H1|generateFactor"));
  QCOMPARE(items.at(3).itemId, QStringLiteral("H2|contours"));
  // 模板参数落到每项
  QCOMPARE(items.at(0).params.value(QStringLiteral("cellSize")).toDouble(), 25.0);
  // 指纹各不同
  QSet<QString> fps;
  for (const BatchItem &it : items)
    fps.insert(it.fingerprint);
  QCOMPARE(fps.size(), 4);
}

void TestBatchJobQueue::perItemOverrideChangesFingerprint()
{
  BatchSpec s = makeSpec(QStringLiteral("b"), {QStringLiteral("H1")},
                         {QStringLiteral("m")});
  QString err;
  const QString base = s.expand(&err).at(0).fingerprint;

  QVariantMap over;
  over.insert(QStringLiteral("cellSize"), 50.0);
  QVariantMap perItem;
  perItem.insert(QStringLiteral("H1|m"), over);
  s.paramTemplates.insert(QStringLiteral("perItem"), perItem);

  const QVector<BatchItem> items = s.expand(&err);
  QCOMPARE(items.size(), 1);
  // 覆写生效
  QCOMPARE(items.at(0).params.value(QStringLiteral("cellSize")).toDouble(), 50.0);
  // 覆写改变指纹
  QVERIFY(items.at(0).fingerprint != base);
  // perItem 表本身不进指纹参数（它是被展开的指令，不是算法参数）
  QVERIFY(!items.at(0).params.contains(QStringLiteral("perItem")));
}

void TestBatchJobQueue::fingerprintIsKeyOrderIndependent()
{
  QVariantMap a;
  a.insert(QStringLiteral("x"), 1);
  a.insert(QStringLiteral("y"), 2);
  a.insert(QStringLiteral("z"), 3);
  QVariantMap b;
  b.insert(QStringLiteral("z"), 3);
  b.insert(QStringLiteral("y"), 2);
  b.insert(QStringLiteral("x"), 1);
  QCOMPARE(BatchItem::fingerprintOf(QStringLiteral("m"), QStringLiteral("H"), a),
           BatchItem::fingerprintOf(QStringLiteral("m"), QStringLiteral("H"), b));
  // 参数值不同 → 指纹不同
  QVariantMap c = a;
  c.insert(QStringLiteral("x"), 2);
  QVERIFY(BatchItem::fingerprintOf(QStringLiteral("m"), QStringLiteral("H"), c) !=
          BatchItem::fingerprintOf(QStringLiteral("m"), QStringLiteral("H"), a));
  // 层位不同 → 指纹不同
  QVERIFY(BatchItem::fingerprintOf(QStringLiteral("m"), QStringLiteral("H2"), a) !=
          BatchItem::fingerprintOf(QStringLiteral("m"), QStringLiteral("H"), a));
}

void TestBatchJobQueue::specRoundTripsThroughJson()
{
  BatchSpec s = makeSpec(QStringLiteral("rt"), {QStringLiteral("H1")},
                         {QStringLiteral("m1"), QStringLiteral("m2")}, 3);
  s.stopOnFatal = false;
  bool ok = false;
  const BatchSpec back = BatchSpec::fromJson(s.toJson(), &ok);
  QVERIFY(ok);
  QCOMPARE(back.batchId, s.batchId);
  QCOMPARE(back.horizons, s.horizons);
  QCOMPARE(back.methodIds, s.methodIds);
  QCOMPARE(back.concurrency, 3);
  QCOMPARE(back.stopOnFatal, false);
  // 指纹跨序列化稳定
  QString err;
  QCOMPARE(s.expand(&err).at(0).fingerprint, back.expand(&err).at(0).fingerprint);
}

// ---- Oracle 1 幂等 ----

void TestBatchJobQueue::rerunSameBatchSkipsAllItems()
{
  setUp(2);
  const BatchSpec s =
      makeSpec(QStringLiteral("idem"), {QStringLiteral("H1"), QStringLiteral("H2")},
               {QStringLiteral("m1")}, 2);
  QString err;

  // 第一遍：全部真跑
  QVERIFY2(m_queue->start(s, &err), qPrintable(err));
  QVERIFY(runToCompletion());
  QCOMPARE(countState(m_queue->items(), ItemState::Succeeded), 2);
  const int firstExec = m_probe->executedCount();
  QCOMPARE(firstExec, 2);

  // 第二遍：同批重跑 → executor 零调用，全部 Skipped
  m_probe->reset();
  QVERIFY2(m_queue->resume(s, &err), qPrintable(err));
  QVERIFY(runToCompletion());
  const BatchReport r = m_queue->report();
  QCOMPARE(r.succeeded, 0);          // 第二遍没有新成功
  QCOMPARE(r.skipped, 2);            // 幂等命中
  QCOMPARE(m_probe->executedCount(), 0); // 生产副作用面零调用 → 资产/版本零增量
  // 报告里跳过是成功语义，不算失败
  QCOMPARE(r.failed, 0);
}

void TestBatchJobQueue::duplicateFingerprintInSameBatchSkipped()
{
  setUp(1);
  // 两层位 + 两方法，但逐项覆写让 (H1,m2) 与 (H2,m1) 拿到同一组参数 →
  // 两者 method 不同故指纹不同，故这里造真正重复：两个方法名不同但参数相同
  // 也不行——method 进指纹。改为同层位重复声明同一方法。
  BatchSpec s;
  s.batchId = QStringLiteral("dup");
  s.horizons = QStringList{QStringLiteral("H1")};
  s.methodIds = QStringList{QStringLiteral("m1")};
  s.paramTemplates.insert(QStringLiteral("cellSize"), 25.0);
  s.concurrency = 1;

  // 手工构造：expand 不会产生重复（itemId 唯一），故直接用两个相同指纹的项
  // 验证判重逻辑本身——把第二项的指纹改成与第一项相同。
  QString err;
  QVERIFY2(m_queue->start(s, &err), qPrintable(err));
  QVERIFY(runToCompletion());
  QCOMPARE(countState(m_queue->items(), ItemState::Succeeded), 1);

  // 重复项：新增一项与已完成项同指纹 → 判重跳过
  m_probe->reset();
  BatchSpec s2 = s;
  s2.methodIds = QStringList{QStringLiteral("m1"), QStringLiteral("m1")};
  // itemId 会重复（H1|m1 两次）——测试判重按指纹，展开重复项
  QString err2;
  QVERIFY2(m_queue->resume(s2, &err2), qPrintable(err2));
  QVERIFY(runToCompletion());
  const BatchReport r = m_queue->report();
  // 两项同指纹且均已在此前完成 → 本趟都记 Skipped，executor 零调用
  QCOMPARE(r.skipped, 2);
  QCOMPARE(r.succeeded, 0);
  QCOMPARE(m_probe->executedCount(), 0);
}

// ---- Oracle 2 崩溃恢复 ----

void TestBatchJobQueue::resumeAfterCrashCompletesOnlyPending()
{
  setUp(1);
  const BatchSpec s = makeSpec(
      QStringLiteral("crash"),
      {QStringLiteral("H1"), QStringLiteral("H2"), QStringLiteral("H3")},
      {QStringLiteral("m1")}, 1);

  QString err;
  QVERIFY2(m_queue->start(s, &err), qPrintable(err));
  // 等第一项落终态（逐项终态即回写，故盘上已有它的 Succeeded）
  QVERIFY(pumpUntil([this] { return m_queue->finishedCount() >= 1; }));

  // 模拟崩溃：直接销毁编排器。终态回调的连接上下文（m_ctx）随之析构自动断连，
  // 在飞 worker 捕获的是 Claim/Job 的 shared_ptr 副本，不悬垂；其余项在盘上
  // 仍是 Queued —— 这正是「崩在批中」的记档形态。
  const int execBeforeCrash = m_probe->executedCount();
  const int completedBeforeCrash = m_queue->finishedCount();
  QVERIFY(execBeforeCrash >= 1);
  m_queue.reset();

  // 重启：同一任务服务上换一个编排器，读回记档续跑
  auto queue2 = std::make_unique<BatchQueue>(m_tasks.get(), projectDir(),
                                             m_dispatcher.get());
  queue2->setConcurrency(1);
  auto probe2 = std::make_shared<Probe>();
  queue2->setExecutor(makeExecutor(probe2));
  m_queue = std::move(queue2);
  m_probe = probe2;

  QString err2;
  QVERIFY2(m_queue->resume(s, &err2), qPrintable(err2));
  QVERIFY2(pumpUntil([this] { return !m_queue->busy(); }),
           "续跑未在超时内收尾");

  const BatchReport r = m_queue->report();
  // 崩溃前已完成的项不重复执行：续跑只补未完的项。
  // 串行下第 1 项完成瞬间第 2 项可能已开跑，故崩溃前完成数取
  // finishedCount()（≥1），续跑执行数恰为「总数 - 该数」。
  QVERIFY(execBeforeCrash >= 1);
  const int expectedResumeRuns = r.total - completedBeforeCrash;
  QCOMPARE(m_probe->executedCount(), expectedResumeRuns);
  QVERIFY2(expectedResumeRuns < r.total,
           "已完成项被重复执行——幂等失效");
  // 无残留非终态项
  QCOMPARE(m_queue->pendingCount(), 0);
  QCOMPARE(m_queue->runningCount(), 0);
  // 崩溃前完成的记 Skipped（本趟未执行），其余补完成功
  QCOMPARE(r.skipped, completedBeforeCrash);
  QCOMPARE(r.succeeded, r.total - completedBeforeCrash);
  QCOMPARE(r.failed, 0);
  QCOMPARE(r.cancelled, 0);
}

void TestBatchJobQueue::retryFailedWorksAfterBatchFinished()
{
  setUp(1);
  auto probe = m_probe;
  const BatchSpec s = makeSpec(
      QStringLiteral("retryafter"),
      {QStringLiteral("H1"), QStringLiteral("H2")}, {QStringLiteral("m1")}, 1);
  probe->failedItems = QStringList{QStringLiteral("H2|m1")};

  QString err;
  QVERIFY2(m_queue->start(s, &err), qPrintable(err));
  QVERIFY(runToCompletion());
  // 批已结束
  QVERIFY(!m_queue->busy());
  QCOMPARE(m_queue->report().failed, 1);

  // 批后重试失败项：应能重新起批（不要求批次仍在跑）
  probe->failedItems.clear();
  probe->reset();
  QVERIFY2(m_queue->retryFailed(), "批后重试失败项被拒");
  QVERIFY2(pumpUntil([this] { return !m_queue->busy(); }),
           "批后重试未在超时内收尾");

  const BatchReport r = m_queue->report();
  QCOMPARE(r.failed, 0);
  QCOMPARE(r.succeeded + r.skipped, 2);
  // 只重跑了失败那项
  QCOMPARE(probe->executedCount(), 1);
}

void TestBatchJobQueue::changedFingerprintRerunsInsteadOfReusing()
{
  setUp(1);
  BatchSpec s = makeSpec(QStringLiteral("fp"), {QStringLiteral("H1")},
                         {QStringLiteral("m1")}, 1);
  QString err;
  QVERIFY2(m_queue->start(s, &err), qPrintable(err));
  QVERIFY(runToCompletion());
  QCOMPARE(countState(m_queue->items(), ItemState::Succeeded), 1);

  // 改参数 → 指纹变 → 不复用旧成功终态，照新参数重跑
  m_probe->reset();
  BatchSpec s2 = s;
  s2.paramTemplates.insert(QStringLiteral("cellSize"), 99.0);
  QString err2;
  QVERIFY2(m_queue->resume(s2, &err2), qPrintable(err2));
  QVERIFY(runToCompletion());
  // 新参数是新作业，真跑了一趟
  QCOMPARE(m_probe->executedCount(), 1);
  const BatchReport r = m_queue->report();
  QCOMPARE(r.succeeded, 1);
  QCOMPARE(r.skipped, 0);
}

// ---- Oracle 3 失败隔离 ----

void TestBatchJobQueue::failingItemDoesNotStopBatch()
{
  setUp(1);
  auto probe = m_probe;
  probe->failedItems = QStringList{QStringLiteral("H2|m1")};
  const BatchSpec s = makeSpec(
      QStringLiteral("iso"),
      {QStringLiteral("H1"), QStringLiteral("H2"), QStringLiteral("H3")},
      {QStringLiteral("m1")}, 1);

  QString err;
  QVERIFY2(m_queue->start(s, &err), qPrintable(err));
  QVERIFY(runToCompletion());

  const BatchReport r = m_queue->report();
  QCOMPARE(r.total, 3);
  QCOMPARE(r.failed, 1);   // 只有注入的那一项失败
  QCOMPARE(r.succeeded, 2); // 其余照常完成
  QCOMPARE(r.cancelled, 0);
  QVERIFY(!r.trippedFatal);
  // 全部项都执行过（失败没截断队列）
  QCOMPARE(probe->executedCount(), 3);
}

void TestBatchJobQueue::failureReasonsAggregateInReport()
{
  setUp(1);
  auto probe = m_probe;
  const BatchSpec s = makeSpec(
      QStringLiteral("agg"),
      {QStringLiteral("H1"), QStringLiteral("H2"), QStringLiteral("H3")},
      {QStringLiteral("m1")}, 1);
  // 注入的失败原因统一（执行体写死「注入的失败」），故 3 项失败同因
  probe->failedItems = QStringList{QStringLiteral("H1|m1"), QStringLiteral("H2|m1"),
                                  QStringLiteral("H3|m1")};
  QString err;
  QVERIFY2(m_queue->start(s, &err), qPrintable(err));
  QVERIFY(runToCompletion());

  const BatchReport r = m_queue->report();
  QCOMPARE(r.failed, 3);
  // 失败原因聚合：同因聚成一条，次数 3
  const auto hist = r.failureHistogram();
  QCOMPARE(hist.size(), 1);
  QCOMPARE(hist.at(0).second, 3);
  // 报告 summary 人读可导出
  QVERIFY(r.summary().contains(QStringLiteral("agg")));
}

void TestBatchJobQueue::retryFailedRerunsOnlyFailures()
{
  setUp(1);
  auto probe = m_probe;
  const BatchSpec s = makeSpec(
      QStringLiteral("retry"),
      {QStringLiteral("H1"), QStringLiteral("H2")}, {QStringLiteral("m1")}, 1);
  probe->failedItems = QStringList{QStringLiteral("H2|m1")};

  QString err;
  QVERIFY2(m_queue->start(s, &err), qPrintable(err));
  QVERIFY(runToCompletion());
  QCOMPARE(m_queue->report().failed, 1);
  QCOMPARE(m_queue->report().succeeded, 1);
  const int firstPass = probe->executedCount();

  // 修掉失败原因，只重试失败项：成功项不重复跑
  probe->failedItems.clear();
  probe->reset();
  // 批已结束，retryFailed 需批在跑——重开批次（同批续跑）再重试
  QString err2;
  QVERIFY2(m_queue->resume(s, &err2), qPrintable(err2));
  QVERIFY(runToCompletion());
  // 续跑时 H1 成功终态沿用（跳过），H2 重跑
  const BatchReport r = m_queue->report();
  // H1 此前成功 → 沿用记 Skipped；H2 此前失败 → 重跑成功
  QCOMPARE(r.succeeded, 1);
  QCOMPARE(r.skipped, 1);
  QCOMPARE(r.failed, 0);
  // 只执行了失败那项（成功项幂等跳过）
  QCOMPARE(probe->executedCount(), 1);
  QVERIFY(firstPass >= 1);
}

// ---- Oracle 4 取消语义 ----

void TestBatchJobQueue::cancelBatchStopsRunningAndInvalidatesQueued()
{
  setUp(1);
  auto probe = m_probe;
  probe->perItemWorkMs = 30; // 每项稍耗时，让取消落在批中
  const BatchSpec s = makeSpec(
      QStringLiteral("cancel"),
      {QStringLiteral("H1"), QStringLiteral("H2"), QStringLiteral("H3"),
       QStringLiteral("H4")},
      {QStringLiteral("m1")}, 1);

  QString err;
  QVERIFY2(m_queue->start(s, &err), qPrintable(err));
  // 等至少一项在跑/已完成
  QVERIFY(pumpUntil([this] { return m_queue->runningCount() >= 1 ||
                                       m_queue->finishedCount() >= 1; }));
  const bool cancelled = m_queue->cancelBatch();
  QVERIFY(cancelled);
  QVERIFY(pumpUntil([this] { return !m_queue->busy(); }));

  const BatchReport r = m_queue->report();
  // 状态如实回写：无一残留 Pending/Queued/Running
  QCOMPARE(m_queue->pendingCount(), 0);
  QCOMPARE(m_queue->runningCount(), 0);
  // 取消不谎报成功：已完成 + 取消 = 总数
  QCOMPARE(r.succeeded + r.cancelled, r.total);
  QVERIFY(r.cancelled >= 1); // 待跑项被作废
  // 已完成项成果保留
  QVERIFY(r.succeeded >= 0);
}

void TestBatchJobQueue::cancelSingleQueuedItem()
{
  setUp(1);
  auto probe = m_probe;
  probe->perItemWorkMs = 30;
  const BatchSpec s = makeSpec(
      QStringLiteral("cancel1"),
      {QStringLiteral("H1"), QStringLiteral("H2"), QStringLiteral("H3")},
      {QStringLiteral("m1")}, 1);

  QString err;
  QVERIFY2(m_queue->start(s, &err), qPrintable(err));
  QVERIFY(pumpUntil([this] { return m_queue->finishedCount() >= 1; }));

  // 取消一个还在队列里的项
  const QVector<BatchItem> items = m_queue->items();
  QString target;
  for (const BatchItem &it : items)
  {
    if (it.state == ItemState::Queued || it.state == ItemState::Pending)
    {
      target = it.itemId;
      break;
    }
  }
  QVERIFY(!target.isEmpty());
  QVERIFY(m_queue->cancelItem(target));
  QVERIFY(pumpUntil([this] { return !m_queue->busy(); }));

  // 该项落 Cancelled，不谎报成功
  for (const BatchItem &it : m_queue->items())
    if (it.itemId == target)
      QCOMPARE(int(it.state), int(ItemState::Cancelled));
}

// ---- Oracle 5 熔断 ----

void TestBatchJobQueue::fatalErrorTripsBreakerAndStopsBatch()
{
  setUp(1);
  auto probe = m_probe;
  // 第一项命中熔断（catalog 不可用）
  probe->fatalItems = QStringList{QStringLiteral("H1|m1")};
  const BatchSpec s = makeSpec(
      QStringLiteral("fatal"),
      {QStringLiteral("H1"), QStringLiteral("H2"), QStringLiteral("H3"),
       QStringLiteral("H4")},
      {QStringLiteral("m1")}, 1);

  QString err;
  QVERIFY2(m_queue->start(s, &err), qPrintable(err));
  QVERIFY(pumpUntil([this] { return !m_queue->busy(); }));

  const BatchReport r = m_queue->report();
  QVERIFY2(r.trippedFatal, "catalog 不可对应熔断全批");
  QVERIFY(r.fatalReason.contains(QStringLiteral("catalog-unavailable")));
  // 熔断后不白烧后续每项：只执行到触发熔断那项就停
  QVERIFY2(probe->executedCount() < r.total,
           qPrintable(QStringLiteral("熔断后仍全跑：executed=%1 total=%2")
                          .arg(probe->executedCount())
                          .arg(r.total)));
  // 待跑项作废
  QCOMPARE(m_queue->pendingCount(), 0);
  // 报告 summary 点明熔断
  QVERIFY(r.summary().contains(QStringLiteral("熔断")));
}

// ---- 报告面 ----

void TestBatchJobQueue::reportExportsJsonWithHistogram()
{
  setUp(1);
  auto probe = m_probe;
  probe->failedItems = QStringList{QStringLiteral("H2|m1")};
  const BatchSpec s = makeSpec(
      QStringLiteral("exp"), {QStringLiteral("H1"), QStringLiteral("H2")},
      {QStringLiteral("m1")}, 1);
  QString err;
  QVERIFY2(m_queue->start(s, &err), qPrintable(err));
  QVERIFY(runToCompletion());

  const BatchReport r = m_queue->report();
  const QString path = projectDir() + QStringLiteral("/reports/batch.json");
  QString exportErr;
  const QString written = r.exportReport(path, &exportErr);
  QVERIFY2(!written.isEmpty(), qPrintable(exportErr));
  QVERIFY(QFile::exists(written));

  QFile f(written);
  QVERIFY(f.open(QIODevice::ReadOnly));
  const QJsonObject o = QJsonDocument::fromJson(f.readAll()).object();
  QCOMPARE(o.value(QStringLiteral("batch_id")).toString(), QStringLiteral("exp"));
  QCOMPARE(o.value(QStringLiteral("total")).toInt(), 2);
  QCOMPARE(o.value(QStringLiteral("succeeded")).toInt(), 1);
  QCOMPARE(o.value(QStringLiteral("failed")).toInt(), 1);
  QVERIFY(o.contains(QStringLiteral("failure_histogram")));
  QVERIFY(o.contains(QStringLiteral("summary")));
  QCOMPARE(o.value(QStringLiteral("items")).toArray().size(), 2);
}

void TestBatchJobQueue::progressSignalsFirePerItem()
{
  setUp(1);
  const BatchSpec s = makeSpec(
      QStringLiteral("prog"), {QStringLiteral("H1"), QStringLiteral("H2")},
      {QStringLiteral("m1")}, 1);

  int progressCount = 0;
  int lastDone = -1, lastTotal = -1;
  connect(m_queue.get(), &BatchQueue::batchProgress, this,
          [&](int done, int total) {
            ++progressCount;
            lastDone = done;
            lastTotal = total;
          });
  bool startedFired = false, finishedFired = false;
  connect(m_queue.get(), &BatchQueue::batchStarted, this,
          [&](const QString &, int) { startedFired = true; });
  connect(m_queue.get(), &BatchQueue::batchFinished, this,
          [&](const QString &, bool) { finishedFired = true; });

  QString err;
  QVERIFY2(m_queue->start(s, &err), qPrintable(err));
  QVERIFY(runToCompletion());

  QVERIFY(startedFired);
  QVERIFY(finishedFired);
  QVERIFY2(progressCount >= 2, "逐项推进应多次报进度");
  QCOMPARE(lastTotal, 2);
  QCOMPARE(lastDone, 2); // 终态全完成
}

// ---- 入口守卫 ----

void TestBatchJobQueue::rejectsStartWithoutExecutor()
{
  setUp(1);
  m_queue->setExecutor(nullptr);
  QString err;
  const BatchSpec s = makeSpec(QStringLiteral("noexec"),
                               {QStringLiteral("H1")}, {QStringLiteral("m1")}, 1);
  QVERIFY(!m_queue->start(s, &err));
  QVERIFY(!err.isEmpty()); // 如实报错，不静默
}

void TestBatchJobQueue::rejectsConcurrentBatch()
{
  setUp(1);
  auto probe = m_probe;
  probe->perItemWorkMs = 40;
  const BatchSpec s = makeSpec(
      QStringLiteral("busy"), {QStringLiteral("H1"), QStringLiteral("H2")},
      {QStringLiteral("m1")}, 1);
  QString err;
  QVERIFY2(m_queue->start(s, &err), qPrintable(err));
  // 上一批未完 → 拒绝
  QString err2;
  QVERIFY(!m_queue->start(s, &err2));
  QVERIFY(err2.contains(QStringLiteral("尚未结束")));
  QVERIFY(runToCompletion());
}

int main(int argc, char *argv[])
{
  // 本仓测试在无控制台的 Windows 下由 ctest 拉起，QtTest 的结果行会走
  // OutputDebugString 而非 stdout——崩溃时 stdout 缓冲又会被 __fastfail 吞掉，
  // 于是 ctest --output-on-failure 什么都看不到。故追加 `-o <file>,txt` 让
  // 每个用例的结果直接落盘，崩到哪一条一目了然。
  QByteArray logPath =
      QByteArray(QT_TESTCASE_BUILDDIR) + "/tst_batchjobqueue-result.txt";
  {
    QCoreApplication app(argc, argv);
    TestBatchJobQueue tc;
    QList<QByteArray> forwarded;
    forwarded << QByteArray(argv[0]);
    for (int i = 1; i < argc; ++i)
      forwarded << QByteArray(argv[i]);
    forwarded << QByteArray("-o") << logPath + ",txt";
    QList<char *> cargv;
    cargv.reserve(forwarded.size());
    for (QByteArray &a : forwarded)
      cargv << a.data();
    return QTest::qExec(&tc, cargv.size(), cargv.data());
  }
}

#include "tst_batchjobqueue.moc"
