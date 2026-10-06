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

    // 行 1：属性种类 + 扫描范围 + 计算动作（运行类动作主色文案——DESIGN.md ribbon-button）
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
    // goal/attr-volume：扫描范围——本剖面（叠加显示）/时间切片（地理参考
    // 栅格 → 层树）/属性体（SATV → 3D 体视）
    row1->addWidget(new QLabel(tr("范围:")));
    m_cboScope = new QComboBox();
    m_cboScope->setObjectName(QStringLiteral("attrScopeCombo"));
    m_cboScope->addItem(tr("本剖面"), 0);
    m_cboScope->addItem(tr("时间切片"), 1);
    m_cboScope->addItem(tr("属性体"), 2);
    m_cboScope->setFixedWidth(88);
    m_cboScope->setToolTip(tr("时间切片/属性体按属性族所需窗口全测网扫描"
                              "（瞬时族整道谱、时窗族垂向窗、相干三维窗）"));
    row1->addWidget(m_cboScope);
    // 时间切片采样位（范围=时间切片时启用；范围值由 dock 按体回填）
    m_lblTimeSample = new QLabel(tr("采样:"));
    row1->addWidget(m_lblTimeSample);
    m_spinTimeSample = new QSpinBox();
    m_spinTimeSample->setObjectName(QStringLiteral("attrTimeSample"));
    m_spinTimeSample->setRange(0, 0);
    m_spinTimeSample->setFixedWidth(64);
    row1->addWidget(m_spinTimeSample);

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
    // goal/attr-volume：相干道距加权档（等权=原语义；道距加权按 IL/XL 道距
    // 反比抬近道贡献——各向异性测网时两档结果不同）
    row2->addWidget(new QLabel(tr("相干加权:")));
    m_cboWeight = new QComboBox();
    m_cboWeight->setObjectName(QStringLiteral("attrWeightCombo"));
    m_cboWeight->addItem(tr("等权"), 0);
    m_cboWeight->addItem(tr("道距加权"), 1);
    m_cboWeight->setFixedWidth(88);
    row2->addWidget(m_cboWeight);
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
        // 扫描范围分派：本剖面走叠加路径；时间切片/属性体走体化扫描
        // （意图信号——编排/登记/上图归 dock 与 app 层）。
        switch (currentScope()) {
        case 1:
            emit timeSliceScanRequested(currentKind(), currentParams(),
                                        currentSampleIndex());
            break;
        case 2:
            emit volumeScanRequested(currentKind(), currentParams());
            break;
        default:
            emit computeRequested(currentKind(), currentParams(), currentAlpha());
            break;
        }
    });
    connect(m_cboScope, &QComboBox::currentIndexChanged, this,
            [this](int) { syncEnabledState(); });
    connect(m_spinTimeSample, &QSpinBox::valueChanged, this,
            [this](int) { m_sampleTouched = true; });
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
    p.coherenceWeighting = m_cboWeight ? m_cboWeight->currentData().toInt() : 0;
    return p;
}

int SeismicAttrPanel::currentScope() const
{
    return m_cboScope ? m_cboScope->currentData().toInt() : 0;
}

int SeismicAttrPanel::currentSampleIndex() const
{
    return m_spinTimeSample ? m_spinTimeSample->value() : 0;
}

void SeismicAttrPanel::setVolumeSampleRange(int sampleCount)
{
    if (!m_spinTimeSample)
        return;
    const int maxIdx = qMax(0, sampleCount - 1);
    m_spinTimeSample->setRange(0, maxIdx);
    // 只在越界或缺省态改值——用户显式选过的采样位不因换体被重置。
    if (m_sampleTouched)
    {
        if (m_spinTimeSample->value() > maxIdx)
            m_spinTimeSample->setValue(maxIdx);
        return;
    }
    // 程序化缺省不标记用户触碰（blockSignals 防 valueChanged 误置标志）。
    m_spinTimeSample->blockSignals(true);
    m_spinTimeSample->setValue(maxIdx / 2); // 缺省中位采样
    m_spinTimeSample->blockSignals(false);
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

void SeismicAttrPanel::showResult(bool ok, const QString &summary,
                                  bool registrable)
{
    m_progress->setValue(ok ? 100 : m_progress->value());
    m_lblStatus->setText(summary);
    m_busy = false;
    // registrable=false：扫描类结果（完成回调里已自动登记/上图）——不点亮
    // 「登记资产」按钮（那是剖面结果专用，防新参数配旧图像的错配登记）。
    m_hasResult = ok && registrable;
    syncEnabledState();
}

void SeismicAttrPanel::syncEnabledState()
{
    m_btnCompute->setEnabled(!m_busy);
    m_btnCancel->setEnabled(m_busy);
    // 「登记资产」只服务剖面结果（扫描产物在完成回调里自动登记——切片
    // 入层树、体入 3D，无需也不应手点：防「新参数 + 旧剖面图像」错配）。
    m_btnRegister->setEnabled(!m_busy && m_hasResult && currentScope() == 0);
    // 采样位仅时间切片范围有意义。
    const bool ts = currentScope() == 1;
    if (m_spinTimeSample)
        m_spinTimeSample->setEnabled(ts);
    if (m_lblTimeSample)
        m_lblTimeSample->setEnabled(ts);
}

} // namespace seismic
