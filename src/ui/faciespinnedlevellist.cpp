// 层：视图
#include "faciespinnedlevellist.h"
#include "../domain/facieshierarchy.h"
#include "../qgis/collapsiblegroup.h"
#include <QGroupBox>
#include <QComboBox>
#include <QHeaderView>
#include <QLabel>
#include <QScrollBar>
#include <QSignalBlocker>
#include <QToolButton>
#include <QTreeWidget>
#include <QVBoxLayout>

QGroupBox *FaciesPinnedLevelList::createGroup(QWidget *parent, FaciesPinnedLevelList **out) {
  // 经 src/qgis 封装创建（ui 不直连 qgs 头，#699 回归）。
  auto *group = paleo::collapsible::create(tr("固定显示层级的相类"), parent, "workbenchFaciesPins", false);
  auto *layout = new QVBoxLayout(group);
  layout->addWidget(*out = new FaciesPinnedLevelList(group));
  return group;
}

FaciesPinnedLevelList::FaciesPinnedLevelList(QWidget *parent) : QWidget(parent) {
  auto *layout = new QVBoxLayout(this);
  layout->setContentsMargins(0, 0, 0, 0);
  m_empty = new QLabel(tr("暂无固定。在地图上右键相面，选「固定显示层级」即可添加。"), this);
  m_empty->setObjectName("faciesPinEmpty");
  m_empty->setTextFormat(Qt::PlainText);
  m_empty->setWordWrap(true);
  layout->addWidget(m_empty);
  m_list = new QTreeWidget(this);
  m_list->setObjectName("faciesPinList");
  m_list->setAccessibleName(tr("固定显示层级的相类"));
  m_list->setColumnCount(3);
  m_list->setHeaderLabels({tr("相类"), tr("固定显示为"), QString()});
  m_list->setRootIsDecorated(false);
  m_list->setSelectionMode(QAbstractItemView::SingleSelection);
  m_list->header()->setSectionResizeMode(0, QHeaderView::Stretch);
  m_list->header()->setSectionResizeMode(1, QHeaderView::ResizeToContents);
  m_list->header()->setSectionResizeMode(2, QHeaderView::ResizeToContents);
  m_list->setMaximumHeight(200);
  layout->addWidget(m_list);
  setState({}, {}, false, QString());
}
void FaciesPinnedLevelList::addRow(QTreeWidgetItem *item, const QString &key, const QString &mode,
                                   bool enabled) {
  item->setData(0, Qt::UserRole, key);
  auto *level = new QComboBox(m_list);
  for (const auto &value : FaciesHierarchy::levels())
    level->addItem(FaciesHierarchy::title(value), value);
  level->setObjectName("faciesPinRowLevel");
  level->setAccessibleName(tr("%1 的固定显示层级").arg(item->text(0)));
  level->setCurrentIndex(qMax(0, level->findData(mode)));
  level->setEnabled(enabled);
  // 排队投递：处理意图会刷新本列表并删除本行控件，不能在其自身信号中同步删除。
  connect(level, &QComboBox::activated, level, [this, level, key, mode] {
    const auto chosen = level->currentData().toString();
    if (chosen != mode)
      emit pinRequested(key, chosen);
  }, Qt::QueuedConnection);
  m_list->setItemWidget(item, 1, level);
  auto *remove = new QToolButton(m_list);
  remove->setObjectName("faciesPinRowRemove");
  remove->setText(tr("删除"));
  remove->setToolTip(tr("删除此固定，恢复跟随图层显示层级"));
  remove->setAccessibleName(tr("删除 %1 的固定").arg(item->text(0)));
  remove->setEnabled(enabled);
  connect(remove, &QToolButton::clicked, remove, [this, key] {
    emit pinRequested(key, QStringLiteral("auto"));
  }, Qt::QueuedConnection);
  m_list->setItemWidget(item, 2, remove);
}
void FaciesPinnedLevelList::setState(const QVariantList &candidates, const QVariantMap &pins,
                                     bool enabled, const QString &hint) {
  const auto oldKey = m_list->currentItem() ? m_list->currentItem()->data(0, Qt::UserRole) : QVariant();
  const int scroll = m_list->verticalScrollBar()->value();
  const QSignalBlocker listBlock(m_list);
  m_list->clear();
  QMap<QString, QVariantMap> nodes;
  for (const auto &value : candidates)
    nodes.insert(value.toMap().value("key").toString(), value.toMap());
  for (auto it = pins.cbegin(); it != pins.cend(); ++it) {
    auto *item = new QTreeWidgetItem(m_list);
    const auto node = nodes.value(it.key());
    // 节点已不在当前分类中时仍列出（标明），便于删除陈旧固定。
    item->setText(0, node.isEmpty() ? tr("%1（不在当前分类中）").arg(it.key())
                                    : node.value("path").toString());
    if (!node.isEmpty())
      item->setToolTip(0, tr("%1（%2）").arg(node.value("path").toString(),
                                            FaciesHierarchy::title(node.value("level").toString())));
    addRow(item, it.key(), it.value().toString(), enabled);
    if (item->data(0, Qt::UserRole) == oldKey)
      m_list->setCurrentItem(item);
  }
  m_list->verticalScrollBar()->setValue(scroll);
  m_list->setVisible(!pins.isEmpty());
  m_empty->setVisible(pins.isEmpty());
  m_list->setEnabled(enabled);
  setToolTip(hint);
  m_list->setToolTip(hint);
}
