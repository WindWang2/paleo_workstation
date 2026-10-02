// 层：视图
#include "propertymodelpanel.h"

#include "../paleotheme.h"

#include <algorithm>

#include <QComboBox>
#include <QFont>
#include <QDoubleSpinBox>
#include <QFormLayout>
#include <QHBoxLayout>
#include <QLabel>
#include <QLineEdit>
#include <QProgressBar>
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
  lay->setContentsMargins(8, 8, 8, 8);
  lay->setSpacing(8);

  auto *form = new QFormLayout();
  form->setSpacing(8);
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
  form->addRow(tr("IDW 幂"), m_power);
  lay->addLayout(form);

  auto *row = new QHBoxLayout();
  row->setSpacing(8);
  m_build = new QToolButton(this);
  m_build->setObjectName(QStringLiteral("propBuildButton"));
  m_build->setText(tr("建立属性体"));
  m_build->setToolTip(tr("按当前参数发建模意图。计算在功能层，面板不改网格。"));
  {
    QPalette pal = m_build->palette();
    pal.setColor(QPalette::ButtonText, PaleoTheme::kColorPrimary);
    m_build->setPalette(pal);
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
  alphaRow->setSpacing(8);
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
}

QString PropertyModelPanel::topHorizon() const { return m_top->text().trimmed(); }
QString PropertyModelPanel::bottomHorizon() const { return m_bot->text().trimmed(); }
QString PropertyModelPanel::curveMnemonic() const { return m_curve->text().trimmed(); }
int PropertyModelPanel::layerCount() const { return m_layers->value(); }
int PropertyModelPanel::aggregator() const { return m_agg->currentData().toInt(); }
double PropertyModelPanel::idwPower() const { return m_power->value(); }
double PropertyModelPanel::overlayAlpha() const { return m_alpha->value() / 100.0; }

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
  m_power->setEnabled(!m_busy);
}
