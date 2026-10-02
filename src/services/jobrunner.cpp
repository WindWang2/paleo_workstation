// 层：数据
#include "jobrunner.h"

#include <QDateTime>
#include <QMetaObject>
#include <QThread>

#include <utility>

namespace paleo::jobs {

QString dropReasonText(DropReason reason)
{
  switch (reason)
  {
    case DropReason::Cancelled:
      return JobRunnerBase::tr("已取消");
    case DropReason::Stale:
      return JobRunnerBase::tr("结果已过期");
    case DropReason::PrepareFailed:
      return JobRunnerBase::tr("准备失败");
  }
  return JobRunnerBase::tr("已丢弃");
}

JobRunnerBase::JobRunnerBase(QObject *dispatcher, QThread *ownerThread)
    : QObject(dispatcher), m_dispatcher(dispatcher),
      m_ownerThread(ownerThread ? ownerThread : QThread::currentThread())
{
  // dispatcher 必须与 owner 线程同线程，否则 commit 段排队回去等于没排。
  Q_ASSERT(dispatcher);
  Q_ASSERT(m_dispatcher->thread() == m_ownerThread);
}

JobRunnerBase::~JobRunnerBase() = default;

quint64 JobRunnerBase::currentGeneration() const
{
  return m_shared->generation.load();
}

bool JobRunnerBase::busy() const
{
  return !m_task.isNull();
}

PaleoTask *JobRunnerBase::currentTask() const
{
  return m_task;
}

void JobRunnerBase::requestCancel()
{
  if (m_cancel)
    m_cancel->store(true);
  if (m_task)
    m_task->requestCancel();
}

JobRunnerBase::Claim JobRunnerBase::claim()
{
  assertOwnerThread("JobRunnerBase::claim");
  Claim c;
  // Claim 与 JobRunner 共享同一计数器对象：worker 侧读 Claim，owner 侧读
  // m_shared，两边同一份状态。取消标志每代新开，不跨代残留。
  c.shared = m_shared;
  c.cancel = std::make_shared<std::atomic_bool>(false);
  m_cancel = c.cancel;
  c.myGeneration = m_shared->generation.fetch_add(1) + 1;
  return c;
}

bool JobRunnerBase::isCurrent(quint64 generation) const
{
  return m_shared->generation.load() == generation;
}

void JobRunnerBase::scheduleOnOwner(std::function<void()> fn)
{
  if (!m_dispatcher || !fn)
    return;
  QMetaObject::invokeMethod(
      m_dispatcher,
      [fn = std::move(fn)]() mutable { fn(); },
      Qt::QueuedConnection);
}

void JobRunnerBase::assertOwnerThread(const char *where) const
{
  // NDEBUG 下 Q_ASSERT_X 被编译掉，where 会变成未引用形参——显式吞掉，
  // 免得引入与文件其余部分不一致的新警告。
  Q_ASSERT_X(m_ownerThread && QThread::currentThread() == m_ownerThread,
             "paleo::jobs::JobRunner", where);
  Q_UNUSED(where);
}

void JobRunnerBase::clearTask()
{
  m_task = nullptr;
  m_cancel.reset();
}

void JobRunnerBase::setCurrentTask(PaleoTask *task)
{
  m_task = task;
}

} // namespace paleo::jobs
