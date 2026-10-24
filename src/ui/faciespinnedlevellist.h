// 层：视图
#pragma once
#include <QVariantList>
#include <QVariantMap>
#include <QWidget>
class QComboBox;
class QGroupBox;
class QLabel;
class QTreeWidget;
class QTreeWidgetItem;
// 固定显示层级的相类列表：只列已固定的相类（在地图上右键相面 →「固定显示层级」
// 添加），每行可改固定层级或删除固定。只收集意图，不改图层。
class FaciesPinnedLevelList : public QWidget {
  Q_OBJECT
public:
  // 「固定显示层级的相类」分组（默认展开、不记忆折叠态），列表经 out 交回。
  static QGroupBox *createGroup(QWidget *parent, FaciesPinnedLevelList **out);
  explicit FaciesPinnedLevelList(QWidget *parent = nullptr);
  // candidates: FaciesHierarchy::pinCandidates；pins: 节点 key -> 层级。
  void setState(const QVariantList &candidates, const QVariantMap &pins, bool enabled,
                const QString &hint);
signals:
  // mode 为 "auto" 表示移除该节点的固定。
  void pinRequested(const QString &nodeKey, const QString &mode);
private:
  void addRow(QTreeWidgetItem *item, const QString &key, const QString &mode, bool enabled);
  QLabel *m_empty;
  QTreeWidget *m_list;
};
