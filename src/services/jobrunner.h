// 层：数据
#pragma once

#include <QDateTime>
#include <QObject>
#include <QPointer>
#include <QString>
#include <QThread>

#include <atomic>
#include <functional>
#include <memory>
#include <type_traits>

#include "paleotaskservice.h"

namespace paleo::jobs {

/// compute 段的取消轮询点：返回 true 表示已被请求取消。compute 侧在循环里自查。
using CancelFn = std::function<bool()>;

/// compute 段的进度回包：percent ∈ [0,100]，stage 取 PaleoTask 标准词表
/// （prepare/geometry/interpolate/encode/scan/parse/index/decode/hash/build/publish）。
/// 内部经 PaleoTask::reportStage 排队回 owner 线程——调用侧不必自己管线程。
using ProgressFn = std::function<void(double, const QString &)>;

/// 未进入 commit 的原因。commit 段只在「成功」与「失败」两种终态下执行
/// （失败时 Job 自带失败态，由迁移者如实上 UI）；取消与陈旧两态**不执行**
/// commit，改为触发 onDropped 回调 + cleanup 钩子。
enum class DropReason {
  Cancelled,     ///< 用户请求取消（或 compute 期间过期）
  Stale,         ///< generation 过期：更新一代已启动，本代结果作废
  PrepareFailed, ///< prepare 段失败，任务根本没建
};

QString dropReasonText(DropReason reason);

/// JobRunner 的非模板基类：承载线程调度、generation 计数、取消标志与任务句柄。
///
/// 跨线程共享的状态放在 Shared 里并以 shared_ptr 副本交给 worker——worker 捕获
/// 的是 Shared 而不是 `this`，因此 JobRunner 析构时在飞的 worker 不会悬垂。
/// finished 回包以 JobRunner 自身为 QObject context，析构即自动断连。
class JobRunnerBase : public QObject
{
  Q_OBJECT
public:
  /// dispatcher：commit 段排队回 owner 线程的中转对象，必须与 ownerThread 同线程
  /// （通常是主窗口或页面）。ownerThread 省略时取当前线程。
  explicit JobRunnerBase(QObject *dispatcher, QThread *ownerThread = nullptr);
  ~JobRunnerBase() override;

  /// 当前代号。每 start 一次自增——commit 段只接受当前代。
  quint64 currentGeneration() const;

  /// 是否有在飞任务（prepare 已过、commit 未完成）。与现状「忙则拒绝」互斥一致。
  bool busy() const;

  /// 在飞任务句柄；无则 nullptr。属 PaleoTaskService，勿跨线程持有。
  PaleoTask *currentTask() const;

  /// 请求取消当前代。已在 commit 段执行的任务不受影响（publish 是临界区）。
  void requestCancel();

  /// 跨线程共享状态。Claim 持其 shared_ptr（同一对象，非副本），
  /// worker 因此不必捕获 `this`——JobRunner 析构时在飞 worker 不会悬垂。
  struct Shared
  {
    std::atomic<quint64> generation{0};
  };

  struct Claim
  {
    /// 指向 JobRunner 内部的同一个计数器对象（不是副本）——worker 侧读它、
    /// owner 侧也读它，两边看到同一份状态，新代自增即让旧代 current() 转假。
    std::shared_ptr<Shared> shared;
    /// 本代专属的取消标志。每代新开，避免上一代的取消残留到下一代。
    std::shared_ptr<std::atomic_bool> cancel;
    quint64 myGeneration = 0;

    bool valid() const { return shared != nullptr && cancel != nullptr; }
    bool cancelled() const { return cancel && cancel->load(); }
    bool current() const
    {
      return shared && shared->generation.load() == myGeneration;
    }
  };

  /// 开新一代。旧代的 commit 回调会被丢弃。owner 线程调用。
  Claim claim();

  /// 本代是否仍为当前代。owner 线程用（commit 入口判定）。
  bool isCurrent(quint64 generation) const;

  /// 排队回 owner 线程执行。已在 owner 线程时也走队列——保持回调时序统一。
  void scheduleOnOwner(std::function<void()> fn);

  /// owner-thread 断言。commit 段入口调用——把 #80 的纪律变成机制。
  void assertOwnerThread(const char *where) const;

  QThread *ownerThread() const { return m_ownerThread; }
  QObject *dispatcher() const { return m_dispatcher; }

signals:
  /// 一次任务未进入 commit（取消/陈旧/prepare 失败）。reason 见 DropReason。
  void claimDropped(quint64 generation, int reason);
  /// 一代任务彻底结束（#159）：commit（成功/失败态）或 drop（取消/陈旧）都已
  /// 执行完、busy() 已转假之后发。UI 收尾应接这个而不是 PaleoTask::finished——
  /// 后者早于 commit，读不到登记结果。committed=false 表示走了 drop。
  void jobCompleted(quint64 generation, bool committed);

protected:
  void clearTask();
  void setCurrentTask(PaleoTask *task);

  /// 当前代的取消标志（无在飞代时为空）。
  std::shared_ptr<std::atomic_bool> m_cancel;

  QPointer<QObject> m_dispatcher;
  QThread *m_ownerThread = nullptr;
  std::shared_ptr<Shared> m_shared = std::make_shared<Shared>();
  QPointer<PaleoTask> m_task;
};

/// 三段式作业契约。Job 由迁移者定义——通常是「输入快照 + 中间产物」的聚合体。
///
/// 三段语义：
///   prepare — owner 线程，抓输入快照。返回 false → 任务不建，触发
///             onDropped(PrepareFailed)；
///   compute — worker 线程，纯计算（不得触碰 catalog / UI）。返回 false 或抛异常
///             → 失败终态，commit 段**仍会执行**，由迁移者读 Job 上的失败态如实上 UI；
///   commit  — owner 线程，登记/发信号。取消或陈旧时**不执行**。
template <class JobT>
class JobRunner : public JobRunnerBase
{
public:
  using Job = JobT;

  struct Callbacks
  {
    std::function<bool(Job &, QString *)> prepare;
    std::function<bool(Job &, const CancelFn &, const ProgressFn &)> compute;
    std::function<bool(Job &, QString *)> commit;
    /// 未进入 commit 时的通知（取消/陈旧/prepare 失败）。cleanup 已先执行。
    std::function<void(Job &, DropReason)> onDropped;
    /// 临时产物清理钩子：未成功发布时（取消/陈旧/失败）在 owner 线程执行一次。
    /// 替代各迁移点原先内联的 dropTemp 义务。
    std::function<void(Job &)> cleanup;
    /// 进度节流毫秒。0 = 每次都报。默认 50（对齐现状三处实际节流值）。
    int progressThrottleMs = 50;
  };

  using JobRunnerBase::JobRunnerBase;

  /// owner 线程调用。忙则返回 nullptr（与现状「已有计算在进行」的拒绝语义一致）。
  ///
  /// Job 以 **shared_ptr 共享所有权** 传给三段：compute 在 worker 上写入的结果，
  /// commit 段必须能看到同一份对象——按值传会得到互不可见的两份副本。
  PaleoTask *start(const QString &title, std::shared_ptr<Job> job, Callbacks cb,
                   const QString &layerId = QString(), bool quiet = false)
  {
    assertOwnerThread("JobRunner::start");
    if (busy() || !job)
      return nullptr;
    Job &mutableJob = *job;

    if (cb.prepare)
    {
      QString err;
      if (!cb.prepare(mutableJob, &err))
      {
        if (cb.cleanup)
          cb.cleanup(mutableJob);
        if (cb.onDropped)
          cb.onDropped(mutableJob, DropReason::PrepareFailed);
        emit claimDropped(currentGeneration(), static_cast<int>(DropReason::PrepareFailed));
        return nullptr;
      }
    }

    const Claim claim = JobRunnerBase::claim();
    const int throttleMs = cb.progressThrottleMs;
    auto lastReport = std::make_shared<std::atomic<qint64>>(-1000);

    PaleoTask *task = m_taskService
                          ? m_taskService->start(
                                title,
                                [job, cb, claim, throttleMs, lastReport](
                                    PaleoTask *running) mutable -> QString {
                                if (claim.cancelled() || !claim.current())
                                  return JobRunnerBase::tr("已取消");
                                // #160：任务页「取消」/工程切换只置 PaleoTask 的
                                // 取消位——compute 轮询必须也看得到它，否则取消
                                // 要等整段算完才生效。
                                CancelFn cancelFn = [claim, running] {
                                  return claim.cancelled() || !claim.current() ||
                                         (running && running->cancelRequested());
                                };
                                ProgressFn progress = [running, throttleMs, lastReport](
                                                          double percent,
                                                          const QString &stage) {
                                  if (!running)
                                    return;
                                  const int pct = percent <= 0.0
                                                      ? 0
                                                      : (percent >= 100.0
                                                             ? 100
                                                             : static_cast<int>(percent + 0.5));
                                  if (throttleMs > 0)
                                  {
                                    const qint64 now = QDateTime::currentMSecsSinceEpoch();
                                    const qint64 prev = lastReport->exchange(now);
                                    if (pct < 100 && now - prev < throttleMs)
                                      return;
                                  }
                                  running->reportStage(stage, pct);
                                };
                                if (!cb.compute || !job)
                                  return QString();
                                try
                                {
                                  if (!cb.compute(*job, cancelFn, progress))
                                  {
                                    if (cancelFn())
                                      return JobRunnerBase::tr("已取消");
                                    return jobError(*job);
                                  }
                                }
                                catch (const std::exception &e)
                                {
                                  return QString::fromUtf8(e.what());
                                }
                                catch (...)
                                {
                                  return JobRunnerBase::tr("未命名异常");
                                }
                                return QString();
                              },
                                layerId, PaleoTask::Priority::High, quiet)
                          : nullptr;
    if (!task)
      return nullptr;

    setCurrentTask(task);
    // #159：commit/drop 在 finished 槽内**同步**执行，不再二次排队。旧实现
    // 再 scheduleOnOwner 一次，导致调用方随后连到 task->finished 的收尾槽
    // 先于 commit 跑（读到空登记、失败也报成功）。finished 本身已由服务经
    // QueuedConnection 派发到 owner 线程，这里不需要再排队。
    // 状态读自信号发送者 task（发射期间必存活），不读 m_task——后者可能被
    // clearFinished/保留策略的 deleteLater 清成空指针，旧代码会把空指针
    // 当成 Succeeded。
    connect(task, &PaleoTask::finished, this, [this, job, cb, claim, task] {
      if (!job)
        return;
      Job &work = *job;
      const PaleoTask::State state = task->state();
      const bool stale = !claim.current();
      // 只清理本代自己的句柄：陈旧代结束时不能把新一代的 m_task 清掉。
      const bool ownsHandle = (m_task.data() == task);
      if (state == PaleoTask::State::Cancelled || stale)
      {
        if (cb.cleanup)
          cb.cleanup(work);
        const DropReason reason = stale && state != PaleoTask::State::Cancelled
                                      ? DropReason::Stale
                                      : DropReason::Cancelled;
        if (cb.onDropped)
          cb.onDropped(work, reason);
        if (ownsHandle)
          clearTask();
        emit claimDropped(claim.myGeneration, static_cast<int>(reason));
        emit jobCompleted(claim.myGeneration, false);
        return;
      }
      if (state == PaleoTask::State::Failed && cb.cleanup)
        cb.cleanup(work);
      // 失败态也进 commit：Job 上已带失败态，迁移者据此如实上 UI（现状语义）。
      assertOwnerThread("JobRunner::commit");
      if (cb.commit)
        cb.commit(work, nullptr);
      if (ownsHandle)
        clearTask();
      emit jobCompleted(claim.myGeneration, true);
    });
    return task;
  }

  /// 绑定任务服务。不绑则 start 恒返回 nullptr（无任务池的测试壳可显式退化）。
  void setTaskService(PaleoTaskService *service) { m_taskService = service; }
  PaleoTaskService *taskService() const { return m_taskService; }

private:
  /// 优先读 Job 上的 `error` 成员作为失败串（现状 4 组三段式的失败串都走这个
  /// 字段）。三种形态都兼容：QString 值、QString*、可选类型（无则回落通用文案）。
  static QString jobError(const Job &job)
  {
    if constexpr (requires { job.error; })
    {
      if constexpr (std::is_pointer_v<std::decay_t<decltype(job.error)>>)
      {
        if (job.error && !job.error->isEmpty())
          return *job.error;
      }
      else if constexpr (std::is_same_v<std::decay_t<decltype(job.error)>, QString>)
      {
        if (!job.error.isEmpty())
          return job.error;
      }
    }
    return JobRunnerBase::tr("作业失败");
  }

  PaleoTaskService *m_taskService = nullptr;
};

} // namespace paleo::jobs
