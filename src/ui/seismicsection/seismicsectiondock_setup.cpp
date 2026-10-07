// 层：视图
// 方向 65：剖面 dock 的 UI 装配 TU——顶部工具栏 / D2.x 显示控制行 / 解释·属性·
// 反演三个面板的构造与信号接线。构造顺序与信号语义逐条照搬，无「顺手改进」。
#include "ui/seismicsection/seismicsectiondockwidget.h"
#include "ui/seismicsection/seismicattrpanel.h"
#include "ui/seismicsection/seismicpickpanel.h"
#include "ui/seismicsection/inversionpanel.h"

#include "ui/paleotheme.h"
#include "ui/paleoviewport.h"

#include <QAction>
#include <QActionGroup>
#include <QCheckBox>
#include <QComboBox>
#include <QDialogButtonBox>
#include <QDoubleSpinBox>
#include <QFormLayout>
#include <QHBoxLayout>
#include <QInputDialog>
#include <QLabel>
#include <QLineEdit>
#include <QProgressBar>
#include <QRegularExpression>
#include <QSlider>
#include <QSpinBox>
#include <QTextEdit>
#include <QToolButton>
#include <QMenu>
#include <QUndoStack>
#include <QVBoxLayout>

namespace seismic {

namespace {

// 显示条共用中性工具按钮与次级标签，随主题重算。
void themedButtonStyle(QWidget *w)
{
    PaleoTheme::applyThemedStyleSheet(w, [] { return PaleoTheme::toolButtonStyleSheet(); });
}
void themedCaptionStyle(QWidget *w)
{
    PaleoTheme::applyThemedStyleSheet(w, [] {
        return PaleoTheme::mutedCaptionStyleSheet();
    });
}
void themedComboStyle(QWidget *w)
{
    PaleoTheme::applyThemedStyleSheet(w, [] {
        const auto &t = PaleoTheme::tokens();
        return PaleoTheme::metricStyleSheet(QStringLiteral(
            "QComboBox { border: 1px solid %1; border-radius: {rounded.sm}px;"
            " padding: {spacing.xs}px {spacing.sm}px; font-size: {typography.label}pt; }"))
            .arg(t.border.name());
    });
}

} // namespace

void SeismicSectionDockWidget::setupUi() {
    auto *container = new QWidget(this);
    auto *mainLay = new QVBoxLayout(container);
    mainLay->setContentsMargins(0, 0, 0, 0);
    mainLay->setSpacing(0);

    // ==========================================
    // 1. Top Toolbar（chrome 全 token，活体注册随主题切换重算）
    // ==========================================
    auto *toolbar = new QWidget(container);
    PaleoTheme::applyThemedStyleSheet(toolbar, [] {
        const auto &t = PaleoTheme::tokens();
        return QStringLiteral("background: %1; border-bottom: 1px solid %2;")
            .arg(t.surface.name().toUpper(), t.border.name().toUpper());
    });
    auto *toolLay = new QHBoxLayout(toolbar);
    toolLay->setContentsMargins(PaleoTheme::tokens().spacingSm, PaleoTheme::tokens().spacingXs, PaleoTheme::tokens().spacingSm, PaleoTheme::tokens().spacingXs);
    toolLay->setSpacing(PaleoTheme::tokens().spacingSm);

    auto applyBtnStyle = [](QToolButton *btn) {
        PaleoTheme::applyThemedStyleSheet(btn, [] { return PaleoTheme::toolButtonStyleSheet(); });
    };
    auto *setup = new QToolButton(toolbar);
    setup->setObjectName("sectionSetupButton");
    setup->setText(tr("连井 / 时深"));
    setup->setToolTip(tr("选择连井顺序、绘制任意折线、调整逐井时深关系"));
    applyBtnStyle(setup);
    toolLay->addWidget(setup);
    connect(setup, &QToolButton::clicked, this,
            &SeismicSectionDockWidget::setupRequested);

    // Title Badge（中性徽章：不占 primary）
    m_lblTitle = new QLabel(tr("测线: 未加载"), toolbar);
    PaleoTheme::applyThemedStyleSheet(m_lblTitle, [] {
        const auto &t = PaleoTheme::tokens();
        return PaleoTheme::metricStyleSheet(QStringLiteral(
            "QLabel { background: %1; color: %2; font-weight: bold; border-radius: {rounded.sm}px; padding: {spacing.xs}px {spacing.sm}px; font-size: {typography.body}pt; }"))
            .arg(t.surfaceAltRaised.name().toUpper(), t.text.name().toUpper());
    });
    toolLay->addWidget(m_lblTitle);

    // Section Mode Combo
    m_cboSectionMode = new QComboBox(toolbar);
    m_cboSectionMode->setObjectName(QStringLiteral("cboSectionMode"));
    m_cboSectionMode->addItems({tr("纵测线 (IL)"), tr("横测线 (XL)"), tr("时间切片 (Time)"), tr("任意测线/井剖面")});
    PaleoTheme::applyThemedStyleSheet(m_cboSectionMode, [] {
        const auto &t = PaleoTheme::tokens();
        return PaleoTheme::metricStyleSheet(QStringLiteral(
            "QComboBox { border: 1px solid %1; border-radius: {rounded.sm}px; padding: {spacing.xs}px {spacing.sm}px; font-size: {typography.label}pt; color: %2; background: %3; font-weight: 500; }"))
            .arg(t.border.name().toUpper(), t.text.name().toUpper(), t.surface.name().toUpper());
    });
    toolLay->addWidget(m_cboSectionMode);

    // Slicing Group (Slider + Spin + Time label)
    m_sliceGroup = new QWidget(toolbar);
    auto *sliceLay = new QHBoxLayout(m_sliceGroup);
    sliceLay->setContentsMargins(0, 0, 0, 0);
    sliceLay->setSpacing(PaleoTheme::tokens().spacingXs);

    m_lblSliceIndex = new QLabel(tr("纵测线:"), m_sliceGroup);
    PaleoTheme::applyThemedStyleSheet(m_lblSliceIndex, [] {
        return PaleoTheme::metricStyleSheet(QStringLiteral("color: %1; font-size: {typography.label}pt; font-weight: 500;"))
            .arg(PaleoTheme::tokens().textMuted.name().toUpper());
    });
    sliceLay->addWidget(m_lblSliceIndex);

    m_sliderSlice = new QSlider(Qt::Horizontal, m_sliceGroup);
    m_sliderSlice->setObjectName(QStringLiteral("sliderSectionSlice"));
    m_sliderSlice->setRange(1, 100);
    m_sliderSlice->setValue(1);
    m_sliderSlice->setFixedWidth(90);
    sliceLay->addWidget(m_sliderSlice);

    m_spinSlice = new QSpinBox(m_sliceGroup);
    m_spinSlice->setObjectName(QStringLiteral("spinSectionSlice"));
    {
        QFont f = PaleoTheme::monoFont();
        f.setPointSize(PaleoTheme::tokens().labelPt);
        m_spinSlice->setFont(f);
    }
    m_spinSlice->setRange(1, 100);
    m_spinSlice->setValue(1);
    m_spinSlice->setFixedWidth(64);
    sliceLay->addWidget(m_spinSlice);

    m_lblTimeMs = new QLabel(QStringLiteral("0.0 ms"), m_sliceGroup);
    m_lblTimeMs->setObjectName(QStringLiteral("lblSectionTimeMs"));
    {
        QFont f = PaleoTheme::monoFont();
        f.setPointSize(PaleoTheme::tokens().labelPt);
        m_lblTimeMs->setFont(f);
    }
    PaleoTheme::applyThemedStyleSheet(m_lblTimeMs, [] {
        return QStringLiteral("color: %1;")
            .arg(PaleoTheme::tokens().text.name().toUpper());
    });
    m_lblTimeMs->setFixedWidth(68);
    m_lblTimeMs->setVisible(false);
    sliceLay->addWidget(m_lblTimeMs);

    toolLay->addWidget(m_sliceGroup);
    toolLay->addStretch(1);
    mainLay->addWidget(new PaleoToolRow(toolbar, container));
    toolbar = new QWidget(container);
    toolLay = new QHBoxLayout(toolbar);
    toolLay->setContentsMargins(PaleoTheme::tokens().spacingSm, PaleoTheme::tokens().spacingXs, PaleoTheme::tokens().spacingSm, PaleoTheme::tokens().spacingXs);
    toolLay->setSpacing(PaleoTheme::tokens().spacingXs);

    // Zoom Buttons
    m_btnZoomIn = new QToolButton(toolbar);
    m_btnZoomIn->setObjectName(QStringLiteral("btnSectionZoomIn"));
    m_btnZoomIn->setText(tr("+ 放大"));
    applyBtnStyle(m_btnZoomIn);
    toolLay->addWidget(m_btnZoomIn);

    m_btnZoomOut = new QToolButton(toolbar);
    m_btnZoomOut->setObjectName(QStringLiteral("btnSectionZoomOut"));
    m_btnZoomOut->setText(tr("- 缩小"));
    applyBtnStyle(m_btnZoomOut);
    toolLay->addWidget(m_btnZoomOut);

    m_btnFit = new QToolButton(toolbar);
    m_btnFit->setObjectName(QStringLiteral("btnSectionFit"));
    m_btnFit->setText(tr("适应窗口"));
    applyBtnStyle(m_btnFit);
    toolLay->addWidget(m_btnFit);

    m_btnReset = new QToolButton(toolbar);
    m_btnReset->setObjectName(QStringLiteral("btnSectionReset"));
    m_btnReset->setText(tr("1:1"));
    applyBtnStyle(m_btnReset);
    toolLay->addWidget(m_btnReset);

    // Vertical Unit Toggle
    m_btnUnitToggle = new QToolButton(toolbar);
    m_btnUnitToggle->setObjectName(QStringLiteral("btnSectionUnitToggle"));
    m_btnUnitToggle->setText(tr("单位: TWT (ms)"));
    m_btnUnitToggle->setToolTip(
        tr("切换 TWT 与常速参考深度尺；井段仍按本井时深表对齐"));
    applyBtnStyle(m_btnUnitToggle);
    toolLay->addWidget(m_btnUnitToggle);

    // Colormap Combo
    auto *lblCmap = new QLabel(tr("色标:"), toolbar);
    PaleoTheme::applyThemedStyleSheet(lblCmap, [] {
        return PaleoTheme::metricStyleSheet(QStringLiteral("color: %1; font-size: {typography.label}pt;"))
            .arg(PaleoTheme::tokens().textMuted.name().toUpper());
    });
    toolLay->addWidget(lblCmap);

    m_cboColorMap = new QComboBox(toolbar);
    m_cboColorMap->setObjectName(QStringLiteral("cboSectionColorMap"));
    m_cboColorMap->addItems({tr("红白蓝 (双极)"), tr("灰度 (单极)"), tr("彩虹谱 (相图)"),
                             tr("蓝白红 (反双极)"), tr("黑-白-蓝 (纸面)"), tr("红-白-黑 (纸面)"),
                             tr("绿-白-品红"), tr("青-白-橙")});
    PaleoTheme::applyThemedStyleSheet(m_cboColorMap, [] {
        const auto &t = PaleoTheme::tokens();
        return PaleoTheme::metricStyleSheet(QStringLiteral(
            "QComboBox { border: 1px solid %1; border-radius: {rounded.sm}px; padding: {spacing.xs}px {spacing.sm}px; font-size: {typography.label}pt; color: %2; background: %3; }"))
            .arg(t.border.name().toUpper(), t.text.name().toUpper(), t.surface.name().toUpper());
    });
    toolLay->addWidget(m_cboColorMap);

    // Gain Control
    auto *lblGain = new QLabel(tr("增益:"), toolbar);
    PaleoTheme::applyThemedStyleSheet(lblGain, [] {
        return PaleoTheme::metricStyleSheet(QStringLiteral("color: %1; font-size: {typography.label}pt;"))
            .arg(PaleoTheme::tokens().textMuted.name().toUpper());
    });
    toolLay->addWidget(lblGain);

    m_sliderGain = new QSlider(Qt::Horizontal, toolbar);
    m_sliderGain->setObjectName(QStringLiteral("sliderSectionGain"));
    m_sliderGain->setRange(10, 500); // 0.1x to 5.0x
    m_sliderGain->setValue(100);     // 1.0x
    m_sliderGain->setFixedWidth(80);
    toolLay->addWidget(m_sliderGain);

    m_spinGain = new QDoubleSpinBox(toolbar);
    m_spinGain->setObjectName(QStringLiteral("spinSectionGain"));
    m_spinGain->setRange(0.1, 5.0);
    m_spinGain->setSingleStep(0.1);
    m_spinGain->setValue(1.0);
    m_spinGain->setSuffix(QStringLiteral("x"));
    m_spinGain->setFixedWidth(56);
    toolLay->addWidget(m_spinGain);

    // Well Overlay Options
    m_btnWellOptions = new QToolButton(toolbar);
    m_btnWellOptions->setObjectName(QStringLiteral("btnSectionWellOptions"));
    m_btnWellOptions->setText(tr("井与分层 ▼"));
    m_btnWellOptions->setPopupMode(QToolButton::InstantPopup);
    applyBtnStyle(m_btnWellOptions);

    auto *wellMenu = new QMenu(m_btnWellOptions);
    auto *actShowWells = wellMenu->addAction(tr("显示井位"));
    actShowWells->setCheckable(true);
    actShowWells->setChecked(true);

    auto *actShowTops = wellMenu->addAction(tr("显示地层分层"));
    actShowTops->setCheckable(true);
    actShowTops->setChecked(true);

    auto *actShowCurves = wellMenu->addAction(tr("显示测井曲线"));
    actShowCurves->setCheckable(true);
    actShowCurves->setChecked(true);

    wellMenu->addSeparator();
    auto *actBuf200 = wellMenu->addAction(tr("缓冲半径: 200 m"));
    auto *actBuf500 = wellMenu->addAction(tr("缓冲半径: 500 m"));
    auto *actBuf1000 = wellMenu->addAction(tr("缓冲半径: 1000 m"));
    auto *bufGroup = new QActionGroup(wellMenu);
    bufGroup->setExclusive(true);
    for (QAction *act : {actBuf200, actBuf500, actBuf1000}) {
        act->setCheckable(true);
        bufGroup->addAction(act);
    }
    // 勾选与画布缺省缓冲（500 m）一致
    actBuf500->setChecked(true);
    wellMenu->addSeparator();
    // D5.7 多井投影开关
    auto *actAllWells = wellMenu->addAction(tr("投影井数: 全部"));
    auto *actNear1 = wellMenu->addAction(tr("投影井数: 最近 1 口"));
    auto *actNear3 = wellMenu->addAction(tr("投影井数: 最近 3 口"));
    wellMenu->addSeparator();
    auto *actWellTrace = wellMenu->addAction(tr("井旁道小图（最近井）…")); // D5.6
    auto *actExtractWavelet = wellMenu->addAction(tr("提取子波（最近井）…")); // 方向22
    auto *actInversionPanel = wellMenu->addAction(tr("反演面板"));          // 方向22
    auto *actArbLine = wellMenu->addAction(tr("任意线编辑器…"));           // D5.1

    m_btnWellOptions->setMenu(wellMenu);
    toolLay->addWidget(m_btnWellOptions);

    toolLay->addStretch(1);

    // Export Snapshot Button
    m_btnExport = new QToolButton(toolbar);
    m_btnExport->setObjectName(QStringLiteral("btnSectionExport"));
    m_btnExport->setText(tr("导出图件"));
    applyBtnStyle(m_btnExport);
    toolLay->addWidget(m_btnExport);

    mainLay->addWidget(new PaleoToolRow(toolbar, container));

    // ==========================================
    // 2. Center Canvas（先建——Display Bar 的控件以 m_canvas 为接收者）
    // ==========================================
    m_canvas = new SeismicSectionCanvas(container);
    m_canvas->setObjectName(QStringLiteral("seismicSectionCanvas"));
    mainLay->addWidget(m_canvas, 1);

    // ==========================================
    // 1b. Display Bar（D2.2–D2.10 显示控制行）
    // ==========================================
    setupDisplayBar(container);

    // D4 解释面板（画布下方，默认折叠）
    setupInterpretationUi(container);

    // ==========================================
    // 3. Bottom Status Bar (Monospace Readout)
    // ==========================================
    auto *statusBar = new QWidget(container);
    PaleoTheme::applyThemedStyleSheet(statusBar, [] {
        const auto &t = PaleoTheme::tokens();
        return QStringLiteral("background: %1; border-top: 1px solid %2; min-height: 24px;")
            .arg(t.surfaceAlt.name().toUpper(), t.border.name().toUpper());
    });
    auto *statusLay = new QHBoxLayout(statusBar);
    statusLay->setContentsMargins(PaleoTheme::tokens().spacingSm, PaleoTheme::tokens().spacingXs, PaleoTheme::tokens().spacingSm, PaleoTheme::tokens().spacingXs);
    statusLay->setSpacing(PaleoTheme::tokens().spacingSm);

    m_lblCoordinates = new QLabel(statusBar);
    m_lblCoordinates->setObjectName(QStringLiteral("lblSectionCoordinates"));
    {
        QFont f = PaleoTheme::monoFont();
        f.setPointSize(PaleoTheme::tokens().labelPt);
        m_lblCoordinates->setFont(f);
    }
    PaleoTheme::applyThemedStyleSheet(m_lblCoordinates, [] {
        return QStringLiteral("color: %1;")
            .arg(PaleoTheme::tokens().text.name().toUpper());
    });
    m_lblCoordinates->setText(tr("道: -- | 距离: -- | TWT: -- ms | 深度: -- m | 振幅: -- | (X: --, Y: --)"));
    statusLay->addWidget(m_lblCoordinates, 1);

    m_progressBar = new QProgressBar(statusBar);
    m_progressBar->setObjectName(QStringLiteral("sectionProgressBar"));
    m_progressBar->setRange(0, 100);
    m_progressBar->setValue(0);
    m_progressBar->setFixedWidth(140);
    m_progressBar->setTextVisible(true);
    m_progressBar->setVisible(false);
    statusLay->addWidget(m_progressBar);

    mainLay->addWidget(statusBar);
    setWidget(container);

    // ==========================================
    // Signals & Slots Connections
    // ==========================================
    connect(m_btnZoomIn, &QToolButton::clicked, m_canvas, &SeismicSectionCanvas::zoomIn);
    connect(m_btnZoomOut, &QToolButton::clicked, m_canvas, &SeismicSectionCanvas::zoomOut);
    connect(m_btnFit, &QToolButton::clicked, m_canvas, &SeismicSectionCanvas::fitToWindow);
    connect(m_btnReset, &QToolButton::clicked, m_canvas, &SeismicSectionCanvas::resetZoom);

    connect(m_btnUnitToggle, &QToolButton::clicked, this, [this]() {
        if (m_canvas->verticalUnit() == SectionVerticalUnit::TwoWayTimeMs) {
            m_canvas->setVerticalUnit(SectionVerticalUnit::DepthMeters);
            m_btnUnitToggle->setText(tr("参考深度（常速）"));
        } else {
            m_canvas->setVerticalUnit(SectionVerticalUnit::TwoWayTimeMs);
            m_btnUnitToggle->setText(tr("单位: TWT (ms)"));
        }
    });

    connect(m_cboColorMap, QOverload<int>::of(&QComboBox::currentIndexChanged), this, [this](int index) {
        m_canvas->setColorMap(static_cast<SectionColorMapType>(index));
    });

    connect(m_sliderGain, &QSlider::valueChanged, this, [this](int value) {
        const double g = static_cast<double>(value) / 100.0;
        m_spinGain->blockSignals(true);
        m_spinGain->setValue(g);
        m_spinGain->blockSignals(false);
        m_canvas->setGain(static_cast<float>(g));
    });

    connect(m_spinGain, QOverload<double>::of(&QDoubleSpinBox::valueChanged), this, [this](double value) {
        m_sliderGain->blockSignals(true);
        m_sliderGain->setValue(qRound(value * 100.0));
        m_sliderGain->blockSignals(false);
        m_canvas->setGain(static_cast<float>(value));
    });

    connect(actShowWells, &QAction::toggled, m_canvas, &SeismicSectionCanvas::setShowWells);
    connect(actShowTops, &QAction::toggled, m_canvas, &SeismicSectionCanvas::setShowFormationTops);
    connect(actShowCurves, &QAction::toggled, m_canvas, &SeismicSectionCanvas::setShowWellCurves);

    connect(actBuf200, &QAction::triggered, this, [this]() { m_canvas->setBufferDistanceM(200.0); });
    connect(actBuf500, &QAction::triggered, this, [this]() { m_canvas->setBufferDistanceM(500.0); });
    connect(actBuf1000, &QAction::triggered, this, [this]() { m_canvas->setBufferDistanceM(1000.0); });
    // D5.7
    connect(actAllWells, &QAction::triggered, this, [this]() { m_canvas->setMaxVisibleWells(0); });
    connect(actNear1, &QAction::triggered, this, [this]() { m_canvas->setMaxVisibleWells(1); });
    connect(actNear3, &QAction::triggered, this, [this]() { m_canvas->setMaxVisibleWells(3); });
    connect(actWellTrace, &QAction::triggered, this, &SeismicSectionDockWidget::showWellSideTrace);
    connect(actExtractWavelet, &QAction::triggered, this,
            &SeismicSectionDockWidget::extractWaveletFromNearestWell);
    connect(actInversionPanel, &QAction::triggered, this, [this]() {
        if (m_invPanel)
            m_invPanel->setVisible(!m_invPanel->isVisible());
    });
    connect(actArbLine, &QAction::triggered, this, &SeismicSectionDockWidget::showArbitraryLineEditor);

    connect(m_cboSectionMode, QOverload<int>::of(&QComboBox::currentIndexChanged),
            this, &SeismicSectionDockWidget::onSectionModeChanged);

    connect(m_sliderSlice, &QSlider::valueChanged, m_spinSlice, &QSpinBox::setValue);
    connect(m_spinSlice, &QSpinBox::valueChanged, m_sliderSlice, &QSlider::setValue);
    connect(m_sliderSlice, &QSlider::valueChanged, this, &SeismicSectionDockWidget::onSliceSliderChanged);

    connect(m_btnExport, &QToolButton::clicked, this, &SeismicSectionDockWidget::onExportSnapshot);

    const auto themedBtnStyle = [] { return PaleoTheme::toolButtonStyleSheet(); };
    m_btnCopy = new QToolButton(toolbar);
    m_btnCopy->setText(tr("复制"));
    m_btnCopy->setToolTip(tr("复制剖面图到剪贴板（含坐标轴与色标）"));
    PaleoTheme::applyThemedStyleSheet(m_btnCopy, themedBtnStyle);
    toolLay->addWidget(m_btnCopy);
    m_btnPrint = new QToolButton(toolbar);
    m_btnPrint->setText(tr("打印"));
    PaleoTheme::applyThemedStyleSheet(m_btnPrint, themedBtnStyle);
    toolLay->addWidget(m_btnPrint);
    connect(m_btnCopy, &QToolButton::clicked, this, &SeismicSectionDockWidget::onCopyImage);
    connect(m_btnPrint, &QToolButton::clicked, this, &SeismicSectionDockWidget::onPrintImage);

    connect(m_canvas, &SeismicSectionCanvas::zoomChanged, this, &SeismicSectionDockWidget::onZoomChanged);
    connect(m_canvas, &SeismicSectionCanvas::traceHovered, this, &SeismicSectionDockWidget::onTraceHovered);
    // D2.11：点击道 → 道头卡
    connect(m_canvas, &SeismicSectionCanvas::traceClicked, this, &SeismicSectionDockWidget::onTraceClicked);
    // 画布键盘 PgUp/PgDn → 切片滑杆步进（±1）
    connect(m_canvas, &SeismicSectionCanvas::sliceStepRequested, this, [this](int delta) {
        m_sliderSlice->setValue(m_sliderSlice->value() + delta);
    });
}

// D2.2–D2.12 显示控制行：显示三模/阈值/极性/AGC/增益曲线/双刻度/拉伸/
// 卷帘/书签/复制/打印。紧凑专业密度（DESIGN.md），全部即时生效。
void SeismicSectionDockWidget::setupDisplayBar(QWidget *parent) {
    auto *bar = new QWidget(parent);
    PaleoTheme::applyThemedStyleSheet(bar, [] {
        const auto &t = PaleoTheme::tokens();
        return QStringLiteral("background: %1; border-bottom: 1px solid %2;")
            .arg(t.surface.name(), t.border.name());
    });
    auto *lay = new QHBoxLayout(bar);
    lay->setContentsMargins(PaleoTheme::tokens().spacingSm, PaleoTheme::tokens().spacingXs, PaleoTheme::tokens().spacingSm, PaleoTheme::tokens().spacingXs);
    lay->setSpacing(PaleoTheme::tokens().spacingSm);

    // D2.2 显示三模
    auto *lblMode = new QLabel(tr("显示:"), bar);
    themedCaptionStyle(lblMode);
    lay->addWidget(lblMode);
    m_cboDisplayMode = new QComboBox(bar);
    m_cboDisplayMode->setObjectName(QStringLiteral("cboSectionDisplayMode"));
    m_cboDisplayMode->addItems({tr("密度"), tr("波形变面积"), tr("混合")});
    themedComboStyle(m_cboDisplayMode);
    lay->addWidget(m_cboDisplayMode);

    // D2.8 反转
    m_chkInvert = new QCheckBox(tr("反转色标"), bar);
    themedCaptionStyle(m_chkInvert);
    lay->addWidget(m_chkInvert);

    // D2.3 阈值 + 极性
    auto *lblThreshold = new QLabel(tr("阈值:"), bar);
    themedCaptionStyle(lblThreshold);
    lay->addWidget(lblThreshold);
    m_spinThreshold = new QDoubleSpinBox(bar);
    m_spinThreshold->setRange(0.0, 0.9);
    m_spinThreshold->setSingleStep(0.05);
    m_spinThreshold->setValue(0.0);
    m_spinThreshold->setToolTip(tr("低于该归一化阈值的振幅压为 0（压噪声底）"));
    m_spinThreshold->setFixedWidth(52);
    lay->addWidget(m_spinThreshold);

    m_btnPolarity = new QToolButton(bar);
    m_btnPolarity->setText(tr("极性 +/-"));
    m_btnPolarity->setCheckable(true);
    m_btnPolarity->setToolTip(tr("极性反转（波峰/波谷互换）"));
    themedButtonStyle(m_btnPolarity);
    lay->addWidget(m_btnPolarity);

    // D2.4 AGC
    m_btnAgc = new QToolButton(bar);
    m_btnAgc->setText(tr("AGC"));
    m_btnAgc->setCheckable(true);
    m_btnAgc->setToolTip(tr("自动增益控制：滑动窗 RMS 归一（压掉道间能量差）"));
    themedButtonStyle(m_btnAgc);
    lay->addWidget(m_btnAgc);
    m_spinAgcWindow = new QSpinBox(bar);
    m_spinAgcWindow->setRange(20, 5000);
    m_spinAgcWindow->setSingleStep(50);
    m_spinAgcWindow->setValue(200);
    m_spinAgcWindow->setSuffix(QStringLiteral("ms"));
    m_spinAgcWindow->setToolTip(tr("AGC 时窗宽度（毫秒）"));
    m_spinAgcWindow->setFixedWidth(72);
    lay->addWidget(m_spinAgcWindow);

    // D2.4 手动增益曲线
    m_btnGainCurve = new QToolButton(bar);
    m_btnGainCurve->setText(tr("增益曲线…"));
    m_btnGainCurve->setToolTip(tr("手动增益曲线：TWT→倍数分段线性控制点编辑"));
    themedButtonStyle(m_btnGainCurve);
    lay->addWidget(m_btnGainCurve);

    // D2.7 纵向拉伸
    auto *lblVExag = new QLabel(tr("纵向拉伸:"), bar);
    themedCaptionStyle(lblVExag);
    lay->addWidget(lblVExag);
    m_spinVExag = new QDoubleSpinBox(bar);
    m_spinVExag->setRange(0.1, 20.0);
    m_spinVExag->setSingleStep(0.1);
    m_spinVExag->setValue(1.0);
    m_spinVExag->setSuffix(QStringLiteral("x"));
    m_spinVExag->setToolTip(tr("纵向拉伸系数（1.0 = 适应窗口基线）"));
    m_spinVExag->setFixedWidth(56);
    lay->addWidget(m_spinVExag);

    // D2.5 双刻度
    m_btnDualScale = new QToolButton(bar);
    m_btnDualScale->setText(tr("双刻度 TWT+深度"));
    m_btnDualScale->setCheckable(true);
    m_btnDualScale->setToolTip(tr("左轴 TWT(ms) + 右缘深度(m) 同显（需有效时深模型）"));
    themedButtonStyle(m_btnDualScale);
    lay->addWidget(m_btnDualScale);

    // D2.10 卷帘对比
    m_btnCurtain = new QToolButton(bar);
    m_btnCurtain->setObjectName(QStringLiteral("btnSectionCurtain"));
    m_btnCurtain->setText(tr("卷帘对比"));
    m_btnCurtain->setCheckable(true);
    m_btnCurtain->setToolTip(tr("相邻线卷帘对比：帘左当前线 / 帘右相邻线，画布内拖分割线"));
    themedButtonStyle(m_btnCurtain);
    lay->addWidget(m_btnCurtain);
    m_sliderCurtain = new QSlider(Qt::Horizontal, bar);
    m_sliderCurtain->setRange(2, 98);
    m_sliderCurtain->setValue(50);
    m_sliderCurtain->setFixedWidth(90);
    m_sliderCurtain->setVisible(false);
    lay->addWidget(m_sliderCurtain);

    // D4 解释：种子拾取/断层线模式
    m_btnPickMode = new QToolButton(bar);
    m_btnPickMode->setText(tr("● 拾取"));
    m_btnPickMode->setCheckable(true);
    m_btnPickMode->setToolTip(tr("解释模式：剖面点击放层位拾取点（拾取面板同步显示）"));
    themedButtonStyle(m_btnPickMode);
    lay->addWidget(m_btnPickMode);

    // goal/seismic-attributes：属性面板开关（计算属性并叠加当前剖面）
    m_btnAttr = new QToolButton(bar);
    m_btnAttr->setText(tr("◈ 属性"));
    m_btnAttr->setObjectName(QStringLiteral("btnAttrPanel"));
    m_btnAttr->setCheckable(true);
    m_btnAttr->setToolTip(tr("属性计算面板：包络/瞬时/相干等属性计算并叠加显示"));
    themedButtonStyle(m_btnAttr);
    lay->addWidget(m_btnAttr);
    connect(m_btnAttr, &QToolButton::toggled, this, [this](bool on) {
        if (m_attrPanel)
            m_attrPanel->setVisible(on);
    });
    m_btnFaultMode = new QToolButton(bar);
    m_btnFaultMode->setText(tr("✂ 断层"));
    m_btnFaultMode->setCheckable(true);
    m_btnFaultMode->setToolTip(tr("断层模式：剖面拖拽画断层折线（红色虚线跟踪，松手存档）"));
    themedButtonStyle(m_btnFaultMode);
    lay->addWidget(m_btnFaultMode);

    lay->addStretch(1);

    // D2.12 书签
    auto *lblBookmark = new QLabel(tr("书签:"), bar);
    themedCaptionStyle(lblBookmark);
    lay->addWidget(lblBookmark);
    m_cboBookmark = new QComboBox(bar);
    m_cboBookmark->setFixedWidth(120);
    themedComboStyle(m_cboBookmark);
    lay->addWidget(m_cboBookmark);
    m_btnBookmarkAdd = new QToolButton(bar);
    m_btnBookmarkAdd->setText(tr("存当前"));
    themedButtonStyle(m_btnBookmarkAdd);
    lay->addWidget(m_btnBookmarkAdd);
    m_btnBookmarkDel = new QToolButton(bar);
    m_btnBookmarkDel->setText(tr("删除"));
    themedButtonStyle(m_btnBookmarkDel);
    lay->addWidget(m_btnBookmarkDel);

    // 显示控制行固定排在工具栏（index 0）之后、画布之前——setupDisplayBar
    // 在画布创建之后才调用（m_canvas 作接收者的连接需要它先在），但视觉
    // 位置不变。
    if (auto *mainLay = qobject_cast<QVBoxLayout *>(parent->layout())) {
        mainLay->insertWidget(1, new PaleoToolRow(bar, parent));
    }

    // ---- 信号接线 ----
    connect(m_cboDisplayMode, QOverload<int>::of(&QComboBox::currentIndexChanged), this, [this](int idx) {
        m_canvas->setDisplayMode(static_cast<SectionDisplayMode>(idx));
    });
    connect(m_chkInvert, &QCheckBox::toggled, m_canvas, &SeismicSectionCanvas::setColorMapInverted);
    connect(m_spinThreshold, QOverload<double>::of(&QDoubleSpinBox::valueChanged), this, [this](double v) {
        m_canvas->setAmplitudeThreshold(static_cast<float>(v));
    });
    connect(m_btnPolarity, &QToolButton::toggled, m_canvas, &SeismicSectionCanvas::setPolarityInverted);
    connect(m_btnAgc, &QToolButton::toggled, this, [this](bool on) {
        m_canvas->setAgcEnabled(on, m_spinAgcWindow->value());
        m_spinAgcWindow->setEnabled(true);
    });
    connect(m_spinAgcWindow, QOverload<int>::of(&QSpinBox::valueChanged), this, [this](int v) {
        if (m_btnAgc->isChecked())
            m_canvas->setAgcEnabled(true, v);
    });
    connect(m_btnGainCurve, &QToolButton::clicked, this, [this]() {
        // 简易控制点编辑器：每行 "TWT ms, 倍数"
        QDialog dlg(this);
        dlg.setWindowTitle(tr("手动增益曲线（TWT ms → 倍数，分段线性）"));
        auto *form = new QFormLayout(&dlg);
        auto *edit = new QTextEdit(&dlg);
        edit->setFont(PaleoTheme::monoFont(PaleoTheme::tokens().bodyPt));
        QStringList lines;
        for (const auto &node : m_canvas->gainCurve())
            lines << QStringLiteral("%1 %2").arg(node.twtMs, 0, 'f', 0).arg(node.gain, 0, 'f', 2);
        edit->setPlainText(lines.join(QLatin1Char('\n')));
        edit->setPlaceholderText(tr("每行一个控制点：TWT毫秒 倍数\n例如：\n0 1.0\n500 1.5\n1500 3.0"));
        form->addRow(edit);
        auto *btnBox = new QDialogButtonBox(QDialogButtonBox::Ok | QDialogButtonBox::Cancel, &dlg);
        connect(btnBox, &QDialogButtonBox::accepted, &dlg, &QDialog::accept);
        connect(btnBox, &QDialogButtonBox::rejected, &dlg, &QDialog::reject);
        form->addRow(btnBox);
        if (dlg.exec() != QDialog::Accepted)
            return;
        std::vector<SectionGainNode> nodes;
        const auto rows = edit->toPlainText().split(QLatin1Char('\n'), Qt::SkipEmptyParts);
        for (const QString &row : rows) {
            const auto parts = row.simplified().split(QRegularExpression(QStringLiteral("[ ,\t]+")), Qt::SkipEmptyParts);
            if (parts.size() != 2)
                continue;
            bool okT = false, okG = false;
            const double t = parts[0].toDouble(&okT);
            const double g = parts[1].toDouble(&okG);
            if (okT && okG && g >= 0.01 && g <= 100.0)
                nodes.push_back({t, g});
        }
        m_canvas->setGainCurve(nodes);
    });
    connect(m_spinVExag, QOverload<double>::of(&QDoubleSpinBox::valueChanged), m_canvas,
            &SeismicSectionCanvas::setVerticalExaggeration);
    connect(m_btnDualScale, &QToolButton::toggled, m_canvas, &SeismicSectionCanvas::setDualScaleEnabled);
    connect(m_btnCurtain, &QToolButton::toggled, this, [this](bool on) {
        m_sliderCurtain->setVisible(on);
        m_canvas->setCompareEnabled(on);
        if (on)
            updateCompareSlice();
    });
    connect(m_sliderCurtain, &QSlider::valueChanged, this, [this](int v) {
        m_canvas->setCurtainPos(v / 100.0);
    });
    connect(m_canvas, &SeismicSectionCanvas::curtainMoved, this, [this](double frac) {
        m_sliderCurtain->blockSignals(true);
        m_sliderCurtain->setValue(int(frac * 100));
        m_sliderCurtain->blockSignals(false);
    });

    // D4 解释模式（互斥单选）
    connect(m_btnPickMode, &QToolButton::toggled, this, [this](bool on) {
        if (on)
            m_btnFaultMode->setChecked(false);
        setPickMode(on ? SectionPickMode::Seed : SectionPickMode::None);
    });
    connect(m_btnFaultMode, &QToolButton::toggled, this, [this](bool on) {
        if (on)
            m_btnPickMode->setChecked(false);
        setPickMode(on ? SectionPickMode::Fault : SectionPickMode::None);
    });

    // D2.12 书签
    connect(m_btnBookmarkAdd, &QToolButton::clicked, this, [this]() {
        bool ok = false;
        const QString name = QInputDialog::getText(this, tr("保存剖面书签"),
                                                   tr("书签名："), QLineEdit::Normal,
                                                   tr("线 %1").arg(m_spinSlice->value()), &ok);
        if (!ok || name.trimmed().isEmpty())
            return;
        addBookmark(name.trimmed());
    });
    connect(m_btnBookmarkDel, &QToolButton::clicked, this, [this]() {
        removeBookmark(m_cboBookmark->currentIndex());
    });
    connect(m_cboBookmark, QOverload<int>::of(&QComboBox::activated), this, [this](int idx) {
        applyBookmark(idx);
    });
    loadBookmarksFromSettings();
}

void SeismicSectionDockWidget::setupInterpretationUi(QWidget *parent) {
    // D4 解释面板：画布下方可折叠行
    m_pickPanel = new SeismicPickPanel(this, parent);
    m_pickPanel->setVisible(false);
    if (auto *mainLay = qobject_cast<QVBoxLayout *>(parent->layout()))
        mainLay->addWidget(m_pickPanel);

    m_undoStack = new QUndoStack(this);
    m_pickPanel->setUndoStack(m_undoStack);
    connect(m_undoStack, &QUndoStack::canUndoChanged, this, [this](bool can) {
        Q_UNUSED(can);
    });
    connect(m_pickPanel, &SeismicPickPanel::sessionChanged, this, [this]() {
        refreshInterpretationOverlay();
    });
    connect(m_pickPanel, &SeismicPickPanel::locateRequested, this, [this](int pickId) {
        const SeismicPick *p = m_session.pickById(pickId);
        if (!p)
            return;
        // 跳到拾取所在剖面：IL 模式 + 线号 + 视口滚到 TWT
        setSectionMode(0);
        m_spinSlice->setValue(p->inlineNo);
        const SectionViewState vs = m_canvas->viewState();
        m_canvas->setViewState(vs);
    });
    connect(m_pickPanel, &SeismicPickPanel::trackRequested, this,
            &SeismicSectionDockWidget::runTracking);

    // 画布拾取/断层信号
    connect(m_canvas, &SeismicSectionCanvas::pickPlaced, this,
            &SeismicSectionDockWidget::addPickFromCanvas);
    connect(m_canvas, &SeismicSectionCanvas::faultDrawn, this,
            &SeismicSectionDockWidget::addFaultFromCanvas);

    setupAttrPanelUi(parent);
    setupInversionPanelUi(parent);
}

// goal/seismic-attributes 属性面板：画布下方可折叠行（拾取面板同模式），
// 面板只发意图信号，任务编排/叠加回填在本 dock。
void SeismicSectionDockWidget::setupAttrPanelUi(QWidget *parent) {
    m_attrPanel = new SeismicAttrPanel(this);
    m_attrPanel->setVisible(false);
    if (auto *mainLay = qobject_cast<QVBoxLayout *>(parent->layout()))
        mainLay->addWidget(m_attrPanel);

    connect(m_attrPanel, &SeismicAttrPanel::computeRequested, this,
            [this](seismic::SeismicTaskService::SeismicAttrKind kind,
                   const seismic::SeismicTaskService::SeismicAttrParams &params,
                   double overlayAlpha) {
                m_canvas->setAttrOverlayAlpha(overlayAlpha);
                computeAttributeOnCurrentSection(kind, params);
            });
    // goal/attr-volume：扫描意图（时间切片/属性体）——编排同剖面路径：
    // 面板 busy/进度/结果回填 + 完成后登记与声明/喂 3D 信号。
    connect(m_attrPanel, &SeismicAttrPanel::timeSliceScanRequested, this,
            [this](seismic::SeismicTaskService::SeismicAttrKind kind,
                   const seismic::SeismicTaskService::SeismicAttrParams &params,
                   int sampleIndex) {
                computeTimeSliceAttribute(kind, params, sampleIndex);
            });
    connect(m_attrPanel, &SeismicAttrPanel::volumeScanRequested, this,
            [this](seismic::SeismicTaskService::SeismicAttrKind kind,
                   const seismic::SeismicTaskService::SeismicAttrParams &params) {
                computeAttributeVolume(kind, params);
            });
    connect(m_attrPanel, &SeismicAttrPanel::alphaChanged, this,
            [this](double alpha) { m_canvas->setAttrOverlayAlpha(alpha); });
    connect(m_attrPanel, &SeismicAttrPanel::cancelRequested, this, [this]() {
        if (m_attrTask)
            m_attrTask->requestCancel();
    });
    connect(m_attrPanel, &SeismicAttrPanel::registerRequested, this,
            [this]() { registerCurrentAttributeAsset(); });
}

// goal/seismic-inversion 反演面板：同属性面板模式——面板只发意图信号，
// 三段式编排（PaleoTaskService 通道）与 DERIVED 登记在本 dock。
void SeismicSectionDockWidget::setupInversionPanelUi(QWidget *parent) {
    m_invPanel = new InversionPanel(this);
    m_invPanel->setVisible(false);
    if (auto *mainLay = qobject_cast<QVBoxLayout *>(parent->layout()))
        mainLay->addWidget(m_invPanel);

    connect(m_invPanel, &InversionPanel::inversionRequested, this,
            [this](const InversionPanelParams &params) { runInversion(params); });
    connect(m_invPanel, &InversionPanel::cancelRequested, this, [this]() {
        if (m_invTask)
            m_invTask->requestCancel();
    });
    connect(m_invPanel, &InversionPanel::extractWaveletRequested, this,
            &SeismicSectionDockWidget::extractWaveletFromNearestWell);
}

} // namespace seismic
