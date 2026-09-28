// 层：视图
#include "taskpanel.h"

#include "../metadata/paleoprojectstore.h"
#include "../services/paleotaskservice.h"

#include <QLabel>
#include <QProgressBar>
#include <QPushButton>
#include <QSet>
#include <QTimer>
#include <QTreeWidget>
#include <QTreeWidgetItem>
#include <QVBoxLayout>

TaskPanel::TaskPanel(PaleoProjectStore *store, PaleoTaskService *tasks,
                     QWidget *parent)
    : QWidget(parent)
    , m_store(store)
    , m_tasks(tasks)
{
  setObjectName(QStringLiteral("taskPanel"));
  auto *lay = new QVBoxLayout(this);
  lay->setContentsMargins(6, 6, 6, 6);
  lay->setSpacing(4);

  auto *topRow = new QHBoxLayout;
  auto *countLabel = new QLabel(this);
  countLabel->setObjectName(QStringLiteral("busyCountLabel"));
  topRow->addWidget(countLabel, 1);
  auto *clearBtn = new QPushButton(tr("清除已完成"), this);
  clearBtn->setObjectName(QStringLiteral("clearFinishedBtn"));
  topRow->addWidget(clearBtn);
  lay->addLayout(topRow);

  auto *list = new QTreeWidget(this);
  list->setObjectName(QStringLiteral("busyList"));
  list->setHeaderLabels({tr("图层"), tr("任务"),
                         tr("进度"), tr("剩余"),
                         tr("状态")});
  list->setRootIsDecorated(false);
  list->setColumnWidth(0, 160);
  list->setColumnWidth(1, 220);
  list->setColumnWidth(2, 120);
  lay->addWidget(list, 1);

  auto *empty = new QLabel(tr("（无运行中的任务）"), this);
  empty->setObjectName(QStringLiteral("busyEmptyHint"));
  lay->addWidget(empty);

  if (m_tasks)
  {
    connect(clearBtn, &QPushButton::clicked, m_tasks,
            &PaleoTaskService::clearFinished);
    connect(m_tasks, &PaleoTaskService::tasksChanged, this,
            &TaskPanel::refresh);
  }
  else
    clearBtn->setVisible(false);

  // Poll: the busy registry is a plain hash with no change signal — a light
  // timer keeps the view honest without coupling the store to UI concerns.
  auto *timer = new QTimer(this);
  timer->setInterval(500);
  connect(timer, &QTimer::timeout, this, &TaskPanel::refresh);
  timer->start();

  refresh();
}

QTreeWidgetItem *TaskPanel::rowForTask(qint64 id)
{
  auto *list = findChild<QTreeWidget *>(QStringLiteral("busyList"));
  if (!list)
    return nullptr;
  auto it = m_taskRows.constFind(id);
  if (it != m_taskRows.constEnd())
    return it.value();
  auto *row = new QTreeWidgetItem;
  list->addTopLevelItem(row);
  m_taskRows.insert(id, row);
  return row;
}

void TaskPanel::updateTaskRow(QTreeWidgetItem *row, PaleoTask *task)
{
  auto *list = findChild<QTreeWidget *>(QStringLiteral("busyList"));
  if (!row || !task)
    return;

  row->setText(0, task->layerId().isEmpty() ? QStringLiteral("—")
                                          : task->layerId());
  row->setText(1, task->title());
  row->setText(3, task->running() ? task->etaText() : QString());

  // Progress column: indeterminate tasks get a marquee bar.
  auto *bar = qobject_cast<QProgressBar *>(list->itemWidget(row, 2));
  if (!bar)
  {
    bar = new QProgressBar(list);
    bar->setObjectName(QStringLiteral("taskProgress"));
    bar->setTextVisible(true);
    list->setItemWidget(row, 2, bar);
  }
  const int pct = task->percent();
  if (pct < 0 && task->running())
  {
    bar->setRange(0, 0);
  }
  else
  {
    bar->setRange(0, 100);
    bar->setValue(pct);
  }

  // Status column: running → cancel button; finished → plain state text.
  if (task->running())
  {
    auto *btn = qobject_cast<QPushButton *>(list->itemWidget(row, 4));
    if (!btn)
    {
      btn = new QPushButton(tr("取消"), list);
      btn->setObjectName(QStringLiteral("taskCancelBtn"));
      list->setItemWidget(row, 4, btn);
      connect(btn, &QPushButton::clicked, task,
              &PaleoTask::requestCancel);
      connect(btn, &QPushButton::clicked, btn, [btn]() {
        btn->setEnabled(false);
        btn->setText(tr("取消中"));
      });
    }
    row->setText(4, task->cancelRequested() ? tr("取消中")
                                          : task->detailText());
  }
  else
  {
    list->removeItemWidget(row, 4);
    switch (task->state())
    {
    case PaleoTask::State::Succeeded:
      row->setText(4, tr("完成"));
      break;
    case PaleoTask::State::Failed:
      row->setText(4, task->errorText().isEmpty() ? tr("失败")
                                                  : tr("失败：%1").arg(task->errorText()));
      break;
    case PaleoTask::State::Cancelled:
      row->setText(4, tr("已取消"));
      break;
    default:
      break;
    }
  }
}

void TaskPanel::rebuildBusyRows()
{
  auto *list = findChild<QTreeWidget *>(QStringLiteral("busyList"));
  if (!list)
    return;

  // Busy entries owned by a service task ("task-N" id) are shown on the task
  // row itself — skip them here to avoid double rows.
  QSet<QString> taskOwned;
  if (m_tasks)
    for (const PaleoTask *t : m_tasks->tasks())
      taskOwned.insert(QStringLiteral("task-%1").arg(t->id()));

  const QVector<PaleoProjectStore::BusyEntry> busy =
      m_store ? m_store->busyLayers()
              : QVector<PaleoProjectStore::BusyEntry>();
  QSet<QString> seen;
  int unowned = 0;
  for (const auto &e : busy)
  {
    if (taskOwned.contains(e.taskId))
      continue;
    ++unowned;
    seen.insert(e.layerId);
    auto *row = m_busyRows.value(e.layerId, nullptr);
    if (!row)
    {
      row = new QTreeWidgetItem;
      list->addTopLevelItem(row);
      m_busyRows.insert(e.layerId, row);
    }
    row->setText(0, e.layerId);
    row->setText(1, e.taskId);
    row->setText(4, e.reason);
  }
  for (auto it = m_busyRows.begin(); it != m_busyRows.end();)
    if (!seen.contains(it.key()))
    {
      delete it.value();
      it = m_busyRows.erase(it);
    }
    else
      ++it;

  auto *count = findChild<QLabel *>(QStringLiteral("busyCountLabel"));
  if (count)
    count->setText(tr("忙图层：%1").arg(unowned));
}

void TaskPanel::refresh()
{
  auto *list = findChild<QTreeWidget *>(QStringLiteral("busyList"));
  auto *empty = findChild<QLabel *>(QStringLiteral("busyEmptyHint"));
  if (!list || !empty)
    return;

  QSet<qint64> seenTasks;
  if (m_tasks)
    for (PaleoTask *t : m_tasks->tasks())
    {
      seenTasks.insert(t->id());
      updateTaskRow(rowForTask(t->id()), t);
    }
  for (auto it = m_taskRows.begin(); it != m_taskRows.end();)
    if (!seenTasks.contains(it.key()))
    {
      delete it.value();
      it = m_taskRows.erase(it);
    }
    else
      ++it;

  rebuildBusyRows();
  empty->setVisible(list->topLevelItemCount() == 0);
}
