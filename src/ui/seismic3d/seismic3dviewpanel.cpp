// 层：视图
#include "seismic3dviewpanel.h"

#include "../paleotheme.h"

#include <QCheckBox>
#include <QDialog>
#include <QDialogButtonBox>
#include <QFormLayout>
#include <QLineEdit>
#include <QRegularExpression>
#include <QTextEdit>
#include <QComboBox>
#include <QDoubleSpinBox>
#include <QFileDialog>
#include <QHBoxLayout>
#include <QInputDialog>
#include <QVBoxLayout>
#include <QGridLayout>
#include <QMessageBox>
#include <QSettings>
#include <QToolButton>
#include <QSlider>
#include <QSpinBox>
#include <QLabel>
#include <QFrame>
#include <QTimer>

#include <qgsmessagelog.h>

#if defined(Q_OS_UNIX)
#include <unistd.h>
#elif defined(Q_OS_WINDOWS)
#include <windows.h>
#endif

namespace seismic {

namespace {

QToolButton *createToolBtn(const QString &text, const QString &tooltip, bool checkable = false, bool checked = false) {
    auto *btn = new QToolButton();
    btn->setText(text);
    btn->setToolTip(tooltip);
    btn->setCheckable(checkable);
    btn->setChecked(checked);
    // 允许横向压缩：9 个文字按钮的最小宽曾是 dock 宽度下限（688px）的来源；
    // 用户主动收窄时文字裁切、tooltip 仍在，总比调不动强。
    btn->setMinimumWidth(0);
    btn->setSizePolicy(QSizePolicy::Ignored, QSizePolicy::Fixed);
    // chrome 全 token + 活体注册；checked 态用 primary 填充属 DESIGN 允许的
    // 「选中 chip」用途。
    PaleoTheme::applyThemedStyleSheet(btn, [] {
        const auto &t = PaleoTheme::tokens();
        return QStringLiteral(
            "QToolButton {"
            "  background-color: %1;"
            "  color: %2;"
            "  border: 1px solid %3;"
            "  border-radius: 4px;"
            "  padding: 3px 5px;"
            "  font-size: 9pt;"
            "}"
            "QToolButton:hover {"
            "  background-color: %4;"
            "}"
            "QToolButton:checked {"
            "  background-color: %5;"
            "  color: %6;"
            "  border-color: %5;"
            "}"
            "QToolButton:pressed {"
            "  background-color: %7;"
            "  color: %6;"
            "}")
            .arg(t.surface.name().toUpper(), t.text.name().toUpper(),
                 t.border.name().toUpper(), t.surfaceAlt.name().toUpper(),
                 t.primary.name().toUpper(), t.onPrimary.name().toUpper(),
                 t.primaryHover.name().toUpper());
    });
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
    PaleoTheme::applyThemedStyleSheet(topBar, [] {
        const auto &t = PaleoTheme::tokens();
        return QStringLiteral("background-color: %1; border-bottom: 1px solid %2;")
            .arg(t.surfaceAlt.name().toUpper(), t.border.name().toUpper());
    });
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
    PaleoTheme::applyThemedStyleSheet(sep, [] {
        return QStringLiteral("color: %1;")
            .arg(PaleoTheme::tokens().border.name().toUpper());
    });
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

    qualityLabel_ = new QLabel(topBar);
    qualityLabel_->setObjectName(QStringLiteral("seismic3DLodLabel"));
    {
        QFont f = PaleoTheme::monoFont();
        f.setPointSize(PaleoTheme::kLabelPt);
        qualityLabel_->setFont(f);
    }
    PaleoTheme::applyThemedStyleSheet(qualityLabel_, [] {
        return QStringLiteral("color: %1; padding: 0 4px;")
            .arg(PaleoTheme::tokens().textMuted.name().toUpper());
    });
    qualityLabel_->setSizePolicy(QSizePolicy::Ignored, QSizePolicy::Fixed);
    topLay->addWidget(qualityLabel_);

    mainLay->addWidget(topBar);

    // 2. Center Viewport
    viewport_ = new Seismic3DViewportWidget(this);
    viewport_->setObjectName(QStringLiteral("seismic3DViewport"));
    mainLay->addWidget(viewport_, 1);

    // 3. Bottom Sliders Bar
    auto *bottomBar = new QWidget(this);
    PaleoTheme::applyThemedStyleSheet(bottomBar, [] {
        const auto &t = PaleoTheme::tokens();
        return QStringLiteral("background-color: %1; border-top: 1px solid %2;")
            .arg(t.surface.name().toUpper(), t.border.name().toUpper());
    });
    auto *bottomLay = new QGridLayout(bottomBar);
    bottomLay->setContentsMargins(12, 8, 12, 8);
    bottomLay->setHorizontalSpacing(10);
    bottomLay->setVerticalSpacing(6);

    const QFont monoFont = PaleoTheme::monoFont();

    // Inline row
    auto *lblIl = new QLabel(tr("纵测线 (IL):"), bottomBar);
    PaleoTheme::applyThemedStyleSheet(lblIl, [] {
        return QStringLiteral("font-weight: 500; color: %1;")
            .arg(PaleoTheme::tokens().text.name().toUpper());
    });
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
    PaleoTheme::applyThemedStyleSheet(lblXl, [] {
        return QStringLiteral("font-weight: 500; color: %1;")
            .arg(PaleoTheme::tokens().text.name().toUpper());
    });
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
    PaleoTheme::applyThemedStyleSheet(lblTime, [] {
        return QStringLiteral("font-weight: 500; color: %1;")
            .arg(PaleoTheme::tokens().text.name().toUpper());
    });
    timeSlider_ = new QSlider(Qt::Horizontal, bottomBar);
    timeSlider_->setObjectName(QStringLiteral("timeSlider"));
    timeSpin_ = new QSpinBox(bottomBar);
    timeSpin_->setObjectName(QStringLiteral("timeSpin"));
    timeSpin_->setFont(monoFont);
    timeSpin_->setFixedWidth(75);
    timeMsLabel_ = new QLabel(QStringLiteral("0.0 ms"), bottomBar);
    timeMsLabel_->setObjectName(QStringLiteral("timeMsLabel"));
    timeMsLabel_->setFont(monoFont);
    PaleoTheme::applyThemedStyleSheet(timeMsLabel_, [] {
        return QStringLiteral("color: %1;")
            .arg(PaleoTheme::tokens().textMuted.name().toUpper());
    });
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
    connect(inlineSpin_, QOverload<int>::of(&QSpinBox::valueChanged), inlineSlider_, &QSlider::setValue);
    connect(inlineSlider_, &QSlider::valueChanged, this, &Seismic3DViewPanel::onInlineSliderChanged);

    connect(xlineSlider_, &QSlider::valueChanged, xlineSpin_, &QSpinBox::setValue);
    connect(xlineSpin_, QOverload<int>::of(&QSpinBox::valueChanged), xlineSlider_, &QSlider::setValue);
    connect(xlineSlider_, &QSlider::valueChanged, this, &Seismic3DViewPanel::onCrosslineSliderChanged);

    connect(timeSlider_, &QSlider::valueChanged, timeSpin_, &QSpinBox::setValue);
    connect(timeSpin_, QOverload<int>::of(&QSpinBox::valueChanged), timeSlider_, &QSlider::setValue);
    connect(timeSlider_, &QSlider::valueChanged, this, &Seismic3DViewPanel::onTimeSliderChanged);

    // LOD 交互（主线4）：按下即请求粗层，松手静止后升细层
    for (QSlider *slider : {inlineSlider_, xlineSlider_, timeSlider_}) {
        connect(slider, &QSlider::sliderPressed, this, &Seismic3DViewPanel::onSliderPressed);
        connect(slider, &QSlider::sliderReleased, this, &Seismic3DViewPanel::onSliderReleased);
    }
    lodRefineTimer_ = new QTimer(this);
    lodRefineTimer_->setSingleShot(true);
    lodRefineTimer_->setInterval(350);
    connect(lodRefineTimer_, &QTimer::timeout, this, [this]() {
        if (!pagedPath_.isEmpty() && activeLod_ != 0)
            switchLod(0, /*refreshAfter=*/true);
    });

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

    // D3.x 显示控制行（colormap/透明度/值域/体渲染/截图/帧率/相机书签）
    buildDisplayBar();

    // D3.2：切片面拖动 → 滑杆（重提取 + 广播 changed 信号联动 2D）
    connect(viewport_, &Seismic3DViewportWidget::sliceDragged, this,
            [this](SeismicSliceSlot slot, int newIndex) {
        if (slot == SeismicSliceSlot::Inline)
            inlineSlider_->setValue(newIndex);
        else if (slot == SeismicSliceSlot::Crossline)
            xlineSlider_->setValue(newIndex);
        else if (slot == SeismicSliceSlot::Time)
            timeSlider_->setValue(newIndex);
    });

    // D3.9 GL 看门狗：3 秒仍无 GL 上下文 → 2D 拼接回退（不崩不空白）
    glWatchTimer_ = new QTimer(this);
    glWatchTimer_->setSingleShot(true);
    glWatchTimer_->setInterval(3000);
    connect(glWatchTimer_, &QTimer::timeout, this, [this]() {
        if (!viewport_->isGlReady())
            activateFallback();
    });
    glWatchTimer_->start();
}

void Seismic3DViewPanel::setTaskService(SeismicTaskService *taskSvc) {
    taskSvc_ = taskSvc;
}

void Seismic3DViewPanel::setPagedWorkspace(const QString &sf3pPath) {
    pagedPath_ = sf3pPath;
    coarsestLod_ = 0;
    activeLod_ = 0;
    if (pagedPath_.isEmpty()) {
        updateQualityLabel(QString());
        return;
    }
    if (!taskSvc_) {
        return;
    }
    // progressiveLod 打开：起步即最粗层，交互拖动便宜；静止后由精化定时器升 L0
    QPointer<Seismic3DViewPanel> guard(this);
    taskSvc_->startPagedOpen(pagedPath_, [guard](bool ok, const SeismicBackendStatus &status, const QString &) {
        if (!guard)
            return;
        if (!ok) {
            guard->updateQualityLabel(QStringLiteral("?"));
            return;
        }
        guard->coarsestLod_ = status.activeLod;
        guard->activeLod_ = status.activeLod;
        guard->updateQualityLabel(status.quality);
        guard->refreshVisibleSlices();
    });
}

void Seismic3DViewPanel::onSliderPressed() {
    lodRefineTimer_->stop();
    if (!pagedPath_.isEmpty() && coarsestLod_ > 0 && activeLod_ != coarsestLod_)
        switchLod(coarsestLod_, /*refreshAfter=*/false);
    // D3.1 体渲染交互降采样：拖动期间只显示 4 层
    if (stackMode_)
        viewport_->setStackLayerCount(4);
}

void Seismic3DViewPanel::onSliderReleased() {
    if (!pagedPath_.isEmpty())
        lodRefineTimer_->start();
    // D3.1 静止精渲：回到全 16 层
    if (stackMode_)
        viewport_->setStackLayerCount(SeismicSliceRenderer::kMaxStackLayers);
}

void Seismic3DViewPanel::switchLod(int level, bool refreshAfter) {
    if (!taskSvc_ || pagedPath_.isEmpty() || lodSwitchInFlight_)
        return;
    lodSwitchInFlight_ = true;
    QPointer<Seismic3DViewPanel> guard(this);
    taskSvc_->startLodSwitch(pagedPath_, level,
                             [guard, level, refreshAfter](bool ok, const QString &quality, const QString &) {
        if (!guard)
            return;
        guard->lodSwitchInFlight_ = false;
        if (!ok)
            return;
        guard->activeLod_ = level;
        guard->updateQualityLabel(quality);
        if (refreshAfter)
            guard->refreshVisibleSlices();
    });
}

void Seismic3DViewPanel::refreshVisibleSlices() {
    auto vol = volume();
    if (!vol || !vol->IsLoaded())
        return;
    requestSliceUpdate(SeismicSliceSlot::Inline, SgySliceType::Inline, currentInline());
    requestSliceUpdate(SeismicSliceSlot::Crossline, SgySliceType::Xline, currentCrossline());
    requestSliceUpdate(SeismicSliceSlot::Time, SgySliceType::Time, currentTimeSample());
}

void Seismic3DViewPanel::updateQualityLabel(const QString &quality) {
    const QString text = quality.isEmpty() ? QString() : QStringLiteral("LOD %1").arg(quality);
    qualityLabel_->setText(text);
    qualityLabel_->setVisible(!text.isEmpty());
    emit lodChanged(text);
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
    // 数值中点未必是真实测线号（测网步长>1/边缘缺线很常见），吸附到
    // 最近真实线号——否则初始提取静默失败，视口只剩包围盒线框。
    const int midInl = volume->FindNearestInlineValue((inlMin + inlMax) / 2.0);
    inlineSlider_->setValue(midInl);
    inlineSpin_->setValue(midInl);
    inlineSlider_->blockSignals(false);
    inlineSpin_->blockSignals(false);

    xlineSlider_->blockSignals(true);
    xlineSpin_->blockSignals(true);
    xlineSlider_->setRange(xlMin, xlMax);
    xlineSpin_->setRange(xlMin, xlMax);
    const int midXl = volume->FindNearestXlineValue((xlMin + xlMax) / 2.0);
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

    checkMemoryBudget(); // D3.8 体量 > RAM/2 → 提示分页通道

    // 体积变化时重发体渲染堆叠层
    if (stackMode_)
        requestStackLayers();
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
    if (auto vol = volume(); vol && vol->IsLoaded()) {
        const int snapped = vol->FindNearestInlineValue(val);
        if (snapped != val) {
            // 回填真实线号（重发 valueChanged → spin 同步 + 本函数以吸附值再入）
            inlineSlider_->setValue(snapped);
            return;
        }
    }
    requestSliceUpdate(SeismicSliceSlot::Inline, SgySliceType::Inline, val);
    emit inlineChanged(val);
}

void Seismic3DViewPanel::onCrosslineSliderChanged(int val) {
    if (auto vol = volume(); vol && vol->IsLoaded()) {
        const int snapped = vol->FindNearestXlineValue(val);
        if (snapped != val) {
            xlineSlider_->setValue(snapped);
            return;
        }
    }
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

        // 回调经服务的任务终态发射；面板可能已先析构（测试 teardown / 关页），
        // QPointer 守卫避免对已亡视口贴图。
        QPointer<Seismic3DViewPanel> guard(this);
        taskSvc_->startSliceExtraction(vol, type, index, [guard, slot, type, index](bool success, std::shared_ptr<const SgySliceImage> image, const QString &error) {
            if (!guard)
                return;
            if (slot == SeismicSliceSlot::Inline) {
                guard->inlineExtracting_ = false;
            } else if (slot == SeismicSliceSlot::Crossline) {
                guard->crosslineExtracting_ = false;
            } else if (slot == SeismicSliceSlot::Time) {
                guard->timeExtracting_ = false;
            }

            if (success && image) {
                // D3.5/D3.3：缓存 values 保真副本（重着色/值域裁剪的原料）
                const std::size_t si = static_cast<std::size_t>(slot) % 3;
                guard->cachedSlices_[si] = *image;
                guard->cachedIndex_[si] = index;
                guard->cachedReady_[si] = true;
                if (guard->customCmapActive_)
                    guard->recolorizeSlice(slot);
                else
                    guard->viewport_->updateSlice(slot, type, index, *image);
                if (guard->fallbackActive_ && guard->fallback_) {
                    const QImage qimg(reinterpret_cast<const uchar *>(image->rgba.data()),
                                      image->width, image->height,
                                      image->width * 4, QImage::Format_RGBA8888);
                    const int fs = slot == SeismicSliceSlot::Crossline ? 1 : (slot == SeismicSliceSlot::Time ? 2 : 0);
                    guard->fallback_->setSlice(fs, qimg.copy(),
                                               QStringLiteral("%1 %2").arg(fs == 0 ? "IL" : (fs == 1 ? "XL" : "T")).arg(index));
                }
            } else {
                // 提取失败必须留痕——静默失败的表现是"只剩包围盒线框"。
                QgsMessageLog::logMessage(
                    tr("地震切片提取失败（槽位 %1，索引 %2）：%3")
                        .arg(static_cast<int>(slot)).arg(index).arg(error),
                    QStringLiteral("Seismic3D"), Qgis::MessageLevel::Warning);
            }
            // Drain pending request if user moved slider during extraction
            if (slot == SeismicSliceSlot::Inline && guard->pendingInline_ >= 0) {
                const int next = guard->pendingInline_;
                guard->pendingInline_ = -1;
                guard->requestSliceUpdate(slot, type, next);
            } else if (slot == SeismicSliceSlot::Crossline && guard->pendingCrossline_ >= 0) {
                const int next = guard->pendingCrossline_;
                guard->pendingCrossline_ = -1;
                guard->requestSliceUpdate(slot, type, next);
            } else if (slot == SeismicSliceSlot::Time && guard->pendingTime_ >= 0) {
                const int next = guard->pendingTime_;
                guard->pendingTime_ = -1;
                guard->requestSliceUpdate(slot, type, next);
            }
        }, pagedPath_);
    } else {
        // Synchronous fallback (e.g. testing)
        SgySliceImage image;
        std::string err;
        if (vol->ExtractSlice(type, index, image, err)) {
            const std::size_t si = static_cast<std::size_t>(slot) % 3;
            cachedSlices_[si] = image;
            cachedIndex_[si] = index;
            cachedReady_[si] = true;
            if (customCmapActive_)
                recolorizeSlice(slot);
            else
                viewport_->updateSlice(slot, type, index, image);
            if (fallbackActive_ && fallback_) {
                const QImage qimg(reinterpret_cast<const uchar *>(image.rgba.data()),
                                  image.width, image.height,
                                  image.width * 4, QImage::Format_RGBA8888);
                const int fs = slot == SeismicSliceSlot::Crossline ? 1 : (slot == SeismicSliceSlot::Time ? 2 : 0);
                fallback_->setSlice(fs, qimg.copy(),
                                    QStringLiteral("%1 %2").arg(fs == 0 ? "IL" : (fs == 1 ? "XL" : "T")).arg(index));
            }
        }
    }
}


// ---- D3.x 显示控制行 ----
void Seismic3DViewPanel::buildDisplayBar() {
    auto *bar = new QWidget(this);
    bar->setStyleSheet(QStringLiteral("background-color: #FFFFFF; border-bottom: 1px solid #DFE5EC;"));
    auto *lay = new QHBoxLayout(bar);
    lay->setContentsMargins(6, 2, 6, 2);
    lay->setSpacing(4);

    const QString lblStyle = QStringLiteral("color: #5D6E80; font-size: 8.5pt;");

    // D3.5 colormap：默认（引擎预烘焙）+ 8 预设 + 自定义
    auto *lblCmap = new QLabel(tr("色标:"), bar);
    lblCmap->setStyleSheet(lblStyle);
    lay->addWidget(lblCmap);
    cboColorMap_ = new QComboBox(bar);
    cboColorMap_->setObjectName(QStringLiteral("cbo3dColorMap"));
    cboColorMap_->addItem(tr("默认（引擎）"));
    cboColorMap_->addItems(Seismic3DColorMap::presetNames());
    cboColorMap_->addItem(tr("自定义…"));
    lay->addWidget(cboColorMap_);
    btnCmapEdit_ = createToolBtn(tr("编辑…"), tr("自定义 colormap 控制点（位置→颜色）"));
    lay->addWidget(btnCmapEdit_);

    // D3.3 透明度 + 值域
    auto *lblAlpha = new QLabel(tr("透明度:"), bar);
    lblAlpha->setStyleSheet(lblStyle);
    lay->addWidget(lblAlpha);
    sliderAlpha_ = new QSlider(Qt::Horizontal, bar);
    sliderAlpha_->setRange(10, 100);
    sliderAlpha_->setValue(100);
    sliderAlpha_->setFixedWidth(70);
    lay->addWidget(sliderAlpha_);
    auto *lblRange = new QLabel(tr("值域:"), bar);
    lblRange->setStyleSheet(lblStyle);
    lay->addWidget(lblRange);
    spinRangeMin_ = new QDoubleSpinBox(bar);
    spinRangeMin_->setRange(0.0, 0.9);
    spinRangeMin_->setSingleStep(0.05);
    spinRangeMin_->setValue(0.0);
    spinRangeMin_->setFixedWidth(48);
    spinRangeMax_ = new QDoubleSpinBox(bar);
    spinRangeMax_->setRange(0.1, 1.0);
    spinRangeMax_->setSingleStep(0.05);
    spinRangeMax_->setValue(1.0);
    spinRangeMax_->setFixedWidth(48);
    lay->addWidget(spinRangeMin_);
    lay->addWidget(new QLabel(QStringLiteral("–"), bar));
    lay->addWidget(spinRangeMax_);

    // D3.1 体渲染（切片堆叠）
    btnStack_ = createToolBtn(tr("体渲染"), tr("切片堆叠体渲染：16 层水平切片半透明叠渲；\n拖动滑杆期间自动降到 4 层（交互降采样），静止后精渲"), true);
    lay->addWidget(btnStack_);

    // D3.10 帧率（debug）
    chkFps_ = new QCheckBox(tr("fps"), bar);
    chkFps_->setStyleSheet(lblStyle);
    chkFps_->setToolTip(tr("显示帧率读数（调试）"));
    lay->addWidget(chkFps_);

    lay->addStretch();

    // D3.6 相机书签
    auto *lblCam = new QLabel(tr("视角:"), bar);
    lblCam->setStyleSheet(lblStyle);
    lay->addWidget(lblCam);
    cboCamBookmark_ = new QComboBox(bar);
    cboCamBookmark_->setFixedWidth(90);
    lay->addWidget(cboCamBookmark_);
    btnCamSave_ = createToolBtn(tr("存"), tr("保存当前视角为书签"));
    btnCamDel_ = createToolBtn(tr("删"), tr("删除选中的视角书签"));
    lay->addWidget(btnCamSave_);
    lay->addWidget(btnCamDel_);

    // D3.7 截图
    btnShot_ = createToolBtn(tr("截图"), tr("导出当前三维视口为 PNG"));
    lay->addWidget(btnShot_);

    if (auto *mainLay = qobject_cast<QVBoxLayout *>(layout()))
        mainLay->insertWidget(1, bar); // 顶栏与视口之间

    // ---- 接线 ----
    connect(cboColorMap_, QOverload<int>::of(&QComboBox::currentIndexChanged), this, [this](int idx) {
        if (idx <= 0) {
            customCmapActive_ = false;
            // 默认：恢复引擎预烘焙 rgba（缓存里保留的原始副本重传）
            for (int slot = 0; slot < 3; ++slot)
                if (cachedReady_[static_cast<std::size_t>(slot)])
                    recolorizeSlice(static_cast<SeismicSliceSlot>(slot));
            return;
        }
        if (idx >= cboColorMap_->count() - 1)
            return; // "自定义…"：由编辑器设置
        cmap_ = Seismic3DColorMap::preset(cboColorMap_->itemText(idx));
        customCmapActive_ = true;
        for (int slot = 0; slot < 3; ++slot)
            if (cachedReady_[static_cast<std::size_t>(slot)])
                recolorizeSlice(static_cast<SeismicSliceSlot>(slot));
    });
    connect(btnCmapEdit_, &QToolButton::clicked, this, [this]() {
        // 控制点编辑：每行 "位置 颜色(#RRGGBB)"，0=负峰 1=正峰
        QDialog dlg(this);
        dlg.setWindowTitle(tr("自定义 colormap 控制点（位置 0..1，颜色 #RRGGBB）"));
        auto *form = new QFormLayout(&dlg);
        auto *edit = new QTextEdit(&dlg);
        edit->setFont(QFont(QStringLiteral("JetBrains Mono"), 9));
        QStringList lines;
        for (const auto &stop : cmap_.stops())
            lines << QStringLiteral("%1 %2").arg(stop.pos, 0, 'f', 2)
                         .arg(QColor(stop.color).name());
        if (lines.isEmpty())
            lines << QStringLiteral("0.00 #1976d2\n0.50 #ffffff\n1.00 #dc2626");
        edit->setPlainText(lines.join(QLatin1Char('\n')));
        form->addRow(edit);
        auto *btnBox = new QDialogButtonBox(QDialogButtonBox::Ok | QDialogButtonBox::Cancel, &dlg);
        connect(btnBox, &QDialogButtonBox::accepted, &dlg, &QDialog::accept);
        connect(btnBox, &QDialogButtonBox::rejected, &dlg, &QDialog::reject);
        form->addRow(btnBox);
        if (dlg.exec() != QDialog::Accepted)
            return;
        QVector<Seismic3DColorStop> stops;
        const auto rows = edit->toPlainText().split(QLatin1Char('\n'), Qt::SkipEmptyParts);
        for (const QString &row : rows) {
            const auto parts = row.simplified().split(QRegularExpression(QStringLiteral("[ ,\\t]+")), Qt::SkipEmptyParts);
            if (parts.size() != 2)
                continue;
            bool okP = false;
            const double pos = parts[0].toDouble(&okP);
            const QColor c(parts[1]);
            if (okP && c.isValid() && pos >= 0.0 && pos <= 1.0)
                stops.push_back({float(pos), c.rgba()});
        }
        if (stops.size() >= 2) {
            cmap_.setName(tr("自定义"));
            cmap_.setStops(stops);
            customCmapActive_ = true;
            cboColorMap_->setCurrentIndex(cboColorMap_->count() - 1);
            for (int slot = 0; slot < 3; ++slot)
                if (cachedReady_[static_cast<std::size_t>(slot)])
                    recolorizeSlice(static_cast<SeismicSliceSlot>(slot));
        }
    });
    connect(sliderAlpha_, &QSlider::valueChanged, this, [this](int v) {
        setSliceAlpha(v / 100.0f);
    });
    const auto rangeChanged = [this]() {
        setValueRange(float(spinRangeMin_->value()), float(spinRangeMax_->value()));
        if (!customCmapActive_) {
            customCmapActive_ = true; // 值域裁剪需要重着色路径
            if (cmap_.stops().isEmpty())
                cmap_ = Seismic3DColorMap::preset(QStringLiteral("红白蓝"));
        }
        for (int slot = 0; slot < 3; ++slot)
            if (cachedReady_[static_cast<std::size_t>(slot)])
                recolorizeSlice(static_cast<SeismicSliceSlot>(slot));
    };
    connect(spinRangeMin_, QOverload<double>::of(&QDoubleSpinBox::valueChanged), this,
            [rangeChanged](double) { rangeChanged(); });
    connect(spinRangeMax_, QOverload<double>::of(&QDoubleSpinBox::valueChanged), this,
            [rangeChanged](double) { rangeChanged(); });

    connect(btnStack_, &QToolButton::toggled, this, [this](bool on) {
        setStackModeEnabled(on);
    });
    connect(chkFps_, &QCheckBox::toggled, viewport_, &Seismic3DViewportWidget::setFpsVisible);

    // D3.7 截图
    connect(btnShot_, &QToolButton::clicked, this, [this]() {
        if (fallbackActive_) {
            QMessageBox::information(this, tr("三维截图"), tr("当前处于 2D 回退模式，请使用右键另存。"));
            return;
        }
        const QString path = QFileDialog::getSaveFileName(
            this, tr("导出三维视口截图"), QStringLiteral("seismic3d.png"),
            tr("PNG 图像 (*.png)"));
        if (path.isEmpty())
            return;
        const QImage img = viewport_->grabViewportImage();
        if (img.isNull()) {
            QMessageBox::warning(this, tr("截图失败"), tr("OpenGL 帧缓冲不可用。"));
            return;
        }
        if (img.save(path, "PNG"))
            QMessageBox::information(this, tr("已导出"), tr("三维视口已保存到:\n%1").arg(path));
        else
            QMessageBox::critical(this, tr("导出失败"), tr("保存 PNG 失败。"));
    });

    // D3.6 相机书签
    connect(btnCamSave_, &QToolButton::clicked, this, [this]() {
        bool ok = false;
        const QString name = QInputDialog::getText(this, tr("保存视角书签"),
                                                   tr("视角名："), QLineEdit::Normal,
                                                   tr("视角 %1").arg(camBookmarks_.size() + 1), &ok);
        if (!ok || name.trimmed().isEmpty())
            return;
        saveCameraBookmark(name.trimmed());
    });
    connect(btnCamDel_, &QToolButton::clicked, this, [this]() {
        const int idx = cboCamBookmark_->currentIndex();
        if (idx < 0 || idx >= camBookmarks_.size())
            return;
        camBookmarks_.removeAt(idx);
        cboCamBookmark_->removeItem(idx);
        // 持久化重写
        QSettings settings;
        QStringList entries;
        for (const auto &bm : camBookmarks_)
            entries << QStringLiteral("%1|%2,%3,%4,%5,%6")
                           .arg(bm.name)
                           .arg(bm.state.yaw).arg(bm.state.pitch)
                           .arg(bm.state.distance)
                           .arg(bm.state.target.x).arg(bm.state.target.y);
        settings.setValue(QStringLiteral("seismic/3dCamBookmarks"), entries);
    });
    connect(cboCamBookmark_, QOverload<int>::of(&QComboBox::activated), this, [this](int idx) {
        applyCameraBookmark(idx);
    });
    // 恢复持久化的相机书签
    QSettings settings;
    const QStringList entries = settings.value(QStringLiteral("seismic/3dCamBookmarks")).toStringList();
    for (const QString &e : entries) {
        const auto kv = e.split(QLatin1Char('|'));
        if (kv.size() != 2)
            continue;
        const auto nums = kv[1].split(QLatin1Char(','));
        if (nums.size() < 6)
            continue;
        CamBookmark bm;
        bm.name = kv[0];
        bm.state.yaw = nums[0].toFloat();
        bm.state.pitch = nums[1].toFloat();
        bm.state.distance = nums[2].toFloat();
        bm.state.target = glm::vec3(nums[3].toFloat(), nums[4].toFloat(), nums[5].toFloat());
        camBookmarks_.append(bm);
        cboCamBookmark_->addItem(bm.name);
    }
}

// ---- D3.5/D3.3：重着色 + 值域裁剪（alpha 带外置 0）+ 上传 ----
void Seismic3DViewPanel::recolorizeSlice(SeismicSliceSlot slot) {
    const std::size_t si = static_cast<std::size_t>(slot) % 3;
    if (!cachedReady_[si] || !viewport_)
        return;
    SgySliceImage img = cachedSlices_[si]; // values 保真副本
    if (customCmapActive_) {
        cmap_.colorizeSlice(img, 1.0f, 1.45f);
        // D3.3 值域裁剪：|v|/absMax 带外 → alpha 0（着色器 discard）
        if (rangeMinFrac_ > 0.0f || rangeMaxFrac_ < 1.0f) {
            const float absMax = std::max(std::abs(img.valueMin), std::abs(img.valueMax));
            if (absMax > 1e-8f) {
                for (std::size_t i = 0; i < img.values.size(); ++i) {
                    const float v = img.values[i];
                    const float a = std::abs(v) / absMax;
                    if (!std::isfinite(v) || a < rangeMinFrac_ || a > rangeMaxFrac_)
                        img.rgba[i * 4 + 3] = 0; // 带外透明（着色器 discard）
                }
            }
        }
    }
    const SgySliceType type = si == 0 ? SgySliceType::Inline
                          : (si == 1 ? SgySliceType::Xline : SgySliceType::Time);
    viewport_->updateSlice(slot, type, cachedIndex_[si], img);
    if (fallbackActive_ && fallback_) {
        const QImage qimg(reinterpret_cast<const uchar *>(img.rgba.data()),
                          img.width, img.height, img.width * 4, QImage::Format_RGBA8888);
        const int fs = slot == SeismicSliceSlot::Crossline ? 1 : (slot == SeismicSliceSlot::Time ? 2 : 0);
        fallback_->setSlice(fs, qimg.copy(),
                            QStringLiteral("%1 %2").arg(fs == 0 ? "IL" : (fs == 1 ? "XL" : "T")).arg(cachedIndex_[si]));
    }
}

void Seismic3DViewPanel::setColorMap(const Seismic3DColorMap &cmap) {
    cmap_ = cmap;
    customCmapActive_ = true;
    for (int slot = 0; slot < 3; ++slot)
        if (cachedReady_[static_cast<std::size_t>(slot)])
            recolorizeSlice(static_cast<SeismicSliceSlot>(slot));
}

void Seismic3DViewPanel::setSliceAlpha(float alpha) {
    alpha_ = std::clamp(alpha, 0.05f, 1.0f);
    viewport_->setSliceAlpha(alpha_);
}

void Seismic3DViewPanel::setValueRange(float minFrac, float maxFrac) {
    rangeMinFrac_ = std::clamp(std::min(minFrac, maxFrac), 0.0f, 1.0f);
    rangeMaxFrac_ = std::clamp(std::max(minFrac, maxFrac), 0.0f, 1.0f);
}

// ---- D3.4 / D3.12 ----
void Seismic3DViewPanel::setWells(const std::vector<Seismic3DWell> &wells) {
    viewport_->setWells(wells);
}

void Seismic3DViewPanel::setSecondaryVolume(std::shared_ptr<const SgyVolume> secondary) {
    viewport_->setSecondaryVolume(std::move(secondary));
}

bool Seismic3DViewPanel::hasSecondaryVolume() const {
    return viewport_->hasSecondaryVolume();
}

// ---- D3.1 体渲染（切片堆叠）----
void Seismic3DViewPanel::setStackModeEnabled(bool enabled) {
    stackMode_ = enabled;
    viewport_->setStackVisible(enabled);
    if (enabled)
        requestStackLayers();
}

void Seismic3DViewPanel::requestStackLayers() {
    if (!stackMode_ || stackExtracting_)
        return;
    auto vol = volume();
    if (!vol || !vol->IsLoaded())
        return;
    const int sampleMax = vol->SampleMax();
    if (sampleMax <= 0)
        return;
    constexpr int kLayers = SeismicSliceRenderer::kMaxStackLayers;
    stackExtracting_ = true;
    stackTargetLayers_ = kLayers;
    int dispatched = 0;
    for (int layer = 0; layer < kLayers; ++layer) {
        const int sample = static_cast<int>((layer + 0.5) / kLayers * (sampleMax + 1));
        if (sample > sampleMax)
            continue;
        const int layerIdx = layer;
        QPointer<Seismic3DViewPanel> guard(this);
        auto dispatch = [this, guard, vol, sample, layerIdx]() {
            if (taskSvc_) {
                taskSvc_->startSliceExtraction(
                    vol, SgySliceType::Time, sample,
                    [guard, layerIdx, sample](bool ok, std::shared_ptr<const SgySliceImage> img, const QString &) {
                        if (!guard || !ok || !img)
                            return;
                        guard->viewport_->updateStackLayer(layerIdx, sample, *img);
                    }, pagedPath_);
            } else {
                SgySliceImage img;
                std::string err;
                if (vol->ExtractSlice(SgySliceType::Time, sample, img, err))
                    guard->viewport_->updateStackLayer(layerIdx, sample, img);
            }
        };
        dispatch();
        ++dispatched;
    }
    stackExtracting_ = false;
    (void)dispatched;
}

// ---- D3.6 相机书签 ----
void Seismic3DViewPanel::saveCameraBookmark(const QString &name) {
    CamBookmark bm;
    bm.name = name;
    bm.state = viewport_->camera().state();
    camBookmarks_.append(bm);
    cboCamBookmark_->addItem(name);
    // QSettings 持久化（全局列表——相机视角跨工区可复用）
    QSettings settings;
    QStringList entries;
    for (const auto &b : camBookmarks_)
        entries << QStringLiteral("%1|%2,%3,%4,%5,%6")
                       .arg(b.name)
                       .arg(b.state.yaw).arg(b.state.pitch)
                       .arg(b.state.distance)
                       .arg(b.state.target.x).arg(b.state.target.y);
    settings.setValue(QStringLiteral("seismic/3dCamBookmarks"), entries);
}

void Seismic3DViewPanel::applyCameraBookmark(int index) {
    if (index < 0 || index >= camBookmarks_.size())
        return;
    viewport_->camera().setState(camBookmarks_[index].state);
    viewport_->update();
}

QStringList Seismic3DViewPanel::cameraBookmarkNames() const {
    QStringList names;
    for (const auto &b : camBookmarks_)
        names << b.name;
    return names;
}

// ---- D3.9 回退 ----
void Seismic3DViewPanel::activateFallback() {
    if (fallbackActive_)
        return;
    fallbackActive_ = true;
    fallback_ = new Seismic3DFallbackWidget(this);
    if (auto *mainLay = qobject_cast<QVBoxLayout *>(layout())) {
        const int idx = mainLay->indexOf(viewport_);
        viewport_->hide();
        mainLay->insertWidget(idx < 0 ? mainLay->count() : idx, fallback_, 1);
    }
    // 已缓存的切片喂给回退件
    for (int slot = 0; slot < 3; ++slot)
        if (cachedReady_[static_cast<std::size_t>(slot)])
            recolorizeSlice(static_cast<SeismicSliceSlot>(slot));
    QgsMessageLog::logMessage(tr("三维视口 GL 不可用，已切换 2D 切片拼接回退视图"),
                              QStringLiteral("Seismic3D"), Qgis::MessageLevel::Warning);
}

// ---- D3.8 内存预算 ----
void Seismic3DViewPanel::checkMemoryBudget() {
    auto vol = volume();
    if (!vol || !vol->IsLoaded())
        return;
    const qint64 volumeBytes = qint64(vol->InlineMax() - vol->InlineMin() + 1) *
                               (vol->XlineMax() - vol->XlineMin() + 1) *
                               (vol->SampleMax() + 1) * 4;
    qint64 totalRam = 0;
#if defined(Q_OS_UNIX)
    const long pages = sysconf(_SC_PHYS_PAGES);
    const long pageSize = sysconf(_SC_PAGESIZE);
    if (pages > 0 && pageSize > 0)
        totalRam = qint64(pages) * pageSize;
#elif defined(Q_OS_WINDOWS)
    MEMORYSTATUSEX status{};
    status.dwLength = sizeof(status);
    if (GlobalMemoryStatusEx(&status))
        totalRam = qint64(status.ullTotalPhys);
#endif
    if (totalRam <= 0 || volumeBytes * 2 < totalRam)
        return; // 体 < RAM/2：无需提示
    const double gb = 1024.0 * 1024.0 * 1024.0;
    const QString hint = pagedPath_.isEmpty()
        ? tr("体 %1 GB 超过内存预算（RAM/2 ≈ %2 GB）——建议「转码分页工作区 (.sf3p)」后启用分页通道")
              .arg(volumeBytes / gb, 0, 'f', 1).arg(totalRam / 2 / gb, 0, 'f', 1)
        : tr("体 %1 GB 超过内存预算——已启用 .sf3p 分页通道（按页取数，常驻内存受控）")
              .arg(volumeBytes / gb, 0, 'f', 1);
    if (!memoryHintLabel_) {
        memoryHintLabel_ = new QLabel(this);
        memoryHintLabel_->setStyleSheet(
            QStringLiteral("color: #F29900; padding: 0 6px; font-size: 8.5pt;"));
        memoryHintLabel_->setWordWrap(true);
        if (auto *mainLay = qobject_cast<QVBoxLayout *>(layout()))
            mainLay->addWidget(memoryHintLabel_);
    }
    memoryHintLabel_->setText(hint);
    memoryHintLabel_->setVisible(true);
    QgsMessageLog::logMessage(hint, QStringLiteral("Seismic3D"), Qgis::MessageLevel::Warning);
}
} // namespace seismic
