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
  // G08：主窗口执行反查命令后回填结果（双击行复用 evidenceActivated 定位）。
  // meta 携带索引新鲜度（index_state / index_error / total / shown）。
  void setReverseResults(const QVariantList &rows, const QString &error = {},
                         const QVariantMap &meta = {});
  // G05/G09：影响分析成功后回喂页面——「按最近影响报告受控重算」按钮的
  // 可用条件（无影响报告时禁用并给 reason，不再让点击后吃错误）。
  void setImpactAvailable(bool ready);
  // X9：索引后台同步完成后自动重发最近一次反查（仅存 kind+argument，
  // 不重新读控件——避免层位/输入变化把查询口径悄悄换掉）。
  void reissueReverseQuery();
  // X10：反查导航回退栈可用性（由 EvidenceInteraction::historyChanged 驱动）。
  void setCanGoBack(bool canBack);
signals:
  void commandRequested(const QString &action, const QVariantMap &parameters);
  void targetLayerChanged(const QString &layerId);
  void evidenceActivated(const QVariantMap &evidence);
  // X10：反查结果行激活 = 定位命中图斑本身（区别于证据来源定位）。
  void reverseHitActivated(const QVariantMap &hit);
  void evidenceBackRequested();
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
  bool m_impactReady = false; // 最近一次影响分析成功（recompute 按钮门控）
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
  // G03 计算归因 tab：冻结输入重放结果的只读展示（状态/决策分布/上游回执/井关联）。
  QLabel *m_attributionSummary = nullptr;
  QPlainTextEdit *m_attributionDetail = nullptr;
  // X14：归因 tab 的可定位对象（图斑内井 / 带定位的冲突来源），双击导航。
  QListWidget *m_attributionTargets = nullptr;
  // X10：反查/图谱导航的回退按钮（可用性由交互层 historyChanged 驱动）。
  QPushButton *m_evidenceBack = nullptr;
  // G08/G12 证据反查：类型下拉 + 检索词 + 结果列表（双击定位图斑）。
  QComboBox *m_reverseKind = nullptr;
  QLineEdit *m_reverseText = nullptr;
  QListWidget *m_reverseResults = nullptr;
  QString m_lastReverseKind, m_lastReverseArgument; // 最近一次反查（同步完自动重发）
  QVariantMap m_evidenceSnapshot;
  bool m_evidenceSnapshotOwned = false;
  int m_historyPage = 0;
  QString m_historyTarget;
  QTimer *m_refreshTimer = nullptr;
  bool m_refreshPending = false;
};
