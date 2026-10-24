// 层：视图
#include "faciespinnedlevellist.h"
#include "../domain/facieshierarchy.h"
#include <QComboBox>
#include <QGridLayout>
#include <QHeaderView>
#include <QLabel>
#include <QPushButton>
#include <QSignalBlocker>
#include <QTreeWidget>

FaciesPinnedLevelList::FaciesPinnedLevelList(QWidget *parent) : QWidget(parent) {
  auto *layout = new QGridLayout(this);
  layout->setContentsMargins(0, 0, 0, 0);
  auto *title = new QLabel(tr("固定显示层级的相类"), this);
  title->setTextFormat(Qt::PlainText);
  layout->addWidget(title, 0, 0, 1, 3);
  m_node = new QComboBox(this);
  m_node->setObjectName("faciesPinNode");
  m_node->setAccessibleName(tr("要固定显示层级的相类"));
  m_node->setSizeAdjustPolicy(QComboBox::AdjustToMinimumContentsLengthWithIcon);
  m_node->setMinimumContentsLength(8);
  layout->addWidget(m_node, 1, 0);
  m_level = levelBox(this);
  m_level->setObjectName("faciesPinLevel");
  m_level->setAccessibleName(tr("固定显示为"));
  layout->addWidget(m_level, 1, 1);
  m_add = new QPushButton(tr("固定"), this);
  m_add->setObjectName("faciesPinAdd");
  m_add->setToolTip(tr("把所选相类（含其下级）固定显示为所选层级；已固定的相类会改为新层级"));
  layout->addWidget(m_add, 1, 2);
  m_list = new QTreeWidget(this);
  m_list->setObjectName("faciesPinList");
  m_list->setAccessibleName(tr("已固定显示层级的相类"));
  m_list->setColumnCount(2);
  m_list->setHeaderLabels({tr("相类"), tr("固定显示为")});
  m_list->setRootIsDecorated(false);
  m_list->setSelectionMode(QAbstractItemView::SingleSelection);
  m_list->header()->setSectionResizeMode(0, QHeaderView::Stretch);
  m_list->header()->setSectionResizeMode(1, QHeaderView::ResizeToContents);
  m_list->setMaximumHeight(140);
  layout->addWidget(m_list, 2, 0, 1, 3);
  m_remove = new QPushButton(tr("移除固定"), this);
  m_remove->setObjectName("faciesPinRemove");
  m_remove->setToolTip(tr("所选相类恢复跟随图层显示层级"));
  layout->addWidget(m_remove, 3, 2);
  connect(m_add, &QPushButton::clicked, this, [this] {
    if (m_node->currentIndex() >= 0)
      emit pinRequested(m_node->currentData().toString(), m_level->currentData().toString());
  });
  connect(m_remove, &QPushButton::clicked, this, [this] {
    if (auto *item = m_list->currentItem())
      emit pinRequested(item->data(0, Qt::UserRole).toString(), QStringLiteral("auto"));
  });
  connect(m_list, &QTreeWidget::currentItemChanged, this, &FaciesPinnedLevelList::updateButtons);
  setState({}, {}, false, QString());
}
QComboBox *FaciesPinnedLevelList::levelBox(QWidget *parent) const {
  auto *box = new QComboBox(parent);
  box->addItem(tr("相"), "facies");
  box->addItem(tr("亚相"), "sub_facies");
  box->addItem(tr("微相"), "micro_facies");
  return box;
}
void FaciesPinnedLevelList::setState(const QVariantList &candidates, const QVariantMap &pins,
                                     bool enabled, const QString &hint) {
  const auto oldNode = m_node->currentData();
  const auto oldPin = m_list->currentItem() ? m_list->currentItem()->data(0, Qt::UserRole) : QVariant();
  const QSignalBlocker nodeBlock(m_node);
  const QSignalBlocker listBlock(m_list);
  m_node->clear();
  QMap<QString, QString> paths;
  for (const auto &value : candidates) {
    const auto node = value.toMap();
    const auto key = node.value("key").toString();
    paths.insert(key, node.value("path").toString());
    m_node->addItem(tr("%1（%2）").arg(node.value("path").toString(),
                                      FaciesHierarchy::title(node.value("level").toString())), key);
  }
  m_node->setCurrentIndex(qMax(0, m_node->findData(oldNode)));
  m_list->clear();
  for (auto it = pins.cbegin(); it != pins.cend(); ++it) {
    auto *item = new QTreeWidgetItem(m_list);
    // 节点已不在当前分类中时仍列出（标明），便于移除陈旧固定。
    item->setText(0, paths.contains(it.key()) ? paths.value(it.key())
                                              : tr("%1（不在当前分类中）").arg(it.key()));
    item->setData(0, Qt::UserRole, it.key());
    auto *level = levelBox(m_list);
    level->setObjectName("faciesPinRowLevel");
    level->setAccessibleName(tr("%1 的固定显示层级").arg(item->text(0)));
    level->setCurrentIndex(qMax(0, level->findData(it.value())));
    level->setEnabled(enabled);
    const auto &key = it.key();
    // 排队投递：处理意图会刷新本列表并删除该行的下拉框，不能在其自身信号中同步删除。
    connect(level, &QComboBox::activated, level, [this, level, key] {
      emit pinRequested(key, level->currentData().toString());
    }, Qt::QueuedConnection);
    m_list->setItemWidget(item, 1, level);
    if (item->data(0, Qt::UserRole) == oldPin)
      m_list->setCurrentItem(item);
  }
  for (auto *widget : {static_cast<QWidget *>(m_node), static_cast<QWidget *>(m_level),
                       static_cast<QWidget *>(m_list)})
    widget->setEnabled(enabled);
  setToolTip(hint);
  m_list->setToolTip(hint);
  updateButtons();
}
void FaciesPinnedLevelList::updateButtons() {
  m_add->setEnabled(m_node->isEnabled() && m_node->count() > 0);
  m_remove->setEnabled(m_list->isEnabled() && m_list->currentItem());
}
