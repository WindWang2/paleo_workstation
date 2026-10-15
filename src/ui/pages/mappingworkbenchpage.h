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
class QTimer;
class QShowEvent;
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
  QString mode() const { return m_mode; }
  void selectEvidenceSource(const QString &id);
  QString horizon() const { return m_horizon; }
  QString selectedLayer() const;
  void setEvidenceSnapshot(const QVariantMap &snapshot);
signals:
  void commandRequested(const QString &action, const QVariantMap &parameters);
  void targetLayerChanged(const QString &layerId);
  void evidenceActivated(const QVariantMap &evidence);
  void evidenceGraphRequested(const QVariantMap &snapshot);

protected:
  void showEvent(QShowEvent *event) override;

private:
  void refreshInputs();
  void scheduleRefresh();
  void updateState();
  void refreshEvidenceCommandGates();
  void renderEvidenceSnapshot(const QVariantMap &snapshot);
  bool nativeSelectionMatchesEvidence() const;
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
  QWidget *m_horizonSourceRow = nullptr;
  QLineEdit *m_horizonFile = nullptr;
  QPushButton *m_chooseHorizonFile = nullptr;
  QDoubleSpinBox *m_cell = nullptr, *m_interval = nullptr;
  QProgressBar *m_progress = nullptr;
  QTableWidget *m_facies = nullptr;
  QComboBox *m_editFacies = nullptr;
  QComboBox *m_displayLevel = nullptr, *m_editLevel = nullptr, *m_evidenceSource = nullptr;
  QPlainTextEdit *m_evidenceText = nullptr;
  QListWidget *m_evidence = nullptr;
  QListWidget *m_generationEvidence = nullptr;
  QComboBox *m_evidenceRole = nullptr, *m_evidenceType = nullptr,
      *m_reviewState = nullptr, *m_verification = nullptr;
  QLineEdit *m_evidenceActor = nullptr, *m_evidenceReason = nullptr, *m_sourceFeatureUid = nullptr;
  QLabel *m_interpretationState = nullptr, *m_quality = nullptr;
  QPlainTextEdit *m_evidenceHistory = nullptr;
  QVariantMap m_evidenceSnapshot;
  bool m_evidenceSnapshotOwned = false;
  int m_historyPage = 0;
  QString m_historyTarget;
  QTimer *m_refreshTimer = nullptr;
  bool m_refreshPending = false;
};
