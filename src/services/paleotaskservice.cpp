// 层：数据
#include "paleotaskservice.h"

#include "../metadata/paleoprojectstore.h"

#include <QMetaObject>
#include <QPointer>
#include <QThreadPool>

namespace
{
// ETA window: only samples inside the trailing 10 s count toward the rate —
// matches the D2 spec (「10s byte-linear ETA」).
constexpr qint64 kEtaWindowMs = 10 * 1000;

class TaskRunner : public QRunnable
{
public:
  TaskRunner(PaleoTask *task, std::function<QString(PaleoTask *)> work)
      : m_task(task), m_work(std::move(work))
  {
  }

  void run() override
  {
    PaleoTask *t = m_task;
    if (!t)
      return;
    QString error;
    try
    {
      if (m_work)
        error = m_work(t);
    }
    catch (const std::exception &e)
    {
      error = QString::fromLocal8Bit(e.what());
    }
    catch (...)
    {
      error = QStringLiteral("任务内部异常");
    }
    // Task objects are never deleted while running (service keeps them until
    // clearFinished), but a shutdown may orphan the event loop — the queued
    // invoke is simply dropped then. applyFinish is private/Q_INVOKABLE —
    // string-based invoke reaches it without friending the runner.
    QMetaObject::invokeMethod(t, "applyFinish", Qt::QueuedConnection,
                              Q_ARG(QString, error));
  }

private:
  QPointer<PaleoTask> m_task;
  std::function<QString(PaleoTask *)> m_work;
};
} // namespace

PaleoTask::PaleoTask(qint64 id, const QString &title, const QString &layerId,
                     QObject *parent)
    : QObject(parent), m_id(id), m_title(title), m_layerId(layerId)
{
  m_clock.start();
}

int PaleoTask::percent() const
{
  if (m_state == State::Succeeded)
    return 100;
  if (m_bytesTotal > 0)
    return static_cast<int>(m_bytesDone * 100 / m_bytesTotal);
  return -1;
}

QString PaleoTask::etaText() const
{
  if (!running() || m_bytesTotal <= 0 || m_rateBytesPerSec <= 0.0)
    return QStringLiteral("--");
  const qint64 remaining = m_bytesTotal - m_bytesDone;
  const double secs = remaining / m_rateBytesPerSec;
  if (secs >= 90.0)
    return tr("约 %1min").arg(QString::number(secs / 60.0, 'f', 1));
  return tr("约 %1s").arg(qMax(1, qRound(secs)));
}

void PaleoTask::reportBytes(qint64 done, qint64 total)
{
  QMetaObject::invokeMethod(
      this, [this, done, total]() { applyProgress(done, total); },
      Qt::QueuedConnection);
}

void PaleoTask::reportDetail(const QString &detail)
{
  QMetaObject::invokeMethod(
      this, [this, detail]() { applyDetail(detail); }, Qt::QueuedConnection);
}

void PaleoTask::reportStage(const QString &stage, int percent)
{
  QMetaObject::invokeMethod(
      this, [this, stage, percent]() { applyStage(stage, percent); },
      Qt::QueuedConnection);
}

void PaleoTask::applyProgress(qint64 done, qint64 total)
{
  m_bytesDone = done;
  m_bytesTotal = total;

  const qint64 now = m_clock.elapsed();
  m_etaSamples.emplace_back(now, done);
  while (m_etaSamples.size() > 2 &&
         now - m_etaSamples.front().first > kEtaWindowMs)
    m_etaSamples.pop_front();
  if (m_etaSamples.size() >= 2)
  {
    const auto &front = m_etaSamples.front();
    const double secs = (now - front.first) / 1000.0;
    m_rateBytesPerSec =
        secs > 0.0 ? (done - front.second) / secs : m_rateBytesPerSec;
  }
  emit changed();
}

void PaleoTask::applyDetail(const QString &detail)
{
  m_detail = detail;
  emit changed();
}

void PaleoTask::applyStage(const QString &stage, int percent)
{
  m_stage = stage;
  m_stagePercent = percent;
  emit changed();
}

void PaleoTask::applyFinish(const QString &error)
{
  if (m_cancel.load())
    m_state = State::Cancelled;
  else if (!error.isEmpty())
  {
    m_state = State::Failed;
    m_error = error;
  }
  else
    m_state = State::Succeeded;
  emit changed();
  emit finished();
  if (!parent())
    deleteLater();
}

// ---------------------------------------------------------------------------

PaleoTaskService::PaleoTaskService(PaleoProjectStore *store, QObject *parent)
    : QObject(parent), m_store(store)
{
  // D4.5：专用池 ≤4 工作线程——不占 globalInstance 默认（UI 侧仍有自己的
  // 池面），也不被 seismic 任务服务（P5 领地）牵连。
  m_pool = new QThreadPool(); // 无父：析构策略见 ~PaleoTaskService
  m_pool->setMaxThreadCount(4);
  m_pool->setExpiryTimeout(30 * 1000);
}

PaleoTaskService::~PaleoTaskService()
{
  // Orphan still-running tasks instead of deleting under their workers:
  // request cancel and detach so the queued finish lands harmlessly during
  // teardown. Finished rows die normally with the service.
  for (PaleoTask *t : m_tasks)
    if (t->running())
    {
      t->requestCancel();
      t->setParent(nullptr);
    }
  // D4.5：清空未开始的排队（运行中的协作取消已发）；池不再 delete——
  // waitForDone 会卡住仍在跑的长任务，孤儿化池（线程随 expiry 退出）与
  // 任务的孤儿化语义一致。
  m_pool->clear();
  m_pool->setParent(nullptr);
}

PaleoTask *PaleoTaskService::start(const QString &title,
                                   std::function<QString(PaleoTask *)> work,
                                   const QString &layerId)
{
  auto *task = new PaleoTask(m_nextId++, title, layerId, this);
  m_tasks.append(task);
  if (m_store && !layerId.isEmpty())
    m_store->markLayerBusy(layerId, QStringLiteral("task-%1").arg(task->id()),
                           title);
  connect(task, &PaleoTask::finished, this,
          [this, task]() { onTaskFinished(task); });
  emit taskAdded(task);
  emit tasksChanged();
  m_pool->start(new TaskRunner(task, std::move(work)));
  return task;
}

PaleoTask *PaleoTaskService::start(const QString &title,
                                   std::function<QString(PaleoTask *)> work,
                                   const QString &layerId, PaleoTask::Priority priority)
{
  auto *task = new PaleoTask(m_nextId++, title, layerId, this);
  m_tasks.append(task);
  if (m_store && !layerId.isEmpty())
    m_store->markLayerBusy(layerId, QStringLiteral("task-%1").arg(task->id()),
                           title);
  connect(task, &PaleoTask::finished, this,
          [this, task]() { onTaskFinished(task); });
  emit taskAdded(task);
  emit tasksChanged();
  // QThreadPool 优先级：数值大者先出队（D4.6 预取 Low 不挡用户点击 High）。
  m_pool->start(new TaskRunner(task, std::move(work)), static_cast<int>(priority));
  return task;
}

int PaleoTaskService::maxWorkerThreads() const
{
  return m_pool->maxThreadCount();
}

void PaleoTaskService::setMaxWorkerThreads(int n)
{
  m_pool->setMaxThreadCount(qBound(1, n, 8));
}

int PaleoTaskService::runningCount() const
{
  int n = 0;
  for (const PaleoTask *t : m_tasks)
    if (t->running())
      ++n;
  return n;
}

void PaleoTaskService::onTaskFinished(PaleoTask *task)
{
  if (m_store && !task->layerId().isEmpty())
    m_store->markLayerFree(task->layerId());
  emit tasksChanged();
}

void PaleoTaskService::clearFinished()
{
  for (int i = m_tasks.size() - 1; i >= 0; --i)
    if (!m_tasks[i]->running())
    {
      m_tasks[i]->deleteLater();
      m_tasks.removeAt(i);
    }
  emit tasksChanged();
}
