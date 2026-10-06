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
class QPlainTextEdit;
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
  QLabel *m_inputHint = nullptr, *m_resultHint = nullptr;
  // 方向51：远端预测状态行（「远端预测未配置，走本地引擎」等，由装配注入）。
  QLabel *m_status = nullptr;
  QString m_labelLayer;
  bool m_labelModeDirty = false;
  QListWidget *m_inputs = nullptr;
  QTreeWidget *m_results;
  QComboBox *m_kind = nullptr, *m_points = nullptr, *m_factor = nullptr;
  QLineEdit *m_field = nullptr, *m_thresholds = nullptr;
  QDoubleSpinBox *m_cell = nullptr, *m_interval = nullptr;
  QProgressBar *m_progress = nullptr;
  QTableWidget *m_facies = nullptr;
  QComboBox *m_editFacies = nullptr;
  QComboBox *m_displayLevel = nullptr, *m_editLevel = nullptr, *m_evidenceSource = nullptr;
  QPlainTextEdit *m_evidenceText = nullptr;
  QListWidget *m_evidence = nullptr;
};
