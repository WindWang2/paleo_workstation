// 层：测试壳
//
// JobRunner 框架自身的断言集（方向 20 Oracle 2）。核心断言：
//   1. compute 在 worker 线程、commit 在 owner 线程（QThread::currentThreadId 比对）；
//   2. generation 过期任务的 commit 被丢弃（Claim 代际语义直接断言）；
//   3. cancel 中断 compute，且取消后 commit 不执行；
//   4. 异常穿越 worker → commit（commit 在 owner 线程收到失败态）；
//   5. 失败终态触发 cleanup、且不进 dropped。
//
// 另附行为等价基线断言（对齐现状 4 组三段式的实际语义）：
//   prepare 失败不建任务、忙则拒绝、cleanup 先于 dropped、无任务池时退化。
//
// 两条实现纪律（都踩过）：
//   - 回调一律捕获 std::shared_ptr<Probe>，不捕获裸指针。teardown 会释放
//     fixture，而 worker 可能仍在跑（超时后服务会 orphan 池），裸指针必 UAF。
//   - 每个 compute 轮询循环都必须同时看取消标志与闸门，且有超时上界——
//     否则死循环的 compute 会拖到 teardown 才崩，症状远离病因。
#include <QCoreApplication>
#include <QElapsedTimer>
#include <QEventLoop>
#include <QMutex>
#include <QMutexLocker>
#include <QSignalSpy>
#include <QString>
#include <QStringList>
#include <QTest>
#include <QThread>

#include <gdal.h>
#include <qgis.h>
#include <qgsapplication.h>

#include <atomic>
#include <functional>
#include <memory>
#include <stdexcept>

#include "services/jobrunner.h"

using paleo::jobs::CancelFn;
using paleo::jobs::DropReason;
using paleo::jobs::JobRunner;
using paleo::jobs::ProgressFn;

namespace {

/// 测试侧观测状态：与 TestJob 分离，保持 TestJob 为纯值语义（框架要求
/// Job 可拷贝，见 start 的 shared_ptr 说明）。
struct Probe
{
  std::atomic_int prepareCalls{0};
  std::atomic_int computeCalls{0};
  std::atomic_int commitCalls{0};
  std::atomic_int cleanupCalls{0};
  std::atomic_int droppedCalls{0};
  std::atomic<Qt::HANDLE> computeThread{nullptr};
  std::atomic<Qt::HANDLE> commitThread{nullptr};
  std::atomic<quintptr> lastDropReason{static_cast<quintptr>(-1)};
  std::atomic_bool computeSawCancel{false};
  std::atomic_bool gateOpen{false};
  mutable QMutex orderMutex;
  QStringList order;

  void note(const QString &step)
  {
    QMutexLocker lock(&orderMutex);
    order << step;
  }
  int indexOf(const QString &step) const
  {
    QMutexLocker lock(&orderMutex);
    return order.indexOf(step);
  }
  int orderSize() const
  {
    QMutexLocker lock(&orderMutex);
    return order.size();
  }
  QString at(int i) const
  {
    QMutexLocker lock(&orderMutex);
    return order.value(i);
  }
};

/// 测试用 Job：输入快照 + 中间产物 + 失败态，形态对齐迁移点的真实 job
/// （LocalDirectionJob / AnalysisContourJob / PropertyModelComputed 同构）。
struct TestJob
{
  int input = 0;
  int result = 0;
  bool ok = false;
  QString error;
};

/// 泵事件循环直到 pred 为真或超时。框架的 commit 段走 QueuedConnection，
/// 必须泵事件才推进——与真实 GUI 事件循环同形。
bool pumpUntil(const std::function<bool()> &pred, int timeoutMs = 5000)
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

/// compute 侧的通用等待：闸门开 或 被取消 或 超时，三者任一即返回。
/// 上界必需——没有它，卡住的 compute 会把崩溃推迟到 teardown，掩盖病因。
void waitOnGate(const std::shared_ptr<Probe> &probe, const CancelFn &cancelled,
                int timeoutMs = 3000)
{
  QElapsedTimer clock;
  clock.start();
  while (!probe->gateOpen.load() && !cancelled() && clock.elapsed() < timeoutMs)
    QThread::msleep(2);
}

} // namespace

class TestJobRunner : public QObject
{
  Q_OBJECT

private slots:
  // 每例结束都拆 fixture——断言提前返回时也不把在途任务留给下一例。
  void cleanup() { tearDownBusy(); }
  void computeOffOwnerCommitOnOwner();
  void staleGenerationDropsCommit();
  void cancelInterruptsComputeAndSkipsCommit();
  void cancelReleasesBusyForNextStart();
  void exceptionCrossesWorkerToCommit();
  void computeFailureReachesCommitAsFailedState();
  void prepareFailureBuildsNoTask();
  void rejectsStartWhenBusy();
  void withoutTaskServiceDegrades();
  void commitPrecedesCallerFinishedSlot();
  void taskCancelReachesCompute();
  void sessionResetDropsInFlightJob();

private:
  /// 每例一份 fixture：dispatcher（owner 线程）+ 可选任务服务 + runner。
  std::unique_ptr<QObject> m_dispatcher;
  std::unique_ptr<PaleoTaskService> m_tasks;
  std::unique_ptr<JobRunner<TestJob>> m_runner;
  std::shared_ptr<TestJob> m_job;
  std::shared_ptr<Probe> m_probe;

  void setUpWithService(bool withService = true)
  {
    m_dispatcher = std::make_unique<QObject>();
    m_runner = std::make_unique<JobRunner<TestJob>>(m_dispatcher.get());
    if (withService)
    {
      m_tasks = std::make_unique<PaleoTaskService>(nullptr, m_dispatcher.get());
      m_runner->setTaskService(m_tasks.get());
    }
    m_job = std::make_shared<TestJob>();
    m_probe = std::make_shared<Probe>();
  }

  void tearDownBusy()
  {
    if (m_tasks)
      m_tasks->shutdown(3000, false);
    m_runner.reset();
    m_tasks.reset();
    m_dispatcher.reset();
    // job/probe 故意不在这儿释放：worker 可能还在跑（服务超时后会 orphan 池）。
    // 回调一律持 shared_ptr，Probe 因此不会悬垂。
    m_job.reset();
    m_probe.reset();
  }
};

// ---------------------------------------------------------------------------
// 1. compute 在 worker 线程，commit 在 owner 线程
// ---------------------------------------------------------------------------
void TestJobRunner::computeOffOwnerCommitOnOwner()
{
  setUpWithService();
  TestJob *raw = m_job.get();
  auto probe = m_probe;
  QThread *owner = QThread::currentThread();

  JobRunner<TestJob>::Callbacks cb;
  cb.prepare = [probe](TestJob &job, QString *) {
    probe->prepareCalls.fetch_add(1);
    probe->note(QStringLiteral("prepare"));
    job.input = 7;
    return true;
  };
  cb.compute = [probe](TestJob &job, const CancelFn &, const ProgressFn &progress) {
    probe->computeCalls.fetch_add(1);
    probe->computeThread.store(QThread::currentThreadId());
    probe->note(QStringLiteral("compute"));
    progress(50.0, QStringLiteral("interpolate"));
    job.result = job.input * 6;
    job.ok = true;
    return true;
  };
  cb.commit = [probe](TestJob &job, QString *) {
    probe->commitCalls.fetch_add(1);
    probe->commitThread.store(QThread::currentThreadId());
    probe->note(QStringLiteral("commit"));
    return job.ok;
  };

  PaleoTask *task = m_runner->start(QStringLiteral("job"), m_job, cb);
  QVERIFY(task);
  QVERIFY(pumpUntil([probe] { return probe->commitCalls.load() == 1; }));

  // compute 跑了，且不在 owner 线程
  QCOMPARE(probe->computeCalls.load(), 1);
  QVERIFY(probe->computeThread.load() != nullptr);
  QVERIFY(probe->computeThread.load() != QThread::currentThreadId());

  // commit 跑了，且恰在 owner 线程
  QCOMPARE(probe->commitCalls.load(), 1);
  QCOMPARE(probe->commitThread.load(), QThread::currentThreadId());
  QCOMPARE(QThread::currentThread(), owner);
  QCOMPARE(m_runner->ownerThread(), owner);

  // 回调序：prepare → compute → commit
  QCOMPARE(probe->orderSize(), 3);
  QCOMPARE(probe->at(0), QStringLiteral("prepare"));
  QCOMPARE(probe->at(1), QStringLiteral("compute"));
  QCOMPARE(probe->at(2), QStringLiteral("commit"));

  // compute 写入的结果，commit 段读的是同一份 Job（shared_ptr 共享所有权）
  QCOMPARE(raw->result, 42);
  QVERIFY(raw->ok);
  QCOMPARE(task->state(), PaleoTask::State::Succeeded);

  tearDownBusy();
}

// ---------------------------------------------------------------------------
// 2. generation 过期任务的 commit 被丢弃
//
// 注意：start() 有 busy() 门控（同一 runner 同时只跑一代，对齐现状「忙则拒绝」），
// 所以「第二代接管 → 第一代陈旧」无法经 start() 制造。陈旧是 Claim 级的
// 安全网：任何绕过 busy 的路径（如将来支持 supersede）都必须丢弃旧代 commit。
// 这里直接对 Claim 断言该语义，不构造假并发。
// ---------------------------------------------------------------------------
void TestJobRunner::staleGenerationDropsCommit()
{
  setUpWithService();

  auto gen1 = m_runner->claim();
  QVERIFY(gen1.valid());
  QVERIFY(gen1.current());
  QCOMPARE(gen1.myGeneration, quint64(1));
  QCOMPARE(m_runner->currentGeneration(), quint64(1));

  auto gen2 = m_runner->claim();
  QVERIFY(gen2.valid());
  QVERIFY(gen2.current());
  QCOMPARE(gen2.myGeneration, quint64(2));

  // 新代接管后旧代立即陈旧——这正是 commit 段丢弃旧代的判据
  QVERIFY(!gen1.current());
  QVERIFY(gen2.current());
  QCOMPARE(m_runner->isCurrent(1), false);
  QCOMPARE(m_runner->isCurrent(2), true);

  // 取消标志按代隔离：取消当前代不影响上一代句柄
  m_runner->requestCancel();
  QVERIFY(gen2.cancelled());
  QVERIFY(!gen1.cancelled());

  tearDownBusy();
}

// ---------------------------------------------------------------------------
// 3. cancel 中断 compute，且取消后 commit 不执行
// ---------------------------------------------------------------------------
void TestJobRunner::cancelInterruptsComputeAndSkipsCommit()
{
  setUpWithService();
  auto probe = m_probe;

  JobRunner<TestJob>::Callbacks cb;
  cb.prepare = [probe](TestJob &, QString *) {
    probe->prepareCalls.fetch_add(1);
    probe->note(QStringLiteral("prepare"));
    return true;
  };
  cb.compute = [probe](TestJob &job, const CancelFn &cancelled, const ProgressFn &) {
    probe->computeCalls.fetch_add(1);
    probe->computeThread.store(QThread::currentThreadId());
    probe->note(QStringLiteral("compute"));
    waitOnGate(probe, cancelled);
    probe->computeSawCancel.store(cancelled());
    return !cancelled();
  };
  cb.commit = [probe](TestJob &, QString *) {
    probe->commitCalls.fetch_add(1);
    probe->note(QStringLiteral("commit"));
    return true;
  };
  cb.cleanup = [probe](TestJob &) {
    probe->cleanupCalls.fetch_add(1);
    probe->note(QStringLiteral("cleanup"));
  };
  cb.onDropped = [probe](TestJob &, DropReason reason) {
    probe->droppedCalls.fetch_add(1);
    probe->lastDropReason.store(static_cast<quintptr>(reason));
    probe->note(QStringLiteral("dropped"));
  };

  PaleoTask *task = m_runner->start(QStringLiteral("cancelable"), m_job, cb);
  QVERIFY(task);
  QVERIFY(pumpUntil([probe] { return probe->computeCalls.load() == 1; }));

  m_runner->requestCancel();
  QVERIFY(pumpUntil([probe] { return probe->droppedCalls.load() == 1; }));

  // compute 观测到取消，且确实在 worker 线程
  QVERIFY(probe->computeThread.load() != QThread::currentThreadId());
  QVERIFY(probe->computeSawCancel.load());

  // commit 不执行；cleanup + dropped 各触发一次
  QCOMPARE(probe->commitCalls.load(), 0);
  QCOMPARE(probe->cleanupCalls.load(), 1);
  QCOMPARE(probe->droppedCalls.load(), 1);
  QCOMPARE(probe->lastDropReason.load(), quintptr(DropReason::Cancelled));

  // cleanup 先于 dropped（迁移点靠这个顺序做临时产物清理再上报）
  const int iCleanup = probe->indexOf(QStringLiteral("cleanup"));
  const int iDropped = probe->indexOf(QStringLiteral("dropped"));
  QVERIFY(iCleanup >= 0);
  QVERIFY(iDropped > iCleanup);

  QCOMPARE(task->state(), PaleoTask::State::Cancelled);
  QVERIFY(!m_runner->busy());

  tearDownBusy();
}

// ---------------------------------------------------------------------------
// 3b. 取消后忙位释放，下一代能接上（现状「忙则拒绝」的反面）
// ---------------------------------------------------------------------------
void TestJobRunner::cancelReleasesBusyForNextStart()
{
  setUpWithService();
  auto probe = m_probe;

  JobRunner<TestJob>::Callbacks cb;
  cb.prepare = [](TestJob &, QString *) { return true; };
  cb.compute = [probe](TestJob &job, const CancelFn &cancelled, const ProgressFn &) {
    waitOnGate(probe, cancelled);
    job.ok = !cancelled();
    return !cancelled();
  };
  cb.commit = [probe](TestJob &, QString *) {
    probe->commitCalls.fetch_add(1);
    return true;
  };
  cb.cleanup = [probe](TestJob &) { probe->cleanupCalls.fetch_add(1); };
  cb.onDropped = [probe](TestJob &, DropReason) { probe->droppedCalls.fetch_add(1); };

  PaleoTask *first = m_runner->start(QStringLiteral("first"), m_job, cb);
  QVERIFY(first);
  QVERIFY(pumpUntil([&] { return m_runner->busy(); }));

  m_runner->requestCancel();
  QVERIFY(pumpUntil([&] { return !m_runner->busy(); }));
  QCOMPARE(probe->droppedCalls.load(), 1);
  QCOMPARE(probe->commitCalls.load(), 0);

  // 忙位已释放：新一代可开，且闸门已开故立即完成
  probe->gateOpen.store(true);
  auto second = std::make_shared<TestJob>();
  PaleoTask *secondTask = m_runner->start(QStringLiteral("second"), second, cb);
  QVERIFY(secondTask);
  QVERIFY(pumpUntil([probe] { return probe->commitCalls.load() == 1; }));
  QCOMPARE(m_runner->currentGeneration(), quint64(2));
  QVERIFY(!m_runner->busy());

  tearDownBusy();
}

// ---------------------------------------------------------------------------
// 4. 异常穿越 worker → commit，commit 在 owner 线程收到失败态
// ---------------------------------------------------------------------------
void TestJobRunner::exceptionCrossesWorkerToCommit()
{
  setUpWithService();
  auto probe = m_probe;

  JobRunner<TestJob>::Callbacks cb;
  cb.prepare = [probe](TestJob &, QString *) {
    probe->prepareCalls.fetch_add(1);
    return true;
  };
  cb.compute = [probe](TestJob &, const CancelFn &, const ProgressFn &) -> bool {
    probe->computeCalls.fetch_add(1);
    probe->computeThread.store(QThread::currentThreadId());
    throw std::runtime_error("计算炸了");
  };
  cb.commit = [probe](TestJob &job, QString *) {
    probe->commitCalls.fetch_add(1);
    probe->commitThread.store(QThread::currentThreadId());
    // 失败态经既有通道上 UI：commit 段读到失败并如实上报
    job.ok = false;
    return false;
  };
  cb.cleanup = [probe](TestJob &) { probe->cleanupCalls.fetch_add(1); };
  cb.onDropped = [probe](TestJob &, DropReason) { probe->droppedCalls.fetch_add(1); };

  PaleoTask *task = m_runner->start(QStringLiteral("throwing"), m_job, cb);
  QVERIFY(task);
  QVERIFY(pumpUntil([probe] { return probe->commitCalls.load() == 1; }));

  // 异常在 worker 抛出、未被吞
  QCOMPARE(probe->computeCalls.load(), 1);
  QVERIFY(probe->computeThread.load() != QThread::currentThreadId());

  // 异常文本经 PaleoTask 失败通道到达
  QCOMPARE(task->state(), PaleoTask::State::Failed);
  QVERIFY(task->errorText().contains(QStringLiteral("计算炸了")));

  // 失败终态仍进 commit（失败要如实上 UI），且在 owner 线程
  QCOMPARE(probe->commitCalls.load(), 1);
  QCOMPARE(probe->commitThread.load(), QThread::currentThreadId());
  QCOMPARE(probe->cleanupCalls.load(), 1);
  QCOMPARE(probe->droppedCalls.load(), 0);
  QVERIFY(!m_runner->busy());

  tearDownBusy();
}

// ---------------------------------------------------------------------------
// 4b. compute 自行返回失败（非异常）：失败串经 job.error 传到任务通道，
//     commit 段仍执行——对齐现状「失败如实上 UI」
// ---------------------------------------------------------------------------
void TestJobRunner::computeFailureReachesCommitAsFailedState()
{
  setUpWithService();
  auto probe = m_probe;

  JobRunner<TestJob>::Callbacks cb;
  cb.prepare = [](TestJob &, QString *) { return true; };
  cb.compute = [probe](TestJob &job, const CancelFn &, const ProgressFn &) {
    probe->computeCalls.fetch_add(1);
    job.ok = false;
    job.error = QStringLiteral("插值不收敛");
    return false;
  };
  cb.commit = [probe](TestJob &job, QString *) {
    probe->commitCalls.fetch_add(1);
    probe->commitThread.store(QThread::currentThreadId());
    if (!job.ok)
      probe->note(QStringLiteral("commit-saw-failure"));
    return job.ok;
  };
  cb.cleanup = [probe](TestJob &) { probe->cleanupCalls.fetch_add(1); };

  PaleoTask *task = m_runner->start(QStringLiteral("failing"), m_job, cb);
  QVERIFY(task);
  QVERIFY(pumpUntil([probe] { return probe->commitCalls.load() == 1; }));

  QCOMPARE(task->state(), PaleoTask::State::Failed);
  QVERIFY(task->errorText().contains(QStringLiteral("插值不收敛")));
  // commit 段读到了 job.error，并如实上报（不吞）
  QCOMPARE(probe->indexOf(QStringLiteral("commit-saw-failure")), 0);
  QCOMPARE(probe->commitThread.load(), QThread::currentThreadId());
  QCOMPARE(probe->cleanupCalls.load(), 1);

  tearDownBusy();
}

// ---------------------------------------------------------------------------
// 5. prepare 失败不建任务（对齐现状：准备失败直接上 UI，不建任务）
// ---------------------------------------------------------------------------
void TestJobRunner::prepareFailureBuildsNoTask()
{
  setUpWithService();
  auto probe = m_probe;

  JobRunner<TestJob>::Callbacks cb;
  QSignalSpy droppedSpy( m_runner.get(), &paleo::jobs::JobRunnerBase::claimDropped );
  cb.prepare = [probe](TestJob &, QString *err) {
    probe->prepareCalls.fetch_add(1);
    if (err)
      *err = QStringLiteral("抓不到输入");
    return false;
  };
  cb.compute = [probe](TestJob &, const CancelFn &, const ProgressFn &) {
    probe->computeCalls.fetch_add(1);
    return true;
  };
  cb.commit = [probe](TestJob &, QString *) {
    probe->commitCalls.fetch_add(1);
    return true;
  };
  cb.cleanup = [probe](TestJob &) { probe->cleanupCalls.fetch_add(1); };
  cb.onDropped = [probe](TestJob &, DropReason reason) {
    probe->droppedCalls.fetch_add(1);
    probe->lastDropReason.store(static_cast<quintptr>(reason));
  };

  PaleoTask *task = m_runner->start(QStringLiteral("prep-fail"), m_job, cb);
  QVERIFY(!task); // 任务根本没建
  QCOMPARE(probe->prepareCalls.load(), 1);
  QCOMPARE(probe->computeCalls.load(), 0);
  QCOMPARE(probe->commitCalls.load(), 0);
  QCOMPARE(probe->droppedCalls.load(), 1);
  QCOMPARE(probe->lastDropReason.load(), quintptr(DropReason::PrepareFailed));
  QCOMPARE(probe->cleanupCalls.load(), 1);
  QVERIFY(!m_runner->busy());
  // generation 未被 prepare 失败推进
  QCOMPARE(m_runner->currentGeneration(), quint64(0));
  // #235：prepare 失败分支发 0（未入代约定）——不发上一代的代号。
  QCOMPARE(droppedSpy.count(), 1);
  QCOMPARE(droppedSpy.at(0).at(0).toULongLong(), 0ULL);
  QCOMPARE(droppedSpy.at(0).at(1).toInt(), static_cast<int>(DropReason::PrepareFailed));

  tearDownBusy();
}

// ---------------------------------------------------------------------------
// 忙则拒绝（对齐现状「已有单因素计算在进行」）
// ---------------------------------------------------------------------------
void TestJobRunner::rejectsStartWhenBusy()
{
  setUpWithService();
  auto probe = m_probe;

  JobRunner<TestJob>::Callbacks cb;
  cb.prepare = [](TestJob &, QString *) { return true; };
  cb.compute = [probe](TestJob &job, const CancelFn &cancelled, const ProgressFn &) {
    waitOnGate(probe, cancelled);
    job.ok = true;
    return true;
  };
  cb.commit = [](TestJob &, QString *) { return true; };

  PaleoTask *first = m_runner->start(QStringLiteral("first"), m_job, cb);
  QVERIFY(first);
  QVERIFY(pumpUntil([&] { return m_runner->busy(); }));

  auto otherJob = std::make_shared<TestJob>();
  PaleoTask *second = m_runner->start(QStringLiteral("second"), otherJob, cb);
  QVERIFY(!second);
  QCOMPARE(m_runner->currentGeneration(), quint64(1));

  probe->gateOpen.store(true);
  QVERIFY(pumpUntil([&] { return !m_runner->busy(); }));

  tearDownBusy();
}

// ---------------------------------------------------------------------------
// 无任务池退化（未接线壳 / 旧测试形态）
// ---------------------------------------------------------------------------
void TestJobRunner::withoutTaskServiceDegrades()
{
  setUpWithService(/*withService=*/false);
  JobRunner<TestJob>::Callbacks cb;
  cb.prepare = [](TestJob &, QString *) { return true; };
  cb.compute = [](TestJob &, const CancelFn &, const ProgressFn &) { return true; };
  cb.commit = [](TestJob &, QString *) { return true; };

  QVERIFY(!m_runner->taskService());
  QVERIFY(!m_runner->start(QStringLiteral("no-svc"), m_job, cb));
  QVERIFY(!m_runner->busy());

  tearDownBusy();
}

// ---------------------------------------------------------------------------
// 10. #159：调用方在 start() 之后连到 task->finished 的收尾槽，必须看到
//     commit 已执行（旧实现 commit 再排队一次，收尾槽先跑、读到空登记）；
//     jobCompleted 在 commit 之后、busy() 已转假时发。
// ---------------------------------------------------------------------------
void TestJobRunner::commitPrecedesCallerFinishedSlot()
{
  setUpWithService();
  auto probe = m_probe;

  JobRunner<TestJob>::Callbacks cb;
  cb.compute = [](TestJob &job, const CancelFn &, const ProgressFn &) {
    job.result = 7;
    job.ok = true;
    return true;
  };
  cb.commit = [probe](TestJob &, QString *) {
    probe->commitCalls.fetch_add(1);
    probe->note(QStringLiteral("commit"));
    return true;
  };

  int commitsSeenByCaller = -1;
  bool busySeenByCompleted = true;
  int completedCalls = 0;
  bool completedCommitted = false;
  // 连接挂在局部 ctx 上：断言提前返回时 ctx 先于上面的局部量析构、自动断连，
  // 不会让排队回调写已出作用域的引用。
  QObject ctx;
  connect(m_runner.get(), &paleo::jobs::JobRunnerBase::jobCompleted, &ctx,
          [&](quint64, bool committed) {
            ++completedCalls;
            completedCommitted = committed;
            busySeenByCompleted = m_runner->busy();
            probe->note(QStringLiteral("completed"));
          });

  PaleoTask *task = m_runner->start(QStringLiteral("order"), m_job, cb);
  QVERIFY(task);
  // 与 PaleoMainWindow::startPropertyModelRun 同形：start 之后才连收尾槽。
  connect(task, &PaleoTask::finished, &ctx, [&, probe] {
    commitsSeenByCaller = probe->commitCalls.load();
    probe->note(QStringLiteral("caller-finished"));
  });

  QVERIFY(pumpUntil([&] { return commitsSeenByCaller >= 0 && completedCalls == 1; }));
  QCOMPARE(commitsSeenByCaller, 1);
  QVERIFY(probe->indexOf(QStringLiteral("commit")) <
          probe->indexOf(QStringLiteral("caller-finished")));
  QVERIFY(probe->indexOf(QStringLiteral("commit")) <
          probe->indexOf(QStringLiteral("completed")));
  QVERIFY(completedCommitted);
  QVERIFY(!busySeenByCompleted);

  tearDownBusy();
}

// ---------------------------------------------------------------------------
// 11. #160：任务页「取消」按钮只调 PaleoTask::requestCancel——compute 的
//     CancelFn 必须立刻看到（旧实现只看 claim 取消位，compute 跑满全程）。
// ---------------------------------------------------------------------------
void TestJobRunner::taskCancelReachesCompute()
{
  setUpWithService();
  auto probe = m_probe;

  JobRunner<TestJob>::Callbacks cb;
  cb.compute = [probe](TestJob &, const CancelFn &cancelled, const ProgressFn &) {
    probe->computeCalls.fetch_add(1);
    QElapsedTimer clock;
    clock.start();
    waitOnGate(probe, cancelled, 3000);
    probe->computeSawCancel.store(cancelled() && clock.elapsed() < 2500);
    return !cancelled();
  };
  cb.commit = [probe](TestJob &, QString *) {
    probe->commitCalls.fetch_add(1);
    return true;
  };
  cb.onDropped = [probe](TestJob &, DropReason reason) {
    probe->droppedCalls.fetch_add(1);
    probe->lastDropReason.store(static_cast<quintptr>(reason));
  };

  PaleoTask *task = m_runner->start(QStringLiteral("panel-cancel"), m_job, cb);
  QVERIFY(task);
  QVERIFY(pumpUntil([probe] { return probe->computeCalls.load() == 1; }));
  task->requestCancel(); // 任务页取消按钮的连接目标
  QVERIFY(pumpUntil([probe] { return probe->droppedCalls.load() == 1; }, 5000));
  QVERIFY2(probe->computeSawCancel.load(),
           "compute did not observe PaleoTask::requestCancel promptly");
  QCOMPARE(probe->commitCalls.load(), 0);
  QCOMPARE(probe->lastDropReason.load(), quintptr(DropReason::Cancelled));

  tearDownBusy();
}

// ---------------------------------------------------------------------------
// 12. #153：工程切换 beginNewSession() → 在途作业被取消、不进入 commit。
// ---------------------------------------------------------------------------
void TestJobRunner::sessionResetDropsInFlightJob()
{
  setUpWithService();
  auto probe = m_probe;

  JobRunner<TestJob>::Callbacks cb;
  cb.compute = [probe](TestJob &, const CancelFn &cancelled, const ProgressFn &) {
    probe->computeCalls.fetch_add(1);
    waitOnGate(probe, cancelled, 3000);
    return true; // 即便 compute 无视取消照常返回成功，结果也必须作废
  };
  cb.commit = [probe](TestJob &, QString *) {
    probe->commitCalls.fetch_add(1);
    return true;
  };
  cb.onDropped = [probe](TestJob &, DropReason) { probe->droppedCalls.fetch_add(1); };

  const quint64 before = m_tasks->session();
  PaleoTask *task = m_runner->start(QStringLiteral("old-project"), m_job, cb);
  QVERIFY(task);
  QCOMPARE(task->session(), before);
  QVERIFY(pumpUntil([probe] { return probe->computeCalls.load() == 1; }));
  QVERIFY(m_tasks->beginNewSession() > before);
  QVERIFY(pumpUntil([probe] { return probe->droppedCalls.load() == 1; }, 5000));
  QCOMPARE(probe->commitCalls.load(), 0);
  QVERIFY(!m_runner->busy());

  tearDownBusy();
}

int main(int argc, char *argv[])
{
  // 本仓测试在无控制台的 Windows 下由 ctest 拉起，QtTest 的结果行会走
  // OutputDebugString 而非 stdout——崩溃时 stdout 缓冲又会被 __fastfail 吞掉，
  // 于是 ctest --output-on-failure 什么都看不到。故追加 `-o <file>,txt` 让
  // 每个用例的结果直接落盘，崩到哪一条一目了然。
  QByteArray logPath = QByteArray(QT_TESTCASE_BUILDDIR) + "/tst_jobrunner-result.txt";
  {
    QgsApplication app(argc, argv, false);
    app.setPrefixPath(qEnvironmentVariable("QGIS_PREFIX_PATH", QStringLiteral("/usr")), true);
    app.initQgis();
    GDALAllRegister();
    TestJobRunner tc;
    QList<QByteArray> forwarded;
    forwarded << QByteArray(argv[0]);
    for (int i = 1; i < argc; ++i)
      forwarded << QByteArray(argv[i]);
    forwarded << QByteArray("-o") << logPath + ",txt";
    QList<char *> cargv;
    cargv.reserve(forwarded.size());
    for (QByteArray &a : forwarded)
      cargv << a.data();
    const int rc = QTest::qExec(&tc, cargv.size(), cargv.data());
    QgsApplication::exitQgis();
    return rc;
  }
}

#include "tst_jobrunner.moc"
