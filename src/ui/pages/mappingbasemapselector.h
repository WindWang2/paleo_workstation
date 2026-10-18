// 层：视图
#pragma once
#include <QWidget>
#include <QVariantMap>
class MappingWorkbench;
class QComboBox;
class QLineEdit;
class QLabel;
class QPushButton;
class MappingBaseMapSelector : public QWidget {
  Q_OBJECT
public:
  MappingBaseMapSelector(MappingWorkbench *workbench, QWidget *parent = nullptr);
  void setHorizon(const QString &horizon);
signals:
  void commandRequested(const QString &action, const QVariantMap &parameters);
private:
  void refresh(); void updateState(); void issue(bool start);
  MappingWorkbench *m_workbench;
  QString m_horizon;
  QComboBox *m_category, *m_source;
  QLineEdit *m_thresholds;
  QLabel *m_hint;
  QPushButton *m_show, *m_start;
};
