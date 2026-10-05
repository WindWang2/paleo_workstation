// 层：视图
#include "propertymodelpanel.h"

#include "../paleotheme.h"

#include <algorithm>

#include <QCheckBox>
#include <QComboBox>
#include <QFont>
#include <QDoubleSpinBox>
#include <QFormLayout>
#include <QGroupBox>
#include <QHBoxLayout>
#include <QLabel>
#include <QLineEdit>
#include <QProgressBar>
#include <QRegularExpression>
#include <QRegularExpressionValidator>
#include <QSlider>
#include <QSpinBox>
#include <QToolButton>
#include <QVBoxLayout>

PropertyModelPanel::PropertyModelPanel(QWidget *parent)
  : QWidget(parent)
{
  buildUi();
  syncEnabledState();
}

void PropertyModelPanel::buildUi()
{
  auto *lay = new QVBoxLayout(this);
  lay->setContentsMargins(PaleoTheme::tokens().spacingSm, PaleoTheme::tokens().spacingSm, PaleoTheme::tokens().spacingSm, PaleoTheme::tokens().spacingSm);
  lay->setSpacing(PaleoTheme::tokens().spacingSm);

  auto *form = new QFormLayout();
  form->setSpacing(PaleoTheme::tokens().spacingSm);
  m_top = new QLineEdit();
  m_top->setObjectName(QStringLiteral("propTopEdit"));
  m_top->setPlaceholderText(tr("上层面位，如 C3"));
  m_bot = new QLineEdit();
  m_bot->setObjectName(QStringLiteral("propBotEdit"));
  m_bot->setPlaceholderText(tr("下层面位，如 D72"));
  m_curve = new QLineEdit(QStringLiteral("GR"));
  m_curve->setObjectName(QStringLiteral("propCurveEdit"));
  m_layers = new QSpinBox();
  m_layers->setObjectName(QStringLiteral("propLayersSpin"));
  m_layers->setRange(1, 200);
  m_layers->setValue(10);
  m_agg = new QComboBox();
  m_agg->setObjectName(QStringLiteral("propAggCombo"));
  m_agg->addItem(tr("弧长加权均值"), 1);
  m_agg->addItem(tr("算术均值"), 0);
  m_agg->addItem(tr("中位数"), 2);
  m_agg->addItem(tr("众数"), 3);
  m_method = new QComboBox();
  m_method->setObjectName(QStringLiteral("propMethodCombo"));
  m_method->addItem(tr("IDW 插值"), 0);
  m_method->addItem(tr("序贯高斯模拟"), 1);
  m_method->setToolTip(
      tr("序贯高斯是随机模拟：结果带种子与参数溯源，不是确定性插值。"));
  m_power = new QDoubleSpinBox();
  m_power->setObjectName(QStringLiteral("propPowerSpin"));
  m_power->setRange(0.5, 8.0);
  m_power->setDecimals(1);
  m_power->setSingleStep(0.5);
  m_power->setValue(2.0);
  form->addRow(tr("顶面"), m_top);
  form->addRow(tr("底面"), m_bot);
  form->addRow(tr("曲线"), m_curve);
  form->addRow(tr("层数"), m_layers);
  form->addRow(tr("粗化"), m_agg);
  form->addRow(tr("方法"), m_method);
  form->addRow(tr("IDW 幂"), m_power);
  lay->addLayout(form);

  // 序贯高斯参数（方法=SGS 时可用；原生 QGroupBox，不发明 chrome）。
  m_sgsGroup = new QGroupBox(tr("序贯高斯参数"));
  m_sgsGroup->setObjectName(QStringLiteral("propSgsGroup"));
  auto *sgsForm = new QFormLayout(m_sgsGroup);
  sgsForm->setSpacing(PaleoTheme::tokens().spacingSm);
  m_variogram = new QComboBox();
  m_variogram->setObjectName(QStringLiteral("propVariogramCombo"));
  m_variogram->addItem(tr("球状"), 0);
  m_variogram->addItem(tr("指数"), 1);
  m_variogram->addItem(tr("高斯"), 2);
  m_nugget = new QDoubleSpinBox();
  m_nugget->setObjectName(QStringLiteral("propNuggetSpin"));
  m_nugget->setRange(0.0, 1e6);
  m_nugget->setDecimals(3);
  m_nugget->setValue(0.0);
  m_sill = new QDoubleSpinBox();
  m_sill->setObjectName(QStringLiteral("propSillSpin"));
  m_sill->setRange(0.001, 1e6);
  m_sill->setDecimals(3);
  m_sill->setValue(1.0);
  m_range = new QDoubleSpinBox();
  m_range->setObjectName(QStringLiteral("propRangeSpin"));
  m_range->setRange(1.0, 1e7);
  m_range->setDecimals(1);
  m_range->setValue(500.0);
  m_range->setSuffix(tr(" m"));
  m_azimuth = new QDoubleSpinBox();
  m_azimuth->setObjectName(QStringLiteral("propAzimuthSpin"));
  m_azimuth->setRange(-360.0, 360.0);
  m_azimuth->setDecimals(1);
  m_azimuth->setValue(0.0);
  m_azimuth->setSuffix(tr("°"));
  m_anisotropy = new QDoubleSpinBox();
  m_anisotropy->setObjectName(QStringLiteral("propAnisotropySpin"));
  m_anisotropy->setRange(1.0, 100.0);
  m_anisotropy->setDecimals(2);
  m_anisotropy->setValue(1.0);
  m_verticalRatio = new QDoubleSpinBox();
  m_verticalRatio->setObjectName(QStringLiteral("propVerticalRatioSpin"));
  m_verticalRatio->setRange(1.0, 1000.0);
  m_verticalRatio->setDecimals(1);
  m_verticalRatio->setValue(5.0);
  m_realizations = new QSpinBox();
  m_realizations->setObjectName(QStringLiteral("propRealizationsSpin"));
  m_realizations->setRange(1, 64);
  m_realizations->setValue(1);
  m_realizations->setToolTip(tr("多实现时每实现各落一个独立 DERIVED 版本。"));
  const auto seedEdit = [](const QString &objectName, const QString &initial) {
    auto *edit = new QLineEdit(initial);
    edit->setObjectName(objectName);
    edit->setValidator(new QRegularExpressionValidator(
        QRegularExpression(QStringLiteral("[0-9]{1,19}")), edit));
    edit->setFont(PaleoTheme::monoFont()); // 种子是数值面（DESIGN mono）
    return edit;
  };
  m_seed = seedEdit(QStringLiteral("propSeedEdit"), QStringLiteral("42"));
  m_seed->setToolTip(tr("mt19937_64 种子。同种子同输入逐位可复现。"));
  sgsForm->addRow(tr("变差模型"), m_variogram);
  sgsForm->addRow(tr("块金"), m_nugget);
  sgsForm->addRow(tr("基台"), m_sill);
  sgsForm->addRow(tr("变程"), m_range);
  sgsForm->addRow(tr("走向"), m_azimuth);
  sgsForm->addRow(tr("水平比"), m_anisotropy);
  sgsForm->addRow(tr("垂向比"), m_verticalRatio);
  sgsForm->addRow(tr("实现数"), m_realizations);
  sgsForm->addRow(tr("种子"), m_seed);
  lay->addWidget(m_sgsGroup);

  // 相带约束（无资产时编排层降级全域口径并回写口径标签）。
  m_facies = new QCheckBox(tr("相带约束（最新相图）"));
  m_facies->setObjectName(QStringLiteral("propFaciesCheck"));
  m_facies->setToolTip(
      tr("勾选后按最新相带草稿图分带：各带独立统计，变差几何共享全局模型。"));
  lay->addWidget(m_facies);

  // 对象建模（可勾选分组；对象 cell 硬覆盖背景场——对象优先口径）。
  m_objectGroup = new QGroupBox(tr("对象建模（对象优先）"));
  m_objectGroup->setObjectName(QStringLiteral("propObjectGroup"));
  m_objectGroup->setCheckable(true);
  m_objectGroup->setChecked(false);
  auto *objForm = new QFormLayout(m_objectGroup);
  objForm->setSpacing(PaleoTheme::tokens().spacingSm);
  m_objectType = new QComboBox();
  m_objectType->setObjectName(QStringLiteral("propObjectTypeCombo"));
  m_objectType->addItem(tr("河道"), 0);
  m_objectType->addItem(tr("点坝"), 1);
  m_objectAzimuth = new QDoubleSpinBox();
  m_objectAzimuth->setObjectName(QStringLiteral("propObjectAzimuthSpin"));
  m_objectAzimuth->setRange(-360.0, 360.0);
  m_objectAzimuth->setDecimals(1);
  m_objectAzimuth->setValue(90.0);
  m_objectAzimuth->setSuffix(tr("°"));
  m_objectLength = new QDoubleSpinBox();
  m_objectLength->setObjectName(QStringLiteral("propObjectLengthSpin"));
  m_objectLength->setRange(0.0, 1e7);
  m_objectLength->setDecimals(1);
  m_objectLength->setValue(0.0);
  m_objectLength->setSuffix(tr(" m"));
  m_objectLength->setSpecialValueText(tr("自动"));
  m_objectWidth = new QDoubleSpinBox();
  m_objectWidth->setObjectName(QStringLiteral("propObjectWidthSpin"));
  m_objectWidth->setRange(1.0, 1e6);
  m_objectWidth->setDecimals(1);
  m_objectWidth->setValue(200.0);
  m_objectWidth->setSuffix(tr(" m"));
  m_objectThickness = new QDoubleSpinBox();
  m_objectThickness->setObjectName(QStringLiteral("propObjectThicknessSpin"));
  m_objectThickness->setRange(0.1, 1e5);
  m_objectThickness->setDecimals(1);
  m_objectThickness->setValue(20.0);
  m_objectThickness->setSuffix(tr(" m"));
  m_objectCurvature = new QDoubleSpinBox();
  m_objectCurvature->setObjectName(QStringLiteral("propObjectCurvatureSpin"));
  m_objectCurvature->setRange(0.0, 1e6);
  m_objectCurvature->setDecimals(1);
  m_objectCurvature->setValue(0.0);
  m_objectCurvature->setSuffix(tr(" m"));
  m_objectValue = new QDoubleSpinBox();
  m_objectValue->setObjectName(QStringLiteral("propObjectValueSpin"));
  m_objectValue->setRange(-1e9, 1e9);
  m_objectValue->setDecimals(3);
  m_objectValue->setValue(1.0);
  m_objectCount = new QSpinBox();
  m_objectCount->setObjectName(QStringLiteral("propObjectCountSpin"));
  m_objectCount->setRange(1, 200);
  m_objectCount->setValue(1);
  m_objectSeed = seedEdit(QStringLiteral("propObjectSeedEdit"), QStringLiteral("43"));
  objForm->addRow(tr("类型"), m_objectType);
  objForm->addRow(tr("走向"), m_objectAzimuth);
  objForm->addRow(tr("长度"), m_objectLength);
  objForm->addRow(tr("宽度"), m_objectWidth);
  objForm->addRow(tr("厚度"), m_objectThickness);
  objForm->addRow(tr("曲率"), m_objectCurvature);
  objForm->addRow(tr("属性值"), m_objectValue);
  objForm->addRow(tr("数量"), m_objectCount);
  objForm->addRow(tr("种子"), m_objectSeed);
  lay->addWidget(m_objectGroup);

  auto *row = new QHBoxLayout();
  row->setSpacing(PaleoTheme::tokens().spacingSm);
  m_build = new QToolButton(this);
  m_build->setObjectName(QStringLiteral("propBuildButton"));
  m_build->setText(tr("建立属性体"));
  m_build->setToolTip(tr("按当前参数发建模意图。计算在功能层，面板不改网格。"));
  {
    PaleoTheme::applyThemedStyleSheet(m_build, [] {
      return PaleoTheme::toolButtonStyleSheet() +
          QStringLiteral("QToolButton { color: %1; } QToolButton:disabled { color: %2; }")
              .arg(PaleoTheme::tokens().primaryText.name(), PaleoTheme::tokens().textDisabled.name());
    });
    QFont font = m_build->font();
    font.setWeight(QFont::Medium);
    m_build->setFont(font);
  }
  m_cancel = new QToolButton(this);
  m_cancel->setObjectName(QStringLiteral("propCancelButton"));
  m_cancel->setText(tr("取消"));
  m_cancel->setToolTip(tr("取消在途建模。未落盘的结果不登记。"));
  row->addWidget(m_build);
  row->addWidget(m_cancel);
  row->addStretch();
  lay->addLayout(row);

  auto *alphaRow = new QHBoxLayout();
  alphaRow->setSpacing(PaleoTheme::tokens().spacingSm);
  alphaRow->addWidget(new QLabel(tr("叠加")));
  m_alpha = new QSlider(Qt::Horizontal);
  m_alpha->setObjectName(QStringLiteral("propAlphaSlider"));
  m_alpha->setRange(0, 100);
  m_alpha->setValue(65);
  alphaRow->addWidget(m_alpha, 1);
  lay->addLayout(alphaRow);

  m_progress = new QProgressBar();
  m_progress->setObjectName(QStringLiteral("propProgress"));
  m_progress->setRange(0, 100);
  m_progress->setValue(0);
  m_progress->setTextVisible(true);
  lay->addWidget(m_progress);

  m_status = new QLabel(tr("选择顶底面与曲线"));
  m_status->setObjectName(QStringLiteral("propStatus"));
  m_status->setWordWrap(true);
  lay->addWidget(m_status);

  // 诚实口径标签（text-muted 次级信息：竖直近似/竖帘/相带/种子来源）。
  m_caliber = new QLabel(QString());
  m_caliber->setObjectName(QStringLiteral("propCaliberLabel"));
  m_caliber->setWordWrap(true);
  m_caliber->setVisible(false);
  PaleoTheme::applyThemedStyleSheet(m_caliber, [] {
    return PaleoTheme::mutedCaptionStyleSheet();
  });
  lay->addWidget(m_caliber);

  connect(m_build, &QToolButton::clicked, this, [this]() {
    emit buildRequested(topHorizon(), bottomHorizon(), curveMnemonic(), layerCount(),
                        aggregator(), idwPower(), overlayAlpha());
  });
  connect(m_cancel, &QToolButton::clicked, this, &PropertyModelPanel::cancelRequested);
  connect(m_alpha, &QSlider::valueChanged, this, [this](int value) {
    emit alphaChanged(value / 100.0);
  });
  connect(m_top, &QLineEdit::textChanged, this, [this](const QString &) { syncEnabledState(); });
  connect(m_bot, &QLineEdit::textChanged, this, [this](const QString &) { syncEnabledState(); });
  connect(m_curve, &QLineEdit::textChanged, this, [this](const QString &) { syncEnabledState(); });
  connect(m_method, &QComboBox::currentIndexChanged, this, [this](int) { syncEnabledState(); });
  connect(m_objectGroup, &QGroupBox::toggled, this, [this](bool) { syncEnabledState(); });
}

QString PropertyModelPanel::topHorizon() const { return m_top->text().trimmed(); }
QString PropertyModelPanel::bottomHorizon() const { return m_bot->text().trimmed(); }
QString PropertyModelPanel::curveMnemonic() const { return m_curve->text().trimmed(); }
int PropertyModelPanel::layerCount() const { return m_layers->value(); }
int PropertyModelPanel::aggregator() const { return m_agg->currentData().toInt(); }
double PropertyModelPanel::idwPower() const { return m_power->value(); }
double PropertyModelPanel::overlayAlpha() const { return m_alpha->value() / 100.0; }

int PropertyModelPanel::method() const { return m_method->currentData().toInt(); }
int PropertyModelPanel::variogramType() const { return m_variogram->currentData().toInt(); }
double PropertyModelPanel::nugget() const { return m_nugget->value(); }
double PropertyModelPanel::sill() const { return m_sill->value(); }
double PropertyModelPanel::rangeMeters() const { return m_range->value(); }
double PropertyModelPanel::azimuthDeg() const { return m_azimuth->value(); }
double PropertyModelPanel::anisotropyRatio() const { return m_anisotropy->value(); }
double PropertyModelPanel::verticalRangeRatio() const { return m_verticalRatio->value(); }
int PropertyModelPanel::realizations() const { return m_realizations->value(); }
unsigned long long PropertyModelPanel::seed() const { return m_seed->text().toULongLong(); }
bool PropertyModelPanel::faciesEnabled() const { return m_facies->isChecked(); }
bool PropertyModelPanel::objectEnabled() const { return m_objectGroup->isChecked(); }
int PropertyModelPanel::objectType() const { return m_objectType->currentData().toInt(); }
double PropertyModelPanel::objectAzimuthDeg() const { return m_objectAzimuth->value(); }
double PropertyModelPanel::objectLengthMeters() const { return m_objectLength->value(); }
double PropertyModelPanel::objectWidthMeters() const { return m_objectWidth->value(); }
double PropertyModelPanel::objectThicknessMeters() const { return m_objectThickness->value(); }
double PropertyModelPanel::objectCurvatureMeters() const { return m_objectCurvature->value(); }
double PropertyModelPanel::objectValue() const { return m_objectValue->value(); }
int PropertyModelPanel::objectCount() const { return m_objectCount->value(); }
unsigned long long PropertyModelPanel::objectSeed() const
{
  return m_objectSeed->text().toULongLong();
}

void PropertyModelPanel::setCaliberNote(const QString &note)
{
  m_caliber->setText(note);
  m_caliber->setVisible(!note.isEmpty());
}

QString PropertyModelPanel::caliberNote() const { return m_caliber->text(); }

void PropertyModelPanel::setBusy(bool busy)
{
  m_busy = busy;
  syncEnabledState();
}

void PropertyModelPanel::updateProgress(int percent, const QString &stageLabel)
{
  m_progress->setValue(std::clamp(percent, 0, 100));
  if (!stageLabel.isEmpty())
    m_status->setText(stageLabel);
}

void PropertyModelPanel::showResult(bool ok, const QString &summary)
{
  m_busy = false;
  m_progress->setValue(ok ? 100 : m_progress->value());
  m_status->setText(summary);
  syncEnabledState();
}

void PropertyModelPanel::syncEnabledState()
{
  const bool named = !topHorizon().isEmpty() && !bottomHorizon().isEmpty() &&
                     !curveMnemonic().isEmpty() && topHorizon() != bottomHorizon();
  m_build->setEnabled(!m_busy && named);
  m_cancel->setEnabled(m_busy);
  m_top->setEnabled(!m_busy);
  m_bot->setEnabled(!m_busy);
  m_curve->setEnabled(!m_busy);
  m_layers->setEnabled(!m_busy);
  m_agg->setEnabled(!m_busy);
  m_power->setEnabled(!m_busy && method() == 0);
  m_method->setEnabled(!m_busy);
  m_sgsGroup->setEnabled(!m_busy && method() == 1);
  m_facies->setEnabled(!m_busy);
  m_objectGroup->setEnabled(!m_busy);
}
