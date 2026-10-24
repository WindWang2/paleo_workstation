// 层：视图
#include "faciespinnedlevellist.h"
#include "../domain/facieshierarchy.h"
#include <QComboBox>
#include <QHash>
#include <QHeaderView>
#include <QLineEdit>
#include <QScrollBar>
#include <QSet>
#include <QSignalBlocker>
#include <QTreeWidget>
#include <QTreeWidgetItemIterator>
#include <QVBoxLayout>
#include <functional>
#include <qgscollapsiblegroupbox.h>

QgsCollapsibleGroupBox *FaciesPinnedLevelList::createGroup(QWidget *parent,
                                                           FaciesPinnedLevelList **out) {
  auto *group = new QgsCollapsibleGroupBox(tr("相类显示层级锁定"), parent);
  group->setObjectName("workbenchFaciesPins");
  group->setSaveCollapsedState(false);
  auto *layout = new QVBoxLayout(group);
  layout->addWidget(*out = new FaciesPinnedLevelList(group));
  return group;
}

FaciesPinnedLevelList::FaciesPinnedLevelList(QWidget *parent) : QWidget(parent) {
  auto *layout = new QVBoxLayout(this);
  layout->setContentsMargins(0, 0, 0, 0);
  m_filter = new QLineEdit(this);
  m_filter->setObjectName("faciesPinFilter");
  m_filter->setAccessibleName(tr("按名称筛选相类"));
  m_filter->setPlaceholderText(tr("筛选相类…"));
  m_filter->setClearButtonEnabled(true);
  layout->addWidget(m_filter);
  m_list = new QTreeWidget(this);
  m_list->setObjectName("faciesPinList");
  m_list->setAccessibleName(tr("相类显示层级锁定"));
  m_list->setColumnCount(2);
  m_list->setHeaderLabels({tr("相类"), tr("锁定层级")});
  m_list->setSelectionMode(QAbstractItemView::SingleSelection);
  m_list->header()->setSectionResizeMode(0, QHeaderView::Stretch);
  m_list->header()->setSectionResizeMode(1, QHeaderView::ResizeToContents);
  m_list->setMinimumHeight(160);
  m_list->setMaximumHeight(320);
  layout->addWidget(m_list);
  connect(m_filter, &QLineEdit::textChanged, this, &FaciesPinnedLevelList::applyFilter);
  setState({}, {}, false, QString());
}
QComboBox *FaciesPinnedLevelList::levelBox(QWidget *parent) const {
  auto *box = new QComboBox(parent);
  box->addItem(tr("自动"), "auto");
  for (const auto &level : FaciesHierarchy::levels())
    box->addItem(FaciesHierarchy::title(level), level);
  return box;
}
void FaciesPinnedLevelList::addRow(QTreeWidgetItem *item, const QString &key, const QString &mode,
                                   bool enabled) {
  item->setData(0, Qt::UserRole, key);
  const bool pinned = !mode.isEmpty();
  auto font = item->font(0);
  font.setBold(pinned);
  item->setFont(0, font);
  auto *level = levelBox(m_list);
  level->setObjectName("faciesPinRowLevel");
  level->setAccessibleName(tr("%1 的锁定层级").arg(item->text(0)));
  level->setCurrentIndex(qMax(0, level->findData(pinned ? mode : QStringLiteral("auto"))));
  level->setEnabled(enabled);
  const auto current = pinned ? mode : QStringLiteral("auto");
  // 排队投递：处理意图会刷新本列表并删除该行的下拉框，不能在其自身信号中同步删除。
  connect(level, &QComboBox::activated, level, [this, level, key, current] {
    const auto chosen = level->currentData().toString();
    if (chosen != current)
      emit pinRequested(key, chosen);
  }, Qt::QueuedConnection);
  m_list->setItemWidget(item, 1, level);
}
void FaciesPinnedLevelList::setState(const QVariantList &candidates, const QVariantMap &pins,
                                     bool enabled, const QString &hint) {
  const auto oldKey = m_list->currentItem() ? m_list->currentItem()->data(0, Qt::UserRole) : QVariant();
  const int scroll = m_list->verticalScrollBar()->value();
  const QSignalBlocker listBlock(m_list);
  m_list->clear();
  // pinCandidates 按路径排序，父节点（路径前缀）总在子节点之前。
  QHash<QString, QTreeWidgetItem *> byPath;
  QSet<QString> listed;
  for (const auto &value : candidates) {
    const auto node = value.toMap();
    const auto key = node.value("key").toString();
    const auto path = node.value("path").toString();
    const int cut = path.lastIndexOf(QStringLiteral(" / "));
    auto *parent = cut >= 0 ? byPath.value(path.left(cut)) : nullptr;
    auto *item = parent ? new QTreeWidgetItem(parent) : new QTreeWidgetItem(m_list);
    item->setText(0, cut >= 0 ? path.mid(cut + 3) : path);
    item->setToolTip(0, tr("%1（%2）").arg(path, FaciesHierarchy::title(node.value("level").toString())));
    byPath.insert(path, item);
    listed.insert(key);
    addRow(item, key, pins.value(key).toString(), enabled);
  }
  // 节点已不在当前分类中的固定仍列在末尾（标明），便于改回自动以移除。
  for (auto it = pins.cbegin(); it != pins.cend(); ++it) {
    if (listed.contains(it.key()))
      continue;
    auto *item = new QTreeWidgetItem(m_list);
    item->setText(0, tr("%1（不在当前分类中）").arg(it.key()));
    addRow(item, it.key(), it.value().toString(), enabled);
  }
  m_list->expandAll();
  for (QTreeWidgetItemIterator it(m_list); *it; ++it)
    if ((*it)->data(0, Qt::UserRole) == oldKey)
      m_list->setCurrentItem(*it);
  applyFilter();
  m_list->verticalScrollBar()->setValue(scroll);
  m_filter->setEnabled(enabled);
  m_list->setEnabled(enabled);
  setToolTip(hint);
  m_list->setToolTip(hint);
}
void FaciesPinnedLevelList::applyFilter() {
  const auto text = m_filter->text().trimmed();
  // 命中项及其祖先可见；父节点命中时整棵子树可见。
  std::function<bool(QTreeWidgetItem *, bool)> visit = [&](QTreeWidgetItem *item, bool ancestorHit) {
    const bool hit = ancestorHit || text.isEmpty() || item->text(0).contains(text, Qt::CaseInsensitive);
    bool any = hit;
    for (int i = 0; i < item->childCount(); ++i)
      any |= visit(item->child(i), hit);
    item->setHidden(!any);
    return any;
  };
  for (int i = 0; i < m_list->topLevelItemCount(); ++i)
    visit(m_list->topLevelItem(i), false);
}
