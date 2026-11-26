// 层：视图
#pragma once
#include <QHash>
#include <QVariantList>
#include <QWidget>
class QCheckBox;
class QComboBox;
class QLabel;
class QPushButton;
class MappingWorkbench;

class ClusterPatchInspector : public QWidget {
  Q_OBJECT
  Q_PROPERTY(bool readOnly READ readOnly WRITE setReadOnly)
public:
  explicit ClusterPatchInspector(MappingWorkbench *workbench, QWidget *parent);
  bool inspect(const QString &layer, qint64 fid);
  void clearSelection();
  bool resolveDrafts();
  bool hasSelection() const { return !m_layer.isEmpty(); }
  bool readOnly() const { return m_readOnly; }
  void setReadOnly(bool value);
  bool canStep(bool undo) const;
  bool step(bool undo);
signals:
  void highlightRequested(const QList<qint64> &ids);
  void draftPreviewRequested(const QString &layer, const QVariantList &edits);
  void versionSaved(const QString &layer, qint64 fid);
  void historyChanged();
private:
  struct Draft {
    QString version;
    QVariantList schema;
    QVariantList edits;
    qsizetype cursor = 0;
  };
  void createEditor();
  void refresh();
  void updateScope();
  void apply();
  bool save(const QString &layer, bool activate);
  MappingWorkbench *m_workbench;
  QHash<QString, Draft> m_drafts;
  QString m_layer;
  QVariantMap m_row;
  QVariantList m_schema;
  quint64 m_scopeRequest = 0;
  QList<qint64> m_scopeIds;
  qint64 m_count = 0;
  bool m_readOnly = false;
  QLabel *m_heading, *m_source, *m_current, *m_evidence, *m_countLabel, *m_feedback;
  QWidget *m_editor;
  QCheckBox *m_whole;
  QComboBox *m_target;
  QPushButton *m_apply, *m_cancel, *m_undo, *m_redo, *m_save;
};
