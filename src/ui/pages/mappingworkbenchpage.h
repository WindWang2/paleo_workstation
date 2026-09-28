// 层：视图
#pragma once
#include <QVariantMap>
#include <QWidget>
class MappingWorkbench;
class QListWidget;
class QTreeWidget;
class QComboBox;
class QLineEdit;
class QDoubleSpinBox;
class QLabel;
class QPushButton;
class QProgressBar;
class QTableWidget;
class MappingWorkbenchPage : public QWidget {
  Q_OBJECT
public:
  MappingWorkbenchPage(const QString &mode, MappingWorkbench *workbench,
                       QWidget *parent = nullptr);
  void setHorizon(const QString &horizon);
  void refresh();
  void selectLayer(const QString &id);
  void showMessage(const QString &message);
  QPushButton *commandButton(const QString &name) const;
  QString horizon() const { return m_horizon; }
  QString selectedLayer() const;
signals:
  void commandRequested(const QString &action, const QVariantMap &parameters);

private:
  void refreshInputs();
  void updateState();
  QStringList checkedInputs() const;
  void issue(const QString &action, QVariantMap parameters = {});
  MappingWorkbench *m_workbench;
  QString m_mode, m_horizon;
  QLabel *m_heading, *m_message, *m_details;
  QListWidget *m_inputs = nullptr;
  QTreeWidget *m_results;
  QComboBox *m_kind = nullptr, *m_points = nullptr, *m_factor = nullptr;
  QLineEdit *m_field = nullptr, *m_thresholds = nullptr;
  QDoubleSpinBox *m_cell = nullptr, *m_interval = nullptr;
  QProgressBar *m_progress = nullptr;
  QTableWidget *m_facies = nullptr;
};
