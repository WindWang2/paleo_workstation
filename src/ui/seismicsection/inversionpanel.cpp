// 层：视图
#include "ui/seismicsection/inversionpanel.h"
#include "ui/paleotheme.h"

#include <QComboBox>
#include <QDoubleSpinBox>
#include <QFileDialog>
#include <QFrame>
#include <QHBoxLayout>
#include <QLabel>
#include <QLineEdit>
#include <QProgressBar>
#include <QSpinBox>
#include <QToolButton>
#include <QVBoxLayout>

namespace seismic {

InversionPanel::InversionPanel(QWidget *parent)
    : QWidget(parent)
{
    buildUi();
}

void InversionPanel::buildUi()
{
    auto *lay = new QVBoxLayout(this);
    lay->setContentsMargins(PaleoTheme::tokens().spacingSm, PaleoTheme::tokens().spacingXs, PaleoTheme::tokens().spacingSm, PaleoTheme::tokens().spacingXs);
    lay->setSpacing(PaleoTheme::tokens().spacingXs);

    // 行 1：方法 + 运行类动作（主色文案——DESIGN.md ribbon-button）
    auto *row1 = new QHBoxLayout();
    row1->addWidget(new QLabel(tr("反演:")));
    m_cboMethod = new QComboBox();
    m_cboMethod->setObjectName(QStringLiteral("invMethodCombo"));
    m_cboMethod->addItem(tr("带限道积分"), QStringLiteral("bandlimited"));
    m_cboMethod->addItem(tr("稀疏脉冲"), QStringLiteral("sparse"));
    m_cboMethod->setFixedWidth(120);
    row1->addWidget(m_cboMethod);

    auto mkBtn = [this](const QString &text, const QString &tip, bool primary) {
        auto *b = new QToolButton(this);
        b->setText(text);
        b->setToolTip(tip);
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
    m_btnRun = mkBtn(tr("▶ 反演"),
                     tr("对整个地震体做确定性叠后反演（道并行，DERIVED 登记）"), /*primary=*/true);
    m_btnRun->setObjectName(QStringLiteral("invRunButton"));
    row1->addWidget(m_btnRun);
    m_btnCancel = mkBtn(tr("取消"), tr("取消在途反演任务"), /*primary=*/false);
    m_btnCancel->setObjectName(QStringLiteral("invCancelButton"));
    row1->addWidget(m_btnCancel);
    row1->addStretch();
    lay->addLayout(row1);

    // 行 2：频带/正则化/迭代 + 子波
    auto *row2 = new QHBoxLayout();
    row2->addWidget(new QLabel(tr("低频截止:")));
    m_spinLowCut = new QDoubleSpinBox();
    m_spinLowCut->setObjectName(QStringLiteral("invLowCut"));
    m_spinLowCut->setRange(2.0, 24.0);
    m_spinLowCut->setDecimals(1);
    m_spinLowCut->setSingleStep(1.0);
    m_spinLowCut->setValue(8.0);
    m_spinLowCut->setSuffix(tr(" Hz"));
    m_spinLowCut->setFixedWidth(72);
    row2->addWidget(m_spinLowCut);

    row2->addWidget(new QLabel(tr("λ(稀疏):")));
    m_spinLambda = new QDoubleSpinBox();
    m_spinLambda->setObjectName(QStringLiteral("invLambda"));
    m_spinLambda->setRange(0.0, 1.0);
    m_spinLambda->setDecimals(3);
    m_spinLambda->setSingleStep(0.005);
    m_spinLambda->setValue(0.0);
    m_spinLambda->setSpecialValueText(tr("自动"));
    m_spinLambda->setFixedWidth(72);
    row2->addWidget(m_spinLambda);

    row2->addWidget(new QLabel(tr("迭代上限:")));
    m_spinIterations = new QSpinBox();
    m_spinIterations->setObjectName(QStringLiteral("invIterations"));
    m_spinIterations->setRange(8, 10000);
    m_spinIterations->setValue(200);
    m_spinIterations->setFixedWidth(64);
    row2->addWidget(m_spinIterations);
    row2->addStretch();
    lay->addLayout(row2);

    // 行 3：子波（提取入口 + 文件路径 + 浏览）
    auto *row3 = new QHBoxLayout();
    m_btnExtractWavelet = mkBtn(tr("提取子波…"),
                                tr("最近井 AC×DEN + 时深 + 井旁道最小二乘提取（DERIVED 登记）"),
                                /*primary=*/false);
    m_btnExtractWavelet->setObjectName(QStringLiteral("invExtractWaveletButton"));
    row3->addWidget(m_btnExtractWavelet);
    m_editWavelet = new QLineEdit();
    m_editWavelet->setObjectName(QStringLiteral("invWaveletPath"));
    m_editWavelet->setPlaceholderText(tr("子波 wavelet.json 路径（先提取或浏览）"));
    row3->addWidget(m_editWavelet, 1);
    m_btnWaveletBrowse = mkBtn(tr("浏览…"), tr("选择已有子波资产文件"), /*primary=*/false);
    row3->addWidget(m_btnWaveletBrowse);
    lay->addLayout(row3);

    // 行 4：进度 + 状态（如实口径：带限 + 低频，不标「高分辨率」）
    m_progress = new QProgressBar();
    m_progress->setObjectName(QStringLiteral("invProgress"));
    m_progress->setRange(0, 100);
    m_progress->setValue(0);
    lay->addWidget(m_progress);
    m_lblStatus = new QLabel(tr("阻抗 = 低频模型(0–低频截止) + 地震带限；绝对趋势由井控承载。"));
    m_lblStatus->setObjectName(QStringLiteral("invStatusLabel"));
    lay->addWidget(m_lblStatus);

    connect(m_btnRun, &QToolButton::clicked, this, [this]() {
        emit inversionRequested(currentParams());
    });
    connect(m_btnCancel, &QToolButton::clicked, this, [this]() { emit cancelRequested(); });
    connect(m_btnExtractWavelet, &QToolButton::clicked, this,
            [this]() { emit extractWaveletRequested(); });
    connect(m_btnWaveletBrowse, &QToolButton::clicked, this, [this]() {
        const QString path = QFileDialog::getOpenFileName(
            this, tr("选择子波文件"), QString(), tr("子波 (*.json)"));
        if (!path.isEmpty())
            m_editWavelet->setText(path);
    });
    connect(m_cboMethod, &QComboBox::currentIndexChanged, this,
            [this]() { syncEnabledState(); });
    syncEnabledState();
}

InversionPanelParams InversionPanel::currentParams() const
{
    InversionPanelParams p;
    p.method = m_cboMethod->currentData().toString();
    p.lowCutHz = m_spinLowCut->value();
    p.lambda = m_spinLambda->value();
    p.maxIterations = m_spinIterations->value();
    p.waveletPath = m_editWavelet->text().trimmed();
    return p;
}

void InversionPanel::setWaveletPath(const QString &path)
{
    m_editWavelet->setText(path);
}

void InversionPanel::setBusy(bool busy)
{
    m_busy = busy;
    m_btnRun->setEnabled(!busy);
    m_btnCancel->setEnabled(busy);
    m_progress->setRange(0, 100);
    m_progress->setValue(busy ? 1 : 0);
    if (!busy)
        m_lblStatus->setText(tr("阻抗 = 低频模型(0–低频截止) + 地震带限；绝对趋势由井控承载。"));
    syncEnabledState();
}

void InversionPanel::updateProgress(int percent, const QString &stageLabel)
{
    m_progress->setValue(qBound(0, percent, 100));
    if (!stageLabel.isEmpty())
        m_lblStatus->setText(stageLabel);
}

void InversionPanel::showResult(bool ok, const QString &summary)
{
    setBusy(false);
    m_lblStatus->setText(summary);
    m_lblStatus->setProperty("ok", ok);
}

void InversionPanel::syncEnabledState()
{
    const bool sparse = m_cboMethod->currentData().toString() == QStringLiteral("sparse");
    m_spinLambda->setEnabled(sparse);
    m_spinIterations->setEnabled(sparse);
}

} // namespace seismic
