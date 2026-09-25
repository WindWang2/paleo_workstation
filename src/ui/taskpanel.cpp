#include "taskpanel.h"

#include "../metadata/paleoprojectstore.h"

#include <QLabel>
#include <QTimer>
#include <QTreeWidget>
#include <QTreeWidgetItem>
#include <QVBoxLayout>

TaskPanel::TaskPanel(PaleoProjectStore *store, QWidget *parent)
  : QWidget(parent)
  , m_store(store)
{
  setObjectName(QStringLiteral("taskPanel"));
  auto *lay = new QVBoxLayout(this);
  lay->setContentsMargins(6, 6, 6, 6);
  lay->setSpacing(4);

  auto *countLabel = new QLabel(this);
  countLabel->setObjectName(QStringLiteral("busyCountLabel"));
  lay->addWidget(countLabel);

  auto *list = new QTreeWidget(this);
  list->setObjectName(QStringLiteral("busyList"));
  list->setHeaderLabels({QStringLiteral("图层"), QStringLiteral("任务"),
                         QStringLiteral("原因")});
  list->setRootIsDecorated(false);
  lay->addWidget(list, 1);

  auto *empty = new QLabel(QStringLiteral("（无运行中的任务）"), this);
  empty->setObjectName(QStringLiteral("busyEmptyHint"));
  lay->addWidget(empty);

  // Poll: the registry is a plain hash, no change signal — a light timer keeps
  // the view honest without coupling the store to UI concerns.
  auto *timer = new QTimer(this);
  timer->setInterval(500);
  connect(timer, &QTimer::timeout, this, &TaskPanel::refresh);
  timer->start();

  refresh();
}

void TaskPanel::refresh()
{
  auto *list = findChild<QTreeWidget *>(QStringLiteral("busyList"));
  auto *countLabel = findChild<QLabel *>(QStringLiteral("busyCountLabel"));
  auto *empty = findChild<QLabel *>(QStringLiteral("busyEmptyHint"));
  if (!list || !countLabel || !empty)
    return;

  list->clear();
  const QVector<PaleoProjectStore::BusyEntry> busy =
      m_store ? m_store->busyLayers() : QVector<PaleoProjectStore::BusyEntry>();
  for (const auto &e : busy)
    list->addTopLevelItem(new QTreeWidgetItem(
        QStringList{e.layerId, e.taskId, e.reason}));
  countLabel->setText(QStringLiteral("忙图层：%1").arg(busy.size()));
  empty->setVisible(busy.isEmpty());
}
