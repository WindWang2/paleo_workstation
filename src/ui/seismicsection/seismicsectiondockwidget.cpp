// 层：视图
#include "ui/seismicsection/seismicsectiondockwidget.h"

#include <QHBoxLayout>
#include <QVBoxLayout>
#include <QMenu>
#include <QAction>
#include <QFileDialog>
#include <QMessageBox>
#include <QThreadPool>
#include <QMetaObject>
#include <cmath>

#include "domain/seismic/sgysectionbuilder.h"

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
}

void SeismicSectionDockWidget::setupUi() {
    auto *container = new QWidget(this);
    auto *mainLay = new QVBoxLayout(container);
    mainLay->setContentsMargins(0, 0, 0, 0);
    mainLay->setSpacing(0);

    // ==========================================
    // 1. Top Toolbar (DESIGN.md Light Theme)
    // ==========================================
    auto *toolbar = new QWidget(container);
    toolbar->setStyleSheet(QStringLiteral("background: #FFFFFF; border-bottom: 1px solid #DFE5EC;"));
    auto *toolLay = new QHBoxLayout(toolbar);
    toolLay->setContentsMargins(8, 4, 8, 4);
    toolLay->setSpacing(6);

    const QString btnStyle = QStringLiteral(
        "QToolButton { background: transparent; border: 1px solid #DFE5EC; border-radius: 4px; padding: 2px 6px; font-size: 8.5pt; color: #24303E; }"
        "QToolButton:hover { background: #EDF1F5; border-color: #1B73D0; }"
        "QToolButton:pressed { background: #E8F0FE; }"
    );

    // Title Badge
    m_lblTitle = new QLabel(tr("测线: 未加载"), toolbar);
    m_lblTitle->setStyleSheet(QStringLiteral(
        "QLabel { background: #E8F0FE; color: #1B73D0; font-weight: bold; border-radius: 4px; padding: 2px 8px; font-size: 9pt; }"));
    toolLay->addWidget(m_lblTitle);

    // Section Mode Combo
    m_cboSectionMode = new QComboBox(toolbar);
    m_cboSectionMode->setObjectName(QStringLiteral("cboSectionMode"));
    m_cboSectionMode->addItems({tr("纵测线 (IL)"), tr("横测线 (XL)"), tr("时间切片 (Time)"), tr("任意测线/井剖面")});
    m_cboSectionMode->setStyleSheet(QStringLiteral(
        "QComboBox { border: 1px solid #DFE5EC; border-radius: 4px; padding: 2px 6px; font-size: 8.5pt; color: #24303E; background: #FFFFFF; font-weight: 500; }"));
    toolLay->addWidget(m_cboSectionMode);

    // Slicing Group (Slider + Spin + Time label)
    m_sliceGroup = new QWidget(toolbar);
    auto *sliceLay = new QHBoxLayout(m_sliceGroup);
    sliceLay->setContentsMargins(0, 0, 0, 0);
    sliceLay->setSpacing(4);

    m_lblSliceIndex = new QLabel(tr("纵测线:"), m_sliceGroup);
    m_lblSliceIndex->setStyleSheet(QStringLiteral("color: #5D6E80; font-size: 8.5pt; font-weight: 500;"));
    sliceLay->addWidget(m_lblSliceIndex);

    m_sliderSlice = new QSlider(Qt::Horizontal, m_sliceGroup);
    m_sliderSlice->setObjectName(QStringLiteral("sliderSectionSlice"));
    m_sliderSlice->setRange(1, 100);
    m_sliderSlice->setValue(1);
    m_sliderSlice->setFixedWidth(90);
    sliceLay->addWidget(m_sliderSlice);

    m_spinSlice = new QSpinBox(m_sliceGroup);
    m_spinSlice->setObjectName(QStringLiteral("spinSectionSlice"));
    m_spinSlice->setFont(QFont(QStringLiteral("JetBrains Mono"), 8));
    m_spinSlice->setRange(1, 100);
    m_spinSlice->setValue(1);
    m_spinSlice->setFixedWidth(64);
    sliceLay->addWidget(m_spinSlice);

    m_lblTimeMs = new QLabel(QStringLiteral("0.0 ms"), m_sliceGroup);
    m_lblTimeMs->setObjectName(QStringLiteral("lblSectionTimeMs"));
    m_lblTimeMs->setFont(QFont(QStringLiteral("JetBrains Mono"), 8));
    m_lblTimeMs->setStyleSheet(QStringLiteral("color: #1B73D0; font-weight: bold;"));
    m_lblTimeMs->setFixedWidth(68);
    m_lblTimeMs->setVisible(false);
    sliceLay->addWidget(m_lblTimeMs);

    toolLay->addWidget(m_sliceGroup);

    // Zoom Buttons
    m_btnZoomIn = new QToolButton(toolbar);
    m_btnZoomIn->setObjectName(QStringLiteral("btnSectionZoomIn"));
    m_btnZoomIn->setText(tr("+ 放大"));
    m_btnZoomIn->setStyleSheet(btnStyle);
    toolLay->addWidget(m_btnZoomIn);

    m_btnZoomOut = new QToolButton(toolbar);
    m_btnZoomOut->setObjectName(QStringLiteral("btnSectionZoomOut"));
    m_btnZoomOut->setText(tr("- 缩小"));
    m_btnZoomOut->setStyleSheet(btnStyle);
    toolLay->addWidget(m_btnZoomOut);

    m_btnFit = new QToolButton(toolbar);
    m_btnFit->setObjectName(QStringLiteral("btnSectionFit"));
    m_btnFit->setText(tr("适应窗口"));
    m_btnFit->setStyleSheet(btnStyle);
    toolLay->addWidget(m_btnFit);

    m_btnReset = new QToolButton(toolbar);
    m_btnReset->setObjectName(QStringLiteral("btnSectionReset"));
    m_btnReset->setText(tr("1:1"));
    m_btnReset->setStyleSheet(btnStyle);
    toolLay->addWidget(m_btnReset);

    // Vertical Unit Toggle
    m_btnUnitToggle = new QToolButton(toolbar);
    m_btnUnitToggle->setObjectName(QStringLiteral("btnSectionUnitToggle"));
    m_btnUnitToggle->setText(tr("单位: TWT (ms)"));
    m_btnUnitToggle->setToolTip(tr("切换纵轴单位：双程旅行时 TWT (ms) 与 深度 (m)"));
    m_btnUnitToggle->setStyleSheet(btnStyle);
    toolLay->addWidget(m_btnUnitToggle);

    // Colormap Combo
    auto *lblCmap = new QLabel(tr("色标:"), toolbar);
    lblCmap->setStyleSheet(QStringLiteral("color: #5D6E80; font-size: 8.5pt;"));
    toolLay->addWidget(lblCmap);

    m_cboColorMap = new QComboBox(toolbar);
    m_cboColorMap->setObjectName(QStringLiteral("cboSectionColorMap"));
    m_cboColorMap->addItems({tr("红白蓝 (双极)"), tr("灰度 (单极)"), tr("彩虹谱 (相图)")});
    m_cboColorMap->setStyleSheet(QStringLiteral(
        "QComboBox { border: 1px solid #DFE5EC; border-radius: 4px; padding: 2px 6px; font-size: 8.5pt; }"));
    toolLay->addWidget(m_cboColorMap);

    // Gain Control
    auto *lblGain = new QLabel(tr("增益:"), toolbar);
    lblGain->setStyleSheet(QStringLiteral("color: #5D6E80; font-size: 8.5pt;"));
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
    m_btnWellOptions->setStyleSheet(btnStyle);

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

    m_btnWellOptions->setMenu(wellMenu);
    toolLay->addWidget(m_btnWellOptions);

    toolLay->addStretch(1);

    // Export Snapshot Button
    m_btnExport = new QToolButton(toolbar);
    m_btnExport->setObjectName(QStringLiteral("btnSectionExport"));
    m_btnExport->setText(tr("导出图件"));
    m_btnExport->setStyleSheet(btnStyle);
    toolLay->addWidget(m_btnExport);

    mainLay->addWidget(toolbar);

    // ==========================================
    // 2. Center Canvas
    // ==========================================
    m_canvas = new SeismicSectionCanvas(container);
    m_canvas->setObjectName(QStringLiteral("seismicSectionCanvas"));
    mainLay->addWidget(m_canvas, 1);

    // ==========================================
    // 3. Bottom Status Bar (Monospace Readout)
    // ==========================================
    auto *statusBar = new QWidget(container);
    statusBar->setStyleSheet(QStringLiteral("background: #F5F7FA; border-top: 1px solid #DFE5EC; min-height: 24px;"));
    auto *statusLay = new QHBoxLayout(statusBar);
    statusLay->setContentsMargins(8, 2, 8, 2);
    statusLay->setSpacing(8);

    m_lblCoordinates = new QLabel(statusBar);
    m_lblCoordinates->setObjectName(QStringLiteral("lblSectionCoordinates"));
    m_lblCoordinates->setStyleSheet(QStringLiteral("font-family: 'JetBrains Mono'; font-size: 8.5pt; color: #24303E;"));
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
            m_btnUnitToggle->setText(tr("单位: 深度 (m)"));
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

    connect(m_cboSectionMode, QOverload<int>::of(&QComboBox::currentIndexChanged),
            this, &SeismicSectionDockWidget::onSectionModeChanged);

    connect(m_sliderSlice, &QSlider::valueChanged, m_spinSlice, &QSpinBox::setValue);
    connect(m_spinSlice, &QSpinBox::valueChanged, m_sliderSlice, &QSlider::setValue);
    connect(m_sliderSlice, &QSlider::valueChanged, this, &SeismicSectionDockWidget::onSliceSliderChanged);

    connect(m_btnExport, &QToolButton::clicked, this, &SeismicSectionDockWidget::onExportSnapshot);

    connect(m_canvas, &SeismicSectionCanvas::zoomChanged, this, &SeismicSectionDockWidget::onZoomChanged);
    connect(m_canvas, &SeismicSectionCanvas::traceHovered, this, &SeismicSectionDockWidget::onTraceHovered);
}

void SeismicSectionDockWidget::setVolume(std::shared_ptr<const SgyVolume> volume) {
    m_volume = volume;
    if (!m_volume || !m_volume->IsLoaded()) {
        m_sliceGroup->setEnabled(false);
        return;
    }

    m_sliceGroup->setEnabled(true);
    onSectionModeChanged(m_cboSectionMode->currentIndex());
}

void SeismicSectionDockWidget::onSectionModeChanged(int modeIndex) {
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
        const int mid = (m_volume->InlineMin() + m_volume->InlineMax()) / 2;
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
        const int mid = (m_volume->XlineMin() + m_volume->XlineMax()) / 2;
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
        const double ms = mid * (m_volume->SampleIntervalUs() / 1000.0);
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
        extractSliceAsync(SgySliceType::Inline, value);
    } else if (mode == 1) {
        extractSliceAsync(SgySliceType::Xline, value);
    } else if (mode == 2) {
        const double ms = value * (m_volume->SampleIntervalUs() / 1000.0);
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
    if (!m_volume || !m_volume->IsLoaded())
        return;

    if (m_isExtractingSlice) {
        m_pendingSliceType = type;
        m_pendingSliceIndex = index;
        return;
    }
    m_isExtractingSlice = true;

    QString title;
    if (type == SgySliceType::Inline) {
        title = QStringLiteral("纵测线剖面 IL %1").arg(index);
    } else if (type == SgySliceType::Xline) {
        title = QStringLiteral("横测线剖面 XL %1").arg(index);
    } else {
        const double ms = index * (m_volume->SampleIntervalUs() / 1000.0);
        title = QStringLiteral("水平时间切片 TWT %1 ms").arg(ms, 0, 'f', 1);
    }
    setLineTitle(title);

    m_progressBar->setValue(0);
    m_progressBar->setVisible(true);

    auto vol = m_volume;
    QThreadPool::globalInstance()->start([this, vol, type, index, title]() {
        SgySliceImage image;
        std::string err;
        auto progressCb = [this](int processed, int total) -> bool {
            if (total > 0) {
                const int pct = std::clamp(static_cast<int>(std::round(100.0 * processed / total)), 0, 100);
                QMetaObject::invokeMethod(this, [this, pct]() {
                    m_progressBar->setValue(pct);
                }, Qt::QueuedConnection);
            }
            return true;
        };

        const bool ok = vol->ExtractSlice(type, index, image, err, progressCb);

        QMetaObject::invokeMethod(this, [this, ok, image, type, index, title, vol, err]() {
            m_isExtractingSlice = false;
            m_progressBar->setVisible(false);

            if (m_pendingSliceIndex >= 0) {
                const auto nextType = m_pendingSliceType;
                const int nextIdx = m_pendingSliceIndex;
                m_pendingSliceIndex = -1;
                extractSliceAsync(nextType, nextIdx);
                if (nextType != type) {
                    return;
                }
            }

            if (!ok) {
                setLineTitle(tr("切片提取失败: %1").arg(QString::fromStdString(err)));
                emit sectionExtractionFinished(false, QString::fromStdString(err));
                return;
            }

            setLineTitle(title);
            if (type == SgySliceType::Time) {
                const double ms = index * (vol->SampleIntervalUs() / 1000.0);
                m_canvas->setTimeSliceData(image, ms, vol->InlineMin(), vol->InlineMax(), vol->XlineMin(), vol->XlineMax());
            } else {
                const float dtMs = vol->SampleIntervalUs() > 0 ? (vol->SampleIntervalUs() / 1000.0f) : 2.0f;
                m_canvas->setSectionData(image, dtMs, 0.0);
            }
            emit sectionExtractionFinished(true, QString());
        }, Qt::QueuedConnection);
    });
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
            QStringLiteral("横测线 (XL): %1 | 纵测线 (IL): %2 | 时间: %3 ms | 深度: %4 m | 振幅: %5")
                .arg(qRound(mapX))
                .arg(qRound(mapY))
                .arg(twtMs, 0, 'f', 1)
                .arg(depthM, 0, 'f', 1)
                .arg(amplitude, 0, 'f', 4));
        return;
    }

    QString text = QStringLiteral("道: %1/%2 | TWT: %3 ms | 深度: %4 m | 振幅: %5")
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
        QMessageBox::information(this, tr("导出成功"), tr("剖面图件已成功保存到:\n%1").arg(filePath));
    } else {
        QMessageBox::critical(this, tr("导出失败"), tr("保存图像文件失败，请检查文件写入权限。"));
    }
}

void SeismicSectionDockWidget::extractSectionFromVolumeAsync(
    std::shared_ptr<const SgyVolume> volume,
    const std::vector<glm::ivec2> &pathPoints,
    const QString &lineTitle,
    const std::vector<glm::dvec2> &mapPolyline,
    const std::vector<SectionWellInfo> &candidateWells)
{
    if (!volume || pathPoints.empty())
        return;

    m_volume = volume;
    m_cboSectionMode->blockSignals(true);
    m_cboSectionMode->setCurrentIndex(3);
    m_cboSectionMode->blockSignals(false);
    m_sliceGroup->setVisible(false);

    setLineTitle(lineTitle.isEmpty() ? tr("剖面抽取中...") : lineTitle);
    m_progressBar->setValue(0);
    m_progressBar->setVisible(true);

    const TimeDepthModel tdModel = m_canvas->timeDepthModel();
    const double bufferM = m_canvas->bufferDistanceM();

    QThreadPool::globalInstance()->start([this, volume, pathPoints, lineTitle, mapPolyline, candidateWells, tdModel, bufferM]() {
        SgySectionOptions opts;
        opts.maxColumns = 2048;
        opts.interpolate = false;

        SgySliceImage image;
        SgySectionStats stats;
        std::string errorMsg;

        auto progressCb = [this](int processed, int total) -> bool {
            if (total > 0) {
                const int pct = std::clamp(static_cast<int>(std::round(100.0 * processed / total)), 0, 100);
                QMetaObject::invokeMethod(this, [this, pct]() {
                    m_progressBar->setValue(pct);
                }, Qt::QueuedConnection);
            }
            return true;
        };

        const bool ok = BuildLineSection(*volume, pathPoints, opts, image, stats, errorMsg, progressCb);

        // Project wells if candidates provided
        std::vector<SectionWellInfo> projectedWells;
        if (!candidateWells.empty() && mapPolyline.size() >= 2) {
            std::vector<double> traceDist;
            traceDist.reserve(stats.columnDistances.size());
            for (float d : stats.columnDistances) {
                traceDist.push_back(static_cast<double>(d));
            }
            projectedWells = SectionWellProjector::ProjectWells(
                mapPolyline, {}, traceDist, candidateWells, bufferM, tdModel);
        }

        const float dtMs = static_cast<float>(volume->SampleIntervalUs()) / 1000.0f;
        const double t0Ms = 0.0;

        QMetaObject::invokeMethod(this, [this, ok, image, dtMs, t0Ms, stats, lineTitle, projectedWells, errorMsg]() mutable {
            m_progressBar->setVisible(false);
            if (ok) {
                setLineTitle(lineTitle);
                m_canvas->setSectionData(image, dtMs, t0Ms, stats.columnDistances);
                if (!projectedWells.empty()) {
                    m_canvas->setWells(projectedWells);
                }
                emit sectionExtractionFinished(true, QString());
            } else {
                setLineTitle(tr("剖面抽取失败"));
                emit sectionExtractionFinished(false, QString::fromStdString(errorMsg));
            }
        }, Qt::QueuedConnection);
    });
}

} // namespace seismic
