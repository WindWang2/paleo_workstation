// 层：视图
#pragma once
#include <QVariantList>
#include <QWidget>
class MappingWorkbench;
class QLabel;
class QListWidget;
class QPlainTextEdit;
class QLineEdit;
class MappingEditBasis : public QWidget {
  Q_OBJECT
public:
  MappingEditBasis(MappingWorkbench *workbench, QWidget *parent = nullptr);
  void setHorizon(const QString &horizon);
  void setReferences(const QVariantList &references);
signals:
  void commandRequested(const QString &action, const QVariantMap &parameters);

private:
  void updateState();
  MappingWorkbench *m_workbench;
  QString m_horizon;
  QLabel *m_state;
  QListWidget *m_sources;
  QPlainTextEdit *m_text;
  QLineEdit *m_reason;
};
