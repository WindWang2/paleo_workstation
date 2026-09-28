// 层：视图
#include "seismic3dviewpanel.h"

#include <QHBoxLayout>
#include <QVBoxLayout>
#include <QGridLayout>
#include <QToolButton>
#include <QSlider>
#include <QSpinBox>
#include <QLabel>
#include <QFrame>

namespace seismic {

namespace {

QToolButton *createToolBtn(const QString &text, const QString &tooltip, bool checkable = false, bool checked = false) {
    auto *btn = new QToolButton();
    btn->setText(text);
    btn->setToolTip(tooltip);
    btn->setCheckable(checkable);
    btn->setChecked(checked);
    btn->setStyleSheet(
        QStringLiteral(
            "QToolButton {"
            "  background-color: #FFFFFF;"
            "  color: #24303E;"
            "  border: 1px solid #DFE5EC;"
            "  border-radius: 4px;"
            "  padding: 3px 8px;"
            "  font-size: 9pt;"
            "}"
            "QToolButton:hover {"
            "  background-color: #EDF1F5;"
            "}"
            "QToolButton:checked {"
            "  background-color: #1B73D0;"
            "  color: #FFFFFF;"
            "  border-color: #1B73D0;"
            "}"
            "QToolButton:pressed {"
            "  background-color: #1565B8;"
            "  color: #FFFFFF;"
            "}"));
    return btn;
}

} // namespace

Seismic3DViewPanel::Seismic3DViewPanel(QWidget *parent)
    : QWidget(parent) {
    buildUi();
}

void Seismic3DViewPanel::buildUi() {
    auto *mainLay = new QVBoxLayout(this);
    mainLay->setContentsMargins(0, 0, 0, 0);
    mainLay->setSpacing(0);

    // 1. Top ToolBar
    auto *topBar = new QWidget(this);
    topBar->setStyleSheet(QStringLiteral("background-color: #EDF1F5; border-bottom: 1px solid #DFE5EC;"));
    auto *topLay = new QHBoxLayout(topBar);
    topLay->setContentsMargins(6, 4, 6, 4);
    topLay->setSpacing(4);

    btnIso_ = createToolBtn(tr("等轴测"), tr("切换至等轴测视角"));
    btnTop_ = createToolBtn(tr("俯视"), tr("切换至俯视 (平面) 视角"));
    btnFront_ = createToolBtn(tr("正视"), tr("切换至正视视角"));
    btnSide_ = createToolBtn(tr("侧视"), tr("切换至侧视视角"));
    btnFit_ = createToolBtn(tr("居中复位"), tr("居中并重置相机视角"));

    topLay->addWidget(btnIso_);
    topLay->addWidget(btnTop_);
    topLay->addWidget(btnFront_);
    topLay->addWidget(btnSide_);
    topLay->addWidget(btnFit_);

    auto *sep = new QFrame(topBar);
    sep->setFrameShape(QFrame::VLine);
    sep->setStyleSheet(QStringLiteral("color: #DFE5EC;"));
    topLay->addWidget(sep);

    btnFrame_ = createToolBtn(tr("包围盒"), tr("显示/隐藏工区三维包围线框"), true, true);
    btnInline_ = createToolBtn(tr("纵测线 (IL)"), tr("显示/隐藏纵测线切片"), true, true);
    btnCrossline_ = createToolBtn(tr("横测线 (XL)"), tr("显示/隐藏横测线切片"), true, true);
    btnTime_ = createToolBtn(tr("时间切片 (Z)"), tr("显示/隐藏时间切片"), true, true);

    topLay->addWidget(btnFrame_);
    topLay->addWidget(btnInline_);
    topLay->addWidget(btnCrossline_);
    topLay->addWidget(btnTime_);
    topLay->addStretch();

    mainLay->addWidget(topBar);

    // 2. Center Viewport
    viewport_ = new Seismic3DViewportWidget(this);
    viewport_->setObjectName(QStringLiteral("seismic3DViewport"));
    mainLay->addWidget(viewport_, 1);

    // 3. Bottom Sliders Bar
    auto *bottomBar = new QWidget(this);
    bottomBar->setStyleSheet(QStringLiteral("background-color: #FFFFFF; border-top: 1px solid #DFE5EC;"));
    auto *bottomLay = new QGridLayout(bottomBar);
    bottomLay->setContentsMargins(12, 8, 12, 8);
    bottomLay->setHorizontalSpacing(10);
    bottomLay->setVerticalSpacing(6);

    QFont monoFont(QStringLiteral("JetBrains Mono"), 9);

    // Inline row
    auto *lblIl = new QLabel(tr("纵测线 (IL):"), bottomBar);
    lblIl->setStyleSheet(QStringLiteral("font-weight: 500; color: #24303E;"));
    inlineSlider_ = new QSlider(Qt::Horizontal, bottomBar);
    inlineSlider_->setObjectName(QStringLiteral("inlineSlider"));
    inlineSpin_ = new QSpinBox(bottomBar);
    inlineSpin_->setObjectName(QStringLiteral("inlineSpin"));
    inlineSpin_->setFont(monoFont);
    inlineSpin_->setFixedWidth(75);

    bottomLay->addWidget(lblIl, 0, 0);
    bottomLay->addWidget(inlineSlider_, 0, 1);
    bottomLay->addWidget(inlineSpin_, 0, 2);

    // Crossline row
    auto *lblXl = new QLabel(tr("横测线 (XL):"), bottomBar);
    lblXl->setStyleSheet(QStringLiteral("font-weight: 500; color: #24303E;"));
    xlineSlider_ = new QSlider(Qt::Horizontal, bottomBar);
    xlineSlider_->setObjectName(QStringLiteral("xlineSlider"));
    xlineSpin_ = new QSpinBox(bottomBar);
    xlineSpin_->setObjectName(QStringLiteral("xlineSpin"));
    xlineSpin_->setFont(monoFont);
    xlineSpin_->setFixedWidth(75);

    bottomLay->addWidget(lblXl, 1, 0);
    bottomLay->addWidget(xlineSlider_, 1, 1);
    bottomLay->addWidget(xlineSpin_, 1, 2);

    // Time row
    auto *lblTime = new QLabel(tr("时间 (Z):"), bottomBar);
    lblTime->setStyleSheet(QStringLiteral("font-weight: 500; color: #24303E;"));
    timeSlider_ = new QSlider(Qt::Horizontal, bottomBar);
    timeSlider_->setObjectName(QStringLiteral("timeSlider"));
    timeSpin_ = new QSpinBox(bottomBar);
    timeSpin_->setObjectName(QStringLiteral("timeSpin"));
    timeSpin_->setFont(monoFont);
    timeSpin_->setFixedWidth(75);
    timeMsLabel_ = new QLabel(QStringLiteral("0.0 ms"), bottomBar);
    timeMsLabel_->setObjectName(QStringLiteral("timeMsLabel"));
    timeMsLabel_->setFont(monoFont);
    timeMsLabel_->setStyleSheet(QStringLiteral("color: #5D6E80;"));
    timeMsLabel_->setFixedWidth(80);

    bottomLay->addWidget(lblTime, 2, 0);
    bottomLay->addWidget(timeSlider_, 2, 1);
    bottomLay->addWidget(timeSpin_, 2, 2);
    bottomLay->addWidget(timeMsLabel_, 2, 3);

    mainLay->addWidget(bottomBar);

    // Connections - Toolbar
    connect(btnIso_, &QToolButton::clicked, this, [this]() {
        viewport_->setPresetView(SeismicCameraController::PresetView::Isometric);
    });
    connect(btnTop_, &QToolButton::clicked, this, [this]() {
        viewport_->setPresetView(SeismicCameraController::PresetView::Top);
    });
    connect(btnFront_, &QToolButton::clicked, this, [this]() {
        viewport_->setPresetView(SeismicCameraController::PresetView::Front);
    });
    connect(btnSide_, &QToolButton::clicked, this, [this]() {
        viewport_->setPresetView(SeismicCameraController::PresetView::Side);
    });
    connect(btnFit_, &QToolButton::clicked, this, [this]() {
        viewport_->fitToBounds();
    });

    connect(btnFrame_, &QToolButton::toggled, this, [this](bool checked) {
        viewport_->setFrameVisible(checked);
    });
    connect(btnInline_, &QToolButton::toggled, this, [this](bool checked) {
        viewport_->setSlotVisible(SeismicSliceSlot::Inline, checked);
    });
    connect(btnCrossline_, &QToolButton::toggled, this, [this](bool checked) {
        viewport_->setSlotVisible(SeismicSliceSlot::Crossline, checked);
    });
    connect(btnTime_, &QToolButton::toggled, this, [this](bool checked) {
        viewport_->setSlotVisible(SeismicSliceSlot::Time, checked);
    });

    // Connections - Sliders & Spinboxes
    connect(inlineSlider_, &QSlider::valueChanged, inlineSpin_, &QSpinBox::setValue);
    connect(inlineSpin_, &QSpinBox::valueChanged, inlineSlider_, &QSlider::setValue);
    connect(inlineSlider_, &QSlider::valueChanged, this, &Seismic3DViewPanel::onInlineSliderChanged);

    connect(xlineSlider_, &QSlider::valueChanged, xlineSpin_, &QSpinBox::setValue);
    connect(xlineSpin_, &QSpinBox::valueChanged, xlineSlider_, &QSlider::setValue);
    connect(xlineSlider_, &QSlider::valueChanged, this, &Seismic3DViewPanel::onCrosslineSliderChanged);

    connect(timeSlider_, &QSlider::valueChanged, timeSpin_, &QSpinBox::setValue);
    connect(timeSpin_, &QSpinBox::valueChanged, timeSlider_, &QSlider::setValue);
    connect(timeSlider_, &QSlider::valueChanged, this, &Seismic3DViewPanel::onTimeSliderChanged);

    connect(viewport_, &Seismic3DViewportWidget::glReady, this, [this]() {
        auto vol = volume();
        if (!vol || !vol->IsLoaded()) return;
        if (!viewport_->isSlotReady(SeismicSliceSlot::Inline)) {
            requestSliceUpdate(SeismicSliceSlot::Inline, SgySliceType::Inline, currentInline());
        }
        if (!viewport_->isSlotReady(SeismicSliceSlot::Crossline)) {
            requestSliceUpdate(SeismicSliceSlot::Crossline, SgySliceType::Xline, currentCrossline());
        }
        if (!viewport_->isSlotReady(SeismicSliceSlot::Time)) {
            requestSliceUpdate(SeismicSliceSlot::Time, SgySliceType::Time, currentTimeSample());
        }
    });
}

void Seismic3DViewPanel::setTaskService(SeismicTaskService *taskSvc) {
    taskSvc_ = taskSvc;
}

void Seismic3DViewPanel::setVolume(std::shared_ptr<SgyVolume> volume) {
    viewport_->setVolume(volume);

    if (!volume || !volume->IsLoaded()) {
        return;
    }

    const int inlMin = volume->InlineMin();
    const int inlMax = volume->InlineMax();
    const int xlMin = volume->XlineMin();
    const int xlMax = volume->XlineMax();
    const int sampleMax = volume->SampleMax();

    inlineSlider_->blockSignals(true);
    inlineSpin_->blockSignals(true);
    inlineSlider_->setRange(inlMin, inlMax);
    inlineSpin_->setRange(inlMin, inlMax);
    const int midInl = (inlMin + inlMax) / 2;
    inlineSlider_->setValue(midInl);
    inlineSpin_->setValue(midInl);
    inlineSlider_->blockSignals(false);
    inlineSpin_->blockSignals(false);

    xlineSlider_->blockSignals(true);
    xlineSpin_->blockSignals(true);
    xlineSlider_->setRange(xlMin, xlMax);
    xlineSpin_->setRange(xlMin, xlMax);
    const int midXl = (xlMin + xlMax) / 2;
    xlineSlider_->setValue(midXl);
    xlineSpin_->setValue(midXl);
    xlineSlider_->blockSignals(false);
    xlineSpin_->blockSignals(false);

    timeSlider_->blockSignals(true);
    timeSpin_->blockSignals(true);
    timeSlider_->setRange(0, sampleMax);
    timeSpin_->setRange(0, sampleMax);
    const int midTime = sampleMax / 2;
    timeSlider_->setValue(midTime);
    timeSpin_->setValue(midTime);
    timeSlider_->blockSignals(false);
    timeSpin_->blockSignals(false);

    updateTimeMsLabel(midTime);

    // Initial 3D slices extraction
    requestSliceUpdate(SeismicSliceSlot::Inline, SgySliceType::Inline, midInl);
    requestSliceUpdate(SeismicSliceSlot::Crossline, SgySliceType::Xline, midXl);
    requestSliceUpdate(SeismicSliceSlot::Time, SgySliceType::Time, midTime);
}

std::shared_ptr<SgyVolume> Seismic3DViewPanel::volume() const {
    return viewport_->volume();
}

void Seismic3DViewPanel::setInline(int inlineNo) {
    inlineSlider_->setValue(inlineNo);
}

void Seismic3DViewPanel::setCrossline(int xlineNo) {
    xlineSlider_->setValue(xlineNo);
}

void Seismic3DViewPanel::setTimeSample(int sampleIdx) {
    timeSlider_->setValue(sampleIdx);
}

int Seismic3DViewPanel::currentInline() const {
    return inlineSlider_->value();
}

int Seismic3DViewPanel::currentCrossline() const {
    return xlineSlider_->value();
}

int Seismic3DViewPanel::currentTimeSample() const {
    return timeSlider_->value();
}

void Seismic3DViewPanel::onInlineSliderChanged(int val) {
    requestSliceUpdate(SeismicSliceSlot::Inline, SgySliceType::Inline, val);
    emit inlineChanged(val);
}

void Seismic3DViewPanel::onCrosslineSliderChanged(int val) {
    requestSliceUpdate(SeismicSliceSlot::Crossline, SgySliceType::Xline, val);
    emit crosslineChanged(val);
}

void Seismic3DViewPanel::onTimeSliderChanged(int val) {
    updateTimeMsLabel(val);
    requestSliceUpdate(SeismicSliceSlot::Time, SgySliceType::Time, val);
    emit timeChanged(val);
}

void Seismic3DViewPanel::updateTimeMsLabel(int sampleIdx) {
    auto vol = volume();
    if (!vol || !vol->IsLoaded()) {
        timeMsLabel_->setText(QStringLiteral("%1").arg(sampleIdx));
        return;
    }
    const double ms = sampleIdx * (vol->SampleIntervalUs() / 1000.0);
    timeMsLabel_->setText(QStringLiteral("%1 ms").arg(ms, 0, 'f', 1));
}

void Seismic3DViewPanel::requestSliceUpdate(SeismicSliceSlot slot, SgySliceType type, int index) {
    auto vol = volume();
    if (!vol || !vol->IsLoaded()) {
        return;
    }

    if (taskSvc_) {
        // Track extracting flag per slot to avoid flooding
        if (slot == SeismicSliceSlot::Inline) {
            if (inlineExtracting_) {
                pendingInline_ = index;
                return;
            }
            inlineExtracting_ = true;
        } else if (slot == SeismicSliceSlot::Crossline) {
            if (crosslineExtracting_) {
                pendingCrossline_ = index;
                return;
            }
            crosslineExtracting_ = true;
        } else if (slot == SeismicSliceSlot::Time) {
            if (timeExtracting_) {
                pendingTime_ = index;
                return;
            }
            timeExtracting_ = true;
        }

        taskSvc_->startSliceExtraction(vol, type, index, [this, slot, type, index](bool success, std::shared_ptr<const SgySliceImage> image, const QString &/*error*/) {
            if (slot == SeismicSliceSlot::Inline) {
                inlineExtracting_ = false;
            } else if (slot == SeismicSliceSlot::Crossline) {
                crosslineExtracting_ = false;
            } else if (slot == SeismicSliceSlot::Time) {
                timeExtracting_ = false;
            }

            if (success && image) {
                viewport_->updateSlice(slot, type, index, *image);
            }

            // Drain pending request if user moved slider during extraction
            if (slot == SeismicSliceSlot::Inline && pendingInline_ >= 0) {
                const int next = pendingInline_;
                pendingInline_ = -1;
                requestSliceUpdate(slot, type, next);
            } else if (slot == SeismicSliceSlot::Crossline && pendingCrossline_ >= 0) {
                const int next = pendingCrossline_;
                pendingCrossline_ = -1;
                requestSliceUpdate(slot, type, next);
            } else if (slot == SeismicSliceSlot::Time && pendingTime_ >= 0) {
                const int next = pendingTime_;
                pendingTime_ = -1;
                requestSliceUpdate(slot, type, next);
            }
        });
    } else {
        // Synchronous fallback (e.g. testing)
        SgySliceImage image;
        std::string err;
        if (vol->ExtractSlice(type, index, image, err)) {
            viewport_->updateSlice(slot, type, index, image);
        }
    }
}

} // namespace seismic
