// 层：视图
#include "seismic3dviewportwidget.h"

#include <QKeyEvent>
#include <QPainter>

#include "../paleotheme.h"

#include <algorithm>
#include <array>
#include <cmath>

#include <glm/ext/matrix_projection.hpp>
#include <glm/gtc/matrix_inverse.hpp>

static void initSeismicResources() {
    Q_INIT_RESOURCE(seismic_shaders);
}

namespace seismic {

SgySliceImage bakePropertyRgba(const SgySliceImage &image) {
    const std::size_t n = static_cast<std::size_t>(std::max(0, image.width) * std::max(0, image.height));
    if (image.rgba.size() == n * 4 || image.values.size() != n || n == 0)
        return image;
    SgySliceImage baked = image;
    float lo = 0.0f, hi = 1.0f;
    bool any = false;
    for (float v : baked.values) {
        if (!std::isfinite(v))
            continue;
        if (!any) {
            lo = hi = v;
            any = true;
        } else {
            lo = std::min(lo, v);
            hi = std::max(hi, v);
        }
    }
    const float span = (hi > lo) ? (hi - lo) : 1.0f;
    baked.rgba.resize(n * 4);
    for (std::size_t i = 0; i < n; ++i) {
        const float v = baked.values[i];
        unsigned char *px = baked.rgba.data() + i * 4;
        if (!std::isfinite(v)) {
            px[0] = px[1] = px[2] = px[3] = 0;
            continue;
        }
        const float t = std::clamp((v - lo) / span, 0.0f, 1.0f);
        px[0] = static_cast<unsigned char>(40 + t * 200);
        px[1] = static_cast<unsigned char>(80 + (1.0f - std::fabs(t - 0.5f) * 2.0f) * 80);
        px[2] = static_cast<unsigned char>(180 - t * 150);
        px[3] = 255;
    }
    if (!any) {
        baked.valueMin = 0.0f;
        baked.valueMax = 1.0f;
    } else if (baked.valueMax <= baked.valueMin) {
        baked.valueMin = lo;
        baked.valueMax = hi;
    }
    return baked;
}

Seismic3DViewportWidget::Seismic3DViewportWidget(QWidget *parent)
    : QOpenGLWidget(parent) {
    initSeismicResources();
    QSurfaceFormat fmt = QSurfaceFormat::defaultFormat();
    fmt.setVersion(3, 3);
    fmt.setProfile(QSurfaceFormat::CoreProfile);
    fmt.setDepthBufferSize(24);
    setFormat(fmt);

    setFocusPolicy(Qt::StrongFocus);
    setMouseTracking(true);
    setToolTip(tr("左键拖拽旋转；右键/Shift+左键拖拽平移；滚轮缩放（Shift 横向平移）\n"
                  "方向键旋转；+/- 缩放；双击居中复位"));

    // D3.11 惯性旋转：松手后按速度衰减续转（16ms 节拍）
    inertiaTimer_.setInterval(16);
    connect(&inertiaTimer_, &QTimer::timeout, this, [this]() { applyInertia(); });
}

Seismic3DViewportWidget::~Seismic3DViewportWidget() {
    if (glInitialized_) {
        makeCurrent();
        sliceRenderer_.Cleanup(this);
        frameRenderer_.Cleanup(this);
        horizonRenderer_.Cleanup(this);
        faultRenderer_.Cleanup(this);
        doneCurrent();
    }
}

void Seismic3DViewportWidget::initializeGL() {
    initSeismicResources();
    initializeOpenGLFunctions();

    glEnable(GL_DEPTH_TEST);
    glDepthFunc(GL_LESS);

    sliceRenderer_.Initialize(this);
    frameRenderer_.Initialize(this);
    horizonRenderer_.Initialize(this);
    faultRenderer_.Initialize(this);
    if (faultMeshPending_) {
        faultRenderer_.Update(this, faultMesh_);
        faultMeshPending_ = false;
    }
    glInitialized_ = true;

    // D7.1：GL 前设置的 TF 在此补传（切片值纹理由面板 glReady 后重喂）
    if (tfActive_ && tfLutBytes_.size() == 256 * 4) {
        sliceRenderer_.SetTransferFunction(this, tfLutBytes_, true);
    }

    // D7.3：GL 前设置的层位面在此补传
    if (horizonsPending_ && volume_ && volume_->IsLoaded()) {
        horizonRenderer_.UpdateHorizons(this, *volume_, horizonItems_);
        horizonsPending_ = false;
    }

    if (volume_ && volume_->IsLoaded()) {
        frameRenderer_.UpdateFromVolume(this, *volume_);
        if (width() > 0 && height() > 0) {
            fitToBounds();
            initialFitDone_ = true;
        }
    }

    if (!pendingSlices_.empty() && volume_) {
        for (const auto &[slot, ps] : pendingSlices_) {
            sliceRenderer_.UpdateSlice(this, ps.slot, *volume_, ps.type, ps.index, ps.image);
        }
        pendingSlices_.clear();
    }

    if (pendingLineSlice_.valid && volume_) {
        sliceRenderer_.UpdateLineSlice(this, *volume_, pendingLineSlice_.pathPoints, pendingLineSlice_.image);
        frameRenderer_.UpdateLineSection(this, *volume_, pendingLineSlice_.pathPoints);
        pendingLineSlice_.valid = false;
    }

    for (const PendingPropertySlice &pending : pendingProperty_) {
        const SgySliceImage baked = bakePropertyRgba(pending.image);
        if (pending.stack) {
            // 上下文晚就绪时也要打开堆叠，否则层已上传但 stackVisible_ 仍为 false。
            if (sliceRenderer_.UpdatePropertyStackLayer(this, pending.stackLayer, pending.axes, pending.index, baked))
                sliceRenderer_.SetStackVisible(true);
        } else {
            sliceRenderer_.UpdatePropertySlice(this, pending.slot, pending.axes, pending.type, pending.index, baked);
        }
    }
    pendingProperty_.clear();

    fpsClock_.start();
    emit glReady();
    update();
}

QSize Seismic3DViewportWidget::physicalViewportSize(int w, int h, qreal devicePixelRatio) {
    return QSize(qRound(w * devicePixelRatio), qRound(h * devicePixelRatio));
}

void Seismic3DViewportWidget::resizeGL(int w, int h) {
    // QOpenGLWidget 的 FBO 是物理像素，resizeGL 收逻辑像素——高 DPI 屏必须
    // 乘 dpr，否则场景只占物理帧buffer的左下角（DESIGN.md「High DPI」）。
    // dpr=1 时 qRound(w*1.0)==w，逐像素行为不变。
    const QSize phys = physicalViewportSize(w, h, devicePixelRatioF());
    glViewport(0, 0, phys.width(), phys.height());
    if (!initialFitDone_ && w > 0 && h > 0 && volume_ && volume_->IsLoaded()) {
        fitToBounds();
        initialFitDone_ = true;
    }
}

void Seismic3DViewportWidget::showEvent(QShowEvent *event) {
    QOpenGLWidget::showEvent(event);
    if (!initialFitDone_ && width() > 0 && height() > 0 && volume_ && volume_->IsLoaded()) {
        fitToBounds();
        initialFitDone_ = true;
    }
}

void Seismic3DViewportWidget::paintGL() {
    // UI 背景跟随主题；地震振幅色标与数据几何颜色保持不变。
    const auto &t = PaleoTheme::tokens();
    glClearColor(t.surfaceAlt.redF(), t.surfaceAlt.greenF(), t.surfaceAlt.blueF(), 1.0f);
    glClear(GL_COLOR_BUFFER_BIT | GL_DEPTH_BUFFER_BIT);

    glEnable(GL_DEPTH_TEST);
    glDepthFunc(GL_LESS);

    const float aspect = height() > 0 ? static_cast<float>(width()) / static_cast<float>(height()) : 1.0f;
    const glm::mat4 proj = faultFit_.valid
                               ? camera_.BuildProjectionMatrix(aspect, 45.f, faultFit_.zNear, faultFit_.zFar)
                               : camera_.BuildProjectionMatrix(aspect);
    const glm::mat4 view = camera_.BuildViewMatrix();
    const glm::mat4 model = glm::mat4(1.0f);

    // #158：体被清空（工程切换到无地震工程）后不再画旧体留在 GPU 缓冲里的
    // 切片/外框/层位——这三类缓冲都只能由已加载体生成。
    if (volume_ && volume_->IsLoaded()) {
        sliceRenderer_.Render(this, view, proj, model);
        frameRenderer_.Render(this, view, proj, model);
        horizonRenderer_.Render(this, view, proj, model);
    }
    faultRenderer_.Render(this, view, proj);

    // D3.10 帧率读数（debug 开关；半秒滚动均值）
    ++fpsFrames_;
    if (fpsClock_.elapsed() >= 500) {
        fps_ = fpsFrames_ * 1000.0 / double(fpsClock_.elapsed());
        fpsFrames_ = 0;
        fpsClock_.restart();
    }
    const bool needOverlay = fpsVisible_ || wellLabelsVisible_;
    if (needOverlay) {
        // 标注是屏幕叠加层，不继承地震切片的深度/裁剪状态。
        glDisable(GL_DEPTH_TEST);
        glDisable(GL_CULL_FACE);
        // 切片上传使用字节对齐 1；Qt 字形位图按默认四字节行步长上传。
        glPixelStorei(GL_UNPACK_ALIGNMENT, 4);
        QPainter p(this);
        p.setFont(PaleoTheme::monoFont(PaleoTheme::tokens().labelPt));
        if (wellLabelsVisible_ && volume_ && volume_->IsLoaded()) {
            // D7.3 井名标注：井口（或轨迹首点）投影到屏幕 + 半透明底卡
            const float hScale = SeismicSliceRenderer::HorizontalScale();
            const float vScale = SeismicSliceRenderer::HeightScale();
            const auto norm = [&](float v, int lo, int hi, float scale) {
                const float range = static_cast<float>(std::max(1, hi - lo));
                return ((v - static_cast<float>(lo)) / range - 0.5f) * scale;
            };
            QFont labelFont = PaleoTheme::bodyFont();
            labelFont.setPointSize(PaleoTheme::tokens().labelPt);
            p.setFont(labelFont);
            for (const Seismic3DWell &well : wellsForLabels_) {
                float il = static_cast<float>(well.inlineNo);
                float xl = static_cast<float>(well.xlineNo);
                float frac = 0.0f; // 标注锚在井口（顶面）
                if (!well.trajectory.empty()) {
                    il = well.trajectory.front().inlineNo;
                    xl = well.trajectory.front().xlineNo;
                    frac = well.trajectory.front().sampleFrac;
                }
                const glm::vec3 world(
                    norm(xl, volume_->XlineMin(), volume_->XlineMax(), hScale),
                    vScale * 0.5f - std::clamp(frac, 0.0f, 1.0f) * vScale,
                    norm(il, volume_->InlineMin(), volume_->InlineMax(), hScale));
                const glm::vec4 clip = proj * view * glm::vec4(world, 1.0f);
                if (clip.w <= 1e-3f)
                    continue;
                const QPointF screen((clip.x / clip.w * 0.5f + 0.5f) * width(),
                                     (1.0f - (clip.y / clip.w * 0.5f + 0.5f)) * height());
                const QString text = well.name;
                const QFontMetrics fm(labelFont);
                const QRect box = fm.boundingRect(text).adjusted(-t.spacingXs, -t.spacingXs, t.spacingXs, t.spacingXs)
                                       .translated(screen.toPoint());
                p.setPen(Qt::NoPen);
                QColor labelCard = PaleoTheme::tokens().surface;
                labelCard.setAlpha(200);
                p.setBrush(labelCard);
                p.drawRoundedRect(box, t.radiusSm, t.radiusSm);
                p.setPen(QPen(PaleoTheme::tokens().text, 1));
                p.drawText(box, Qt::AlignCenter, text);
            }
        }
        if (fpsVisible_) {
            p.setFont(PaleoTheme::monoFont(PaleoTheme::tokens().labelPt));
            p.setPen(PaleoTheme::tokens().primaryText);
            p.drawText(rect().adjusted(t.spacingSm, t.spacingXs, -t.spacingSm, -t.spacingXs), Qt::AlignTop | Qt::AlignRight,
                       QStringLiteral("%1 fps").arg(fps_, 0, 'f', 1));
        }
        p.end();
    }
}

void Seismic3DViewportWidget::setVolume(std::shared_ptr<SgyVolume> volume) {
    volume_ = std::move(volume);
    pendingSlices_.clear();
    pendingProperty_.clear(); // 属性切片与地震切片共用槽位，换体丢弃未上传的属性面
    pendingLineSlice_.valid = false;
    activeSectionPath_.clear();
    sectionPickPoints_.clear();
    sectionHoverValid_ = false;
    initialFitDone_ = false;
    if (glInitialized_ && volume_ && volume_->IsLoaded()) {
        makeCurrent();
        frameRenderer_.UpdateFromVolume(this, *volume_);
        if (width() > 0 && height() > 0) {
            fitToBounds();
            initialFitDone_ = true;
        }
        doneCurrent();
        update();
    } else if (!volume_) {
        update(); // 清空：重画一帧（paintGL 跳过体相关渲染）
    }
    emit volumeLoaded();
}

bool Seismic3DViewportWidget::updateSlice(
    SeismicSliceSlot slot,
    SgySliceType type,
    int index,
    const SgySliceImage &image) {
    if (!volume_) {
        return false;
    }
    sliceIndex_[static_cast<int>(slot) % 3] = index; // D3.2 拖动换算基准
    if (!glInitialized_) {
        pendingSlices_[slot] = {slot, type, index, image};
        return true;
    }
    makeCurrent();
    const bool ok = sliceRenderer_.UpdateSlice(this, slot, *volume_, type, index, image);
    doneCurrent();
    if (ok) {
        update();
    }
    return ok;
}

bool Seismic3DViewportWidget::updatePropertySlice(
    SeismicSliceSlot slot,
    SgySliceType type,
    int index,
    const PropertyBrickAxes &axes,
    const SgySliceImage &image) {
    if (slot == SeismicSliceSlot::Line || image.width <= 0 || image.height <= 0)
        return false;
    const SgySliceImage baked = bakePropertyRgba(image);
    if (!glInitialized_) {
        PendingPropertySlice pending;
        pending.slot = slot;
        pending.type = type;
        pending.index = index;
        pending.axes = axes;
        pending.image = baked;
        pendingProperty_.erase(std::remove_if(pendingProperty_.begin(), pendingProperty_.end(),
                                              [&](const PendingPropertySlice &item) {
                                                return !item.stack && item.slot == slot;
                                              }),
                               pendingProperty_.end());
        pendingProperty_.push_back(std::move(pending));
        return true;
    }
    makeCurrent();
    const bool ok = sliceRenderer_.UpdatePropertySlice(this, slot, axes, type, index, baked);
    doneCurrent();
    if (ok)
        update();
    return ok;
}

bool Seismic3DViewportWidget::updatePropertyStackLayer(
    int layerIdx,
    int kIndex,
    const PropertyBrickAxes &axes,
    const SgySliceImage &image) {
    if (layerIdx < 0 || layerIdx >= SeismicSliceRenderer::kMaxStackLayers)
        return false;
    const SgySliceImage baked = bakePropertyRgba(image);
    if (!glInitialized_) {
        PendingPropertySlice pending;
        pending.stack = true;
        pending.stackLayer = layerIdx;
        pending.index = kIndex;
        pending.axes = axes;
        pending.image = baked;
        // 同一 stackLayer 只留最后一次，GL 未就绪时不要无限追加。
        pendingProperty_.erase(std::remove_if(pendingProperty_.begin(), pendingProperty_.end(),
                                              [&](const PendingPropertySlice &item) {
                                                return item.stack && item.stackLayer == layerIdx;
                                              }),
                               pendingProperty_.end());
        pendingProperty_.push_back(std::move(pending));
        return true;
    }
    makeCurrent();
    const bool ok = sliceRenderer_.UpdatePropertyStackLayer(this, layerIdx, axes, kIndex, baked);
    doneCurrent();
    if (ok) {
        sliceRenderer_.SetStackVisible(true);
        update();
    }
    return ok;
}

bool Seismic3DViewportWidget::updateLineSlice(
    const std::vector<glm::ivec2> &pathPoints,
    const SgySliceImage &image) {
    if (!volume_) {
        return false;
    }
    activeSectionPath_ = pathPoints; // D7.2：取消拾取后恢复顶面线用
    if (!glInitialized_) {
        pendingLineSlice_ = {pathPoints, image, true};
        return true;
    }
    makeCurrent();
    const bool ok = sliceRenderer_.UpdateLineSlice(this, *volume_, pathPoints, image);
    frameRenderer_.UpdateLineSection(this, *volume_, pathPoints);
    doneCurrent();
    if (ok) {
        update();
    }
    return ok;
}

void Seismic3DViewportWidget::setSlotVisible(SeismicSliceSlot slot, bool visible) {
    sliceRenderer_.SetSlotVisible(slot, visible);
    update();
}

bool Seismic3DViewportWidget::isSlotVisible(SeismicSliceSlot slot) const {
    return sliceRenderer_.IsSlotVisible(slot);
}

bool Seismic3DViewportWidget::isSlotReady(SeismicSliceSlot slot) const {
    return sliceRenderer_.IsSlotReady(slot);
}

void Seismic3DViewportWidget::setFrameVisible(bool visible) {
    frameRenderer_.SetVisible(visible);
    update();
}

bool Seismic3DViewportWidget::isFrameVisible() const {
    return frameRenderer_.IsVisible();
}

void Seismic3DViewportWidget::setPresetView(SeismicCameraController::PresetView preset) {
    camera_.ApplyPreset(preset);
    update();
    emit cameraChanged();
}

void Seismic3DViewportWidget::fitToBounds() {
    const float halfH = SeismicSliceRenderer::HorizontalScale() * 0.5f;
    const float halfV = SeismicSliceRenderer::HeightScale() * 0.5f;
    const glm::vec3 minBound(-halfH, -halfV, -halfH);
    const glm::vec3 maxBound(halfH, halfV, halfH);
    const float aspect = height() > 0 ? static_cast<float>(width()) / static_cast<float>(height()) : 1.0f;
    camera_.FitToBounds(minBound, maxBound, aspect);
    update();
    emit cameraChanged();
}

void Seismic3DViewportWidget::mousePressEvent(QMouseEvent *event) {
    lastMousePos_ = event->position();
    rotateVelocity_ = QPointF(0, 0);
    inertiaTimer_.stop();
    // D7.2 剖面拾取模式：左键消费为拾取点（不进旋转/拖面分支）
    if (sectionPickActive_ && event->button() == Qt::LeftButton &&
        !(event->modifiers() & Qt::ControlModifier)) {
        glm::ivec2 grid;
        if (pickTopPlaneGrid(event->position(), grid)) {
            // 与末点重合（抖动/双击次击）不入列
            if (sectionPickPoints_.empty() || sectionPickPoints_.back() != grid) {
                sectionPickPoints_.push_back(grid);
                if (sectionAutoCommitTwo_ && sectionPickPoints_.size() >= 2) {
                    commitSectionPath();
                } else {
                    updateSectionPreview();
                }
            }
        }
        return;
    }
    if (event->button() == Qt::LeftButton && !(event->modifiers() & Qt::ShiftModifier)) {
        // D3.2：点中切片面 → 拖面（Ctrl 按住强制旋转，避免抢交互）
        if (event->modifiers() & Qt::ControlModifier) {
            dragMode_ = DragMode::Rotate;
        } else {
            const SeismicSliceSlot hit = pickSliceAt(event->position());
            if (hit != SeismicSliceSlot::Line && sliceRenderer_.IsSlotVisible(hit) &&
                sliceRenderer_.IsSlotReady(hit)) {
                dragMode_ = DragMode::SliceDrag;
                dragSlot_ = hit;
                setCursor(Qt::SizeAllCursor);
                return;
            }
            dragMode_ = DragMode::Rotate;
        }
    } else if (event->button() == Qt::RightButton || event->button() == Qt::MiddleButton ||
               (event->button() == Qt::LeftButton && (event->modifiers() & Qt::ShiftModifier))) {
        dragMode_ = DragMode::Pan;
    } else {
        dragMode_ = DragMode::None;
    }
}

void Seismic3DViewportWidget::mouseMoveEvent(QMouseEvent *event) {
    // D7.2 拾取悬停点刷新（橡皮线跟手）
    if (sectionPickActive_ && !sectionPickPoints_.empty()) {
        glm::ivec2 grid;
        if (pickTopPlaneGrid(event->position(), grid) && grid != sectionHoverPoint_) {
            sectionHoverPoint_ = grid;
            sectionHoverValid_ = true;
            updateSectionPreview();
        }
        return;
    }
    if (dragMode_ == DragMode::None) {
        // D3.2 悬停高亮提示：落在切片面上给拖拽光标
        if (!sliceRenderer_.IsStackVisible()) {
            const SeismicSliceSlot hit = pickSliceAt(event->position());
            setCursor((hit != SeismicSliceSlot::Line && sliceRenderer_.IsSlotReady(hit))
                          ? Qt::SizeAllCursor : Qt::ArrowCursor);
        }
        return;
    }

    const QPointF delta = event->position() - lastMousePos_;
    const float dx = static_cast<float>(delta.x());
    const float dy = static_cast<float>(delta.y());
    lastMousePos_ = event->position();

    if (dragMode_ == DragMode::Rotate) {
        camera_.Rotate(dx, dy);
        rotateVelocity_ = delta; // D3.11 惯性速度采样
    } else if (dragMode_ == DragMode::Pan) {
        camera_.Pan(dx, dy);
    } else if (dragMode_ == DragMode::SliceDrag) {
        // D3.2：拖面换索引 → 面板重提取 + 广播（联动 2D 剖面）
        const int newIndex = draggedSliceIndex(dragSlot_, delta);
        if (newIndex != sliceIndex_[static_cast<int>(dragSlot_)]) {
            sliceIndex_[static_cast<int>(dragSlot_)] = newIndex;
            emit sliceDragged(dragSlot_, newIndex);
        }
        return; // 拖面不动相机
    }

    update();
    emit cameraChanged();
}

void Seismic3DViewportWidget::mouseReleaseEvent(QMouseEvent * /*event*/) {
    if (dragMode_ == DragMode::Rotate && inertiaEnabled_ &&
        std::abs(rotateVelocity_.x()) + std::abs(rotateVelocity_.y()) > 2.0) {
        inertiaTimer_.start(); // D3.11 惯性续转
    }
    dragMode_ = DragMode::None;
    setCursor(Qt::ArrowCursor);
}

void Seismic3DViewportWidget::applyInertia() {
    // 速度衰减 0.92/帧；低于阈值停表
    rotateVelocity_ *= 0.92;
    if (std::abs(rotateVelocity_.x()) + std::abs(rotateVelocity_.y()) < 0.6) {
        inertiaTimer_.stop();
        return;
    }
    camera_.Rotate(static_cast<float>(rotateVelocity_.x()),
                   static_cast<float>(rotateVelocity_.y()));
    update();
    emit cameraChanged();
}

void Seismic3DViewportWidget::wheelEvent(QWheelEvent *event) {
    const float delta = static_cast<float>(event->angleDelta().y()) / 120.0f;
    if (event->modifiers() & Qt::ShiftModifier) {
        // Shift+滚轮：横向平移（纵向平移由右键/中键拖拽承担）。
        camera_.Pan(delta * 24.0f, 0.0f);
    } else {
        // 裸滚轮（含 Ctrl）：缩放——3D 惯例；平移走右键/中键拖拽。
        camera_.Zoom(delta);
    }
    update();
    emit cameraChanged();
}

void Seismic3DViewportWidget::keyPressEvent(QKeyEvent *event) {
    // D7.2 拾取模式键：回车=提交栅栏、Esc=取消（优先于相机快捷键）
    if (sectionPickActive_) {
        if (event->key() == Qt::Key_Return || event->key() == Qt::Key_Enter) {
            if (sectionPickPoints_.size() >= 2)
                commitSectionPath();
            event->accept();
            return;
        }
        if (event->key() == Qt::Key_Escape) {
            cancelSectionPick();
            event->accept();
            return;
        }
    }
    // 方向键旋转（12px 当量 ≈ 3°/次），+/- 缩放
    constexpr float kRotateStepPx = 12.0f;
    switch (event->key()) {
    case Qt::Key_Left:
        camera_.Rotate(-kRotateStepPx, 0.0f);
        break;
    case Qt::Key_Right:
        camera_.Rotate(kRotateStepPx, 0.0f);
        break;
    case Qt::Key_Up:
        camera_.Rotate(0.0f, kRotateStepPx);
        break;
    case Qt::Key_Down:
        camera_.Rotate(0.0f, -kRotateStepPx);
        break;
    case Qt::Key_Plus:
    case Qt::Key_Equal:
        camera_.Zoom(1.0f);
        break;
    case Qt::Key_Minus:
        camera_.Zoom(-1.0f);
        break;
    default:
        QOpenGLWidget::keyPressEvent(event);
        return;
    }
    update();
    emit cameraChanged();
    event->accept();
}

void Seismic3DViewportWidget::mouseDoubleClickEvent(QMouseEvent * /*event*/) {
    // D7.2 拾取模式：双击提交栅栏（次击的重复点已由 mousePress 去重）
    if (sectionPickActive_) {
        if (sectionPickPoints_.size() >= 2)
            commitSectionPath();
        return;
    }
    fitToBounds();
}

// ---- D7.2 任意剖面拾取：顶面射线求交 + 预览 + 提交/取消 ----

// 屏幕 → NDC → 双深度反投影 → 与 y=+HeightScale/2 平面求交 → 测网格坐标。
bool Seismic3DViewportWidget::pickTopPlaneGrid(const QPointF &pos, glm::ivec2 &outGrid) const {
    if (!volume_ || !volume_->IsLoaded() || width() <= 0 || height() <= 0)
        return false;
    const float aspect = static_cast<float>(width()) / static_cast<float>(height());
    const glm::mat4 proj = camera_.BuildProjectionMatrix(aspect);
    const glm::mat4 view = camera_.BuildViewMatrix();
    const glm::vec4 vp(0.0f, 0.0f, static_cast<float>(width()), static_cast<float>(height()));

    // unProject 收窗口像素坐标（GL 惯例：左下原点——y 翻转；z 0/1 = 近远裁剪）
    const glm::vec3 winNear(static_cast<float>(pos.x()), vp.w - static_cast<float>(pos.y()), 0.0f);
    const glm::vec3 winFar(static_cast<float>(pos.x()), vp.w - static_cast<float>(pos.y()), 1.0f);
    const glm::vec3 pNear = glm::unProject(winNear, view, proj, vp);
    const glm::vec3 pFar = glm::unProject(winFar, view, proj, vp);
    const glm::vec3 dir = pFar - pNear;
    const float yTop = SeismicSliceRenderer::HeightScale() * 0.5f;
    if (std::abs(dir.y) < 1e-6f)
        return false;
    const float t = (yTop - pNear.y) / dir.y;
    if (t < 0.0f || t > 1.0f)
        return false;
    const glm::vec3 hit = pNear + dir * t;

    // BuildSliceVertices 的逆映射（x↔xline、z↔inline，水平同尺度）
    const float hScale = SeismicSliceRenderer::HorizontalScale();
    const auto toGrid = [&](float coord, int minV, int maxV) {
        const float range = static_cast<float>(std::max(1, maxV - minV));
        const float raw = minV + (coord / hScale + 0.5f) * range;
        return static_cast<int>(std::lround(raw));
    };
    int il = toGrid(hit.z, volume_->InlineMin(), volume_->InlineMax());
    int xl = toGrid(hit.x, volume_->XlineMin(), volume_->XlineMax());
    il = std::clamp(il, volume_->InlineMin(), volume_->InlineMax());
    xl = std::clamp(xl, volume_->XlineMin(), volume_->XlineMax());
    il = volume_->FindNearestInlineValue(il);
    xl = volume_->FindNearestXlineValue(xl);
    outGrid = glm::ivec2(il, xl);
    return true;
}

// 顶面橡皮线：已拾点 + 悬停点（≥2 才画；GL 未就绪仅存内存）
void Seismic3DViewportWidget::updateSectionPreview() {
    std::vector<glm::ivec2> path = sectionPickPoints_;
    if (sectionHoverValid_ && !sectionPickPoints_.empty())
        path.push_back(sectionHoverPoint_);
    if (path.size() < 2 || !volume_ || !volume_->IsLoaded())
        return;
    if (glInitialized_) {
        makeCurrent();
        frameRenderer_.UpdateLineSection(this, *volume_, path);
        doneCurrent();
        update();
    }
}

void Seismic3DViewportWidget::commitSectionPath() {
    const std::vector<glm::ivec2> path = sectionPickPoints_;
    setSectionPickMode(false, sectionAutoCommitTwo_);
    if (path.size() >= 2)
        emit sectionPathCommitted(path);
}

void Seismic3DViewportWidget::cancelSectionPick() {
    setSectionPickMode(false, sectionAutoCommitTwo_);
    // 恢复已贴剖面的顶面线（无则清线）
    if (glInitialized_ && volume_) {
        makeCurrent();
        if (activeSectionPath_.size() >= 2)
            frameRenderer_.UpdateLineSection(this, *volume_, activeSectionPath_);
        else
            frameRenderer_.ClearLineSection();
        doneCurrent();
        update();
    }
}

void Seismic3DViewportWidget::setSectionPickMode(bool enabled, bool autoCommitAtTwo) {
    sectionPickActive_ = enabled;
    sectionAutoCommitTwo_ = autoCommitAtTwo;
    sectionPickPoints_.clear();
    sectionHoverValid_ = false;
    if (enabled) {
        setCursor(Qt::CrossCursor);
        setToolTip(tr("拾取模式：单击拾取剖面路径点（顶面）%1；Esc 取消")
                       .arg(autoCommitAtTwo ? tr("，两点自动成剖") : tr("，回车/双击提交栅栏")));
    } else {
        setCursor(Qt::ArrowCursor);
        setToolTip(tr("左键拖拽旋转；右键/Shift+左键拖拽平移；滚轮缩放（Shift 横向平移）\n"
                      "方向键旋转；+/- 缩放；双击居中复位"));
    }
    emit sectionPickModeChanged(sectionPickActive_);
}

bool Seismic3DViewportWidget::isLineSectionReady() const {
    return pendingLineSlice_.valid || sliceRenderer_.IsSlotReady(SeismicSliceSlot::Line);
}

void Seismic3DViewportWidget::clearLineSection() {
    pendingLineSlice_.valid = false;
    activeSectionPath_.clear();
    if (glInitialized_) {
        makeCurrent();
        sliceRenderer_.ClearSlot(this, SeismicSliceSlot::Line);
        frameRenderer_.ClearLineSection();
        doneCurrent();
    }
    update();
}

// ---- D3.2 切片面拾取与拖拽换算 ----

SeismicSliceSlot Seismic3DViewportWidget::pickSliceAt(const QPointF &pos) const {
    if (!volume_ || !volume_->IsLoaded() || glInitialized_ == false)
        return SeismicSliceSlot::Line;
    const float aspect = height() > 0 ? static_cast<float>(width()) / static_cast<float>(height()) : 1.0f;
    const glm::mat4 mvp = camera_.BuildProjectionMatrix(aspect) * camera_.BuildViewMatrix();

    struct PlaneDef {
        SeismicSliceSlot slot;
        SgySliceType type;
        int index;
    };
    const PlaneDef planes[3] = {
        {SeismicSliceSlot::Inline, SgySliceType::Inline, sliceIndex_[0]},
        {SeismicSliceSlot::Crossline, SgySliceType::Xline, sliceIndex_[1]},
        {SeismicSliceSlot::Time, SgySliceType::Time, sliceIndex_[2]},
    };

    // 构造与切片渲染一致的 4 角（与 BuildSliceVertices 同映射），投影到屏幕
    const float hScale = SeismicSliceRenderer::HorizontalScale();
    const float vScale = SeismicSliceRenderer::HeightScale();
    const auto normalize = [&](int value, int minValue, int maxValue, float scale) {
        const float range = static_cast<float>(std::max(1, maxValue - minValue));
        return ((static_cast<float>(value - minValue) / range) - 0.5f) * scale;
    };
    const float xMin = normalize(volume_->XlineMin(), volume_->XlineMin(), volume_->XlineMax(), hScale);
    const float xMax = normalize(volume_->XlineMax(), volume_->XlineMin(), volume_->XlineMax(), hScale);
    const float zMin = normalize(volume_->InlineMin(), volume_->InlineMin(), volume_->InlineMax(), hScale);
    const float zMax = normalize(volume_->InlineMax(), volume_->InlineMin(), volume_->InlineMax(), hScale);
    const float yTop = vScale * 0.5f;
    const float yBottom = -vScale * 0.5f;

    for (const PlaneDef &plane : planes) {
        if (!sliceRenderer_.IsSlotVisible(plane.slot))
            continue;
        std::array<glm::vec3, 4> corners{};
        if (plane.type == SgySliceType::Inline) {
            const float z = normalize(plane.index, volume_->InlineMin(), volume_->InlineMax(), hScale);
            corners = {{{xMin, yBottom, z}, {xMax, yBottom, z}, {xMax, yTop, z}, {xMin, yTop, z}}};
        } else if (plane.type == SgySliceType::Xline) {
            const float x = normalize(plane.index, volume_->XlineMin(), volume_->XlineMax(), hScale);
            corners = {{{x, yBottom, zMin}, {x, yBottom, zMax}, {x, yTop, zMax}, {x, yTop, zMin}}};
        } else {
            const float y = -((static_cast<float>(plane.index) / std::max(1.0f, static_cast<float>(volume_->SampleMax()))) - 0.5f) * vScale;
            corners = {{{xMin, y, zMin}, {xMax, y, zMin}, {xMax, y, zMax}, {xMin, y, zMax}}};
        }
        // 投影 + 点在四边形内测试（符号一致法）
        int inside = 0;
        for (const glm::vec3 &c : corners) {
            const glm::vec4 clip = mvp * glm::vec4(c, 1.0f);
            if (clip.w <= 1e-4f) { inside = -99; break; }
            const float sx = (clip.x / clip.w * 0.5f + 0.5f) * width();
            const float sy = (1.0f - (clip.y / clip.w * 0.5f + 0.5f)) * height();
            const float cross = (sx - pos.x()) * (sx - pos.x()) + (sy - pos.y()) * (sy - pos.y());
            (void)cross;
        }
        // 简化：屏幕空间四边形（凸）— 用角点连线做 point-in-polygon
        bool allSame = true;
        bool lastSign = false;
        bool valid = true;
        std::array<QPointF, 5> poly;
        for (std::size_t i = 0; i < 4; ++i) {
            const glm::vec4 clip = mvp * glm::vec4(corners[i], 1.0f);
            if (clip.w <= 1e-4f) { valid = false; break; }
            poly[i] = QPointF((clip.x / clip.w * 0.5f + 0.5f) * width(),
                              (1.0f - (clip.y / clip.w * 0.5f + 0.5f)) * height());
        }
        if (!valid)
            continue;
        poly[4] = poly[0];
        for (std::size_t i = 0; i < 4; ++i) {
            const QPointF &a = poly[i];
            const QPointF &b = poly[i + 1];
            const double cross = (b.x() - a.x()) * (pos.y() - a.y()) -
                                 (b.y() - a.y()) * (pos.x() - a.x());
            const bool sign = cross >= 0.0;
            if (i == 0)
                lastSign = sign;
            else if (sign != lastSign)
                allSame = false;
        }
        if (allSame)
            return plane.slot;
    }
    return SeismicSliceSlot::Line; // 未命中
}

int Seismic3DViewportWidget::draggedSliceIndex(SeismicSliceSlot slot, const QPointF &delta) const {
    if (!volume_ || !volume_->IsLoaded())
        return sliceIndex_[static_cast<int>(slot)];
    int index = sliceIndex_[static_cast<int>(slot)];
    int minV = 0, maxV = 1;
    if (slot == SeismicSliceSlot::Inline) {
        minV = volume_->InlineMin();
        maxV = volume_->InlineMax();
        // 屏幕横向（近似测线方向）拖动换 IL；左右方向与相机 yaw 相关——用
        // 简单启发：dx 正 → IL 增
        index += static_cast<int>(std::round(delta.x() * 0.15));
    } else if (slot == SeismicSliceSlot::Crossline) {
        minV = volume_->XlineMin();
        maxV = volume_->XlineMax();
        index -= static_cast<int>(std::round(delta.x() * 0.15));
    } else {
        maxV = volume_->SampleMax();
        index -= static_cast<int>(std::round(delta.y() * 0.1)); // 上拖 → 时间上移
    }
    return std::clamp(index, minV, std::max(minV, maxV));
}

// ---- D3.1 / D3.3 / D3.4 / D3.7 / D3.12 ----

bool Seismic3DViewportWidget::updateStackLayer(int layerIdx, int sampleIndex, const SgySliceImage &image) {
    if (!volume_)
        return false;
    if (!glInitialized_) {
        return false; // GL 未就绪时堆叠层由面板在 glReady 后重发
    }
    makeCurrent();
    const bool ok = sliceRenderer_.UpdateStackLayer(this, layerIdx, *volume_, sampleIndex, image);
    doneCurrent();
    if (ok)
        update();
    return ok;
}

void Seismic3DViewportWidget::setStackVisible(bool visible) {
    sliceRenderer_.SetStackVisible(visible);
    update();
}

bool Seismic3DViewportWidget::isStackVisible() const {
    return sliceRenderer_.IsStackVisible();
}

void Seismic3DViewportWidget::setStackLayerCount(int count) {
    const int clamped = std::clamp(count, 0, SeismicSliceRenderer::kMaxStackLayers);
    if (clamped == stackLayerCount_)
        return;
    // 只显示前 N 层（层序 = 体积分布序）
    for (int i = 0; i < SeismicSliceRenderer::kMaxStackLayers; ++i)
        sliceRenderer_.SetStackLayerVisible(i, i < clamped);
    stackLayerCount_ = clamped;
    update();
}

void Seismic3DViewportWidget::setSliceAlpha(float alpha) {
    sliceRenderer_.SetSliceAlpha(alpha);
    update();
}

// D7.1：LUT 重传（256B）。enable 翻转时切片/堆叠层纹理需按新模式重喂——
// 由面板持有 values 缓存方（cachedSlices_/堆叠缓存）负责，视口只管 LUT。
void Seismic3DViewportWidget::setTransferFunction(const std::vector<unsigned char> &lutRgba, bool enable) {
    tfLutBytes_ = lutRgba;
    tfActive_ = enable;
    if (!glInitialized_ || lutRgba.size() != 256 * 4)
        return;
    makeCurrent();
    sliceRenderer_.SetTransferFunction(this, lutRgba, enable);
    doneCurrent();
    update();
}

QImage Seismic3DViewportWidget::grabViewportImage() {
    if (!glInitialized_)
        return QImage();
    return grabFramebuffer();
}

void Seismic3DViewportWidget::setWells(const std::vector<Seismic3DWell> &wells) {
    wellsForLabels_ = wells; // D7.3 标注叠绘原料（无 GL 也更新）
    if (!volume_)
        return;
    if (glInitialized_) {
        makeCurrent();
        frameRenderer_.UpdateWells(this, *volume_, wells);
        doneCurrent();
        update();
    }
}

// ---- D7.3 层位面 ----
void Seismic3DViewportWidget::setFaultSceneMesh(const FaultSceneMesh &mesh) {
    faultMesh_ = mesh;
    faultRenderer_.setMesh(mesh);
    if (glInitialized_) {
        makeCurrent();
        faultRenderer_.Update(this, mesh);
        doneCurrent();
        update();
    } else {
        faultMeshPending_ = true;
    }
}

void Seismic3DViewportWidget::clearFaultSceneMesh() {
    setFaultSceneMesh(FaultSceneMesh{});
    faultFit_ = {};
}

int Seismic3DViewportWidget::faultSceneTriangleCount() const {
    return faultRenderer_.triangleCount();
}

void Seismic3DViewportWidget::fitFaultSurfaces(float aspect) {
    const float safeAspect = aspect > 0.f ? aspect : 1.f;
    faultFit_ = fitFaultSceneCamera(camera_, faultMesh_, safeAspect);
}

bool Seismic3DViewportWidget::faultSceneContainsBounds(float aspect) const {
    const float safeAspect = aspect > 0.f ? aspect : 1.f;
    return faultSceneBoundsInsideFrustum(camera_, faultFit_, safeAspect);
}

void Seismic3DViewportWidget::setHorizons(const std::vector<Seismic3DHorizonSurface> &items) {
    horizonItems_ = items;
    if (!volume_ || !volume_->IsLoaded())
        return;
    if (glInitialized_) {
        makeCurrent();
        horizonRenderer_.UpdateHorizons(this, *volume_, horizonItems_);
        doneCurrent();
        update();
    } else {
        horizonsPending_ = true; // initializeGL 补传
    }
}

void Seismic3DViewportWidget::setHorizonVisible(int index, bool visible) {
    if (index >= 0 && index < int(horizonItems_.size()))
        horizonItems_[static_cast<std::size_t>(index)].visible = visible;
    horizonRenderer_.SetHorizonVisible(index, visible);
    update();
}

bool Seismic3DViewportWidget::isHorizonVisible(int index) const {
    if (index < 0 || index >= int(horizonItems_.size()))
        return false;
    return horizonItems_[static_cast<std::size_t>(index)].visible;
}

void Seismic3DViewportWidget::setHorizonsVisible(bool visible) {
    horizonRenderer_.SetVisible(visible);
    update();
}

void Seismic3DViewportWidget::setWellLabelsVisible(bool visible) {
    wellLabelsVisible_ = visible;
    update();
}

void Seismic3DViewportWidget::setSecondaryVolume(std::shared_ptr<const SgyVolume> secondary) {
    secondaryVolume_ = std::move(secondary);
    if (!volume_ || !glInitialized_)
        return;
    makeCurrent();
    frameRenderer_.SetSecondaryVolume(this, *volume_, secondaryVolume_);
    doneCurrent();
    update();
}

} // namespace seismic
