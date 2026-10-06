// 层：视图
// token 例外：DESIGN 数据符号例外：井轨迹配色与色带编辑示例字符串，保存为数据样式。（tools/ui-token-exceptions.json 精确计数）。
#include "seismic3dviewpanel.h"

#include "../paleotheme.h"
#include "domain/seismic/sgysectionbuilder.h"
#include "seismic3dtfeditor.h"
#include "services/paleotaskservice.h" // PaleoTask 完整类型（QPointer 在途句柄）

#include <QApplication>
#include <QCheckBox>
#include <QCoreApplication>
#include <QDialog>
#include <QMenu>
#include <QDialogButtonBox>
#include <QFormLayout>
#include <QLineEdit>
#include <QRegularExpression>
#include <QTextEdit>
#include <QComboBox>
#include <QDoubleSpinBox>
#include <QFileDialog>
#include <QDir>
#include <QHBoxLayout>
#include <QInputDialog>
#include <QVBoxLayout>
#include <QGridLayout>
#include "../notifications/paleonotify.h"
#include <QSettings>
#include <QToolButton>
#include <QSlider>
#include <QSpinBox>
#include <QLabel>
#include <QFrame>
#include <QShowEvent>
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
    // Ignored 保留窄 dock 的可压缩性；分配剩余宽度，避免文字按钮归零。
    QSizePolicy sizePolicy(QSizePolicy::Ignored, QSizePolicy::Fixed);
    sizePolicy.setHorizontalStretch(1);
    btn->setSizePolicy(sizePolicy);
    PaleoTheme::applyThemedStyleSheet(btn, [] { return PaleoTheme::toolButtonStyleSheet(); });
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
    topLay->setContentsMargins(PaleoTheme::tokens().spacingSm, PaleoTheme::tokens().spacingXs, PaleoTheme::tokens().spacingSm, PaleoTheme::tokens().spacingXs);
    topLay->setSpacing(PaleoTheme::tokens().spacingXs);

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
        f.setPointSize(PaleoTheme::tokens().labelPt);
        qualityLabel_->setFont(f);
    }
    PaleoTheme::applyThemedStyleSheet(qualityLabel_, [] {
        return PaleoTheme::metricStyleSheet(QStringLiteral("color: %1; padding: 0 {spacing.xs}px;"))
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
    bottomLay->setContentsMargins(PaleoTheme::tokens().spacingMd, PaleoTheme::tokens().spacingSm, PaleoTheme::tokens().spacingMd, PaleoTheme::tokens().spacingSm);
    bottomLay->setHorizontalSpacing(PaleoTheme::tokens().spacingSm);
    bottomLay->setVerticalSpacing(PaleoTheme::tokens().spacingSm);

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
        // D7.1：GL 前设置的 TF 在 GL 就绪后补传 LUT（值纹理随切片重喂）
        if (tfActive_)
            viewport_->setTransferFunction(tf_.buildLutRgba(), true);
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

    // D3.9 GL 看门狗：显示后 3 秒仍无 GL 上下文 → 2D 拼接回退（不崩不空白）。
    // 计时在首个 showEvent 才武装——3D dock 构造时是隐藏的，QOpenGLWidget
    // 要等真正可见才会建上下文，提前计时必误判成 GL 失败。
    glWatchTimer_ = new QTimer(this);
    glWatchTimer_->setSingleShot(true);
    glWatchTimer_->setInterval(3000);
    connect(glWatchTimer_, &QTimer::timeout, this, [this]() {
        if (viewport_->isGlReady())
            return;
        if (!viewport_->isVisible())
            return; // 又藏起来了——下次 showEvent 再武装
        activateFallback();
    });
}

void Seismic3DViewPanel::showEvent(QShowEvent *event) {
    QWidget::showEvent(event);
    // D3.9：看门狗只在真显示后计时（dock 初始隐藏，GL 要等首个 show 才建）。
    if (!fallbackActive_ && !viewport_->isGlReady() && glWatchTimer_ &&
        !glWatchTimer_->isActive())
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
        // A2（wave/deepen-perf）：粗层首见后静止自动精化——此前要等用户拖一次
        // 滑杆才升 L0，默认视图永远停在 progressive 打开的最粗层。
        if (guard->activeLod_ != 0)
            guard->lodRefineTimer_->start();
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
            guard->refreshVisibleSlices(/*onlyStale=*/true); // A2：只补粗层槽位
    });
}

void Seismic3DViewPanel::refreshVisibleSlices(bool onlyStale) {
    auto vol = volume();
    if (!vol || !vol->IsLoaded())
        return;
    const struct { SeismicSliceSlot slot; SgySliceType type; int index; } wanted[3] = {
        {SeismicSliceSlot::Inline, SgySliceType::Inline, currentInline()},
        {SeismicSliceSlot::Crossline, SgySliceType::Xline, currentCrossline()},
        {SeismicSliceSlot::Time, SgySliceType::Time, currentTimeSample()},
    };
    for (int i = 0; i < 3; ++i) {
        // A2（wave/deepen-perf）：内容已是当前 LOD 层级（slotCoarse_=false 即
        // L0 产物）且索引未变的槽位跳过——拖动只动一个滑杆时，另外两个槽位
        // 的整组重取是纯冗余（改前每手势固定 3 请求，改后 1 请求）。
        if (onlyStale && !slotCoarse_[std::size_t(i)] && cachedReady_[std::size_t(i)] &&
            cachedIndex_[std::size_t(i)] == wanted[i].index)
            continue;
        requestSliceUpdate(wanted[i].slot, wanted[i].type, wanted[i].index);
    }
}

void Seismic3DViewPanel::updateQualityLabel(const QString &quality) {
    const QString text = quality.isEmpty() ? QString() : QStringLiteral("LOD %1").arg(quality);
    qualityLabel_->setText(text);
    qualityLabel_->setVisible(!text.isEmpty());
    emit lodChanged(text);
}

void Seismic3DViewPanel::setVolume(std::shared_ptr<SgyVolume> volume) {
    // #158：换体/清体先中止旧体在途的切片读取（结果按槽位句柄丢弃）。
    for (std::size_t si = 0; si < slotTasks_.size(); ++si)
        supersedeInFlightSlice(si);
    viewport_->setVolume(volume);

    const bool loaded = volume && volume->IsLoaded();
    // 无体（如切到无地震的工程）时切片控件禁用，不再对旧 SEG-Y 发起读取。
    for (QWidget *w : {static_cast<QWidget *>(inlineSlider_), static_cast<QWidget *>(inlineSpin_),
                       static_cast<QWidget *>(xlineSlider_), static_cast<QWidget *>(xlineSpin_),
                       static_cast<QWidget *>(timeSlider_), static_cast<QWidget *>(timeSpin_)})
        if (w)
            w->setEnabled(loaded);
    if (!loaded) {
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
        const std::size_t si = static_cast<std::size_t>(slot) % 3;
        // Track extracting flag per slot to avoid flooding
        if (slot == SeismicSliceSlot::Inline) {
            if (inlineExtracting_) {
                pendingInline_ = index;
                supersedeInFlightSlice(si); // A2：被顶替的读协作中止，不跑完全程
                return;
            }
            inlineExtracting_ = true;
        } else if (slot == SeismicSliceSlot::Crossline) {
            if (crosslineExtracting_) {
                pendingCrossline_ = index;
                supersedeInFlightSlice(si);
                return;
            }
            crosslineExtracting_ = true;
        } else if (slot == SeismicSliceSlot::Time) {
            if (timeExtracting_) {
                pendingTime_ = index;
                supersedeInFlightSlice(si);
                return;
            }
            timeExtracting_ = true;
        }

        // 回调经服务的任务终态发射；面板可能已先析构（测试 teardown / 关页），
        // QPointer 守卫避免对已亡视口贴图。
        QPointer<Seismic3DViewPanel> guard(this);
        const int lodAtDispatch = activeLod_; // A2：判定内容粗细以派发时层级为准
        PaleoTask *task = taskSvc_->startSliceExtraction(
            vol, type, index,
            [guard, slot, si, type, index, lodAtDispatch](bool success, std::shared_ptr<const SgySliceImage> image, const QString &error) {
            if (!guard)
                return;
            if (slot == SeismicSliceSlot::Inline) {
                guard->inlineExtracting_ = false;
            } else if (slot == SeismicSliceSlot::Crossline) {
                guard->crosslineExtracting_ = false;
            } else if (slot == SeismicSliceSlot::Time) {
                guard->timeExtracting_ = false;
            }
            const bool superseded = guard->slotSuperseded_[si];
            guard->slotSuperseded_[si] = false;
            guard->slotTasks_[si] = nullptr;

            if (success && image) {
                // D3.5/D3.3：缓存 values 保真副本（重着色/值域裁剪的原料）
                guard->cachedSlices_[si] = *image;
                guard->cachedIndex_[si] = index;
                guard->cachedReady_[si] = true;
                guard->slotCoarse_[si] = lodAtDispatch != 0; // A2：粗层产物标记（精化需重取）
                if (guard->tfActive_) {
                    // D7.1：TF 模式直传 values（渲染器 LUT 取色），CPU 零重烘焙
                    guard->viewport_->updateSlice(slot, type, index, *image);
                } else if (guard->customCmapActive_)
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
                // A2 例外：本端主动顶替触发的取消不是失败（新请求已在路上）。
                // goal/ui-experience-polish：留痕升级——MessageLog 之外面板内
                // warning 行可见（用户不打开日志窗口也能看到失败原因）。
                if (!superseded) {
                    const QString why = tr("地震切片提取失败（槽位 %1，索引 %2）：%3")
                                            .arg(static_cast<int>(slot))
                                            .arg(index)
                                            .arg(error);
                    QgsMessageLog::logMessage(
                        why, QStringLiteral("Seismic3D"), Qgis::MessageLevel::Warning);
                    if (guard)
                        QMetaObject::invokeMethod(
                            guard.data(), [guard, why] { guard->showInlineWarning(why); },
                            Qt::QueuedConnection);
                }
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
        slotTasks_[si] = task; // A2：在途句柄（缓存命中同步回调时 task 已终态，QPointer 自清）
    } else {
        // Synchronous fallback (e.g. testing)
        SgySliceImage image;
        std::string err;
        if (vol->ExtractSlice(type, index, image, err)) {
            const std::size_t si = static_cast<std::size_t>(slot) % 3;
            cachedSlices_[si] = image;
            cachedIndex_[si] = index;
            cachedReady_[si] = true;
            slotCoarse_[si] = false;
            if (tfActive_)
                viewport_->updateSlice(slot, type, index, image); // D7.1 值直传
            else if (customCmapActive_)
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

// A2（wave/deepen-perf）：新请求顶替在途读——协作取消（引擎 CancelToken 轮询
// 谓词 / 直读 progress 回调），被拖过的索引不再占用整段读取时长。
void Seismic3DViewPanel::supersedeInFlightSlice(std::size_t slotIndex)
{
    if (auto *inFlight = slotTasks_[slotIndex].data()) {
        slotSuperseded_[slotIndex] = true;
        inFlight->requestCancel();
    }
}


// ---- D3.x 显示控制行 ----
void Seismic3DViewPanel::buildDisplayBar() {
    auto *bar = new QWidget(this);
    PaleoTheme::applyThemedStyleSheet(bar, [] {
        const auto &t = PaleoTheme::tokens();
        return QStringLiteral("background-color: %1; border-bottom: 1px solid %2;")
            .arg(t.surface.name(), t.border.name());
    });
    auto *lay = new QHBoxLayout(bar);
    lay->setContentsMargins(PaleoTheme::tokens().spacingSm, PaleoTheme::tokens().spacingXs, PaleoTheme::tokens().spacingSm, PaleoTheme::tokens().spacingXs);
    lay->setSpacing(PaleoTheme::tokens().spacingXs);

    const auto themedCaption = [](QWidget *w) {
        PaleoTheme::applyThemedStyleSheet(w, [] {
            return PaleoTheme::mutedCaptionStyleSheet();
        });
    };

    // D3.5 colormap：默认（引擎预烘焙）+ 8 预设 + 自定义
    auto *lblCmap = new QLabel(tr("色标:"), bar);
    themedCaption(lblCmap);
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
    themedCaption(lblAlpha);
    lay->addWidget(lblAlpha);
    sliderAlpha_ = new QSlider(Qt::Horizontal, bar);
    sliderAlpha_->setRange(10, 100);
    sliderAlpha_->setValue(100);
    sliderAlpha_->setFixedWidth(70);
    lay->addWidget(sliderAlpha_);
    auto *lblRange = new QLabel(tr("值域:"), bar);
    themedCaption(lblRange);
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

    // D7.1 传递函数（GPU LUT 体渲染）
    btnTf_ = createToolBtn(tr("传递函数"), tr("体渲染传递函数：色彩+不透明度 LUT 实时作用于 3D 体\n（GPU 侧取色，修改零取数零重烘焙）"), true);
    lay->addWidget(btnTf_);
    btnTfEdit_ = createToolBtn(tr("TF…"), tr("编辑传递函数曲线（不透明度分段线性）"));
    lay->addWidget(btnTfEdit_);

    // D3.1 体渲染（切片堆叠）
    btnStack_ = createToolBtn(tr("体渲染"), tr("切片堆叠体渲染：16 层水平切片半透明叠渲；\n拖动滑杆期间自动降到 4 层（交互降采样），静止后精渲"), true);
    lay->addWidget(btnStack_);

    // D7.2 任意剖面（两点斜剖面 / 多点栅栏）
    btnOblique_ = createToolBtn(tr("斜剖面"), tr("任意斜剖面：顶面拾取起止两点 → 后端取数 → 剖面贴入 3D 场景"), true);
    lay->addWidget(btnOblique_);
    btnFence_ = createToolBtn(tr("栅栏"), tr("栅栏剖面：顶面拾取多段折线，回车/双击提交 → 折线剖面贴入 3D 场景"), true);
    lay->addWidget(btnFence_);
    btnSectionClear_ = createToolBtn(tr("清除剖面"), tr("移除 3D 场景中的任意剖面与顶面路径线"));
    lay->addWidget(btnSectionClear_);

    // D7.3 解释 overlay：层位面/井轨迹/井名标注 checkable 菜单
    btnOverlay_ = createToolBtn(tr("解释"), tr("层位面/井轨迹/井名标注的 3D 叠显与显隐"));
    overlayMenu_ = new QMenu(this);
    btnOverlay_->setPopupMode(QToolButton::InstantPopup);
    btnOverlay_->setMenu(overlayMenu_);
    lay->addWidget(btnOverlay_);

    // D7.4 扫掠动画：轴向 + 帧率 + 播放/暂停 + PNG 序列导出
    cboSweepAxis_ = new QComboBox(bar);
    cboSweepAxis_->setObjectName(QStringLiteral("cbo3dSweepAxis"));
    cboSweepAxis_->addItem(tr("T"));
    cboSweepAxis_->addItem(tr("IL"));
    cboSweepAxis_->addItem(tr("XL"));
    cboSweepAxis_->setToolTip(tr("扫掠轴向：时间切片 / 纵测线 / 横测线"));
    cboSweepAxis_->setFixedWidth(48);
    lay->addWidget(cboSweepAxis_);
    spinSweepFps_ = new QSpinBox(bar);
    spinSweepFps_->setObjectName(QStringLiteral("spin3dSweepFps"));
    spinSweepFps_->setRange(1, 30);
    spinSweepFps_->setValue(8);
    spinSweepFps_->setSuffix(tr(" fps"));
    spinSweepFps_->setToolTip(tr("扫掠帧率（播放中可调，即时生效）"));
    spinSweepFps_->setFixedWidth(64);
    lay->addWidget(spinSweepFps_);
    btnSweepPlay_ = createToolBtn(tr("扫掠"), tr("切片动画扫掠：按轴向逐帧推进\n（当前帧异步取数 + 前向预取，UI 不阻塞）"), true);
    lay->addWidget(btnSweepPlay_);
    btnSweepExport_ = createToolBtn(tr("导出序列"), tr("选择目录并导出扫掠 PNG 序列（frame_NNNNN.png）"));
    lay->addWidget(btnSweepExport_);

    // D3.10 帧率（debug）
    chkFps_ = new QCheckBox(tr("fps"), bar);
    themedCaption(chkFps_);
    chkFps_->setToolTip(tr("显示帧率读数（调试）"));
    lay->addWidget(chkFps_);

    lay->addStretch();

    // D3.6 相机书签
    auto *lblCam = new QLabel(tr("视角:"), bar);
    themedCaption(lblCam);
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
        edit->setFont(PaleoTheme::monoFont(PaleoTheme::tokens().bodyPt));
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

    // D7.1 传递函数
    connect(btnTf_, &QToolButton::toggled, this, [this](bool on) {
        setTransferFunctionEnabled(on);
    });
    // D7.2 任意剖面：斜剖面/栅栏互斥；拾取结束（提交/取消）回写按钮态
    connect(btnOblique_, &QToolButton::toggled, this, [this](bool on) {
        if (on) {
            btnFence_->setChecked(false);
            enterObliqueSectionPick();
        } else if (viewport_ && viewport_->isSectionPickMode()) {
            viewport_->setSectionPickMode(false);
        }
    });
    connect(btnFence_, &QToolButton::toggled, this, [this](bool on) {
        if (on) {
            btnOblique_->setChecked(false);
            enterFenceSectionPick();
        } else if (viewport_ && viewport_->isSectionPickMode()) {
            viewport_->setSectionPickMode(false);
        }
    });
    if (viewport_) {
        QObject::connect(viewport_, &Seismic3DViewportWidget::sectionPickModeChanged,
                         this, [this](bool active) {
                             if (!active) {
                                 btnOblique_->setChecked(false);
                                 btnFence_->setChecked(false);
                             }
                         });
        QObject::connect(viewport_, &Seismic3DViewportWidget::sectionPathCommitted,
                         this, [this](const std::vector<glm::ivec2> &points) {
                             requestLineSection(points);
                         });
    }
    connect(btnSectionClear_, &QToolButton::clicked, this, [this]() {
        clearLineSection();
    });
    // D7.4 扫掠
    connect(btnSweepPlay_, &QToolButton::toggled, this, [this](bool on) {
        if (on) {
            const SgySliceType axis = cboSweepAxis_->currentIndex() == 1
                ? SgySliceType::Inline
                : (cboSweepAxis_->currentIndex() == 2 ? SgySliceType::Xline
                                                      : SgySliceType::Time);
            startSweep(axis, spinSweepFps_->value());
            if (!isSweepRunning()) // 无体/空轴——按钮弹回
                btnSweepPlay_->setChecked(false);
        } else {
            pauseSweep();
        }
    });
    connect(cboSweepAxis_, QOverload<int>::of(&QComboBox::activated), this, [this](int idx) {
        if (!isSweepRunning())
            return;
        const SgySliceType axis = idx == 1
            ? SgySliceType::Inline
            : (idx == 2 ? SgySliceType::Xline : SgySliceType::Time);
        startSweep(axis, spinSweepFps_->value()); // 换轴重起
    });
    connect(spinSweepFps_, QOverload<int>::of(&QSpinBox::valueChanged), this, [this](int fps) {
        if (sweepTimer_ && sweepTimer_->isActive())
            sweepTimer_->setInterval(1000 / std::clamp(fps, 1, 60));
    });
    connect(btnSweepExport_, &QToolButton::clicked, this, [this]() {
        const QString dir = QFileDialog::getExistingDirectory(
            this, tr("选择扫掠 PNG 序列导出目录"));
        if (dir.isEmpty())
            return;
        setSweepExportDir(dir);
        QgsMessageLog::logMessage(
            tr("扫掠序列将导出到：%1（frame_NNNNN.png，播放期间逐帧落盘）").arg(dir),
            QStringLiteral("Seismic3D"), Qgis::MessageLevel::Info);
    });
    connect(btnTfEdit_, &QToolButton::clicked, this, [this]() {
        // 编辑器对话框：预设 + 曲线（变更实时生效——每拖一下只重传 256B LUT）
        if (!tf_.isValid())
            tf_ = Seismic3DTransferFunction::preset(QStringLiteral("均匀半透明"));
        QDialog dlg(this);
        dlg.setWindowTitle(tr("体渲染传递函数（0=负峰 1=正峰）"));
        dlg.setModal(false);
        auto *lay = new QVBoxLayout(&dlg);
        auto *presetRow = new QWidget(&dlg);
        auto *presetLay = new QHBoxLayout(presetRow);
        presetLay->setContentsMargins(0, 0, 0, 0);
        presetLay->addWidget(new QLabel(tr("预设:"), presetRow));
        auto *cboPreset = new QComboBox(presetRow);
        cboPreset->addItem(QString());
        cboPreset->addItems(Seismic3DTransferFunction::presetNames());
        presetLay->addWidget(cboPreset);
        presetLay->addStretch(1);
        lay->addWidget(presetRow);
        auto *editor = new Seismic3DTfEditorWidget(&dlg);
        editor->setTransferFunction(tf_);
        lay->addWidget(editor, 1);
        auto *hint = new QLabel(
            tr("拖圆点调位置/不透明度；双击插入停靠点；右键删除。"), &dlg);
        PaleoTheme::applyThemedStyleSheet(hint, [] {
            return PaleoTheme::mutedCaptionStyleSheet();
        });
        lay->addWidget(hint);
        connect(cboPreset, &QComboBox::activated, this, [this, editor](int idx) {
            if (idx <= 0)
                return;
            const auto tf = Seismic3DTransferFunction::preset(
                Seismic3DTransferFunction::presetNames().value(idx - 1));
            editor->setTransferFunction(tf);
            setTransferFunction(tf);
        });
        connect(editor, &Seismic3DTfEditorWidget::transferFunctionChanged, this,
                [this](const Seismic3DTransferFunction &tf) {
                    setTransferFunction(tf); // 实时（拖动中逐帧生效）
                });
        dlg.exec();
    });

    connect(btnStack_, &QToolButton::toggled, this, [this](bool on) {
        setStackModeEnabled(on);
    });
    connect(chkFps_, &QCheckBox::toggled, viewport_, &Seismic3DViewportWidget::setFpsVisible);

    // D3.7 截图
    connect(btnShot_, &QToolButton::clicked, this, [this]() {
        if (fallbackActive_) {
            PaleoNotify::information(this, tr("三维截图"), tr("当前处于 2D 回退模式，请使用右键另存。"));
            return;
        }
        const QString path = QFileDialog::getSaveFileName(
            this, tr("导出三维视口截图"), QStringLiteral("seismic3d.png"),
            tr("PNG 图像 (*.png)"));
        if (path.isEmpty())
            return;
        const QImage img = viewport_->grabViewportImage();
        if (img.isNull()) {
            PaleoNotify::warning(this, tr("截图失败"), tr("OpenGL 帧缓冲不可用。"));
            return;
        }
        if (img.save(path, "PNG"))
            PaleoNotify::information(this, tr("已导出"), tr("三维视口已保存到:\n%1").arg(path));
        else
            PaleoNotify::critical(this, tr("导出失败"), tr("保存 PNG 失败。"));
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
    if (tfActive_) {
        // D7.1：TF 模式色彩/不透明度全由 GPU LUT 决定——直传 values
        const SgySliceType type0 = si == 0 ? SgySliceType::Inline
                                 : (si == 1 ? SgySliceType::Xline : SgySliceType::Time);
        viewport_->updateSlice(slot, type0, cachedIndex_[si], cachedSlices_[si]);
        return;
    }
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

// ---- D7.1 传递函数 ----
void Seismic3DViewPanel::setTransferFunction(const Seismic3DTransferFunction &tf) {
    tf_ = tf;
    if (!tfActive_)
        setTransferFunctionEnabled(true);
    else
        applyTransferFunction();
}

void Seismic3DViewPanel::setTransferFunctionEnabled(bool enabled) {
    if (tfActive_ == enabled)
        return;
    tfActive_ = enabled;
    applyTransferFunction();
}

// LUT 重传（256B）+ 按当前模式重喂缓存切片/堆叠层（TF 开=值纹理、关=恢复
// CPU 预烘焙色）。取数零新增——全部走面板既有 values 缓存。
void Seismic3DViewPanel::applyTransferFunction() {
    if (!viewport_)
        return;
    viewport_->setTransferFunction(tf_.buildLutRgba(), tfActive_);
    for (int slot = 0; slot < 3; ++slot) {
        if (!cachedReady_[static_cast<std::size_t>(slot)])
            continue;
        const SeismicSliceSlot s = static_cast<SeismicSliceSlot>(slot);
        if (tfActive_) {
            // 值纹理直传（渲染器按 LUT 取色）
            const SgySliceType type = slot == 0 ? SgySliceType::Inline
                                  : (slot == 1 ? SgySliceType::Xline : SgySliceType::Time);
            viewport_->updateSlice(s, type, cachedIndex_[static_cast<std::size_t>(slot)],
                                   cachedSlices_[static_cast<std::size_t>(slot)]);
        } else if (customCmapActive_) {
            recolorizeSlice(s);
        } else {
            // 引擎预烘焙 rgba 副本仍在缓存里
            const SgySliceType type = slot == 0 ? SgySliceType::Inline
                                  : (slot == 1 ? SgySliceType::Xline : SgySliceType::Time);
            viewport_->updateSlice(s, type, cachedIndex_[static_cast<std::size_t>(slot)],
                                   cachedSlices_[static_cast<std::size_t>(slot)]);
        }
    }
    for (int layer = 0; layer < SeismicSliceRenderer::kMaxStackLayers; ++layer) {
        if (!cachedStackReady_[static_cast<std::size_t>(layer)])
            continue;
        viewport_->updateStackLayer(layer, cachedStackSamples_[static_cast<std::size_t>(layer)],
                                    cachedStackImages_[static_cast<std::size_t>(layer)]);
    }
    if (cachedLineReady_ && cachedLinePath_.size() >= 2)
        viewport_->updateLineSlice(cachedLinePath_, cachedLineImage_); // D7.2 重喂
}

// ---- D7.2 任意斜剖面 / 栅栏 ----
void Seismic3DViewPanel::enterObliqueSectionPick() {
    if (!viewport_ || !volume() || !volume()->IsLoaded())
        return;
    viewport_->setSectionPickMode(true, /*autoCommitAtTwo=*/true);
}

void Seismic3DViewPanel::enterFenceSectionPick() {
    if (!viewport_ || !volume() || !volume()->IsLoaded())
        return;
    viewport_->setSectionPickMode(true, /*autoCommitAtTwo=*/false);
}

// 拾取路径 → 任意剖面取数（服务异步；BuildLineSection 同步回落）→ 贴入 3D。
// 两点=斜剖面、N 点=栅栏——同一通道（BuildLineSection 原生多段折线）。
void Seismic3DViewPanel::requestLineSection(const std::vector<glm::ivec2> &pathPoints) {
    auto vol = volume();
    if (!vol || !vol->IsLoaded() || pathPoints.size() < 2)
        return;

    if (taskSvc_) {
        QPointer<Seismic3DViewPanel> guard(this);
        SgySectionOptions options; // 默认：≤2048 列、最近道（数值可靠）
        taskSvc_->startSectionExtraction(
            vol, pathPoints, options,
            [guard, pathPoints](bool success, std::shared_ptr<const SgySliceImage> image,
                                const SgySectionStats &stats, const QString &error) {
                if (!guard)
                    return;
                if (success && image && image->width > 1) {
                    guard->cachedLineImage_ = *image;
                    guard->cachedLinePath_ = pathPoints;
                    guard->cachedLineReady_ = true;
                    guard->viewport_->updateLineSlice(pathPoints, *image);
                } else if (success) {
                    QgsMessageLog::logMessage(
                        QObject::tr("任意剖面提取列数不足（路径可能全部落空）：%1 列")
                            .arg(stats.columns),
                        QStringLiteral("Seismic3D"), Qgis::MessageLevel::Warning);
                } else {
                    QgsMessageLog::logMessage(
                        QObject::tr("任意剖面提取失败：%1").arg(error),
                        QStringLiteral("Seismic3D"), Qgis::MessageLevel::Warning);
                }
            });
        return;
    }

    // 同步回落（测试/无服务）：BuildLineSection 直取
    SgySliceImage image;
    SgySectionStats stats;
    std::string err;
    if (BuildLineSection(*vol, pathPoints, SgySectionOptions{}, image, stats, err)) {
        cachedLineImage_ = image;
        cachedLinePath_ = pathPoints;
        cachedLineReady_ = true;
        viewport_->updateLineSlice(pathPoints, image);
    } else {
        QgsMessageLog::logMessage(tr("任意剖面提取失败：%1").arg(QString::fromStdString(err)),
                                  QStringLiteral("Seismic3D"), Qgis::MessageLevel::Warning);
    }
}

void Seismic3DViewPanel::clearLineSection() {
    cachedLineReady_ = false;
    cachedLinePath_.clear();
    if (viewport_)
        viewport_->clearLineSection();
}

// ---- D7.4 切片动画扫掠 ----
int Seismic3DViewPanel::sweepOrdinalCount() const {
    auto vol = volume();
    if (!vol || !vol->IsLoaded())
        return 0;
    switch (sweepAxis_) {
    case SgySliceType::Inline: return int(vol->InlineValues().size());
    case SgySliceType::Xline: return int(vol->XlineValues().size());
    default: return vol->SampleMax() + 1;
    }
}

int Seismic3DViewPanel::sweepIndexAt(int ordinal) const {
    auto vol = volume();
    if (!vol || !vol->IsLoaded())
        return 0;
    switch (sweepAxis_) {
    case SgySliceType::Inline: {
        const auto &v = vol->InlineValues();
        return ordinal >= 0 && ordinal < int(v.size()) ? v[std::size_t(ordinal)]
                                                       : vol->InlineMin();
    }
    case SgySliceType::Xline: {
        const auto &v = vol->XlineValues();
        return ordinal >= 0 && ordinal < int(v.size()) ? v[std::size_t(ordinal)]
                                                       : vol->XlineMin();
    }
    default:
        return ordinal; // T：原始采样号
    }
}

SeismicSliceSlot Seismic3DViewPanel::sweepSlot() const {
    switch (sweepAxis_) {
    case SgySliceType::Inline: return SeismicSliceSlot::Inline;
    case SgySliceType::Xline: return SeismicSliceSlot::Crossline;
    default: return SeismicSliceSlot::Time;
    }
}

void Seismic3DViewPanel::startSweep(SgySliceType axis, int fps) {
    auto vol = volume();
    if (!vol || !vol->IsLoaded())
        return;
    sweepAxis_ = axis;
    if (sweepOrdinalCount() <= 0)
        return;
    if (!sweepTimer_) {
        sweepTimer_ = new QTimer(this);
        sweepTimer_->setTimerType(Qt::CoarseTimer);
        connect(sweepTimer_, &QTimer::timeout, this, [this]() { onSweepTick(); });
    }
    sweepTimer_->setInterval(1000 / std::clamp(fps, 1, 60));
    sweepOrdinal_ = 0;
    sweepPrefetchInFlight_ = 0;
    sweepTimer_->start();
    emit sweepFrameChanged(sweepIndexAt(0));
}

void Seismic3DViewPanel::pauseSweep() {
    if (sweepTimer_)
        sweepTimer_->stop();
}

void Seismic3DViewPanel::resumeSweep() {
    auto vol = volume();
    if (sweepTimer_ && !sweepTimer_->isActive() && vol && vol->IsLoaded() &&
        sweepOrdinalCount() > 0)
        sweepTimer_->start();
}

void Seismic3DViewPanel::stopSweep() {
    if (sweepTimer_)
        sweepTimer_->stop();
    sweepPrefetchInFlight_ = 0;
}

void Seismic3DViewPanel::setSweepExportDir(const QString &dir) {
    sweepExportDir_ = dir;
}

void Seismic3DViewPanel::onSweepTick() {
    auto vol = volume();
    const int count = sweepOrdinalCount();
    if (!vol || !vol->IsLoaded() || count <= 0) {
        stopSweep();
        return;
    }

    // 1. 导出上一帧（先导后进：grabFramebuffer 同步渲染当前态；无 GL 回退
    //    导缓存切片 rgba——数据帧序列，离屏/回退态仍可出片）
    if (!sweepExportDir_.isEmpty()) {
        QImage img = viewport_->grabViewportImage();
        if (img.isNull()) {
            const std::size_t si = static_cast<std::size_t>(sweepSlot()) % 3;
            if (cachedReady_[si]) {
                const SgySliceImage &c = cachedSlices_[si];
                img = QImage(c.rgba.data(), c.width, c.height,
                             c.width * 4, QImage::Format_RGBA8888).copy();
            }
        }
        if (!img.isNull()) {
            const QString path = QDir(sweepExportDir_).filePath(
                QStringLiteral("frame_%1.png").arg(sweepExportedCount_, 5, 10, QLatin1Char('0')));
            if (img.save(path, "PNG"))
                ++sweepExportedCount_;
        }
    }

    // 2. 帧推进（回绕）+ 滑杆跟手（slider 路径内建吸附/取数/changed 广播）
    sweepOrdinal_ = (sweepOrdinal_ + 1) % count;
    const int index = sweepIndexAt(sweepOrdinal_);
    if (sweepAxis_ == SgySliceType::Inline)
        setInline(index);
    else if (sweepAxis_ == SgySliceType::Xline)
        setCrossline(index);
    else
        setTimeSample(index);

    // 3. 前向预取窗口（结果丢弃、只暖 SgyDataCache；在途 > 2×窗口 时让路
    //    UI 交互取数——预取是播放加速，不该抢并发闸）
    if (taskSvc_ && sweepPrefetchInFlight_ < kSweepPrefetchWindow * 2) {
        for (int k = 1; k <= kSweepPrefetchWindow; ++k) {
            const int ord = (sweepOrdinal_ + k) % count;
            const int idx = sweepIndexAt(ord);
            ++sweepPrefetchInFlight_;
            QPointer<Seismic3DViewPanel> guard(this);
            taskSvc_->startSliceExtraction(
                vol, sweepAxis_, idx,
                [guard](bool, std::shared_ptr<const SgySliceImage>, const QString &) {
                    if (guard)
                        --guard->sweepPrefetchInFlight_;
                }, pagedPath_);
        }
    }
    emit sweepFrameChanged(index);
}

// ---- D3.4 / D3.12 / D7.3 ----
void Seismic3DViewPanel::setWells(const std::vector<Seismic3DWell> &wells) {
    lastWells_ = wells;
    viewport_->setWells(wellsVisible_ ? wells : std::vector<Seismic3DWell>{});
}

// ---- D7.3 层位面 + overlay 显隐 ----
void Seismic3DViewPanel::setHorizons(const QStringList &names,
                                     const std::vector<SeismicHorizonGrid> &grids) {
    std::vector<Seismic3DHorizonSurface> items;
    items.reserve(grids.size());
    // 层位兜底色板（数据符号色，非 UI token；主呈色是 twt→彩虹谱逐顶点映射）
    const QColor kColors[] = {
        QColor(46, 134, 193), QColor(231, 76, 60), QColor(39, 174, 96),
        QColor(243, 156, 18), QColor(142, 68, 173), QColor(0, 172, 193),
    };
    overlayHorizonNames_.clear();
    for (std::size_t i = 0; i < grids.size(); ++i) {
        const SeismicHorizonGrid &g = grids[i];
        Seismic3DHorizonSurface s;
        s.name = names.value(int(i), tr("层位%1").arg(i + 1));
        s.color = kColors[i % std::size(kColors)];
        s.visible = true;
        if (g.isValid() && g.twtMs.size() ==
                               std::size_t(g.inlineCount) * std::size_t(g.xlineCount)) {
            s.inlineMin = g.inlineMin;
            s.inlineCount = g.inlineCount;
            s.inlineStep = std::max(1, g.inlineStep);
            s.xlineMin = g.xlineMin;
            s.xlineCount = g.xlineCount;
            s.xlineStep = std::max(1, g.xlineStep);
            s.twtMs = g.twtMs;
        }
        overlayHorizonNames_ << s.name;
        items.push_back(std::move(s));
    }
    viewport_->setHorizons(items);
    rebuildOverlayMenu();
}

void Seismic3DViewPanel::setHorizonVisible(const QString &name, bool visible) {
    const int idx = overlayHorizonNames_.indexOf(name);
    if (idx < 0)
        return;
    viewport_->setHorizonVisible(idx, visible);
    rebuildOverlayMenu();
    emit horizonVisibilityChanged(name, visible);
}

bool Seismic3DViewPanel::isHorizonVisible(const QString &name) const {
    const int idx = overlayHorizonNames_.indexOf(name);
    return idx >= 0 && viewport_ && viewport_->isHorizonVisible(idx);
}

QStringList Seismic3DViewPanel::horizonNames() const {
    return overlayHorizonNames_;
}

void Seismic3DViewPanel::setWellLabelsVisible(bool visible) {
    viewport_->setWellLabelsVisible(visible);
}

bool Seismic3DViewPanel::wellLabelsVisible() const {
    return viewport_ && viewport_->wellLabelsVisible();
}

// checkable 菜单：井/标注总开关 + 逐层位开关。动作态即真值——外部经
// setHorizonVisible 改动后重建，菜单内点击只拨视口不重建（发射中删动作
// 的悬空风险）。
void Seismic3DViewPanel::rebuildOverlayMenu() {
    if (!overlayMenu_)
        return;
    overlayMenu_->clear();
    QAction *actWells = overlayMenu_->addAction(tr("井轨迹"));
    actWells->setCheckable(true);
    actWells->setChecked(wellsVisible_);
    connect(actWells, &QAction::toggled, this, [this](bool on) {
        wellsVisible_ = on;
        viewport_->setWells(on ? lastWells_ : std::vector<Seismic3DWell>{});
        emit wellVisibilityChanged(on);
    });
    QAction *actLabels = overlayMenu_->addAction(tr("井名标注"));
    actLabels->setCheckable(true);
    actLabels->setChecked(viewport_ && viewport_->wellLabelsVisible());
    connect(actLabels, &QAction::toggled, this, [this](bool on) {
        viewport_->setWellLabelsVisible(on);
    });
    if (!overlayHorizonNames_.isEmpty()) {
        overlayMenu_->addSeparator();
        const QStringList names = overlayHorizonNames_;
        for (int i = 0; i < names.size(); ++i) {
            const QString &name = names[i];
            QAction *act = overlayMenu_->addAction(name);
            act->setCheckable(true);
            act->setChecked(viewport_ && viewport_->isHorizonVisible(i));
            connect(act, &QAction::toggled, this, [this, name](bool on) {
                const int idx = overlayHorizonNames_.indexOf(name);
                if (idx >= 0)
                    viewport_->setHorizonVisible(idx, on);
                emit horizonVisibilityChanged(name, on);
            });
        }
    }
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

// A1/A2（wave/deepen-perf）堆叠层取数通道：
// - paged 通道且体量估算 ≤ 256MB：一次 ReadVoxelWindow 取整窗（按激活 LOD
//   降采样 IL/XL），16 层平面就地切出——16 次逐层请求 → 1 次体窗请求
//   （页面读取/解码只走一遍）。
// - 其余（直读/工作区后端、或大体量细层超预算）：逐层切片回落。直读后端的
//   引擎体窗是逐道顺序整读（无 mmap 并行窄读优势），逐层反而更省。
qint64 Seismic3DViewPanel::estimateStackWindowBytes() const {
    auto vol = volume();
    if (!vol || !vol->IsLoaded())
        return 0;
    qint64 bytes = qint64(vol->InlineCount()) * vol->XlineCount() *
                   (vol->SampleMax() + 1) * 4;
    if (pagedPath_.isEmpty() || activeLod_ <= 0)
        return bytes;
    // LOD 面积因子：L1=4×4、L2=8×8、L3=16×16 → 16^level（采样轴因子恒 1）
    for (int level = 0; level < activeLod_; ++level)
        bytes /= 16;
    return bytes;
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
    std::vector<int> samples;
    samples.reserve(kLayers);
    for (int layer = 0; layer < kLayers; ++layer) {
        const int sample = static_cast<int>((layer + 0.5) / kLayers * (sampleMax + 1));
        if (sample <= sampleMax)
            samples.push_back(sample);
    }
    const int layerCount = int(samples.size());

    constexpr qint64 kStackWindowBudgetBytes = 256ll * 1024 * 1024;
    if (taskSvc_ && !pagedPath_.isEmpty() && estimateStackWindowBytes() <= kStackWindowBudgetBytes) {
        // 体窗合并通道：全网格 × 全采样（引擎按激活 LOD 映射到粗层轴）
        engine::VoxelWindowRequest request;
        request.inlineBegin = vol->InlineMin();
        request.xlineBegin = vol->XlineMin();
        request.sampleBegin = 0;
        request.inlineCount = vol->InlineCount();
        request.xlineCount = vol->XlineCount();
        request.sampleCount = sampleMax + 1;
        stackExtracting_ = true;
        stackTargetLayers_ = layerCount;
        QPointer<Seismic3DViewPanel> guard(this);
        taskSvc_->startVoxelWindow(
            pagedPath_, request,
            [guard, samples](bool ok, const engine::VoxelWindow &window, const QString &error) {
                if (!guard)
                    return;
                guard->stackExtracting_ = false;
                const QString cancelledMark =
                    QCoreApplication::translate("SeismicTaskService", "体素窗口读取已取消");
                if (!ok || window.empty()) {
                    if (error != cancelledMark) {
                        QgsMessageLog::logMessage(
                            QObject::tr("体窗堆叠取数失败，回落逐层切片：%1").arg(error),
                            QStringLiteral("Seismic3D"), Qgis::MessageLevel::Warning);
                        guard->requestStackLayersPerLayer(samples); // 如实回落（非取消态）
                    }
                    return;
                }
                auto volume = guard->volume();
                for (int i = 0; i < int(samples.size()); ++i) {
                    SgySliceImage plane;
                    if (!SeismicTaskService::slicePlaneFromWindow(window, samples[std::size_t(i)], plane))
                        continue;
                    if (volume && !guard->tfActive_)
                        volume->Recolorize(plane); // 与切片通道同一份预烘焙色彩
                    guard->cachedStackImages_[std::size_t(i)] = plane; // D7.1 values 缓存
                    guard->cachedStackSamples_[std::size_t(i)] = samples[std::size_t(i)];
                    guard->cachedStackReady_[std::size_t(i)] = true;
                    guard->viewport_->updateStackLayer(i, samples[std::size_t(i)], plane);
                }
            });
        return;
    }
    requestStackLayersPerLayer(samples);
}

// 逐层切片通道（原 D3.1 路径：每层一次窄读时间片）
void Seismic3DViewPanel::requestStackLayersPerLayer(const std::vector<int> &samples) {
    auto vol = volume();
    if (!vol || !vol->IsLoaded())
        return;
    const int layerCount = int(samples.size());
    stackExtracting_ = true;
    stackTargetLayers_ = layerCount;
    auto remaining = std::make_shared<int>(layerCount);
    for (int i = 0; i < layerCount; ++i) {
        const int sample = samples[std::size_t(i)];
        const int layerIdx = i;
        QPointer<Seismic3DViewPanel> guard(this);
        auto dispatch = [this, guard, vol, sample, layerIdx, remaining]() {
            if (taskSvc_) {
                taskSvc_->startSliceExtraction(
                    vol, SgySliceType::Time, sample,
                    [guard, layerIdx, sample, remaining](bool ok, std::shared_ptr<const SgySliceImage> img, const QString &) {
                        if (ok && img && guard) {
                            guard->cachedStackImages_[std::size_t(layerIdx)] = *img; // D7.1
                            guard->cachedStackSamples_[std::size_t(layerIdx)] = sample;
                            guard->cachedStackReady_[std::size_t(layerIdx)] = true;
                            guard->viewport_->updateStackLayer(layerIdx, sample, *img);
                        }
                        if (guard && --(*remaining) == 0)
                            guard->stackExtracting_ = false;
                    }, pagedPath_);
            } else {
                SgySliceImage img;
                std::string err;
                if (vol->ExtractSlice(SgySliceType::Time, sample, img, err)) {
                    if (guard) {
                        guard->cachedStackImages_[std::size_t(layerIdx)] = img; // D7.1
                        guard->cachedStackSamples_[std::size_t(layerIdx)] = sample;
                        guard->cachedStackReady_[std::size_t(layerIdx)] = true;
                        guard->viewport_->updateStackLayer(layerIdx, sample, img);
                    }
                }
                if (guard && --(*remaining) == 0)
                    guard->stackExtracting_ = false;
            }
        };
        dispatch();
    }
    if (!taskSvc_ && layerCount == 0)
        stackExtracting_ = false;
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
        PaleoTheme::applyThemedStyleSheet(memoryHintLabel_, [] {
            return PaleoTheme::metricStyleSheet(QStringLiteral("color: %1; padding: 0 {spacing.sm}px; font-size: {typography.label}pt;"))
                .arg(PaleoTheme::tokens().warningText.name());
        });
        memoryHintLabel_->setWordWrap(true);
        if (auto *mainLay = qobject_cast<QVBoxLayout *>(layout()))
            mainLay->addWidget(memoryHintLabel_);
    }
    memoryHintLabel_->setText(hint);
    memoryHintLabel_->setVisible(true);
    QgsMessageLog::logMessage(hint, QStringLiteral("Seismic3D"), Qgis::MessageLevel::Warning);
}

// goal/ui-experience-polish：非阻塞行内告警（切片提取失败等）——复用内存
// 提示条控件；新告警覆盖旧文案，成功路径不再自动清（下次装配重建）。
void Seismic3DViewPanel::showInlineWarning(const QString &text)
{
    if (!memoryHintLabel_) {
        memoryHintLabel_ = new QLabel(this);
        PaleoTheme::applyThemedStyleSheet(memoryHintLabel_, [] {
            return PaleoTheme::metricStyleSheet(QStringLiteral("color: %1; padding: 0 {spacing.sm}px; font-size: {typography.label}pt;"))
                .arg(PaleoTheme::tokens().warningText.name());
        });
        memoryHintLabel_->setWordWrap(true);
        if (auto *mainLay = qobject_cast<QVBoxLayout *>(layout()))
            mainLay->addWidget(memoryHintLabel_);
    }
    memoryHintLabel_->setText(text);
    memoryHintLabel_->setVisible(true);
}
} // namespace seismic
