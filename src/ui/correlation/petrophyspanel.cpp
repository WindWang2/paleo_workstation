// 层：视图
#include "ui/correlation/petrophyspanel.h"
#include "ui/paleotheme.h"

#include <QCheckBox>
#include <QComboBox>
#include <QDoubleSpinBox>
#include <QFormLayout>
#include <QHBoxLayout>
#include <QLabel>
#include <QLineEdit>
#include <QProgressBar>
#include <QToolButton>
#include <QVBoxLayout>

#include <limits>

namespace paleo::petrophys
{

using Formula = PetroPhysTaskService::Formula;
using Params = PetroPhysTaskService::FormulaParams;
constexpr double kUnset = std::numeric_limits<double>::quiet_NaN();

PetroPhysPanel::PetroPhysPanel(QWidget *parent) : QWidget(parent)
{
  buildUi();
  syncOutputMnemonic();
  syncEnabledState();
}

void PetroPhysPanel::buildUi()
{
  auto *lay = new QVBoxLayout(this);
  lay->setContentsMargins(PaleoTheme::tokens().spacingSm, PaleoTheme::tokens().spacingXs, PaleoTheme::tokens().spacingSm, PaleoTheme::tokens().spacingXs); // spacing.xs/sm（SeismicAttrPanel 同口径）
  lay->setSpacing(PaleoTheme::tokens().spacingXs);

  // 行 1：公式 + 动作（主色文案 = DESIGN.md ribbon-button 惯例）
  auto *row1 = new QHBoxLayout();
  row1->addWidget(new QLabel(tr("公式:")));
  m_cboKind = new QComboBox();
  m_cboKind->setObjectName(QStringLiteral("petrophysKindCombo"));
  const QList<QPair<Formula, QString>> kinds = {
      {Formula::VshGrLinear, PetroPhysTaskService::formulaName(Formula::VshGrLinear)},
      {Formula::VshGrLarionovYoung,
       PetroPhysTaskService::formulaName(Formula::VshGrLarionovYoung)},
      {Formula::VshGrLarionovOld,
       PetroPhysTaskService::formulaName(Formula::VshGrLarionovOld)},
      {Formula::VshGrClavier, PetroPhysTaskService::formulaName(Formula::VshGrClavier)},
      {Formula::PhiDensity, PetroPhysTaskService::formulaName(Formula::PhiDensity)},
      {Formula::PhiNeutron, PetroPhysTaskService::formulaName(Formula::PhiNeutron)},
      {Formula::PhiSonicWyllie, PetroPhysTaskService::formulaName(Formula::PhiSonicWyllie)},
      {Formula::SwArchie, PetroPhysTaskService::formulaName(Formula::SwArchie)},
      {Formula::Expression, PetroPhysTaskService::formulaName(Formula::Expression)}};
  for (const auto &k : kinds)
    m_cboKind->addItem(k.second, int(k.first));
  m_cboKind->setFixedWidth(170);
  row1->addWidget(m_cboKind);

  auto mkBtn = [this](const QString &text, const QString &tip, bool primary) {
    auto *b = new QToolButton(this);
    b->setText(text);
    b->setToolTip(tip);
    // 主色文案走 PaleoTheme token（同 seismicattrpanel，#77）：裸 hex 在暗色主题下
    // 不翻转；applyThemedStyleSheet 随主题切换活体重算。hover 用 surfaceAltRaised
    //（token 注释钉死的 hover 槽位，surfaceAlt 是次级面底色不是 hover）。
    if (primary)
    {
      PaleoTheme::applyThemedStyleSheet(b, [] {
        const auto &t = PaleoTheme::tokens();
        return QStringLiteral("QToolButton{color:%1;font-weight:500;}"
                              "QToolButton:hover{background:%2;}")
            .arg(t.primaryText.name(), t.surfaceAltRaised.name());
      });
    }
    return b;
  };
  m_btnCompute = mkBtn(tr("▶ 批量计算"), tr("对当前井集逐井计算并写回结果曲线"),
                       /*primary=*/true);
  m_btnCompute->setObjectName(QStringLiteral("petrophysComputeButton"));
  row1->addWidget(m_btnCompute);
  m_btnCancel = mkBtn(tr("取消"), tr("取消在途批任务（已完成井成果保留）"), false);
  m_btnCancel->setObjectName(QStringLiteral("petrophysCancelButton"));
  row1->addWidget(m_btnCancel);
  row1->addStretch();
  m_lblWells = new QLabel(tr("井集：—"));
  m_lblWells->setObjectName(QStringLiteral("petrophysWellsLabel"));
  row1->addWidget(m_lblWells);
  lay->addLayout(row1);

  // 行 2：公式参数（QFormLayout 两列；出处进 tooltip——预填=文献值显式形态）
  auto *params = new QWidget(this);
  params->setObjectName(QStringLiteral("petrophysParamsHost"));
  auto *form = new QFormLayout(params);
  form->setContentsMargins(0, 0, 0, 0);
  form->setSpacing(PaleoTheme::tokens().spacingXs);
  form->setFieldGrowthPolicy(QFormLayout::AllNonFixedFieldsGrow);

  auto mkSpin = [this, params](const QString &name, double lo, double hi, int dec,
                               double val, double step, const QString &tip) {
    auto *s = new QDoubleSpinBox(params);
    s->setObjectName(name);
    s->setRange(lo, hi);
    s->setDecimals(dec);
    s->setValue(val);
    s->setSingleStep(step);
    s->setFixedWidth(72);
    s->setToolTip(tip);
    return s;
  };

  // GR 基线（Vsh 族）：井内极值缺省（确定性留痕）；手动则显式 min/max
  auto *grRow = new QWidget(params);
  auto *grLay = new QHBoxLayout(grRow);
  grLay->setContentsMargins(0, 0, 0, 0);
  grLay->setSpacing(PaleoTheme::tokens().spacingXs);
  m_chkGrAuto = new QCheckBox(tr("井内极值"), grRow);
  m_chkGrAuto->setObjectName(QStringLiteral("petrophysGrAuto"));
  m_chkGrAuto->setChecked(true);
  m_chkGrAuto->setToolTip(
      tr("勾选：该井 GR 非空样本 min/max 作基线（产物 extra 记实取值）。\n"
         "不勾：用右侧手动值。出处 AK04 ch.4（纯砂岩/纯泥岩 GR 基线）。"));
  grLay->addWidget(m_chkGrAuto);
  m_spinGrMin = mkSpin(QStringLiteral("petrophysGrMin"), 0.0, 2000.0, 1, 30.0, 5.0,
                       tr("纯砂岩段 GR 下限（API）"));
  m_spinGrMax = mkSpin(QStringLiteral("petrophysGrMax"), 0.0, 2000.0, 1, 120.0, 5.0,
                       tr("纯泥岩段 GR 上限（API）"));
  grLay->addWidget(m_spinGrMin);
  grLay->addWidget(m_spinGrMax);
  grLay->addStretch();
  form->addRow(tr("GR 基线:"), grRow);

  // 密度（φD + Archie 内联）：ρma 石英砂岩 2.65 / 淡水 1.0（AK04 ch.3）
  auto *rhoRow = new QWidget(params);
  auto *rhoLay = new QHBoxLayout(rhoRow);
  rhoLay->setContentsMargins(0, 0, 0, 0);
  rhoLay->setSpacing(PaleoTheme::tokens().spacingXs);
  m_spinRhoMa = mkSpin(QStringLiteral("petrophysRhoMa"), 1.8, 3.5, 3, 2.65, 0.01,
                       tr("骨架密度 ρma（g/cm³）：砂岩 2.65 / 灰岩 2.71 / 白云岩 "
                          "2.87（AK04 ch.3 文献值）"));
  m_spinRhoFluid = mkSpin(QStringLiteral("petrophysRhoFluid"), 0.5, 1.5, 3, 1.0, 0.01,
                          tr("流体密度 ρf（g/cm³）：淡水 1.0（AK04 ch.3）"));
  rhoLay->addWidget(m_spinRhoMa);
  rhoLay->addWidget(m_spinRhoFluid);
  rhoLay->addStretch();
  form->addRow(tr("ρma / ρf:"), rhoRow);

  // 中子：% → v/v 口径显式选择（不猜单位）
  m_chkNphiPct = new QCheckBox(tr("NPHI 输入为 %"), params);
  m_chkNphiPct->setObjectName(QStringLiteral("petrophysNphiPct"));
  m_chkNphiPct->setToolTip(tr("勾选：NPHI 按百分数读入并换算 v/v；不勾：已是小数 "
                              "孔隙度直读（AK04 ch.3）"));
  form->addRow(tr("中子口径:"), m_chkNphiPct);

  // 声波 Wyllie：砂岩 182 / 淡水 620 µs·m⁻¹（µs·ft⁻¹ 时 55.5/189，AK04 ch.3）
  auto *dtRow = new QWidget(params);
  auto *dtLay = new QHBoxLayout(dtRow);
  dtLay->setContentsMargins(0, 0, 0, 0);
  dtLay->setSpacing(PaleoTheme::tokens().spacingXs);
  m_spinDtMa = mkSpin(QStringLiteral("petrophysDtMa"), 40.0, 250.0, 1, 182.0, 1.0,
                      tr("骨架声波 Δtma：砂岩 182 µs/m（55.5 µs/ft）、灰岩 156 "
                         "（Wyllie 1956；AK04 ch.3）——单位须与曲线一致"));
  m_spinDtFluid = mkSpin(QStringLiteral("petrophysDtFluid"), 100.0, 1000.0, 1, 620.0,
                         5.0, tr("流体声波 Δtf：淡水 620 µs/m（189 µs/ft）"));
  m_spinCp = mkSpin(QStringLiteral("petrophysCp"), 1.0, 2.0, 2, 1.0, 0.05,
                    tr("压实校正因子 Cp（Wyllie 1956；1 = 不校正）"));
  dtLay->addWidget(m_spinDtMa);
  dtLay->addWidget(m_spinDtFluid);
  dtLay->addWidget(m_spinCp);
  dtLay->addStretch();
  form->addRow(tr("Δtma/Δtf/Cp:"), dtRow);

  // Archie：a/m/n 文献经典值（AK04 ch.6）；Rw 必填不预填（无通用文献值）
  auto *archRow = new QWidget(params);
  auto *archLay = new QHBoxLayout(archRow);
  archLay->setContentsMargins(0, 0, 0, 0);
  archLay->setSpacing(PaleoTheme::tokens().spacingXs);
  m_spinA = mkSpin(QStringLiteral("petrophysArchieA"), 0.1, 2.0, 2, 1.0, 0.01,
                   tr("Archie a：经典 1.0 / Humble 0.62（AK04 ch.6）"));
  m_spinM = mkSpin(QStringLiteral("petrophysArchieM"), 1.0, 3.0, 2, 2.0, 0.05,
                   tr("胶结指数 m：经典 2.0 / Humble 2.15（AK04 ch.6）"));
  m_spinN = mkSpin(QStringLiteral("petrophysArchieN"), 1.0, 3.0, 2, 2.0, 0.05,
                   tr("饱和指数 n：2.0（Archie 1942 原式）"));
  m_spinRw = mkSpin(QStringLiteral("petrophysArchieRw"), 0.0, 10.0, 4, 0.0, 0.01,
                    tr("地层水电阻率 Rw（Ω·m）——工区水性质定，必填（无文献通"
                       "用值，不预填臆造）"));
  m_spinRw->setSpecialValueText(tr("—必填—"));
  archLay->addWidget(m_spinA);
  archLay->addWidget(m_spinM);
  archLay->addWidget(m_spinN);
  archLay->addWidget(m_spinRw);
  archLay->addStretch();
  form->addRow(tr("a/m/n/Rw:"), archRow);

  // Archie φ 来源（显式曲线名；空 = ρma/ρf 密度内联）
  m_editPhiMnemonic = new QLineEdit(params);
  m_editPhiMnemonic->setObjectName(QStringLiteral("petrophysPhiMnemonic"));
  m_editPhiMnemonic->setPlaceholderText(tr("空 = 用上方 ρma/ρf 从 RHOB 内联算 φD"));
  m_editPhiMnemonic->setFixedWidth(200);
  form->addRow(tr("Archie φ 曲线:"), m_editPhiMnemonic);

  // 表达式
  m_editExpr = new QLineEdit(params);
  m_editExpr->setObjectName(QStringLiteral("petrophysExprEdit"));
  m_editExpr->setPlaceholderText(
      tr("逐点表达式，如 RHOB - 0.05*NPHI 或 where(GR > 100, GR, 0)"));
  form->addRow(tr("表达式:"), m_editExpr);

  // 输出曲线名 / QC 门
  auto *outRow = new QWidget(params);
  auto *outLay = new QHBoxLayout(outRow);
  outLay->setContentsMargins(0, 0, 0, 0);
  outLay->setSpacing(PaleoTheme::tokens().spacingXs);
  m_editOutMnemonic = new QLineEdit(outRow);
  m_editOutMnemonic->setObjectName(QStringLiteral("petrophysOutMnemonic"));
  m_editOutMnemonic->setFixedWidth(80);
  outLay->addWidget(m_editOutMnemonic);
  m_chkQc = new QCheckBox(tr("QC 门"), outRow);
  m_chkQc->setObjectName(QStringLiteral("petrophysQcEnabled"));
  m_chkQc->setChecked(true);
  m_chkQc->setToolTip(tr("结果曲线越界区间计数（如 Vsh/φ/Sw 出 [0,1] 记异常段）"));
  outLay->addWidget(m_chkQc);
  m_spinQcLo = mkSpin(QStringLiteral("petrophysQcLo"), -100.0, 100.0, 3, 0.0, 0.1,
                      tr("QC 下限"));
  m_spinQcHi = mkSpin(QStringLiteral("petrophysQcHi"), -100.0, 100.0, 3, 1.0, 0.1,
                      tr("QC 上限"));
  outLay->addWidget(m_spinQcLo);
  outLay->addWidget(m_spinQcHi);
  outLay->addStretch();
  form->addRow(tr("输出/QC:"), outRow);
  lay->addWidget(params);

  // 行 3：进度 + 状态
  auto *row3 = new QHBoxLayout();
  m_progress = new QProgressBar();
  m_progress->setObjectName(QStringLiteral("petrophysProgress"));
  m_progress->setRange(0, 100);
  m_progress->setValue(0);
  m_progress->setFixedWidth(160);
  m_progress->setTextVisible(false);
  row3->addWidget(m_progress);
  m_lblStatus = new QLabel(tr("未计算"));
  m_lblStatus->setObjectName(QStringLiteral("petrophysStatus"));
  row3->addWidget(m_lblStatus, 1);
  lay->addLayout(row3);

  connect(m_cboKind, &QComboBox::currentIndexChanged, this,
          [this]() { syncOutputMnemonic(); });
  connect(m_btnCompute, &QToolButton::clicked, this,
          [this]() { emit computeRequested(currentRequest()); });
  connect(m_btnCancel, &QToolButton::clicked, this, [this]() { emit cancelRequested(); });
}

Formula PetroPhysPanel::currentFormula() const
{
  return Formula(m_cboKind->currentData().toInt());
}

void PetroPhysPanel::syncOutputMnemonic()
{
  QString def;
  switch (currentFormula())
  {
    case Formula::VshGrLinear:
    case Formula::VshGrLarionovYoung:
    case Formula::VshGrLarionovOld:
    case Formula::VshGrClavier:
      def = QStringLiteral("VSH");
      break;
    case Formula::PhiDensity: def = QStringLiteral("PHID"); break;
    case Formula::PhiNeutron: def = QStringLiteral("PHIN"); break;
    case Formula::PhiSonicWyllie: def = QStringLiteral("PHIS"); break;
    case Formula::SwArchie: def = QStringLiteral("SW"); break;
    case Formula::Expression: def = QStringLiteral("EXPR"); break;
  }
  m_editOutMnemonic->setText(def);
}

QString PetroPhysPanel::currentOutputMnemonic() const
{
  return m_editOutMnemonic->text().trimmed();
}

PetroPhysTaskService::BatchRequest PetroPhysPanel::currentRequest() const
{
  PetroPhysTaskService::BatchRequest r;
  r.formula = currentFormula();
  r.expression = m_editExpr->text();
  r.outputMnemonic = currentOutputMnemonic();
  r.outputUnit = QStringLiteral("v/v");
  r.outputDescr = QStringLiteral("petrophysics %1")
                      .arg(PetroPhysTaskService::formulaKey(r.formula));
  Params p;
  p.grAutoBaseline = m_chkGrAuto->isChecked();
  p.grMin = m_spinGrMin->value();
  p.grMax = m_spinGrMax->value();
  p.rhoMa = m_spinRhoMa->value();
  p.rhoFluid = m_spinRhoFluid->value();
  p.neutronInPercent = m_chkNphiPct->isChecked();
  p.dtMa = m_spinDtMa->value();
  p.dtFluid = m_spinDtFluid->value();
  p.cpFactor = m_spinCp->value();
  p.archieA = m_spinA->value();
  p.archieM = m_spinM->value();
  p.archieN = m_spinN->value();
  // Rw=0（「必填」占位）→ NaN：显式未设，服务校验拒绝而非臆造
  p.rw = m_spinRw->value() > 0.0 ? m_spinRw->value() : kUnset;
  p.swPorosityMnemonic = m_editPhiMnemonic->text().trimmed();
  r.params = p;
  r.qcBandEnabled = m_chkQc->isChecked();
  r.qcLo = m_spinQcLo->value();
  r.qcHi = m_spinQcHi->value();
  return r;
}

void PetroPhysPanel::setWellScope(int wellCount, int withLas)
{
  m_lblWells->setText(tr("井集：%1 井（%2 有 LAS）").arg(wellCount).arg(withLas));
}

void PetroPhysPanel::setBusy(bool busy)
{
  m_busy = busy;
  if (busy)
  {
    m_progress->setValue(0);
    m_lblStatus->setText(tr("计算中…"));
  }
  syncEnabledState();
}

void PetroPhysPanel::updateProgress(int percent, const QString &stageLabel)
{
  m_progress->setValue(percent);
  if (!stageLabel.isEmpty())
    m_lblStatus->setText(tr("计算中… %1 %2%").arg(stageLabel).arg(percent));
}

void PetroPhysPanel::showResult(bool ok, const QString &summary)
{
  m_progress->setValue(ok ? 100 : m_progress->value());
  m_lblStatus->setText(summary);
  m_busy = false;
  syncEnabledState();
}

void PetroPhysPanel::syncEnabledState()
{
  m_btnCompute->setEnabled(!m_busy);
  m_btnCancel->setEnabled(m_busy);
}

} // namespace paleo::petrophys
