// 层：视图
#pragma once
#include <QVariantList>
#include <QVariantMap>
#include <QWidget>
class QComboBox;
class QPushButton;
class QTreeWidget;
// 相类固定显示层级列表：把某个相／亚相／微相节点（含其下级）固定显示为
// 相、亚相或微相，不随图层显示层级与比例尺切换。只收集意图，不改图层。
class FaciesPinnedLevelList : public QWidget {
  Q_OBJECT
public:
  explicit FaciesPinnedLevelList(QWidget *parent = nullptr);
  // candidates: FaciesHierarchy::pinCandidates；pins: 节点 key -> 层级。
  void setState(const QVariantList &candidates, const QVariantMap &pins, bool enabled,
                const QString &hint);
signals:
  // mode 为 "auto" 表示移除该节点的固定。
  void pinRequested(const QString &nodeKey, const QString &mode);
private:
  QComboBox *levelBox(QWidget *parent) const;
  void updateButtons();
  QComboBox *m_node;
  QComboBox *m_level;
  QPushButton *m_add;
  QPushButton *m_remove;
  QTreeWidget *m_list;
};
