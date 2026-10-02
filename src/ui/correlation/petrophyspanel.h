// 层：视图
#pragma once
#include <QWidget>

#include "services/petrophyscomputeservice.h" // BatchRequest/Formula/FormulaParams（意图载体类型；src/ 根解析）

class QCheckBox;
class QComboBox;
class QDoubleSpinBox;
class QLabel;
class QLineEdit;
class QProgressBar;
class QToolButton;

namespace paleo::petrophys
{

// ui/ — PetroPhysPanel: 测井计算参数面板（goal/petrophysics-logs）。
// 视图只发信号不干活：表单 → computeRequested/cancelRequested 意图；
// busy/progress/result 由编排方（壳 attach：PetroPhysTaskService 批任务）
// 回填进 setBusy/updateProgress/showResult 槽。井集与 catalog 解析是编排
// 方的事——面板发出的 BatchRequest.wells 恒空。
//
// 参数预填 = 文献值显式形态（出处 tooltip 注明，AK04 = Asquith &
// Krygowski 2004）：ρma 2.65（石英砂岩）/ρf 1.0（淡水）/Δtma 182µs·m⁻¹/
// Δtf 620µs·m⁻¹（AK04 ch.3）；a=1/m=2/n=2（AK04 ch.6）。Rw 无文献通用值
// （工区水性质定）——spinbox 置 0 显示「必填」，不臆造预填。GR 基线缺省
// 井内极值（确定性，产物留痕实取值）。
class PetroPhysPanel : public QWidget
{
  Q_OBJECT
public:
  explicit PetroPhysPanel(QWidget *parent = nullptr);

  // 表单当前值 → 意图（wells 留空由编排方填）。
  PetroPhysTaskService::BatchRequest currentRequest() const;
  QString currentOutputMnemonic() const;

  // 回填面（编排方调用）
  void setWellScope(int wellCount, int withLas);
  void setBusy(bool busy);
  void updateProgress(int percent, const QString &stageLabel);
  void showResult(bool ok, const QString &summary);

signals:
  void computeRequested(const PetroPhysTaskService::BatchRequest &request);
  void cancelRequested();

private:
  void buildUi();
  void syncEnabledState();
  void syncOutputMnemonic();
  PetroPhysTaskService::Formula currentFormula() const;

  QComboBox *m_cboKind = nullptr;
  QCheckBox *m_chkGrAuto = nullptr;
  QDoubleSpinBox *m_spinGrMin = nullptr, *m_spinGrMax = nullptr;
  QDoubleSpinBox *m_spinRhoMa = nullptr, *m_spinRhoFluid = nullptr;
  QCheckBox *m_chkNphiPct = nullptr;
  QDoubleSpinBox *m_spinDtMa = nullptr, *m_spinDtFluid = nullptr, *m_spinCp = nullptr;
  QDoubleSpinBox *m_spinA = nullptr, *m_spinM = nullptr, *m_spinN = nullptr,
                  *m_spinRw = nullptr;
  QLineEdit *m_editPhiMnemonic = nullptr;
  QLineEdit *m_editExpr = nullptr;
  QLineEdit *m_editOutMnemonic = nullptr;
  QCheckBox *m_chkQc = nullptr;
  QDoubleSpinBox *m_spinQcLo = nullptr, *m_spinQcHi = nullptr;
  QLabel *m_lblWells = nullptr, *m_lblStatus = nullptr;
  QProgressBar *m_progress = nullptr;
  QToolButton *m_btnCompute = nullptr, *m_btnCancel = nullptr;
  bool m_busy = false;
};

} // namespace paleo::petrophys
