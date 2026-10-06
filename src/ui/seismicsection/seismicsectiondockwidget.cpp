// 层：视图
// token 例外：DESIGN 数据符号例外：井旁道的分层位置标记，与数据图像使用原有蓝色。（tools/ui-token-exceptions.json 精确计数）。
#include "ui/seismicsection/seismicsectiondockwidget.h"
#include "ui/seismicsection/seismicattrpanel.h"
#include "ui/seismicsection/inversionpanel.h"
#include "workflow/faultinterpretationcontroller.h"

#include <QAction>
#include <QActionGroup>
#include <QCheckBox>
#include <QClipboard>
#include <QDialog>
#include <QDialogButtonBox>
#include <QFileDialog>
#include <QFormLayout>
#include <QHeaderView>
#include "ui/notifications/notificationmanager.h"
#include <QPainter>
#include <QPrinter>
#include <QPrintDialog>
#include <QSettings>
#include <QSpinBox>
#include <QTableWidget>
#include <QApplication>
#include <QInputDialog>
#include <QLineEdit>
#include <QRegularExpression>
#include <QTextEdit>
#include <QJsonArray>
#include <QJsonDocument>
#include <QJsonObject>
#include <QFileInfo>
#include <QHBoxLayout>
#include <QMenu>
#include <QVBoxLayout>

#include "domain/seismic/sectionaxis.h"
#include "domain/seismic/welltracelocate.h" // #129 井口 XY → 井旁道
#include "domain/seismic/sgycoordinatemapper.h"
#include "catalog/datacatalog.h"
#include "workflow/derivedassets.h"
#include "workflow/inversionworkflow.h"
#include "workflow/wellimpedancetwt.h"
#include <algorithm>
#include <cmath>
#include <limits>

#include "domain/seismic/sectiongeometry.h"
#include "domain/seismic/sgysectionbuilder.h"
#include "ui/paleotheme.h"
#include "ui/paleoviewport.h"
#include <QUndoStack>

#include "services/seismictaskservice.h"
#include "ui/seismicsection/seismicpickpanel.h"

namespace seismic {

SeismicSectionDockWidget::SeismicSectionDockWidget(QWidget *parent)
    : SeismicSectionDockWidget(tr("地震剖面 / 井震综合"), parent)
{
}

SeismicSectionDockWidget::SeismicSectionDockWidget(const QString &title, QWidget *parent)
    : QDockWidget(title, parent)
{
    setObjectName(QStringLiteral("seismicSectionDock"));
    setAllowedAreas(Qt::AllDockWidgetAreas);
    setupUi();
    auto *tasks = new PaleoTaskService(nullptr, this);
    m_taskService = new SeismicTaskService(tasks, 256, this);
}

// 迟到回调由 QPointer 守卫兜底（无 UAF）；这里取消是为归还并发闸——
// 注入共享服务时（app 装配），排队/在途读取不随 dock 析构消失，
// 不主动取消会占住 ≤4 闸直到读完。
SeismicSectionDockWidget::~SeismicSectionDockWidget() {
    if (m_sliceTask) {
        m_sliceTask->requestCancel();
        m_sliceTask.clear();
    }
    if (m_compareTask) {
        m_compareTask->requestCancel();
        m_compareTask.clear();
    }
    if (m_trackTask) {
        m_trackTask->requestCancel();
        m_trackTask.clear();
    }
    if (m_attrTask) {
        m_attrTask->requestCancel();
        m_attrTask.clear();
    }
    if (m_attrPreviewTask) {
        m_attrPreviewTask->requestCancel();
        m_attrPreviewTask.clear();
    }
    if (m_extraction)
        m_extraction->requestCancel();
}

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

// ---- D4 解释工具 ---------------------------------------------------------------

namespace {

// D5.6 井旁道 wiggle 小图（匿名命名空间——本文件局部）
class WellSideTraceWidget : public QWidget {
public:
    WellSideTraceWidget(const SectionWellInfo *well, int col, const SgySliceImage &slice,
                        float dtMs, double t0Ms, QWidget *parent)
        : QWidget(parent), m_well(well), m_dtMs(dtMs > 0.01f ? dtMs : 2.0f), m_t0Ms(t0Ms)
    {
        const int h = slice.height;
        m_trace.reserve(std::size_t(h));
        for (int y = 0; y < h; ++y)
            m_trace.push_back(slice.Value(col, y));
        setMinimumSize(260, 460);
    }

protected:
    void paintEvent(QPaintEvent *) override {
        QPainter p(this);
        p.fillRect(rect(), PaleoTheme::tokens().surface);
        if (m_trace.empty())
            return;
        float maxAbs = 1e-6f;
        for (float v : m_trace)
            if (std::isfinite(v))
                maxAbs = std::max(maxAbs, std::abs(v));
        const double halfW = width() * 0.32;
        const double cx = width() * 0.5;
        const double y0 = 24.0, y1 = height() - 8.0;
        p.setPen(QPen(PaleoTheme::tokens().border, 1.0));
        p.drawLine(QPointF(cx, y0), QPointF(cx, y1));
        QPolygonF wave;
        QPolygonF fill;
        bool fillOpen = false;
        for (std::size_t i = 0; i < m_trace.size(); ++i) {
            const float v = m_trace[i];
            const double py = y0 + double(i) / double(m_trace.size() - 1) * (y1 - y0);
            const double amp = std::isfinite(v) ? std::clamp(double(v) / maxAbs, -1.0, 1.0) : 0.0;
            const double px = cx + amp * halfW;
            wave.append(QPointF(px, py));
            if (amp > 0.0) {
                if (!fillOpen) { fill.append(QPointF(cx, py)); fillOpen = true; }
                fill.append(QPointF(px, py));
            } else if (fillOpen) {
                fill.append(QPointF(cx, py));
                fillOpen = false;
            }
        }
        if (fill.size() >= 3) {
            p.setPen(Qt::NoPen);
            p.setBrush(PaleoTheme::tokens().text);
            p.drawPolygon(fill);
        }
        p.setPen(QPen(PaleoTheme::tokens().text, 1.0));
        p.setBrush(Qt::NoBrush);
        p.drawPolyline(wave);
        // 分层刻度（右缘）
        p.setFont(PaleoTheme::bodyFont(PaleoTheme::tokens().labelPt));
        for (const WellTopItem &top : m_well->tops) {
            // 道首样 TWT = 记录延迟 t0（#146 同口径）
            const double frac = std::clamp((top.twtMs - m_t0Ms) / double(m_trace.size() * m_dtMs),
                                           0.0, 1.0);
            const double py = y0 + frac * (y1 - y0);
            p.setPen(QPen(QColor(QStringLiteral("#1B73D0")), 1.4));
            p.drawLine(QPointF(width() - 46.0, py), QPointF(width() - 6.0, py));
            p.setPen(PaleoTheme::tokens().textMuted);
            p.drawText(QRectF(width() - 90.0, py - 7.0, 46.0, 14.0), Qt::AlignRight, top.topName);
        }
    }

private:
    const SectionWellInfo *m_well = nullptr;
    std::vector<float> m_trace;
    float m_dtMs = 2.0f;
    double m_t0Ms = 0.0;
};


// undo 命令（D4.6）：对会话模型的原子操作 + 面板/叠加刷新
class PickCommandBase : public QUndoCommand
{
public:
    PickCommandBase(SeismicSectionDockWidget *dock, QUndoCommand *parent = nullptr)
        : QUndoCommand(parent), m_dock(dock) {}

protected:
    void refresh()
    {
        if (m_dock && m_dock->pickPanel())
            m_dock->pickPanel()->refreshFromSession();
        if (m_dock)
            m_dock->saveInterpretationSession(); // 自动保存（D4.8 会话持久化）
    }
    SeismicSectionDockWidget *m_dock;
};

class AddPicksCommand : public PickCommandBase
{
public:
    AddPicksCommand(SeismicSectionDockWidget *dock, QList<SeismicPick> picks)
        : PickCommandBase(dock), m_picks(std::move(picks))
    {
        setText(QObject::tr("添加 %1 个拾取").arg(m_picks.size()));
    }
    void undo() override
    {
        auto &session = m_dock->mutableSession();
        for (const SeismicPick &p : m_picks)
        {
            const int idx = session.picks.indexOf(p);
            if (idx >= 0)
                session.picks.removeAt(idx);
        }
        refresh();
    }
    void redo() override
    {
        auto &session = m_dock->mutableSession();
        for (SeismicPick p : m_picks)
        {
            p.id = session.nextId++;
            session.picks.append(p);
        }
        m_dock->refreshInterpretationOverlay();
        refresh();
    }

private:
    QList<SeismicPick> m_picks;
};

class RemovePickCommand : public PickCommandBase
{
public:
    RemovePickCommand(SeismicSectionDockWidget *dock, SeismicPick pick)
        : PickCommandBase(dock), m_pick(pick)
    {
        setText(QObject::tr("删除拾取 %1").arg(pick.id));
    }
    void undo() override
    {
        m_dock->mutableSession().picks.append(m_pick);
        refresh();
    }
    void redo() override
    {
        auto &session = m_dock->mutableSession();
        const int idx = session.picks.indexOf(m_pick);
        if (idx >= 0)
            session.picks.removeAt(idx);
        refresh();
    }

private:
    SeismicPick m_pick;
};

class RenamePickCommand : public PickCommandBase
{
public:
    RenamePickCommand(SeismicSectionDockWidget *dock, int pickId, QString oldName, QString newName)
        : PickCommandBase(dock), m_id(pickId), m_old(std::move(oldName)), m_new(std::move(newName))
    {
        setText(QObject::tr("拾取 %1 改层位 %2→%3").arg(pickId).arg(m_old, m_new));
    }
    void apply(const QString &name)
    {
        for (SeismicPick &p : m_dock->mutableSession().picks)
            if (p.id == m_id)
                p.horizonName = name;
        refresh();
    }
    void undo() override { apply(m_old); }
    void redo() override { apply(m_new); }

private:
    int m_id;
    QString m_old, m_new;
};

// goal/horizon-autotrack — 追踪合并替换（一步 undo）：redo = 移除被超越的
// 机器拾取 + 添加新拾取；undo = 原样复原（被移拾取按原 id 回位）。
// 手动拾取（conf==1，D4.10 语义）不参与替换——编排层已在入栈前滤除。
class ReplacePicksCommand : public PickCommandBase
{
public:
    ReplacePicksCommand(SeismicSectionDockWidget *dock,
                        QList<SeismicPick> removed, QList<SeismicPick> added)
        : PickCommandBase(dock), m_removed(std::move(removed)), m_added(std::move(added))
    {
        setText(QObject::tr("追踪替换 %1 个拾取").arg(m_added.size()));
    }
    void undo() override
    {
        auto &picks = m_dock->mutableSession().picks;
        for (const int id : std::as_const(m_lastAddedIds))
            for (int i = picks.size() - 1; i >= 0; --i)
                if (picks[i].id == id)
                {
                    picks.removeAt(i);
                    break;
                }
        for (const SeismicPick &p : std::as_const(m_removed))
            picks.append(p); // 原 id 复原
        m_dock->refreshInterpretationOverlay();
        refresh();
    }
    void redo() override
    {
        auto &picks = m_dock->mutableSession().picks;
        auto &session = m_dock->mutableSession();
        for (const SeismicPick &p : std::as_const(m_removed))
            for (int i = picks.size() - 1; i >= 0; --i)
                if (picks[i].id == p.id)
                {
                    picks.removeAt(i);
                    break;
                }
        m_lastAddedIds.clear();
        for (SeismicPick p : std::as_const(m_added))
        {
            p.id = session.nextId++;
            picks.append(p);
            m_lastAddedIds.append(p.id);
        }
        m_dock->refreshInterpretationOverlay();
        refresh();
    }

private:
    QList<SeismicPick> m_removed;
    QList<SeismicPick> m_added;
    QList<int> m_lastAddedIds; // 最近一次 redo 分配的 id（undo 按此回收）
};

} // namespace

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

void SeismicSectionDockWidget::clearRoute() {
  m_route.clear();
  m_distances.clear();
}

void SeismicSectionDockWidget::setVolume(std::shared_ptr<const SgyVolume> volume) {
  if (m_extraction)
    m_extraction->requestCancel();
  // 切片/卷帘在途任务一并取消并摘牌：切体后旧体的读取不再占并发闸，
  // 迟到的结果由世代号+任务身份双重守卫丢弃（不出错、不闪旧图）。
  if (m_sliceTask) {
    m_sliceTask->requestCancel();
    m_sliceTask.clear();
  }
  if (m_compareTask) {
    m_compareTask->requestCancel();
    m_compareTask.clear();
  }
  m_sliceIndex = -1; // 去抖键随体身份作废（同号线也不再等价）
  ++m_generation;
  m_route.clear();
  m_distances.clear();
  m_canvas->clearData();
  m_canvas->setWells({});
  m_progressBar->hide();
  m_volume = volume;
  if (!m_volume || !m_volume->IsLoaded()) {
    m_sliceGroup->setEnabled(false);
    return;
  }
  // goal/attr-volume：时间切片采样位范围随体回填（缺省中位采样）。
  if (m_attrPanel)
    m_attrPanel->setVolumeSampleRange(m_volume->SampleCount());

    m_sliceGroup->setEnabled(true);
    loadBookmarksFromSettings(); // D2.12：体身份确定后才有 settings 键
    // D4.8：会话锚定到 SEG-Y 伴生文件（存在即自动恢复）
    m_session = SeismicInterpretationSession{};
    m_session.sourceSgyPath = QString::fromStdString(m_volume->Path().string());
    SeismicTaskService::loadSession(m_session.sourceSgyPath, m_session, nullptr);
    refreshInterpretationOverlay();
    // 任意线（模式 3）保持待编辑态，不强制重提剖面（wave/sections）
    if (m_cboSectionMode->currentIndex() != 3)
        onSectionModeChanged(m_cboSectionMode->currentIndex());
}

void SeismicSectionDockWidget::onSectionModeChanged(int modeIndex) {
    if (modeIndex == 3) {
        if (m_extraction) m_extraction->requestCancel();
        // 任意线顶替切片显示：在途切片/卷帘一并取消摘牌、去抖键作废——
        // 只推世代号不清任务会让切片在途结果被自己的世代号毒化丢弃
        //（3→0 回切时去抖命中一个已注定被丢弃的请求 → 静默空白）。
        if (m_sliceTask) {
            m_sliceTask->requestCancel();
            m_sliceTask.clear();
        }
        if (m_compareTask) {
            m_compareTask->requestCancel();
            m_compareTask.clear();
        }
        m_sliceIndex = -1;
        ++m_generation;
        m_progressBar->hide();
        m_sliceGroup->hide();
        emit setupRequested();
        return;
    }
    if (!m_volume || !m_volume->IsLoaded()) {
        m_sliceGroup->setVisible(modeIndex != 3);
        return;
    }

    m_sliderSlice->blockSignals(true);
    m_spinSlice->blockSignals(true);

    if (modeIndex == 0) { // Inline
        m_sliceGroup->setVisible(true);
        m_lblSliceIndex->setText(tr("纵测线:"));
        m_sliderSlice->setRange(m_volume->InlineMin(), m_volume->InlineMax());
        m_spinSlice->setRange(m_volume->InlineMin(), m_volume->InlineMax());
        const int mid = m_volume->FindNearestInlineValue(
            (m_volume->InlineMin() + m_volume->InlineMax()) / 2.0); // 吸附真实线号
        m_sliderSlice->setValue(mid);
        m_spinSlice->setValue(mid);
        m_lblTimeMs->setVisible(false);
        m_sliderSlice->blockSignals(false);
        m_spinSlice->blockSignals(false);
        extractSliceAsync(SgySliceType::Inline, mid);
    } else if (modeIndex == 1) { // Crossline
        m_sliceGroup->setVisible(true);
        m_lblSliceIndex->setText(tr("横测线:"));
        m_sliderSlice->setRange(m_volume->XlineMin(), m_volume->XlineMax());
        m_spinSlice->setRange(m_volume->XlineMin(), m_volume->XlineMax());
        const int mid = m_volume->FindNearestXlineValue(
            (m_volume->XlineMin() + m_volume->XlineMax()) / 2.0);
        m_sliderSlice->setValue(mid);
        m_spinSlice->setValue(mid);
        m_lblTimeMs->setVisible(false);
        m_sliderSlice->blockSignals(false);
        m_spinSlice->blockSignals(false);
        extractSliceAsync(SgySliceType::Xline, mid);
    } else if (modeIndex == 2) { // Time Slice
        m_sliceGroup->setVisible(true);
        m_lblSliceIndex->setText(tr("时间采样:"));
        m_sliderSlice->setRange(0, m_volume->SampleMax());
        m_spinSlice->setRange(0, m_volume->SampleMax());
        const int mid = m_volume->SampleMax() / 2;
        m_sliderSlice->setValue(mid);
        m_spinSlice->setValue(mid);
        const double ms =
            m_timeOriginMs + mid * (m_volume->SampleIntervalUs() / 1000.0);
        m_lblTimeMs->setText(QStringLiteral("%1 ms").arg(ms, 0, 'f', 1));
        m_lblTimeMs->setVisible(true);
        m_sliderSlice->blockSignals(false);
        m_spinSlice->blockSignals(false);
        extractSliceAsync(SgySliceType::Time, mid);
    } else { // Arbitrary line
        m_sliceGroup->setVisible(false);
        m_sliderSlice->blockSignals(false);
        m_spinSlice->blockSignals(false);
    }
}

void SeismicSectionDockWidget::onSliceSliderChanged(int value) {
    if (!m_volume || !m_volume->IsLoaded())
        return;

    const int mode = m_cboSectionMode->currentIndex();
    if (mode == 0) {
        // 吸附到真实测线号（测网步长>1 时滑杆中点/拖动值可能不存在）
        const int snapped = m_volume->FindNearestInlineValue(value);
        if (snapped != value) {
            m_sliderSlice->setValue(snapped); // 重发 valueChanged，spin 同步后本函数再入
            return;
        }
        extractSliceAsync(SgySliceType::Inline, value);
    } else if (mode == 1) {
        const int snapped = m_volume->FindNearestXlineValue(value);
        if (snapped != value) {
            m_sliderSlice->setValue(snapped);
            return;
        }
        extractSliceAsync(SgySliceType::Xline, value);
    } else if (mode == 2) {
      const double ms =
          m_timeOriginMs + value * (m_volume->SampleIntervalUs() / 1000.0);
      m_lblTimeMs->setText(QStringLiteral("%1 ms").arg(ms, 0, 'f', 1));
      extractSliceAsync(SgySliceType::Time, value);
    }
}

void SeismicSectionDockWidget::setSectionMode(int modeIndex) {
    if (m_cboSectionMode && m_cboSectionMode->currentIndex() != modeIndex) {
        m_cboSectionMode->setCurrentIndex(modeIndex);
    } else if (m_volume && m_volume->IsLoaded()) {
        onSectionModeChanged(modeIndex);
    }
}

void SeismicSectionDockWidget::extractSliceAsync(SgySliceType type, int index) {
    if (!m_volume || !m_volume->IsLoaded() || !m_taskService)
        return;

    // 切片顶替任意线显示：取消其在途任务——迟到的任意线结果不再覆盖已
    // 应用的切片（旧实现无此守卫，属真实缺陷）。世代号在去抖判定之后才
    // 推进：重复请求走早退，不能毒化它本想去重的那个在途任务。
    if (m_extraction) {
        m_extraction->requestCancel();
        m_extraction.clear();
    }

    // 同型同号在途时去抖（滑杆吸附重入 / slider 与 spin 双发同值）。
    if (m_sliceTask && m_sliceType == type && m_sliceIndex == index)
        return;
    // 走到这里必然是顶替或新请求：推进世代号作废被取消的任意线迟到回调，
    // 再顶替旧切片在途（协作取消——worker 逐线检查点退出，不再占并发闸）。
    // 先摘牌再启新：服务的立即失败路径会同步回调，此时请求号守卫以
    // 「计数已推进」放行如实报错。
    ++m_generation;
    if (m_sliceTask) {
        m_sliceTask->requestCancel();
        m_sliceTask.clear();
    }
    m_sliceType = type;
    m_sliceIndex = index;
    // #224：换线即清属性叠加——同体同尺寸的相邻线几何全等，画布按尺寸
    // 清叠加的防线挡不住旧线属性图残留。
    m_canvas->clearAttrOverlay();

    // 常规 IL/XL/Time 切换：丢弃任意线旧状态（route/井叠加），
    // hasRoute() 复归 false（wave/sections 语义）。
    m_route.clear();
    m_distances.clear();
    m_lastPathPoints.clear(); // 断层剖面身份随之失效（goal/fault-interpretation）
    m_canvas->setWells({});
    const double origin = m_timeOriginMs;

    QString title;
    if (type == SgySliceType::Inline) {
        title = tr("纵测线剖面 IL %1").arg(index);
    } else if (type == SgySliceType::Xline) {
        title = tr("横测线剖面 XL %1").arg(index);
    } else {
        const double ms = origin + index * (m_volume->SampleIntervalUs() / 1000.0);
        title = tr("水平时间切片 TWT %1 ms").arg(ms, 0, 'f', 1);
    }
    setLineTitle(title);

    m_progressBar->setRange(0, 100);
    m_progressBar->setValue(0);
    m_progressBar->setVisible(true);

    // 体快照（SgyVolume 拷贝廉价：索引经 shared_ptr 不可变共享，服务通道
    // 只走 const 面）。经 SeismicTaskService：≤4 并发闸 + 协作取消 +
    // 切片 LRU + 引擎 Auto 后端（sf3c 工作区热切换后自动吃随机访问红利）。
    auto vol = std::make_shared<SgyVolume>(*m_volume);
    const auto generation = m_generation;
    const auto request = ++m_sliceRequest;
    QPointer<SeismicSectionDockWidget> guard(this);

    PaleoTask *rawTask = m_taskService->startSliceExtraction(
        vol, type, index,
        [guard, generation, request, vol, type, index, title, origin](
            bool ok, std::shared_ptr<const SgySliceImage> image, const QString &error) {
            // 双重守卫：世代号（切体/重开）+ 请求号（被更新的切片请求
            // 顶替）。被顶替/取消的旧任务静默丢弃——cancelled 不是失败，
            // 不弹误导错误、不覆盖新请求已应用的画面。
            if (!guard || guard->m_generation != generation || guard->m_sliceRequest != request)
                return;
            guard->m_sliceTask.clear();
            guard->m_progressBar->setVisible(false);

            if (!ok) {
                // D2.14：原因态——画布显示可读原因而非空白
                guard->setLineTitle(guard->tr("切片提取失败: %1").arg(error));
                guard->m_canvas->clearData();
                guard->m_canvas->setNoDataReason(guard->tr("剖面不可用\n%1").arg(error));
                emit guard->sectionExtractionFinished(false, error);
                return;
            }
            if (!image || image->width <= 0 || image->height <= 0 || image->values.empty()) {
                // D2.14：空数据原因态（如无有效道的线号）
                guard->setLineTitle(title);
                guard->m_canvas->clearData();
                guard->m_canvas->setNoDataReason(
                    guard->tr("%1\n该线无有效地震道（工区覆盖范围外）").arg(title));
                emit guard->sectionExtractionFinished(false, guard->tr("空切片"));
                return;
            }

            guard->setLineTitle(title);
            if (guard->m_btnCurtain && guard->m_btnCurtain->isChecked())
                guard->updateCompareSlice(); // D2.10：当前线变了，相邻线 B 图同步
            if (type == SgySliceType::Time) {
                const double ms = origin + index * (vol->SampleIntervalUs() / 1000.0);
                guard->m_canvas->setTimeSliceData(*image, ms, vol->InlineMin(),
                                                  vol->InlineMax(), vol->XlineMin(),
                                                  vol->XlineMax());
            } else {
                const float dtMs = vol->SampleIntervalUs() > 0
                                       ? (vol->SampleIntervalUs() / 1000.0f) : 2.0f;
                guard->m_canvas->setSectionData(*image, dtMs, origin);
            }
            // D4：剖面身份 + 最近切片（拾取解析/追踪原料）
            {
                SectionRef ref;
                ref.valid = true;
                ref.type = type;
                ref.index = index;
                if (type == SgySliceType::Inline)
                    ref.colMin = vol->XlineMin(), ref.colMax = vol->XlineMax(),
                    ref.colLines = vol->XlineValues();
                else if (type == SgySliceType::Xline)
                    ref.colMin = vol->InlineMin(), ref.colMax = vol->InlineMax(),
                    ref.colLines = vol->InlineValues();
                else
                    ref.colMin = vol->XlineMin(), ref.colMax = vol->XlineMax();
                // 列轴长度与切片列数不符（理论上不会）→ 不用轴表，避免错位映射。
                if (!ref.colLines.empty() && int(ref.colLines.size()) != image->width)
                    ref.colLines.clear();
                ref.t0Ms = origin; // #146：剖面时间原点（记录延迟）
                guard->m_canvas->setSectionRef(ref);
                if (type != SgySliceType::Time)
                    guard->m_lastSlice = *image;
            }
            guard->refreshInterpretationOverlay();
            emit guard->sectionExtractionFinished(true, QString());
        });
    m_sliceTask = rawTask;

    // 进度条由任务字节进度驱动（quiet 任务照常 reportBytes；直读后端逐线
    // 上报、引擎后端完成时一次上报）。按请求号守卫：被顶替的旧任务其
    // changed() 连接仍存活到任务终态，无守卫会把旧线的百分比写进新请求
    // 的进度条（fast sweep 时可见回跳）。
    if (rawTask) {
        QPointer<PaleoTask> progressTask = rawTask;
        connect(rawTask, &PaleoTask::changed, this, [this, progressTask, request]() {
            if (!progressTask || m_sliceRequest != request)
                return;
            if (progressTask->bytesTotal() > 0)
                m_progressBar->setValue(std::max(0, progressTask->percent()));
        });
    }
}

void SeismicSectionDockWidget::setSectionData(
    const SgySliceImage &image,
    float sampleIntervalMs,
    double startSampleMs,
    const std::vector<float> &columnDistancesM,
    const std::vector<glm::dvec2> &mapCoords)
{
    m_canvas->setSectionData(image, sampleIntervalMs, startSampleMs, columnDistancesM, mapCoords);
}

void SeismicSectionDockWidget::setWells(const std::vector<SectionWellInfo> &wells) {
    m_canvas->setWells(wells);
}

void SeismicSectionDockWidget::setTimeDepthModel(const TimeDepthModel &model) {
    m_canvas->setTimeDepthModel(model);
}

void SeismicSectionDockWidget::setLineTitle(const QString &title) {
    m_lblTitle->setText(title.isEmpty() ? tr("测线: 未加载") : title);
}

void SeismicSectionDockWidget::onZoomChanged(double) {
    // Zoom update
}

void SeismicSectionDockWidget::onTraceHovered(
    int traceIndex, double twtMs, double depthM, float amplitude, double mapX, double mapY)
{
    if (m_canvas->traceCount() <= 0)
        return;

    if (m_canvas->orientation() == SectionOrientation::TimeSlice) {
        m_lblCoordinates->setText(
            tr("横测线 (XL): %1 | 纵测线 (IL): %2 | 时间: %3 ms | 深度: %4 m | 振幅: %5")
                .arg(qRound(mapX))
                .arg(qRound(mapY))
                .arg(twtMs, 0, 'f', 1)
                .arg(depthM, 0, 'f', 1)
                .arg(amplitude, 0, 'f', 4));
        return;
    }

    QString text = tr("道: %1/%2 | TWT: %3 ms | 深度: %4 m | 振幅: %5")
        .arg(traceIndex + 1)
        .arg(m_canvas->traceCount())
        .arg(twtMs, 0, 'f', 1)
        .arg(depthM, 0, 'f', 1)
        .arg(amplitude, 0, 'f', 4);

    if (std::abs(mapX) > 1e-3 || std::abs(mapY) > 1e-3) {
        text += QStringLiteral(" | (X: %1, Y: %2)").arg(mapX, 0, 'f', 1).arg(mapY, 0, 'f', 1);
    }

    m_lblCoordinates->setText(text);
}

void SeismicSectionDockWidget::onExportSnapshot() {
    const QString filePath = QFileDialog::getSaveFileName(
        this, tr("导出剖面图件"), QStringLiteral("seismic_section.png"),
        tr("PNG 图像 (*.png);;JPEG 图像 (*.jpg *.jpeg)"));
    if (filePath.isEmpty())
        return;

    QImage img(m_canvas->size(), QImage::Format_ARGB32_Premultiplied);
    m_canvas->render(&img);
    if (img.save(filePath)) {
        paleo::ui::NotificationManager::showInfo(this, tr("导出成功"), tr("剖面图件已成功保存到:\n%1").arg(filePath));
    } else {
        paleo::ui::NotificationManager::showError(this, tr("导出失败"), tr("保存图像文件失败，请检查文件写入权限。"));
    }
}

void SeismicSectionDockWidget::refreshWellOverlay(
    const std::vector<SectionWellInfo> &wells) {
  if (m_route.size() < 2)
    return;
  std::vector<double> distances(m_distances.begin(), m_distances.end());
  // Keep every candidate's offset; buffer changes must not require
  // re-extraction.
  auto projected = SectionWellProjector::ProjectWells(
      m_route, {}, distances, wells, std::numeric_limits<double>::max(),
      m_canvas->timeDepthModel());
  m_canvas->setWells(projected);
}

void SeismicSectionDockWidget::extractSectionFromVolumeAsync(
    std::shared_ptr<const SgyVolume> volume,
    const std::vector<glm::ivec2> &pathPoints, const QString &lineTitle,
    const std::vector<glm::dvec2> &mapPolyline,
    const std::vector<SectionWellInfo> &candidateWells) {
  if (!volume || pathPoints.size() < 2 || !m_taskService)
    return;
  if (m_extraction)
    m_extraction->requestCancel();
  // 任意线顶替切片显示：取消在途切片/卷帘任务并作废去抖键——迟到的
  // 切片结果由世代号丢弃，旧体读取不再占并发闸。
  if (m_sliceTask) {
    m_sliceTask->requestCancel();
    m_sliceTask.clear();
  }
  if (m_compareTask) {
    m_compareTask->requestCancel();
    m_compareTask.clear();
  }
  m_sliceIndex = -1;
  const auto generation = ++m_generation;
  m_volume = volume;
  m_route.clear();
  m_distances.clear();
  // goal/fault-interpretation：任意线剖面身份 = 路径点串；同时作废 IL/XL
  // 陈旧 SectionRef（旧实现任意线不设 ref，拾取/断层会误归属上一条线）。
  m_lastPathPoints = pathPoints;
  m_canvas->setSectionRef(SectionRef{});
  m_canvas->setWells({});
  m_cboSectionMode->blockSignals(true);
  m_cboSectionMode->setCurrentIndex(3);
  m_cboSectionMode->blockSignals(false);
  m_sliceGroup->hide();
  setLineTitle(tr("%1 · 提取中…").arg(lineTitle));
  m_progressBar->setRange(0, 0);
  m_progressBar->show();
  const double origin = m_timeOriginMs;
  SgySectionOptions options;
  options.maxColumns = 2048;
  options.interpolate = false;
  QPointer<SeismicSectionDockWidget> guard(this);
  m_extraction = m_taskService->startSectionExtraction(
      std::make_shared<SgyVolume>(*volume), pathPoints, options,
      [guard, generation, volume, pathPoints, lineTitle, mapPolyline,
       candidateWells,
       origin](bool ok, std::shared_ptr<const SgySliceImage> image,
               const SgySectionStats &stats, const QString &error) {
        if (!guard || guard->m_generation != generation)
          return;
        guard->m_progressBar->hide();
        if (!ok) {
          guard->setLineTitle(guard->tr("剖面提取失败：%1").arg(error));
          emit guard->sectionExtractionFinished(false, error);
          return;
        }
        const auto geometry = SectionGeometry::fromColumns(
            pathPoints, mapPolyline, stats.columnDistances);
        guard->m_route = mapPolyline;
        guard->m_distances = geometry.distancesM;
        guard->m_canvas->setSectionData(
            *image, volume->SampleIntervalUs() / 1000.0f, origin,
            geometry.distancesM, geometry.coordinates);
        guard->refreshWellOverlay(candidateWells);
        guard->setLineTitle(lineTitle);
        // #225：入参候选井写入成员——computeWellTrajectories /
        // computeSyntheticOverlays / 井旁道 / 子波提取 / 反演低频井读的都是
        // m_candidateWells，旧实现只按入参判空、成员恒空，四条链生产恒死。
        guard->m_candidateWells = candidateWells;
        // D5.3/D5.4：井轨迹投影 + 合成记录（任意线链路，wave/seismic-chain-deep）
        if (!candidateWells.empty()) {
          guard->computeWellTrajectories(mapPolyline);
          guard->computeSyntheticOverlays();
        }
        // 与切片路径同拍刷新解释叠加：剖面身份变了（任意线），
        // FaultSet 棒按新身份重新过滤回显（goal/fault-interpretation）。
        guard->refreshInterpretationOverlay();
        emit guard->sectionExtractionFinished(true, QString());
      });
}


// ---- D2.12 书签操作 ----
void SeismicSectionDockWidget::addBookmark(const QString &name) {
    SectionBookmark bm;
    bm.name = name;
    bm.modeIndex = m_cboSectionMode->currentIndex();
    bm.sliceValue = m_spinSlice->value();
    bm.view = m_canvas->viewState();
    m_bookmarks.append(bm);
    saveBookmarksToSettings();
    m_cboBookmark->addItem(bm.name);
}

void SeismicSectionDockWidget::removeBookmark(int index) {
    if (index < 0 || index >= m_bookmarks.size())
        return;
    m_bookmarks.removeAt(index);
    saveBookmarksToSettings();
    m_cboBookmark->removeItem(index);
}

void SeismicSectionDockWidget::applyBookmark(int index) {
    if (index < 0 || index >= m_bookmarks.size())
        return;
    const SectionBookmark &bm = m_bookmarks[index];
    if (m_cboSectionMode->currentIndex() != bm.modeIndex)
        m_cboSectionMode->setCurrentIndex(bm.modeIndex); // 触发重提取
    else if (bm.modeIndex <= 2 && m_spinSlice->value() != bm.sliceValue)
        m_spinSlice->setValue(bm.sliceValue);
    m_canvas->setViewState(bm.view);
}

// ---- D2.12 书签持久化 ----
QString SeismicSectionDockWidget::volumeSettingsKey() const {
    if (!m_volume || !m_volume->IsLoaded())
        return QString();
    const auto &path = m_volume->Path();
    QString key = QString::fromStdString(path.string());
    // Windows 的 path.string() 用反斜杠——QSettings 注册表键不允许 '\'，
    // 两种分隔符都归一为 '_'（平台稳定的体身份键）。
    key.replace(QLatin1Char('/'), QLatin1Char('_'));
    key.replace(QLatin1Char('\\'), QLatin1Char('_'));
    return key;
}

void SeismicSectionDockWidget::saveBookmarksToSettings() const {
    const QString key = volumeSettingsKey();
    if (key.isEmpty())
        return;
    QJsonArray arr;
    for (const auto &bm : m_bookmarks) {
        QJsonObject o;
        o.insert("name", bm.name);
        o.insert("mode", bm.modeIndex);
        o.insert("slice", bm.sliceValue);
        o.insert("zoomX", bm.view.zoomX);
        o.insert("zoomY", bm.view.zoomY);
        o.insert("panX", bm.view.panX);
        o.insert("panY", bm.view.panY);
        arr.append(o);
    }
    QSettings settings;
    settings.setValue(QStringLiteral("seismic/sectionBookmarks/%1").arg(key),
                      QString::fromUtf8(QJsonDocument(arr).toJson(QJsonDocument::Compact)));
}

void SeismicSectionDockWidget::loadBookmarksFromSettings() {
    m_bookmarks.clear();
    m_cboBookmark->clear();
    const QString key = volumeSettingsKey();
    if (key.isEmpty())
        return;
    QSettings settings;
    const QString raw = settings.value(QStringLiteral("seismic/sectionBookmarks/%1").arg(key)).toString();
    if (raw.isEmpty())
        return;
    const QJsonDocument doc = QJsonDocument::fromJson(raw.toUtf8());
    for (const auto &v : doc.array()) {
        const QJsonObject o = v.toObject();
        SectionBookmark bm;
        bm.name = o.value("name").toString();
        bm.modeIndex = o.value("mode").toInt();
        bm.sliceValue = o.value("slice").toInt();
        bm.view.zoomX = o.value("zoomX").toDouble();
        bm.view.zoomY = o.value("zoomY").toDouble();
        bm.view.panX = o.value("panX").toDouble();
        bm.view.panY = o.value("panY").toDouble();
        if (!bm.name.isEmpty()) {
            m_bookmarks.append(bm);
            m_cboBookmark->addItem(bm.name);
        }
    }
}

// ---- D2.10 卷帘 B 图：相邻线提取（同一切片服务通道） ----
void SeismicSectionDockWidget::updateCompareSlice() {
    if (!m_volume || !m_volume->IsLoaded() || !m_taskService)
        return;
    const int mode = m_cboSectionMode->currentIndex();
    if (mode != 0 && mode != 1)
        return; // 卷帘仅支持 IL/XL 模式
    const int current = m_spinSlice->value();
    int neighbor = -1;
    QString label;
    if (mode == 0) { // Inline：取相邻 IL（优先 +1，没有则 -1）
        const auto &ils = m_volume->InlineValues();
        const auto it = std::find(ils.begin(), ils.end(), current);
        if (it != ils.end()) {
            if (it + 1 != ils.end())
                neighbor = *(it + 1);
            else if (it != ils.begin())
                neighbor = *(it - 1);
        }
        label = QStringLiteral("IL %1").arg(neighbor);
    } else {
        const auto &xls = m_volume->XlineValues();
        const auto it = std::find(xls.begin(), xls.end(), current);
        if (it != xls.end()) {
            if (it + 1 != xls.end())
                neighbor = *(it + 1);
            else if (it != xls.begin())
                neighbor = *(it - 1);
        }
        label = QStringLiteral("XL %1").arg(neighbor);
    }
    if (neighbor < 0) {
        m_canvas->setCompareData(SgySliceImage{}, tr("无相邻线"));
        return;
    }

    // 顶替旧在途相邻线请求（快速换线时不再排队）；与主切片共用切片 LRU——
    // 相邻线一旦看过，滑到该线的主图即缓存命中。
    if (m_compareTask) {
        m_compareTask->requestCancel();
        m_compareTask.clear();
    }
    auto vol = std::make_shared<SgyVolume>(*m_volume);
    const auto type = mode == 0 ? SgySliceType::Inline : SgySliceType::Xline;
    const auto generation = m_generation;
    const auto request = ++m_compareRequest;
    QPointer<SeismicSectionDockWidget> guard(this);

    PaleoTask *rawTask = m_taskService->startSliceExtraction(
        vol, type, neighbor,
        [guard, generation, request, label](bool ok,
                                            std::shared_ptr<const SgySliceImage> image,
                                            const QString &error) {
            // 世代号（切体）+ 请求号（被新相邻线顶替）守卫：取消不弹错。
            if (!guard || guard->m_generation != generation || guard->m_compareRequest != request)
                return;
            guard->m_compareTask.clear();
            if (!ok) {
                // 如实降级：失败给原因文案，不静默留空白帘（诚实失败契约）。
                guard->m_canvas->setCompareData(
                    SgySliceImage{}, guard->tr("相邻线提取失败: %1").arg(error));
                return;
            }
            if (!image || image->values.empty()) {
                guard->m_canvas->setCompareData(
                    SgySliceImage{}, guard->tr("%1\n该线无有效地震道").arg(label));
                return;
            }
            guard->m_canvas->setCompareData(*image, label);
        });
    m_compareTask = rawTask;
}

// ---- D2.11 道头信息卡 ----
void SeismicSectionDockWidget::showTraceHeaderCard(int traceIndex) {
    if (!m_volume || !m_volume->IsLoaded())
        return;
    const QString sgyPath = QString::fromStdString(m_volume->Path().string());
    const SeismicTraceHeaderInfo info = SeismicTaskService::readTraceHeader(sgyPath, traceIndex);

    if (!m_traceCard) {
        m_traceCard = new QDialog(this);
        m_traceCard->setWindowTitle(tr("道头信息"));
        m_traceCard->setModal(false);
        m_traceCard->setMinimumSize(360, 300);
        auto *lay = new QVBoxLayout(m_traceCard);
        m_traceCardTable = new QTableWidget(m_traceCard);
        m_traceCardTable->setColumnCount(2);
        m_traceCardTable->setHorizontalHeaderLabels({tr("字段"), tr("值")});
        m_traceCardTable->horizontalHeader()->setStretchLastSection(true);
        m_traceCardTable->verticalHeader()->setVisible(false);
        m_traceCardTable->setEditTriggers(QAbstractItemView::NoEditTriggers);
        lay->addWidget(m_traceCardTable);
        auto *btnClose = new QToolButton(m_traceCard);
        btnClose->setText(tr("关闭"));
        connect(btnClose, &QToolButton::clicked, m_traceCard, &QDialog::close);
        lay->addWidget(btnClose, 0, Qt::AlignRight);
    }
    const auto addRow = [this](const QString &k, const QString &v) {
        const int row = m_traceCardTable->rowCount();
        m_traceCardTable->insertRow(row);
        m_traceCardTable->setItem(row, 0, new QTableWidgetItem(k));
        m_traceCardTable->setItem(row, 1, new QTableWidgetItem(v));
    };
    m_traceCardTable->setRowCount(0);
    if (info.ok) {
        addRow(tr("道序号（0 基）"), QString::number(info.traceIndex));
        addRow(tr("文件偏移 (B)"), QString::number(info.fileOffset));
        addRow(tr("INLINE (189-192)"), QString::number(info.inlineNo));
        addRow(tr("CROSSLINE (193-196)"), QString::number(info.xlineNo));
        addRow(tr("field record (9-12)"), QString::number(info.fieldRecord));
        addRow(tr("CDP ensemble (21-24)"), QString::number(info.cdpEnsemble));
        addRow(tr("CDP X (73-76)"), QString::number(info.cdpX, 'f', 2));
        addRow(tr("CDP Y (77-80)"), QString::number(info.cdpY, 'f', 2));
        addRow(tr("采样数 (115-116)"), QString::number(info.sampleCount));
        addRow(tr("采样间隔 (117-118, μs)"), QString::number(info.sampleIntervalUs));
    } else {
        addRow(tr("错误"), info.error);
    }
    m_traceCard->show();
    m_traceCard->raise();
    m_traceCard->activateWindow();
}

void SeismicSectionDockWidget::onTraceClicked(int traceIndex, double twtMs, double depthM,
                                              float amplitude, double, double) {
    // D2.11：点击道 → 道头信息卡；状态栏同步读数
    showTraceHeaderCard(traceIndex);
    onTraceHovered(traceIndex, twtMs, depthM, amplitude, 0.0, 0.0);
}

// ---- D2.13 复制 / 打印 ----
void SeismicSectionDockWidget::onCopyImage() {
    QApplication::clipboard()->setImage(m_canvas->grabCanvasImage(2.0));
    setLineTitle(tr("剖面图已复制到剪贴板"));
}

void SeismicSectionDockWidget::onPrintImage() {
    QPrinter printer(QPrinter::HighResolution);
    QPrintDialog dlg(&printer, this);
    dlg.setWindowTitle(tr("打印地震剖面"));
    if (dlg.exec() != QDialog::Accepted)
        return;
    QPainter painter(&printer);
    const QImage img = m_canvas->grabCanvasImage(2.0);
    const QRectF pageRect = printer.pageRect(QPrinter::DevicePixel);
    const double scale = std::min(pageRect.width() / img.width(), pageRect.height() / img.height());
    const QSizeF target(img.width() * scale, img.height() * scale);
    const QPointF offset((pageRect.width() - target.width()) / 2.0, (pageRect.height() - target.height()) / 2.0);
    painter.drawImage(QRectF(offset, target), img);
    painter.end();
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

void SeismicSectionDockWidget::computeAttributeOnCurrentSection(
    SeismicTaskService::SeismicAttrKind kind,
    const SeismicTaskService::SeismicAttrParams &params) {
    if (!m_attrPanel)
        return;
    if (!m_taskService) {
        m_attrPanel->showResult(false, tr("任务服务未注入"));
        return;
    }
    if (!m_volume) {
        m_attrPanel->showResult(false, tr("地震体未加载（先打开 SEG-Y）"));
        return;
    }
    const int mode = m_cboSectionMode ? m_cboSectionMode->currentIndex() : 0;
    if (mode != 0 && mode != 1) {
        m_attrPanel->showResult(
            false, tr("属性计算支持 Inline/Crossline 剖面（时间片/任意线见 TODOS）"));
        return;
    }
    const SgySliceType type = mode == 0 ? SgySliceType::Inline : SgySliceType::Xline;
    const int index = m_spinSlice ? m_spinSlice->value() : 0;

    const QString sourcePath = QString::fromStdString(m_volume->Path().string());
    m_attrPanel->setBusy(true);
    // #224：请求时快照剖面身份（世代号随换线/换体/任意线推进）+ 迟到回调守卫。
    // 换线换体后迟到的属性图不得贴到新剖面上，也不得顶替可登记结果。
    const quint64 generation = m_generation;
    QPointer<SeismicSectionDockWidget> guard(this);
    PaleoTask *task = m_taskService->startAttributeSlice(
        m_volume, kind, params, type, index,
        [guard, generation, params, sourcePath](
            bool ok, const SeismicTaskService::SeismicAttrResult &r) {
            if (!guard || !guard->m_attrPanel)
                return;
            SeismicSectionDockWidget *self = guard.data();
            if (self->m_generation != generation) {
                self->m_attrPanel->showResult(
                    false, self->tr("剖面已切换，本次属性结果丢弃（请在当前剖面重新计算）"));
                return;
            }
            if (ok && r.image) {
                self->m_lastAttrResult = r;
                self->m_lastAttrParams = params;
                self->m_lastAttrSourcePath = sourcePath;
                self->m_canvas->setAttrOverlay(*r.image);
                self->m_attrPanel->showResult(
                    true, self->tr("✓ %1 完成（读 %2ms / 算 %3ms，有效道 %4/%5）")
                              .arg(r.attrId)
                              .arg(int(r.readMs))
                              .arg(int(r.computeMs))
                              .arg(r.validTraceCount)
                              .arg(r.traceCount));
            } else {
                self->m_attrPanel->showResult(false, r.error);
            }
        });
    // 拒绝路径（缺线/边缘线/时间切片等）服务已同步回调具体原因——此处
    // 不覆盖状态；task 为空的场景 progress 连接跳过即可。
    m_attrTask = task;
    if (task) {
        connect(task, &PaleoTask::changed, this, [this, task]() {
            if (m_attrPanel && task->running())
                m_attrPanel->updateProgress(task->percent(), task->stage());
        });
    }
}

QString SeismicSectionDockWidget::registerCurrentAttributeAsset(QString *error) {
    if (!m_catalog) {
        if (error)
            *error = tr("catalog 未注入（应用层需调 setInterpretationCatalog）");
        return QString();
    }
    if (!m_lastAttrResult.ok || !m_lastAttrResult.image) {
        if (error)
            *error = tr("无可登记的成功属性结果");
        return QString();
    }
    return SeismicTaskService::registerAttributeSliceAsset(
        m_catalog, m_catalogAssetId, m_catalogVersionId, m_lastAttrResult,
        m_lastAttrParams, m_lastAttrSourcePath, m_interpretationDir, error);
}

// goal/attr-volume 扫描产物目录：解释目录优先（应用层注入）；未注入时
// 落 SEG-Y 伴生目录 <sgy>.attrs（会话伴生文件 <sgy>.seispicks.json 同
// 先例——项目无关可携带）。体扫描必写产物，无目录即被服务如实拒绝。
QString SeismicSectionDockWidget::attrScanOutputDir() const {
    if (!m_interpretationDir.isEmpty())
        return m_interpretationDir;
    if (!m_volume)
        return QString();
    const QString sgy = QString::fromStdString(m_volume->Path().string());
    return sgy.isEmpty() ? QString()
                         : sgy + QStringLiteral(".attrs");
}

// goal/attr-volume：时间切片扫描编排——完成即登记（DERIVED + 层树声明）；
// 取消/失败如实回面板不登记（取消无半成品 DERIVED）。登记上下文按扫描
// 启动时的快照取（回调期 catalog/装配可能换装——不读当时的成员状态）。
void SeismicSectionDockWidget::computeTimeSliceAttribute(
    SeismicTaskService::SeismicAttrKind kind,
    const SeismicTaskService::SeismicAttrParams &params, int sampleIndex) {
    if (!m_attrPanel)
        return;
    if (!m_taskService) {
        m_attrPanel->showResult(false, tr("任务服务未注入"));
        return;
    }
    if (!m_volume) {
        m_attrPanel->showResult(false, tr("地震体未加载（先打开 SEG-Y）"));
        return;
    }
    const QString sourcePath = QString::fromStdString(m_volume->Path().string());
    const QString outputDir = attrScanOutputDir();
    const QPointer<DataCatalog> catalog = m_catalog;
    const QString assetId = m_catalogAssetId;
    const QString versionId = m_catalogVersionId;
    // 扫描产物自动登记——「登记资产」按钮只服务剖面结果；清空旧剖面
    // 结果防「新参数 + 旧图像」的 SATR 错配登记。
    m_lastAttrResult = {};
    m_lastAttrParams = params;
    m_lastAttrSourcePath = sourcePath;
    m_attrPanel->setBusy(true);
    QPointer<SeismicSectionDockWidget> guard(this); // 迟到回调守卫（体传播同范式）
    PaleoTask *task = m_taskService->startTimeSliceAttribute(
        m_volume, kind, params, sampleIndex, outputDir,
        [this, guard, kind, params, sourcePath, outputDir, catalog, assetId,
         versionId](bool ok,
            const SeismicTaskService::SeismicAttrTimeSliceResult &r) {
            if (!guard)
                return; // dock 已亡：丢弃
            if (!m_attrPanel)
                return;
            if (ok) {
                const QString suffix =
                    r.cacheHit ? tr("（缓存命中）") : QString();
                m_attrPanel->showResult(
                    true, tr("✓ %1 时间切片 t=%2ms 完成 %3（有效 %4/%5）")
                              .arg(r.attrId)
                              .arg(int(r.timeMs))
                              .arg(suffix)
                              .arg(r.validCells)
                              .arg(qint64(r.nIl) * r.nXl),
                    /*registrable=*/false);
                // 登记 + 层树声明（扫描产物缓存命名，登记幂等按 param_hash）。
                // 登记失败：面板如实报错——不冒充已上图。
                if (catalog) {
                    LayerDeclaration decl;
                    QString err;
                    const QString path = SeismicTaskService::
                        registerTimeSliceAttributeAsset(
                            catalog, assetId, versionId, kind, params, r,
                            sourcePath, outputDir, &err, &decl);
                    if (path.isEmpty()) {
                        m_attrPanel->showResult(
                            false, tr("✓ 扫描完成但登记失败：%1").arg(err));
                    } else {
                        emit timeSliceAttrLayerReady(decl);
                    }
                } else {
                    m_attrPanel->showResult(
                        false, tr("✓ 扫描完成（catalog 未注入，未登记上图）"));
                }
            } else {
                m_attrPanel->showResult(false, r.error);
            }
        });
    m_attrTask = task;
    if (task) {
        connect(task, &PaleoTask::changed, this, [this, task]() {
            if (m_attrPanel && task->running())
                m_attrPanel->updateProgress(task->percent(), task->stage());
        });
    }
}

// goal/attr-volume：整体属性体扫描编排——完成即登记（SATV DERIVED）；
// 3D 喂入走静默预览任务（SATV 可达 GB 级，主线程同步读会冻 UI——取数
// 归服务线程，视口只收烘焙好的图像面）。
void SeismicSectionDockWidget::computeAttributeVolume(
    SeismicTaskService::SeismicAttrKind kind,
    const SeismicTaskService::SeismicAttrParams &params) {
    if (!m_attrPanel)
        return;
    if (!m_taskService) {
        m_attrPanel->showResult(false, tr("任务服务未注入"));
        return;
    }
    if (!m_volume) {
        m_attrPanel->showResult(false, tr("地震体未加载（先打开 SEG-Y）"));
        return;
    }
    const QString sourcePath = QString::fromStdString(m_volume->Path().string());
    const QString outputDir = attrScanOutputDir();
    const QPointer<DataCatalog> catalog = m_catalog;
    const QString assetId = m_catalogAssetId;
    const QString versionId = m_catalogVersionId;
    m_lastAttrResult = {};
    m_lastAttrParams = params;
    m_lastAttrSourcePath = sourcePath;
    m_attrPanel->setBusy(true);
    QPointer<SeismicSectionDockWidget> guard(this); // 迟到回调守卫（体传播同范式）
    PaleoTask *task = m_taskService->startAttributeVolume(
        m_volume, kind, params, outputDir,
        [this, guard, taskSvc = QPointer<SeismicTaskService>(m_taskService), kind,
         params, sourcePath, outputDir, catalog, assetId,
         versionId](bool ok, const SeismicTaskService::SeismicAttrVolumeResult &r) {
            if (guard && !ok) {
                emit attrVolumeReady({}, false, r.error);
                if (m_attrPanel)
                    m_attrPanel->showResult(false, r.error);
                return;
            }
            if (!guard || !m_attrPanel)
                return;
            const QString suffix = r.cacheHit ? tr("（缓存命中）") : QString();
            QString message;
            if (catalog) {
                QString err;
                if (SeismicTaskService::registerAttributeVolumeAsset(
                        catalog, assetId, versionId, kind, params, r,
                        sourcePath, &err)
                        .isEmpty())
                    message = tr("登记失败：%1").arg(err);
            } else {
                message = tr("catalog 未注入，未登记");
            }
            m_attrPanel->showResult(
                true, tr("✓ %1 属性体完成 %2（%3×%4×%5，有效 %6）%7")
                          .arg(r.attrId)
                          .arg(suffix)
                          .arg(r.nIl)
                          .arg(r.nXl)
                          .arg(r.nS)
                          .arg(r.validCells)
                          .arg(message.isEmpty() ? QString()
                                                 : QStringLiteral("；") + message),
                /*registrable=*/false);
            // 3D 预览取数：静默任务（不占任务面板注意力），完成经信号
            // 喂视口；取数失败如实随信号报因。世代守卫：新预览发布即取消
            // 旧预览并丢弃其迟到回调（陈旧属性体不得覆盖新结果上图）。
            if (!taskSvc)
                return;
            if (m_attrPreviewTask)
                m_attrPreviewTask->requestCancel();
            const quint64 previewGen = ++m_attrPreviewGen;
            auto previewPtr =
                std::make_shared<SeismicTaskService::AttributeVolumePreview>();
            PaleoTask *previewTask = taskSvc->startBounded(
                QObject::tr("属性体 3D 预览取数"),
                [r, previewPtr](PaleoTask *) -> QString {
                    *previewPtr =
                        SeismicTaskService::loadAttributeVolumePreview(r.path);
                    return previewPtr->ok ? QString() : previewPtr->error;
                },
                QString(), /*quiet=*/true);
            m_attrPreviewTask = previewTask;
            if (previewTask) {
                connect(previewTask, &PaleoTask::finished, this,
                        [this, guard, previewGen, previewPtr]() {
                          if (!guard || previewGen != m_attrPreviewGen)
                            return; // dock 已亡/已被更新预览顶替：丢弃
                          m_attrPreviewTask.clear();
                          emit attrVolumeReady(*previewPtr, previewPtr->ok,
                                               previewPtr->error);
                        });
            }
        });
    m_attrTask = task;
    if (task) {
        connect(task, &PaleoTask::changed, this, [this, task]() {
            if (m_attrPanel && task->running())
                m_attrPanel->updateProgress(task->percent(), task->stage());
        });
    }
}

QString SeismicSectionDockWidget::sessionFilePath() const {
    return m_session.sourceSgyPath.isEmpty() ? QString()
        : m_session.sourceSgyPath + QStringLiteral(".seispicks.json");
}

void SeismicSectionDockWidget::setInterpretationCatalog(DataCatalog *catalog,
                                                        const QString &assetId,
                                                        const QString &versionId,
                                                        const QString &outputDir) {
    m_catalog = catalog;
    m_catalogAssetId = assetId;
    m_catalogVersionId = versionId;
    m_interpretationDir = outputDir;
}

SeismicInterpretationSession &SeismicSectionDockWidget::mutableSession() {
    return m_session; // undo 命令的写入口（友元路径）
}

void SeismicSectionDockWidget::refreshInterpretationOverlay() {
    m_canvas->setPickOverlays(m_session.picks, m_session.faults);
    if (m_pickPanel)
        m_pickPanel->refreshFromSession();
    refreshFaultStickOverlay(); // FaultSet 棒随会话刷新同拍更新
}

void SeismicSectionDockWidget::setPickMode(SectionPickMode mode) {
    m_canvas->setPickMode(mode);
    if (m_pickPanel)
        m_pickPanel->setVisible(mode != SectionPickMode::None);
    if (mode != SectionPickMode::None && m_session.interpreters.isEmpty()) {
        m_session.interpreters << QStringLiteral("解释员A");
        if (m_pickPanel)
            m_pickPanel->refreshFromSession();
    }
}

void SeismicSectionDockWidget::addPickFromCanvas(int traceCol, double twtMs) {
    const SectionRef ref = m_canvas->sectionRef();
    if (!ref.valid || !m_volume || !m_volume->IsLoaded())
        return;
    const float dtMs = m_volume->SampleIntervalUs() > 0
        ? m_volume->SampleIntervalUs() / 1000.0f : 2.0f;
    SeismicPick pick;
    // #146：画布 TWT 含记录延迟 t0，样点号相对剖面首样。
    pick.sampleIndex = sectionSampleForTwt(twtMs, ref.t0Ms, dtMs);
    if (pick.sampleIndex < 0)
        return;
    pick.twtMs = sectionTwtForSample(pick.sampleIndex, ref.t0Ms, dtMs);
    // #147：列 → 实际测线号（非单位线距不能 colMin+col）。
    int lineNo = 0;
    if (!sectionLineForColumn(ref.colLines, ref.colMin, traceCol, &lineNo))
        return;
    if (ref.type == SgySliceType::Inline) {
        pick.inlineNo = ref.index;
        pick.xlineNo = lineNo;
    } else if (ref.type == SgySliceType::Xline) {
        pick.xlineNo = ref.index;
        pick.inlineNo = lineNo;
    } else {
        return; // 时间切片不拾取（水平向无 TWT 概念）
    }
    pick.confidence = 1.0f;
    pick.interpreter = m_pickPanel ? m_pickPanel->currentInterpreter() : QString();
    pick.horizonName = m_pickPanel ? m_pickPanel->currentHorizon() : QStringLiteral("H1");
    if (pick.interpreter.isEmpty())
        pick.interpreter = QStringLiteral("解释员A");
    if (!m_session.interpreters.contains(pick.interpreter))
        m_session.interpreters << pick.interpreter;
    if (pick.horizonName.isEmpty())
        pick.horizonName = QStringLiteral("H1");

    m_undoStack->push(new AddPicksCommand(this, {pick}));
    refreshInterpretationOverlay();
}

void SeismicSectionDockWidget::addPicks(const QList<SeismicPick> &picks) {
    if (picks.isEmpty())
        return;
    m_undoStack->push(new AddPicksCommand(this, picks));
    refreshInterpretationOverlay();
}

void SeismicSectionDockWidget::removePick(int id) {
    const SeismicPick *p = m_session.pickById(id);
    if (!p)
        return;
    m_undoStack->push(new RemovePickCommand(this, *p));
    refreshInterpretationOverlay();
}

void SeismicSectionDockWidget::renamePickHorizon(int id, const QString &newName) {
    const SeismicPick *p = m_session.pickById(id);
    if (!p)
        return;
    m_undoStack->push(new RenamePickCommand(this, id, p->horizonName, newName));
    refreshInterpretationOverlay();
}

void SeismicSectionDockWidget::addFaultFromCanvas(const QVector<QPair<double, double>> &points) {
    if (points.size() < 2)
        return;
    if (m_faultController) {
        // goal/fault-interpretation：拾取落 FaultSet（undo 入编排器栈，
        // 落工程存储）。不再双写会话伴生文件——FaultSet 是断层权威路径。
        paleo::fault::FaultSectionRef section;
        if (!currentFaultSection(&section))
            return; // 时间切片等无剖面身份，不拾取
        paleo::fault::FaultStick stick;
        stick.section = section;
        stick.points = points;
        stick.interpreter = m_pickPanel ? m_pickPanel->currentInterpreter() : QString();
        m_faultController->addStick(stick); // 模型变更经 faultSetChanged 回刷
        return;
    }
    const SectionRef ref = m_canvas->sectionRef();
    if (!ref.valid)
        return;
    SeismicFaultSegment seg;
    seg.id = m_session.nextId++;
    seg.sectionType = ref.type;
    seg.sectionIndex = ref.index;
    seg.points = points;
    seg.interpreter = m_pickPanel ? m_pickPanel->currentInterpreter() : QString();
    seg.name = QStringLiteral("F%1").arg(m_session.faults.size() + 1);
    m_session.faults.append(seg);
    saveInterpretationSession();
    refreshInterpretationOverlay();
}

void SeismicSectionDockWidget::setFaultController(
    paleo::fault::FaultInterpretationController *controller) {
    if (m_faultController == controller)
        return;
    m_faultController = controller;
    if (!controller) {
        refreshFaultStickOverlay();
        return;
    }
    connect(controller, &paleo::fault::FaultInterpretationController::faultSetChanged, this,
            &SeismicSectionDockWidget::refreshFaultStickOverlay);
    connect(controller, &paleo::fault::FaultInterpretationController::faultSelectionChanged, this,
            &SeismicSectionDockWidget::refreshFaultStickOverlay);
    refreshFaultStickOverlay();
}

bool SeismicSectionDockWidget::currentFaultSection(paleo::fault::FaultSectionRef *out) const {
    const SectionRef ref = m_canvas->sectionRef();
    paleo::fault::FaultSectionRef section;
    if (ref.valid && ref.type == SgySliceType::Inline) {
        section.kind = paleo::fault::FaultSectionRef::Inline;
        section.index = ref.index;
        section.displayName = tr("IL %1").arg(ref.index);
    } else if (ref.valid && ref.type == SgySliceType::Xline) {
        section.kind = paleo::fault::FaultSectionRef::Xline;
        section.index = ref.index;
        section.displayName = tr("XL %1").arg(ref.index);
    } else if (m_lastPathPoints.size() >= 2) {
        // 任意线身份 = IL/XL 路径点串（同路径重提取 → 同 pathId → 棒回显）
        QStringList pts;
        for (const glm::ivec2 &p : m_lastPathPoints)
            pts << QStringLiteral("%1,%2").arg(p.x).arg(p.y);
        section.kind = paleo::fault::FaultSectionRef::Arbitrary;
        section.pathId = pts.join(QLatin1Char(';'));
        section.displayName = tr("任意线 %1").arg(section.pathId);
    } else {
        return false; // 时间切片 / 无剖面身份
    }
    if (out)
        *out = section;
    return true;
}

void SeismicSectionDockWidget::setFaultSurfaceCut(
    const SeismicSectionCanvas::FaultSurfaceCutDisplay &cut) {
    if (m_canvas)
        m_canvas->setFaultSurfaceCut(cut);
}

void SeismicSectionDockWidget::refreshFaultStickOverlay() {
    if (!m_faultController) {
        m_canvas->setFaultStickOverlays({});
        return;
    }
    paleo::fault::FaultSectionRef section;
    QVector<SeismicSectionCanvas::FaultStickDisplay> displays;
    if (currentFaultSection(&section)) {
        const QStringList selected = m_faultController->selectedFaultIds();
        for (const auto &pair : m_faultController->faultSet().sticksForSection(section)) {
            SeismicSectionCanvas::FaultStickDisplay d;
            d.points = pair.second.points;
            d.highlighted = selected.contains(pair.first);
            displays.append(d);
        }
    }
    m_canvas->setFaultStickOverlays(displays);
}

bool SeismicSectionDockWidget::saveInterpretationSession(QString *error) {
    if (m_session.sourceSgyPath.isEmpty())
        return false;
    return SeismicTaskService::saveSession(m_session, error);
}

bool SeismicSectionDockWidget::loadInterpretationSession(QString *error) {
    if (m_session.sourceSgyPath.isEmpty())
        return false;
    return SeismicTaskService::loadSession(m_session.sourceSgyPath, m_session, error);
}

QString SeismicSectionDockWidget::registerCurrentHorizonAsset(QString *error) {
    if (!m_catalog) {
        if (error)
            *error = tr("catalog 未注入（应用层需调 setInterpretationCatalog）");
        return QString();
    }
    const QString horizon = m_pickPanel ? m_pickPanel->currentHorizon() : QString();
    QList<SeismicPick> picks;
    for (const SeismicPick &p : m_session.picks)
        if (horizon.isEmpty() || p.horizonName == horizon)
            picks << p;
    // goal/horizon-autotrack：CSV + 层位栅格 GeoTIFF + 可上图声明
    LayerDeclaration decl;
    const QString path = SeismicTaskService::registerHorizonAsset(
        m_catalog, m_catalogAssetId, m_catalogVersionId,
        horizon.isEmpty() ? QStringLiteral("H1") : horizon,
        picks, m_interpretationDir, error, &decl);
    if (!path.isEmpty() && !decl.layerId.isEmpty())
        emit horizonLayerDeclared(decl);
    return path;
}

QString SeismicSectionDockWidget::registerCurrentFaultAsset(QString *error) {
    if (!m_catalog) {
        if (error)
            *error = tr("catalog 未注入（应用层需调 setInterpretationCatalog）");
        return QString();
    }
    return SeismicTaskService::registerFaultAsset(
        m_catalog, m_catalogAssetId, m_catalogVersionId,
        QStringLiteral("F1"), m_session.faults, m_interpretationDir, error);
}

void SeismicSectionDockWidget::runTracking() {
    // goal/horizon-autotrack：多种子异步追踪（原 D4.2 单种子同步升级）。
    // 种子集 = 当前剖面、同层位的手动拾取（conf==1，D4.10 语义）；种子
    // pick 缺席时回落最后一个拾取。
    if (m_trackTask)
        return; // 在途中不重复触发（取消走 cancelTracking）
    const SeismicPick *seed = m_session.pickById(m_trackSeedPick);
    if (!seed && !m_session.picks.isEmpty())
        seed = &m_session.picks.last();
    if (!seed || m_lastSlice.values.empty())
        return;

    const SectionRef ref = m_canvas->sectionRef();
    if (!ref.valid)
        return;
    // 按值捕获（异步回调点火时本函数栈已退——引用捕获会悬垂）
    const auto onSection = [ref](const SeismicPick &p) {
        return ref.type == SgySliceType::Inline ? p.inlineNo == ref.index
                                                : p.xlineNo == ref.index;
    };
    const auto colOf = [ref](const SeismicPick &p) {
        return sectionColumnForLine(ref.colLines, ref.colMin,
                                    ref.type == SgySliceType::Inline ? p.xlineNo : p.inlineNo);
    };
    const QString interpreter = seed->interpreter;
    const QString horizon = seed->horizonName;
    const double dtMsSeed = m_volume && m_volume->SampleIntervalUs() > 0
        ? m_volume->SampleIntervalUs() / 1000.0 : 2.0;
    QList<QPair<int, int>> seeds;
    for (const SeismicPick &p : m_session.picks)
        if (p.horizonName == horizon && p.confidence == 1.0f && onSection(p))
        {
            const int col = colOf(p);
            // #146：种子样点按 TWT 重算（旧会话 sampleIndex 可能未扣记录延迟）。
            const int sample = sectionSampleForTwt(p.twtMs, ref.t0Ms, dtMsSeed);
            if (col >= 0 && col < m_lastSlice.width && sample >= 0 && sample < m_lastSlice.height)
                seeds.append({col, sample});
        }
    if (seeds.isEmpty())
        return;

    const float dtMs = m_volume && m_volume->SampleIntervalUs() > 0
        ? m_volume->SampleIntervalUs() / 1000.0f : 2.0f;
    if (m_pickPanel)
        m_pickPanel->setTrackingActive(true);
    QPointer<SeismicSectionDockWidget> guard(this); // 注入共享服务时迟到回调守卫
    SeismicTrackOptions trackOptions = m_trackOptions;
    trackOptions.columnLines = ref.colLines; // #147
    trackOptions.startTimeMs = ref.t0Ms;     // #146
    m_trackTask = m_taskService->startHorizonTracking(
        m_lastSlice, ref.type, ref.index, ref.colMin, ref.colMax, seeds,
        trackOptions, interpreter, horizon, dtMs,
        [this, guard, ref, horizon, colOf, onSection](bool ok, const QList<SeismicPick> &picks,
                                                      const SeismicTrackReport &report,
                                                      const QString &error) {
            if (!guard)
                return; // dock 已亡（取消后迟到回调）：丢弃
            m_trackTask = nullptr;
            m_lastTrackReport = report;
            if (m_pickPanel)
            {
                m_pickPanel->setTrackingActive(false);
                if (ok)
                    m_pickPanel->showTrackReport(report);
                else if (!error.isEmpty())
                    m_pickPanel->showTrackError(error);
            }
            if (!ok)
            {
                emit trackingFinished(false);
                return;
            }
            // 合并替换（一步 undo）：同列新旧机器拾取取高置信；手动列不动
            QHash<int, const SeismicPick *> machineByCol;
            QSet<int> manualCols;
            for (const SeismicPick &p : m_session.picks)
            {
                if (p.horizonName != horizon || !onSection(p))
                    continue;
                const int col = colOf(p);
                if (col < 0 || col >= m_lastSlice.width)
                    continue;
                if (p.confidence == 1.0f)
                    manualCols.insert(col);
                else if (!machineByCol.contains(col))
                    machineByCol.insert(col, &p);
            }
            QList<SeismicPick> removed, added;
            for (const SeismicPick &p : picks)
            {
                const int col = colOf(p);
                if (manualCols.contains(col))
                    continue; // 手动优先（含种子列）：不覆盖不重复
                const auto it = machineByCol.constFind(col);
                if (it != machineByCol.constEnd())
                {
                    if (p.confidence > (*it)->confidence)
                    {
                        removed << *(*it);
                        added << p;
                    }
                }
                else
                {
                    added << p;
                }
            }
            if (!removed.isEmpty() || !added.isEmpty())
                m_undoStack->push(new ReplacePicksCommand(this, removed, added));
            emit trackingFinished(true);
        });
}

void SeismicSectionDockWidget::cancelTracking() {
    if (m_trackTask)
        m_trackTask->requestCancel();
}

// ---- goal/horizon-3d 层位 3D 体传播 ---------------------------------------

SeismicSectionDockWidget::PropagationReadiness
SeismicSectionDockWidget::propagationReadiness(QString *reason) const {
    if (!m_volume || !m_volume->IsLoaded()) {
        if (reason)
            *reason = tr("无可用地震体（先加载 SEG-Y）");
        return PropagationReadiness::NoVolume;
    }
    const SectionRef ref = m_canvas ? m_canvas->sectionRef() : SectionRef{};
    if (!ref.valid || ref.type != SgySliceType::Inline) {
        if (reason)
            *reason = tr("3D 传播需以 inline 剖面为种子剖面（当前非 inline 切片/任意线）");
        return PropagationReadiness::NotInlineSection;
    }
    const QString horizon = m_pickPanel ? m_pickPanel->currentHorizon() : QString();
    const auto hasSeed = [ref, horizon](const SeismicPick &p) {
        return p.inlineNo == ref.index &&
               (horizon.isEmpty() || p.horizonName == horizon) &&
               p.confidence == 1.0f; // 手动拾取为种子（机器拾取不自带起点权威）
    };
    for (const SeismicPick &p : m_session.picks)
        if (hasSeed(p)) {
            if (reason)
                reason->clear();
            return PropagationReadiness::Ready;
        }
    if (reason)
        *reason = tr("种子剖面无当前层位的手动拾取（先 Ctrl+左键 拾取）");
    return PropagationReadiness::NoSeeds;
}

void SeismicSectionDockWidget::runVolumePropagation() {
    // goal/horizon-3d：种子剖面（当前 inline）手动拾取 → IL 全体滑窗扩散
    // → 层位面资产（DERIVED 锚源体版本）+ GeoTIFF 上图声明。结果不进会话
    // 拾取表——全体拾取集是图件级对象，非单剖面编辑对象（QC 走报告/图层）。
    if (m_propTask)
        return; // 在途中不重复触发（取消走 cancelVolumePropagation）
    QString reason;
    if (propagationReadiness(&reason) != PropagationReadiness::Ready) {
        if (m_pickPanel)
            m_pickPanel->showTrackError(reason);
        emit propagationFinished(false);
        return;
    }
    const SectionRef ref = m_canvas->sectionRef();
    const QString horizon = m_pickPanel && !m_pickPanel->currentHorizon().isEmpty()
                                ? m_pickPanel->currentHorizon()
                                : QStringLiteral("H1");
    QString interpreter;
    const double dtMsSeed = m_volume->SampleIntervalUs() > 0
                                ? m_volume->SampleIntervalUs() / 1000.0 : 2.0;
    SeismicTaskService::VolumePropagationRequest req;
    req.seedInline = ref.index;
    for (const SeismicPick &p : m_session.picks) {
        if (p.inlineNo != ref.index ||
            (!horizon.isEmpty() && p.horizonName != horizon) ||
            p.confidence != 1.0f)
            continue;
        // #146：种子样点按 TWT 重算（旧会话 sampleIndex 可能未扣记录延迟）
        const int sample = sectionSampleForTwt(p.twtMs, ref.t0Ms, dtMsSeed);
        if (sample >= 0 && sample < m_volume->SampleCount())
            req.seeds.append({p.xlineNo, sample}); // 种子域 = (xline 线号, 采样)
        if (interpreter.isEmpty())
            interpreter = p.interpreter;
    }
    if (req.seeds.isEmpty()) {
        if (m_pickPanel)
            m_pickPanel->showTrackError(tr("种子拾取无法换算到采样域（TWT 越界）"));
        emit propagationFinished(false);
        return;
    }
    req.windowSamples = m_trackOptions.windowSamples;
    req.maxSearchSamples = m_trackOptions.maxSearchSamples;
    req.correlationThreshold = m_trackOptions.correlationThreshold;
    req.dipHistoryPicks = m_trackOptions.dipHistoryPicks;
    req.startTimeMs = ref.t0Ms; // #146 记录延迟入 TWT 域
    req.interpreter = interpreter.isEmpty() ? tr("解释") : interpreter;
    req.horizonName = horizon;

    if (m_pickPanel)
        m_pickPanel->setPropagateActive(true);
    QPointer<SeismicSectionDockWidget> guard(this); // 迟到回调守卫（同 runTracking）
    m_propTask = m_taskService->startVolumePropagation(
        m_volume, req,
        [this, guard, horizon](bool ok, const QList<SeismicPick> &picks,
                               const SeismicTrackReport &report,
                               const QString &error) {
            if (!guard)
                return; // dock 已亡：丢弃
            m_propTask = nullptr;
            if (m_pickPanel) {
                m_pickPanel->setPropagateActive(false);
                if (ok)
                    m_pickPanel->showTrackReport(report);
                else if (!error.isEmpty())
                    m_pickPanel->showTrackError(error);
            }
            if (!ok) {
                emit propagationFinished(false);
                return;
            }
            // DERIVED 资产登记（catalog 未注入 = 无登记上下文，如实报出）
            if (m_catalog) {
                LayerDeclaration decl;
                QString err;
                const QString path = SeismicTaskService::registerPropagatedHorizonAsset(
                    m_catalog, m_catalogAssetId, m_catalogVersionId, horizon,
                    picks, m_interpretationDir, &err, &decl);
                if (!path.isEmpty()) {
                    if (!decl.layerId.isEmpty())
                        emit horizonLayerDeclared(decl); // 上图（app 装配接 declare）
                } else if (m_pickPanel) {
                    m_pickPanel->showTrackError(tr("层位资产登记失败：%1").arg(err));
                }
            } else if (m_pickPanel) {
                m_pickPanel->showTrackError(
                    tr("传播完成，但 catalog 未注入——层位面未登记上图"));
            }
            emit propagationFinished(true);
        });
}

void SeismicSectionDockWidget::cancelVolumePropagation() {
    if (m_propTask)
        m_propTask->requestCancel();
}

// ---- D5 井震与任意线 -----------------------------------------------------------

void SeismicSectionDockWidget::setCandidateWells(const std::vector<SectionWellInfo> &wells) {
    m_candidateWells = wells;
    computeSyntheticOverlays();
}

// 点到折线的最近投影（剖面横向比例 0..1）
static double ProjectPointOntoPolyline(const std::vector<glm::dvec2> &poly,
                                       const glm::dvec2 &pt, double *offsetOut = nullptr) {
    double bestT = 0.0, bestDist = std::numeric_limits<double>::max(), bestOffset = 0.0;
    double total = 0.0;
    for (std::size_t i = 1; i < poly.size(); ++i) {
        const glm::dvec2 a = poly[i - 1], b = poly[i];
        const glm::dvec2 ab = b - a;
        const double len2 = glm::dot(ab, ab);
        const double t = len2 > 1e-12 ? std::clamp(glm::dot(pt - a, ab) / len2, 0.0, 1.0) : 0.0;
        const glm::dvec2 proj = a + ab * t;
        const double d = glm::length(pt - proj);
        if (d < bestDist) {
            bestDist = d;
            bestT = (total + t * std::sqrt(len2));
            bestOffset = d;
        }
        total += std::sqrt(len2);
    }
    if (offsetOut)
        *offsetOut = bestOffset;
    return total > 1e-9 ? bestT / total : 0.0;
}

void SeismicSectionDockWidget::computeWellTrajectories(const std::vector<glm::dvec2> &mapPolyline) {
    m_lastMapPolyline = mapPolyline;
    std::vector<SeismicSectionCanvas::WellTrajectory> trajectories;
    if (mapPolyline.size() >= 2) {
        for (const SectionWellInfo &well : m_candidateWells) {
            SeismicSectionCanvas::WellTrajectory t;
            t.wellId = well.wellId;
            t.topTracePos = ProjectPointOntoPolyline(mapPolyline, {well.surfaceX, well.surfaceY});
            const glm::dvec2 bottom(well.bottomX != 0.0 ? well.bottomX : well.surfaceX,
                                    well.bottomY != 0.0 ? well.bottomY : well.surfaceY);
            t.bottomTracePos = ProjectPointOntoPolyline(mapPolyline, bottom);
            t.topTwtMs = 0.0;
            t.bottomTwtMs = m_canvas->timeDepthModel().DepthToTwtMs(well.totalDepth);
            // goal/well-trajectory：测斜站逐点投影成折线（twt 用 workbench
            // 逐站校准值；未对齐站回退画布时深模型，再不行顶点不画）。
            const TimeDepthModel &td = m_canvas->timeDepthModel();
            for (const WellTrajSample &s : well.trajectory) {
                SeismicSectionCanvas::TrajVertex v;
                v.tracePos = ProjectPointOntoPolyline(mapPolyline, {s.x, s.y});
                v.twtMs = std::isfinite(s.twtMs) && s.twtMs > 0.0
                              ? s.twtMs
                              : (s.tvd > 0.0 ? td.DepthToTwtMs(s.tvd) : s.twtMs);
                t.vertices.push_back(v);
            }
            trajectories.push_back(t);
        }
    }
    m_canvas->setWellTrajectories(trajectories);

    // D5.3 原因态：时深无实测检查点 → 状态栏注记（仍按均速投影）
    if (!m_candidateWells.empty() && !m_canvas->timeDepthModel().hasCheckshots())
        setLineTitle(tr("%1（时深用默认均速——无实测检查点表）").arg(m_lblTitle->text()));
}

void SeismicSectionDockWidget::computeSyntheticOverlays() {
    std::vector<SeismicSectionCanvas::SyntheticOverlay> overlays;
    const TimeDepthModel &td = m_canvas->timeDepthModel();
    for (const SectionWellInfo &well : m_candidateWells) {
        SeismicSectionCanvas::SyntheticOverlay ov;
        ov.wellId = well.wellId;
        const WellCurveItem *ac = nullptr;
        const WellCurveItem *den = nullptr;
        for (const WellCurveItem &c : well.curves) {
            if (c.curveName == QLatin1String("AC") && !c.values.empty())
                ac = &c;
            if ((c.curveName == QLatin1String("DEN") || c.curveName == QLatin1String("RHOB")) && !c.values.empty())
                den = &c;
        }
        if (!ac || !den) {
            ov.reason = !ac ? tr("缺声波曲线 AC") : tr("缺密度曲线 DEN");
            overlays.push_back(ov);
            continue;
        }
        const auto result = SeismicTaskService::computeSyntheticSeismogram(
            ac->depthsM, ac->values, den->depthsM, den->values, td, 25.0);
        ov.ok = result.ok;
        ov.reason = result.reason;
        ov.twtMs = result.twtMs;
        ov.amplitude = result.amplitude;
        overlays.push_back(ov);
    }
    m_canvas->setSyntheticOverlays(overlays);
}

// D5.1 任意线路径编辑器：多段折线（il xl 每行一节点）→ 提取
void SeismicSectionDockWidget::showArbitraryLineEditor() {
    if (!m_volume || !m_volume->IsLoaded()) {
        paleo::ui::NotificationManager::showWarning(this, tr("任意线编辑器"), tr("请先加载地震体。"));
        return;
    }
    QDialog dlg(this);
    dlg.setWindowTitle(tr("任意线编辑器（每行一个节点：inline xline）"));
    dlg.setMinimumSize(420, 320);
    auto *lay = new QVBoxLayout(&dlg);
    auto *edit = new QTextEdit(&dlg);
    edit->setFont(PaleoTheme::monoFont(PaleoTheme::tokens().bodyPt));
    edit->setPlaceholderText(tr("1000 2000\n1002 2005\n1005 2012"));
    lay->addWidget(edit, 1);
    auto *chkWells = new QCheckBox(tr("投影候选井（井震综合）"), &dlg);
    chkWells->setChecked(!m_candidateWells.empty());
    lay->addWidget(chkWells);
    auto *btnBox = new QDialogButtonBox(QDialogButtonBox::Ok | QDialogButtonBox::Cancel, &dlg);
    connect(btnBox, &QDialogButtonBox::accepted, &dlg, &QDialog::accept);
    connect(btnBox, &QDialogButtonBox::rejected, &dlg, &QDialog::reject);
    lay->addWidget(btnBox);
    if (dlg.exec() != QDialog::Accepted)
        return;

    std::vector<glm::ivec2> pathPoints;
    const auto rows = edit->toPlainText().split(QLatin1Char('\n'), Qt::SkipEmptyParts);
    for (const QString &row : rows) {
        const auto parts = row.simplified().split(QRegularExpression(QStringLiteral("[ ,\\t]+")), Qt::SkipEmptyParts);
        if (parts.size() != 2)
            continue;
        bool okA = false, okB = false;
        const int il = parts[0].toInt(&okA);
        const int xl = parts[1].toInt(&okB);
        if (okA && okB)
            pathPoints.push_back({il, xl});
    }
    if (pathPoints.size() < 2) {
        paleo::ui::NotificationManager::showWarning(this, tr("任意线编辑器"), tr("至少需要 2 个有效节点。"));
        return;
    }
    const std::vector<SectionWellInfo> wells =
        chkWells->isChecked() ? m_candidateWells : std::vector<SectionWellInfo>{};
    extractSectionFromVolumeAsync(
        m_volume, pathPoints, tr("任意线（%1 节点）").arg(pathPoints.size()), {}, wells);
}

// D5.6 井旁道小图：最近井位置的地震道 wiggle + 分层刻度
void SeismicSectionDockWidget::showWellSideTrace() {
    if (!m_volume || !m_volume->IsLoaded() || m_candidateWells.empty()) {
        paleo::ui::NotificationManager::showInfo(this, tr("井旁道"), tr("无可用的候选井。"));
        return;
    }
    // 最近井（离当前剖面最近）
    const SectionWellInfo *best = &m_candidateWells.front();
    for (const SectionWellInfo &w : m_candidateWells)
        if (std::abs(w.offsetDistanceM) < std::abs(best->offsetDistanceM))
            best = &w;
    // 井口 XY → 测网 (IL, XL)（#129：FindNearest* 的参数是测线号，不能直接喂米制坐标；
    // 先经道头拟合的测网仿射换算，再吸附到体的实际线号）。
    if (!m_volume->Index()) {
        paleo::ui::NotificationManager::showWarning(this, tr("井旁道"), tr("地震体无道头索引，无法把井口坐标换算到测网。"));
        return;
    }
    // 换算/覆盖/吸附/取列统一走 domain/seismic/welltracelocate（tst_welltracelocate
    // 以 IL 步长 2、XL 步长 4、旋转测网钉住）；覆盖余量按实际线距的半步。
    const SgyCoordinateMapper mapper = SgyCoordinateMapper::Fit(*m_volume->Index());
    const seismic::WellTraceLocation loc =
        seismic::locateWellTrace(*m_volume->Index(), mapper, best->surfaceX, best->surfaceY);
    if (!loc.ok && loc.outOfCoverage) {
        paleo::ui::NotificationManager::showInfo(this, tr("井旁道"),
                                                 tr("井 %1 井口 (%2, %3) 在测网覆盖范围外（连续解 IL %4 / XL %5），不取井旁道。")
                                                     .arg(best->wellName)
                                                     .arg(best->surfaceX, 0, 'f', 1)
                                                     .arg(best->surfaceY, 0, 'f', 1)
                                                     .arg(loc.inlineF, 0, 'f', 1)
                                                     .arg(loc.xlineF, 0, 'f', 1));
        return;
    }
    if (!loc.ok && !mapper.valid()) {
        paleo::ui::NotificationManager::showWarning(this, tr("井旁道"),
                             tr("测网坐标拟合不可用：%1").arg(QString::fromStdString(mapper.Describe())));
        return;
    }
    if (!loc.ok) {
        paleo::ui::NotificationManager::showWarning(this, tr("井旁道"),
                             tr("井口无法定位到测网道：%1").arg(QString::fromStdString(loc.error)));
        return;
    }
    const int il = loc.inlineNo;
    const int xl = loc.xlineNo;

    // 提取该 IL 剖面再取井列（同步——单剖面读取毫秒级）
    SgySliceImage slice;
    std::string err;
    if (!m_volume->ExtractSlice(SgySliceType::Inline, il, slice, err)) {
        paleo::ui::NotificationManager::showWarning(this, tr("井旁道"), tr("道提取失败：%1").arg(QString::fromStdString(err)));
        return;
    }
    // 列 = xl 在体实际线号表中的位置（#147 同口径：线距可 >1）；剖面宽度须与
    // 线号表一致，否则列轴对不上——如实拒绝，不按单位线距猜。
    const int col = int(m_volume->XlineValues().size()) == slice.width ? loc.column : -1;
    if (col < 0 || col >= slice.width) {
        paleo::ui::NotificationManager::showWarning(this, tr("井旁道"), tr("XL %1 不在 IL %2 剖面列轴上").arg(xl).arg(il));
        return;
    }

    QDialog dlg(this);
    dlg.setWindowTitle(tr("井旁道 · %1（IL %2 / XL %3）").arg(best->wellName).arg(il).arg(xl));
    dlg.setMinimumSize(300, 520);
    auto *lay = new QVBoxLayout(&dlg);
    auto *trace = new class WellSideTraceWidget(best, col, slice, m_canvas->sampleIntervalMs(),
                                                 m_timeOriginMs, &dlg);
    lay->addWidget(trace, 1);
    auto *btnClose = new QToolButton(&dlg);
    btnClose->setText(tr("关闭"));
    connect(btnClose, &QToolButton::clicked, &dlg, &QDialog::close);
    lay->addWidget(btnClose, 0, Qt::AlignRight);
    dlg.exec();
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

namespace {
// #125：井 AC×DEN → TWT 域阻抗散样。TWT 取曲线自带的逐井时间轴（SectionWorkbench
// 按该井时深表/常速近似 + 平移算好，与剖面井叠加同口径），不用画布全局模型重算；
// 时深无效的井返回 false（调用方跳过并提示），不补默认速度。
bool wellTimeDepthTable(const WellCurveItem *ac, const WellCurveItem *den,
                        std::vector<double> *tdDepthM, std::vector<double> *tdTwtMs)
{
    std::vector<std::pair<double, double>> pairs;
    paleo::inv::appendTimeDepthPairs(ac->depthsM, ac->twtMs, &pairs);
    paleo::inv::appendTimeDepthPairs(den->depthsM, den->twtMs, &pairs);
    return paleo::inv::buildTimeDepthTable(std::move(pairs), tdDepthM, tdTwtMs);
}

bool wellImpedanceTwt(const WellCurveItem *ac, const WellCurveItem *den,
                      std::vector<double> *twtMs, std::vector<float> *impedance)
{
    std::vector<double> tdD, tdT;
    if (!wellTimeDepthTable(ac, den, &tdD, &tdT))
        return false;
    return paleo::inv::wellImpedanceTwt(ac->depthsM, ac->values, den->depthsM, den->values, tdD, tdT,
                                        twtMs, impedance);
}
} // namespace

// 「提取子波（最近井）」：AC×DEN+时深+井旁道 → Wiener LS 子波 → DERIVED
// wavelet 资产（catalog 未注入时落临时文件并如实说明）。
void SeismicSectionDockWidget::extractWaveletFromNearestWell() {
    if (!m_volume || !m_volume->IsLoaded()) {
        if (m_invPanel)
            m_invPanel->showResult(false, tr("地震体未加载（先打开 SEG-Y）"));
        return;
    }
    if (m_candidateWells.empty()) {
        if (m_invPanel)
            m_invPanel->showResult(false, tr("剖面缓冲带内无候选井"));
        return;
    }
    const SectionWellInfo *best = nullptr;
    for (const SectionWellInfo &w : m_candidateWells)
        if (!best || w.offsetDistanceM < best->offsetDistanceM)
            best = &w;
    const WellCurveItem *ac = nullptr;
    const WellCurveItem *den = nullptr;
    for (const WellCurveItem &c : best->curves) {
        if (c.curveName == QLatin1String("AC") && !c.values.empty())
            ac = &c;
        if ((c.curveName == QLatin1String("DEN") || c.curveName == QLatin1String("RHOB")) &&
            !c.values.empty())
            den = &c;
    }
    if (!ac || !den) {
        if (m_invPanel)
            m_invPanel->showResult(
                false, tr("井 %1 缺 %2 曲线").arg(best->wellId, !ac ? QStringLiteral("AC") : QStringLiteral("DEN")));
        return;
    }

    // 时深表（#125）：取该井曲线自带的逐井 TWT（含时间平移），不用画布全局模型。
    std::vector<double> tdDepthM, tdTwtMs;
    if (!wellTimeDepthTable(ac, den, &tdDepthM, &tdTwtMs)) {
        if (m_invPanel)
            m_invPanel->showResult(false, tr("井 %1 缺有效时深（未对齐），不能提取子波").arg(best->wellId));
        return;
    }
    paleo::inv::InversionWorkflow wf;
    paleo::inv::WaveletJobRequest req;
    req.wellId = best->wellId;
    req.wellX = best->surfaceX;
    req.wellY = best->surfaceY;
    req.acDepthsM = ac->depthsM;
    req.acUsPerM = ac->values;
    req.denDepthsM = den->depthsM;
    req.denValues = den->values;
    req.tdDepthM = std::move(tdDepthM);
    req.tdTimeMs = std::move(tdTwtMs);
    req.seismicPath = m_session.sourceSgyPath;
    req.parentVersionId = m_catalogVersionId;

    paleo::inv::InversionWorkflow::WaveletJob job;
    QString err;
    if (!wf.prepareWaveletJob(req, &job, &err) || !wf.computeWaveletJob(&job)) {
        if (m_invPanel)
            m_invPanel->showResult(false, err.isEmpty() ? job.error : err);
        return;
    }

    QString waveletPath;
    if (m_catalog) {
        DerivedAssetRegistrar registrar(m_catalog, m_interpretationDir);
        const QString versionId = wf.publishWaveletJob(job, registrar, &err);
        if (!versionId.isEmpty()) {
            for (const CatalogVersion &v : m_catalog->versions()) {
                if (v.id == versionId)
                    waveletPath = QDir(m_interpretationDir).filePath(v.path);
            }
        }
    }
    if (waveletPath.isEmpty()) {
        // catalog 未注入或发布失败：临时文件承载（如实说明，不进谱系）。
        waveletPath = QDir(QDir::temp()).filePath(QStringLiteral("paleo-wavelet-%1.json").arg(best->wellId));
        QFile f(waveletPath);
        if (f.open(QIODevice::WriteOnly))
            f.write(paleo::inversion::waveletToJson(job.wavelet));
    }
    if (m_invPanel) {
        m_invPanel->setWaveletPath(waveletPath);
        m_invPanel->showResult(
            true, tr("子波已提取（%1）：主频 %2 Hz，拟合相关 %3")
                      .arg(job.nearestTraceNote)
                      .arg(job.wavelet.dominantFreqHz(), 0, 'f', 1)
                      .arg(job.fitCorrelation, 0, 'f', 3));
    }
}

// 「反演」：InversionWorkflow 三段式。prepare+compute 在任务线程，发布回
// GUI 线程（DERIVED 登记）；世代号丢弃陈旧发布。
void SeismicSectionDockWidget::runInversion(const InversionPanelParams &params) {
    if (!m_invPanel)
        return;
    if (!m_taskService) {
        m_invPanel->showResult(false, tr("任务服务未注入"));
        return;
    }
    if (!m_volume || !m_volume->IsLoaded()) {
        m_invPanel->showResult(false, tr("地震体未加载（先打开 SEG-Y）"));
        return;
    }
    if (params.waveletPath.isEmpty() || !QFile::exists(params.waveletPath)) {
        m_invPanel->showResult(false, tr("子波文件无效（先「提取子波」或浏览已有资产）"));
        return;
    }

    // 低频井输入：候选井 AC×DEN → TWT 阻抗散样（无层位 → 全局趋势回退，
    // 语义如实记入产物 extra.layersUsed）。
    std::vector<paleo::inv::InversionWorkflow::LowFreqWellInput> wells;
    QStringList skippedNoTd;
    for (const SectionWellInfo &w : m_candidateWells) {
        const WellCurveItem *ac = nullptr;
        const WellCurveItem *den = nullptr;
        for (const WellCurveItem &c : w.curves) {
            if (c.curveName == QLatin1String("AC") && !c.values.empty())
                ac = &c;
            if ((c.curveName == QLatin1String("DEN") || c.curveName == QLatin1String("RHOB")) &&
                !c.values.empty())
                den = &c;
        }
        if (!ac || !den)
            continue;
        paleo::inv::InversionWorkflow::LowFreqWellInput lw;
        lw.wellId = w.wellId;
        lw.x = w.surfaceX;
        lw.y = w.surfaceY;
        if (!wellImpedanceTwt(ac, den, &lw.twtMs, &lw.impedance)) {
            skippedNoTd << w.wellId; // 缺有效逐井时深：不参与低频模型（#125）
            continue;
        }
        wells.push_back(std::move(lw));
    }

    auto request = std::make_shared<paleo::inv::InversionWorkflow::InversionJobRequest>();
    request->seismicPath = m_session.sourceSgyPath;
    request->waveletPath = params.waveletPath;
    request->method = params.method;
    request->lowCutHz = params.lowCutHz;
    request->lambda = params.lambda;
    request->maxIterations = params.maxIterations;
    request->lowFreqWells = std::move(wells);
    request->displayName = params.method == QStringLiteral("sparse")
                               ? tr("波阻抗体·稀疏脉冲")
                               : tr("波阻抗体·带限道积分");
    if (!m_session.sourceSgyPath.isEmpty())
        request->parentPaths = QStringList{m_session.sourceSgyPath, params.waveletPath};

    auto job = std::make_shared<paleo::inv::InversionWorkflow::InversionJob>();
    const quint64 gen = ++m_invGeneration;
    m_invPanel->setBusy(true);
    m_invTask = m_taskService->startBounded(
        tr("地震反演"),
        [request, job](PaleoTask *task) -> QString {
            if (task->cancelRequested())
                return tr("已取消");
            task->reportStage(QStringLiteral("build"), 2);
            QString err;
            if (!paleo::inv::InversionWorkflow().prepareInversionJob(*request, job.get(), &err))
                return err;
            task->reportStage(QStringLiteral("decode"), 5);
            if (!paleo::inv::InversionWorkflow().computeInversionJob(
                    job.get(), [task] { return task->cancelRequested(); },
                    [task](double f) {
                        if (task)
                            task->reportStage(QStringLiteral("decode"),
                                              5 + int(f * 90.0));
                    }))
                return job->error;
            return QString();
        },
        QString(), /*quiet=*/false);

    if (!m_invTask)
        return;
    connect(m_invTask, &PaleoTask::changed, this, [this]() {
        if (m_invPanel && m_invTask && m_invTask->running())
            m_invPanel->updateProgress(m_invTask->stagePercent() < 0 ? 0 : m_invTask->stagePercent(),
                                       m_invTask->stage());
    });
    connect(m_invTask, &PaleoTask::finished, this, [this, gen, job, skippedNoTd]() {
        if (!m_invPanel || !m_invTask)
            return;
        if (gen != m_invGeneration) {
            m_invPanel->showResult(false, tr("已有更新的反演请求，本次结果丢弃"));
            return;
        }
        if (m_invTask->state() == PaleoTask::State::Cancelled) {
            m_invPanel->showResult(false, tr("反演已取消"));
            return;
        }
        if (m_invTask->state() != PaleoTask::State::Succeeded || !job->ok) {
            m_invPanel->showResult(false,
                                   job->error.isEmpty() ? m_invTask->errorText() : job->error);
            return;
        }
        if (!m_catalog) {
            m_invPanel->showResult(false, tr("反演完成但 catalog 未注入，无法登记（体在 %1）")
                                             .arg(job->tempVolumePath));
            return;
        }
        DerivedAssetRegistrar registrar(m_catalog, m_interpretationDir);
        QString pubErr;
        const auto pub = paleo::inv::InversionWorkflow().publishInversionJob(*job, registrar, &pubErr);
        if (!pub.ok) {
            m_invPanel->showResult(false, pubErr);
            return;
        }
        m_invPanel->showResult(
            true,
            tr("反演完成：体 %1×%2×%3，处理 %4 道（失败 %5）｜频段 0–%6Hz+带限｜%7")
                .arg(job->nIl)
                .arg(job->nXl)
                .arg(job->nS)
                .arg(job->processedTraces)
                .arg(job->failedTraces)
                .arg(job->req.lowCutHz)
                .arg(job->req.method == QStringLiteral("sparse")
                         ? tr("平均残差能量比 %1").arg(job->meanResidualEnergyRatio, 0, 'f', 3)
                         : tr("低频方差占比 %1").arg(job->meanLowFreqVarianceFraction, 0, 'f', 3))
                + (job->wavelet.amplitudeScale > 0.0
                       ? QString()
                       : tr("｜子波无振幅标定（旧资产/解析子波），道振幅按反射系数直接使用"))
                + (skippedNoTd.isEmpty()
                       ? QString()
                       : tr("｜缺有效时深未参与低频模型：%1").arg(skippedNoTd.join(QStringLiteral("、")))));
    });
}

} // namespace seismic
