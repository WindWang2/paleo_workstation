// 层：视图
#include "ui/seismicsection/seismicattrpanel.h"
#include "ui/paleotheme.h"

#include <QComboBox>
#include <QHBoxLayout>
#include <QLabel>
#include <QProgressBar>
#include <QSlider>
#include <QSpinBox>
#include <QToolButton>
#include <QVBoxLayout>

namespace seismic {

using Kind = SeismicTaskService::SeismicAttrKind;

SeismicAttrPanel::SeismicAttrPanel(QWidget *parent)
    : QWidget(parent)
{
    buildUi();
}

void SeismicAttrPanel::buildUi()
{
    auto *lay = new QVBoxLayout(this);
    lay->setContentsMargins(PaleoTheme::tokens().spacingSm, PaleoTheme::tokens().spacingXs, PaleoTheme::tokens().spacingSm, PaleoTheme::tokens().spacingXs);
    lay->setSpacing(PaleoTheme::tokens().spacingXs);

    // 行 1：属性种类 + 计算动作（运行类动作主色文案——DESIGN.md ribbon-button）
    auto *row1 = new QHBoxLayout();
    row1->addWidget(new QLabel(tr("属性:")));
    m_cboKind = new QComboBox();
    m_cboKind->setObjectName(QStringLiteral("attrKindCombo"));
    const QList<QPair<Kind, QString>> kinds = {
        {Kind::Envelope, SeismicTaskService::seismicAttrDisplayName(Kind::Envelope)},
        {Kind::InstPhase, SeismicTaskService::seismicAttrDisplayName(Kind::InstPhase)},
        {Kind::InstFreq, SeismicTaskService::seismicAttrDisplayName(Kind::InstFreq)},
        {Kind::InstQ, SeismicTaskService::seismicAttrDisplayName(Kind::InstQ)},
        {Kind::Rms, SeismicTaskService::seismicAttrDisplayName(Kind::Rms)},
        {Kind::MaxAbs, SeismicTaskService::seismicAttrDisplayName(Kind::MaxAbs)},
        {Kind::MeanEnergy, SeismicTaskService::seismicAttrDisplayName(Kind::MeanEnergy)},
        {Kind::Coherence, SeismicTaskService::seismicAttrDisplayName(Kind::Coherence)},
        {Kind::Sweetness, SeismicTaskService::seismicAttrDisplayName(Kind::Sweetness)}};
    for (const auto &k : kinds)
        m_cboKind->addItem(k.second, int(k.first));
    m_cboKind->setFixedWidth(150);
    row1->addWidget(m_cboKind);

    auto mkBtn = [this](const QString &text, const QString &tip, bool primary) {
        auto *b = new QToolButton(this);
        b->setText(text);
        b->setToolTip(tip);
        // 运行类主色文案走 PaleoTheme token（#77：原裸 hex hover 底在暗色主题
        // 下不翻转）；applyThemedStyleSheet 随主题切换活体重算。
        if (primary) {
            PaleoTheme::applyThemedStyleSheet(b, [] {
                const auto &t = PaleoTheme::tokens();
                return QStringLiteral("QToolButton{color:%1;font-weight:500;}"
                                      "QToolButton:hover{background:%2;}")
                    .arg(t.primaryText.name(), t.surfaceAltRaised.name());
            });
        }
        return b;
    };
    m_btnCompute = mkBtn(tr("▶ 计算属性"),
                         tr("对当前剖面计算所选属性并叠加显示"), /*primary=*/true);
    m_btnCompute->setObjectName(QStringLiteral("attrComputeButton"));
    row1->addWidget(m_btnCompute);
    m_btnCancel = mkBtn(tr("取消"), tr("取消在途属性任务"), /*primary=*/false);
    m_btnCancel->setObjectName(QStringLiteral("attrCancelButton"));
    row1->addWidget(m_btnCancel);
    m_btnRegister = mkBtn(tr("登记资产"),
                          tr("把最近一次属性结果登记为派生资产（需应用层注入 catalog）"),
                          /*primary=*/false);
    m_btnRegister->setObjectName(QStringLiteral("attrRegisterButton"));
    row1->addWidget(m_btnRegister);
    row1->addStretch();
    lay->addLayout(row1);

    // 行 2：参数（时窗族/相干各用各的；alpha 叠加透明度实时生效）
    auto *row2 = new QHBoxLayout();
    row2->addWidget(new QLabel(tr("时窗半窗(样):")));
    m_spinWindowHalf = new QSpinBox();
    m_spinWindowHalf->setObjectName(QStringLiteral("attrWindowHalf"));
    m_spinWindowHalf->setRange(0, 128);
    m_spinWindowHalf->setValue(8);
    m_spinWindowHalf->setFixedWidth(56);
    row2->addWidget(m_spinWindowHalf);
    row2->addWidget(new QLabel(tr("相干 IL/XL/垂向半窗:")));
    m_spinIlHalf = new QSpinBox();
    m_spinIlHalf->setObjectName(QStringLiteral("attrIlHalf"));
    m_spinIlHalf->setRange(1, 4);
    m_spinIlHalf->setValue(1);
    m_spinIlHalf->setFixedWidth(40);
    row2->addWidget(m_spinIlHalf);
    m_spinXlHalf = new QSpinBox();
    m_spinXlHalf->setObjectName(QStringLiteral("attrXlHalf"));
    m_spinXlHalf->setRange(1, 4);
    m_spinXlHalf->setValue(1);
    m_spinXlHalf->setFixedWidth(40);
    row2->addWidget(m_spinXlHalf);
    m_spinTimeHalf = new QSpinBox();
    m_spinTimeHalf->setObjectName(QStringLiteral("attrTimeHalf"));
    m_spinTimeHalf->setRange(1, 16);
    m_spinTimeHalf->setValue(2);
    m_spinTimeHalf->setFixedWidth(40);
    row2->addWidget(m_spinTimeHalf);
    row2->addWidget(new QLabel(tr("叠加透明度:")));
    m_sliderAlpha = new QSlider(Qt::Horizontal);
    m_sliderAlpha->setObjectName(QStringLiteral("attrAlpha"));
    m_sliderAlpha->setRange(0, 100);
    m_sliderAlpha->setValue(65);
    m_sliderAlpha->setFixedWidth(110);
    row2->addWidget(m_sliderAlpha);
    row2->addStretch();
    lay->addLayout(row2);

    // 行 3：进度 + 状态
    auto *row3 = new QHBoxLayout();
    m_progress = new QProgressBar();
    m_progress->setObjectName(QStringLiteral("attrProgress"));
    m_progress->setRange(0, 100);
    m_progress->setValue(0);
    m_progress->setFixedWidth(160);
    m_progress->setTextVisible(false);
    row3->addWidget(m_progress);
    m_lblStatus = new QLabel(tr("未计算"));
    m_lblStatus->setObjectName(QStringLiteral("attrStatus"));
    row3->addWidget(m_lblStatus, 1);
    lay->addLayout(row3);

    connect(m_btnCompute, &QToolButton::clicked, this, [this]() {
        emit computeRequested(currentKind(), currentParams(), currentAlpha());
    });
    connect(m_btnCancel, &QToolButton::clicked, this, [this]() {
        emit cancelRequested();
    });
    connect(m_btnRegister, &QToolButton::clicked, this, [this]() {
        emit registerRequested();
    });
    connect(m_sliderAlpha, &QSlider::valueChanged, this, [this](int v) {
        emit alphaChanged(double(v) / 100.0);
    });
    syncEnabledState();
}

Kind SeismicAttrPanel::currentKind() const
{
    return Kind(m_cboKind->currentData().toInt());
}

SeismicTaskService::SeismicAttrParams SeismicAttrPanel::currentParams() const
{
    SeismicTaskService::SeismicAttrParams p;
    p.windowHalfSamples = m_spinWindowHalf->value();
    p.coherenceIlHalf = m_spinIlHalf->value();
    p.coherenceXlHalf = m_spinXlHalf->value();
    p.coherenceTimeHalf = m_spinTimeHalf->value();
    return p;
}

double SeismicAttrPanel::currentAlpha() const
{
    return double(m_sliderAlpha->value()) / 100.0;
}

void SeismicAttrPanel::setBusy(bool busy)
{
    m_busy = busy;
    if (busy)
    {
        m_progress->setValue(0);
        m_lblStatus->setText(tr("计算中…"));
    }
    syncEnabledState();
}

void SeismicAttrPanel::updateProgress(int percent, const QString &stageLabel)
{
    m_progress->setValue(percent);
    if (!stageLabel.isEmpty())
        m_lblStatus->setText(tr("计算中… %1 %2%").arg(stageLabel).arg(percent));
}

void SeismicAttrPanel::showResult(bool ok, const QString &summary)
{
    m_progress->setValue(ok ? 100 : m_progress->value());
    m_lblStatus->setText(summary);
    m_busy = false;
    m_hasResult = ok;
    syncEnabledState();
}

void SeismicAttrPanel::syncEnabledState()
{
    m_btnCompute->setEnabled(!m_busy);
    m_btnCancel->setEnabled(m_busy);
    m_btnRegister->setEnabled(!m_busy && m_hasResult);
}

} // namespace seismic
