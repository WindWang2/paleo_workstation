// 层：视图
#pragma once
#include <QVariantList>
#include <QVariantMap>
#include <QWidget>
class QComboBox;
class QgsCollapsibleGroupBox;
class QLineEdit;
class QTreeWidget;
class QTreeWidgetItem;
// 相类显示层级锁定：按相 / 亚相 / 微相树列出当前分类的全部相类，每行一个
// 「自动 / 相 / 亚相 / 微相」下拉框；选非自动即把该节点（含其下级）固定显示
// 为所选层级，选自动即移除固定。只收集意图，不改图层。
class FaciesPinnedLevelList : public QWidget {
  Q_OBJECT
public:
  // 「相类显示层级锁定」分组（默认展开、不记忆折叠态），列表经 out 交回。
  static QgsCollapsibleGroupBox *createGroup(QWidget *parent, FaciesPinnedLevelList **out);
  explicit FaciesPinnedLevelList(QWidget *parent = nullptr);
  // candidates: FaciesHierarchy::pinCandidates；pins: 节点 key -> 层级。
  void setState(const QVariantList &candidates, const QVariantMap &pins, bool enabled,
                const QString &hint);
signals:
  // mode 为 "auto" 表示移除该节点的固定。
  void pinRequested(const QString &nodeKey, const QString &mode);
private:
  QComboBox *levelBox(QWidget *parent) const;
  void addRow(QTreeWidgetItem *item, const QString &key, const QString &mode, bool enabled);
  void applyFilter();
  QLineEdit *m_filter;
  QTreeWidget *m_list;
};
