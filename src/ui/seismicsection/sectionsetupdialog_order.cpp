// 层：视图
// SectionSetupDialog 部分类拆分（datalist_* 先例）：吸附井序落盘与勾选查询。
// applyWellOrder 供壳在画线吸附完成（≥2 口井）或「按空间关系重排」后回写：
// 给定序置顶并勾选，其余井保持当前相对序、置未勾选——不增删井行。
#include "sectionsetupdialog.h"
#include <QListWidget>
#include <QMap>
#include <QPushButton>
#include <QSet>
#include <QSignalBlocker>
#include <QVariantMap>

void SectionSetupDialog::applyWellOrder(const QStringList &ids) {
  QMap<QString, QVariantMap> rows;
  QStringList rest;
  for (int i = 0; i < m_wells->count(); ++i) {
    auto *item = m_wells->item(i);
    const auto row = item->data(Qt::UserRole).toMap();
    const QString id = row.value("id").toString();
    rows[id] = row;
    if (!ids.contains(id))
      rest << id;
  }
  const QSignalBlocker block(m_wells);
  m_wells->clear();
  auto addRow = [this, &rows](const QString &id, bool checked) {
    const auto row = rows.value(id);
    auto *item = new QListWidgetItem(row.value("name").toString(), m_wells);
    item->setData(Qt::UserRole, row);
    item->setToolTip(row.value("status").toString());
    item->setCheckState(checked ? Qt::Checked : Qt::Unchecked);
    if (!row.value("coordinates").toBool()) {
      item->setFlags(item->flags() & ~Qt::ItemIsUserCheckable);
      item->setToolTip(tr("缺少有效井口坐标"));
    }
  };
  // 去重（保首次出现序）：调用方传入的吸附井序理论上已去重，但折线回环等
  // 路径仍可能带来重复 id——重复项会让同一口井在列表里出现两行（并双计
  // 勾选数），故在此兜底，与「不增删井行」契约一致（一行一井）。
  QSet<QString> added;
  for (const QString &id : ids)
    if (rows.contains(id) && !added.contains(id)) {
      addRow(id, true);
      added.insert(id);
    }
  for (const QString &id : rest)
    addRow(id, false);
  if (m_wells->count())
    m_wells->setCurrentRow(0);
  showWell();
  // 与 setWells 末尾同口径：Blocker 抑制了 itemChanged，按钮门控手工刷新。
  int checkedCount = 0;
  for (int i = 0; i < m_wells->count(); ++i)
    checkedCount += m_wells->item(i)->checkState() == Qt::Checked;
  findChild<QPushButton *>("buildWellSection")->setEnabled(checkedCount >= 2);
  if (auto *reorderBtn = findChild<QPushButton *>("spatialReorderBtn"))
    reorderBtn->setEnabled(checkedCount >= 2);
}

QStringList SectionSetupDialog::checkedWellIds() const {
  QStringList ids;
  for (int i = 0; i < m_wells->count(); ++i)
    if (m_wells->item(i)->checkState() == Qt::Checked)
      ids << m_wells->item(i)->data(Qt::UserRole).toMap().value("id").toString();
  return ids;
}
