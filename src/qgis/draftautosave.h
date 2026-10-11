// 层：QGIS 封装
#pragma once
// facies-ux 4：工作副本（draft.*）属性修订随时自动保存。
//
// 规则：
// - 只在图层已处于编辑态时提交（编辑锁归编辑工具栏 / beginEdit 所有，本类
//   从不 startEditing / 结束编辑）；
// - 只自动提交「纯属性修改」（改相、改分类字段）：commitChanges(false)
//   原子写回工作副本文件并保持编辑态；几何增删改仍留在编辑缓冲，保留
//   QGIS 撤销栈，由「保存编辑」统一提交；
// - 提交结果必查：失败时编辑缓冲保留、发 failed()，不吞错；
// - 提交会清空 QGIS 撤销栈，因此自带属性撤销历史（旧值取自提交前的数据
//   源），undo() 以一条新的属性修改写回旧值并同样自动保存。
#include <QList>
#include <QObject>
#include <QPointer>
#include <QVariant>

class QgsVectorLayer;

class DraftAutosave : public QObject {
  Q_OBJECT
public:
  // 幂等：同一图层只挂一个（作为图层子对象，随图层销毁）。
  static DraftAutosave *attach(QgsVectorLayer *layer);
  static DraftAutosave *of(QgsVectorLayer *layer);
  bool canUndo() const { return !m_history.isEmpty(); }
  bool undo(QString *error = nullptr);
  // 立即尝试提交（测试与「保存」前同步调用）；返回是否有内容被提交。
  bool flush();
  QString lastError() const { return m_lastError; }

signals:
  void saved(int changedFeatures);
  void failed(const QString &error);
  void historyChanged();

private:
  explicit DraftAutosave(QgsVectorLayer *layer);
  void schedule();
  struct Change {
    qint64 fid;
    int field;
    QVariant before;
  };
  QPointer<QgsVectorLayer> m_layer;
  QList<QList<Change>> m_history;
  bool m_pending = false;
  bool m_undoing = false;
  QString m_lastError;
};
