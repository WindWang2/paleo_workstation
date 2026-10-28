// 层：视图
#include "seismic3dviewportwidget.h"

#include <QKeyEvent>
#include <QPainter>

#include "../paleotheme.h"

#include <algorithm>
#include <array>
#include <cmath>

#include <glm/ext/matrix_projection.hpp>
#include <limits>
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
    setToolTip(tr("任意位置左键拖拽旋转（含剖面）；Alt+左键移动切片；右键/中键/Shift+左键平移；滚轮缩放\n"
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

void Seismic3DViewportWidget::onContextAboutToBeDestroyed() {
    // PW-098：浮动/回停 dock 会重建上下文。旧名字须在旧上下文里释放（共享组
    // 下拿旧名字去新上下文 delete 可能误删别的部件新分配的对象），并把全部
    // CPU 侧内容标记待重放，由下次 initializeGL 无条件补传。
    if (glInitialized_) {
        makeCurrent();
        sliceRenderer_.Cleanup(this);
        frameRenderer_.Cleanup(this);
        horizonRenderer_.Cleanup(this);
        faultRenderer_.Cleanup(this);
        doneCurrent();
    }
    glInitialized_ = false;
    faultMeshPending_ = true;
    horizonsPending_ = !horizonItems_.empty();
    if (replayLineSlice_.valid)
        pendingLineSlice_ = replayLineSlice_;
    pendingProperty_ = replayProperty_;
}

void Seismic3DViewportWidget::initializeGL() {
    initSeismicResources();
    initializeOpenGLFunctions();
    if (QOpenGLContext *ctx = context())
        connect(ctx, &QOpenGLContext::aboutToBeDestroyed, this,
                &Seismic3DViewportWidget::onContextAboutToBeDestroyed, Qt::UniqueConnection);

    glEnable(GL_DEPTH_TEST);
    glDepthFunc(GL_LESS);

    // D.15：Initialize 失败不能装作就绪——isGlReady() 为真会让看门狗永不切 2D 拼接回退
    const bool renderersOk = sliceRenderer_.Initialize(this) && frameRenderer_.Initialize(this) &&
                             horizonRenderer_.Initialize(this) && faultRenderer_.Initialize(this);
    if (!renderersOk) {
        sliceRenderer_.Cleanup(this); frameRenderer_.Cleanup(this);
        horizonRenderer_.Cleanup(this); faultRenderer_.Cleanup(this);
        return;
    }
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
        // D3.4/D3.12：GL 前（或体前）设置的井/次级体在此补传（同层位 pending）
        if (wellsPending_) {
            frameRenderer_.UpdateWells(this, *volume_, wellsForLabels_);
            wellsPending_ = false;
        }
        if (secondaryPending_) {
            frameRenderer_.SetSecondaryVolume(this, *volume_, secondaryVolume_);
            secondaryPending_ = false;
        }
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

    // D.14：GL 前暂存的堆叠层在此补传（面板 glReady 只重发三槽切片，不补堆叠层）
    if (volume_)
        sliceRenderer_.ReplayStagedStackLayers(this, *volume_);

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
    replayLineSlice_.valid = false;
    replayProperty_.clear();
    activeSectionPath_.clear();
    sectionPickPoints_.clear();
    sectionHoverValid_ = false;
    sliceRenderer_.ResetForVolumeChange(); // D.13/D.14：槽位/堆叠层/暂存层按体而异，换体作废
    initialFitDone_ = false;
    if (glInitialized_ && volume_ && volume_->IsLoaded()) {
        makeCurrent();
        // D.13：换体即换归一化基准——按体而异的 GL 几何必须重建/清除（错位/越界）
        frameRenderer_.ResetForVolumeChange(this, *volume_);
        // D3.4/D3.12：体前设置的井/次级体轮廓随新体补传（此前只暂存）
        if (wellsPending_) {
            frameRenderer_.UpdateWells(this, *volume_, wellsForLabels_);
            wellsPending_ = false;
        }
        if (secondaryPending_) {
            frameRenderer_.SetSecondaryVolume(this, *volume_, secondaryVolume_);
            secondaryPending_ = false;
        }
        horizonRenderer_.UpdateHorizons(this, *volume_, horizonItems_); // 层位面按新体重建
        if (width() > 0 && height() > 0) {
            fitToBounds();
            initialFitDone_ = true;
        }
        doneCurrent();
        update();
    } else if (!volume_) {
        update(); // 清空：重画一帧（paintGL 跳过体相关渲染）
    } else if (!horizonItems_.empty()) {
        // #145：层位先于体设置（数据预览→三维立体标准流程）时 setHorizons 因
        // 无体早退、horizonsPending_ 从未置位——GL 就绪后不会补传，层位面静默
        // 丢失。这里补置，initializeGL 体积就绪后统一补传。
        horizonsPending_ = true;
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
    {
        PendingPropertySlice rec; // PW-098：上下文重建重放源
        rec.slot = slot; rec.type = type; rec.index = index; rec.axes = axes; rec.image = image;
        rememberPropertyReplay(std::move(rec));
    }
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
    {
        PendingPropertySlice rec; // PW-098
        rec.stack = true; rec.stackLayer = layerIdx; rec.index = kIndex; rec.axes = axes; rec.image = image;
        rememberPropertyReplay(std::move(rec));
    }
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
    replayLineSlice_ = {pathPoints, image, true}; // PW-098
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
    dragMode_ = DragMode::None;
    lastMousePos_ = event->position();
    rotateVelocity_ = QPointF(0, 0);
    inertiaTimer_.stop();
    // D7.2 剖面拾取模式：左键消费为拾取点（不进旋转/拖面分支）
    if (sectionPickActive_ && event->button() == Qt::LeftButton &&
        !(event->modifiers() & (Qt::ControlModifier | Qt::AltModifier | Qt::ShiftModifier))) {
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
        // 任意位置默认旋转；移动切片必须显式按住 Alt，不抢占剖面上的旋转。
        dragMode_ = DragMode::Rotate;
        if (event->modifiers() & Qt::AltModifier) {
            dragRemainder_ = 0.0f;
            const SeismicSliceSlot hit = pickSliceAt(event->position());
            if (hit != SeismicSliceSlot::Line && sliceRenderer_.IsSlotVisible(hit) &&
                sliceRenderer_.IsSlotReady(hit)) {
                dragMode_ = DragMode::SliceDrag;
                dragSlot_ = hit;
                setCursor(Qt::SizeAllCursor);
            }
        }
    } else if (event->button() == Qt::RightButton || event->button() == Qt::MiddleButton ||
               (event->button() == Qt::LeftButton && (event->modifiers() & Qt::ShiftModifier))) {
        dragMode_ = DragMode::Pan;
    } else {
        dragMode_ = DragMode::None;
    }
}

void Seismic3DViewportWidget::mouseMoveEvent(QMouseEvent *event) {
    // D7.2 拾取悬停点刷新（橡皮线跟手）。拾取态一律在此分支收口——无点时
    // 也直接 return，不落入下方普通悬停分支把 setSectionPickMode 设好的十字
    // 光标重置掉（#805：首点前十字光标须保持）。
    if (sectionPickActive_ && dragMode_ == DragMode::None) {
        if (!sectionPickPoints_.empty()) {
            glm::ivec2 grid;
            if (pickTopPlaneGrid(event->position(), grid) && grid != sectionHoverPoint_) {
                sectionHoverPoint_ = grid;
                sectionHoverValid_ = true;
                updateSectionPreview();
            }
        }
        return;
    }
    if (dragMode_ == DragMode::None) {
        // 仅 Alt 手势提示切片移动，普通悬停不再误示为拖面。
        const auto hit = (event->modifiers() & Qt::AltModifier) && !sliceRenderer_.IsStackVisible()
            ? pickSliceAt(event->position()) : SeismicSliceSlot::Line;
        setCursor(hit != SeismicSliceSlot::Line && sliceRenderer_.IsSlotReady(hit)
                      ? Qt::SizeAllCursor : Qt::ArrowCursor);
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
        setToolTip(tr("拾取模式：单击拾取剖面路径点（顶面）%1；Ctrl+左键临时旋转；Shift+左键平移；Esc 取消")
                       .arg(autoCommitAtTwo ? tr("，两点自动成剖") : tr("，回车/双击提交栅栏")));
    } else {
        setCursor(Qt::ArrowCursor);
        setToolTip(tr("任意位置左键拖拽旋转（含剖面）；Alt+左键移动切片；右键/中键/Shift+左键平移；滚轮缩放\n"
                      "方向键旋转；+/- 缩放；双击居中复位"));
    }
    emit sectionPickModeChanged(sectionPickActive_);
}

bool Seismic3DViewportWidget::isLineSectionReady() const {
    return pendingLineSlice_.valid || sliceRenderer_.IsSlotReady(SeismicSliceSlot::Line);
}

void Seismic3DViewportWidget::clearLineSection() {
    pendingLineSlice_.valid = false;
    replayLineSlice_.valid = false;
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

    const glm::mat4 invMvp = glm::inverse(mvp);
    const float ndcX = static_cast<float>(pos.x() / std::max(1, width())) * 2.0f - 1.0f;
    const float ndcY = 1.0f - static_cast<float>(pos.y() / std::max(1, height())) * 2.0f;
    glm::vec4 nearP = invMvp * glm::vec4(ndcX, ndcY, -1.0f, 1.0f);
    glm::vec4 farP = invMvp * glm::vec4(ndcX, ndcY, 1.0f, 1.0f);
    if (std::abs(nearP.w) < 1e-12f || std::abs(farP.w) < 1e-12f)
        return SeismicSliceSlot::Line;
    const glm::vec3 rayOrigin = glm::vec3(nearP) / nearP.w;
    const glm::vec3 rayDir = glm::vec3(farP) / farP.w - rayOrigin;
    float bestT = std::numeric_limits<float>::infinity();
    SeismicSliceSlot best = SeismicSliceSlot::Line; // 未命中

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
        // PW-094：射线-平面求交，取最近命中面（原实现按固定顺序返回第一个
        // 屏幕多边形命中，前面的面会被后面的 Inline 抢走）。
        const glm::vec3 c0 = corners[0], c2 = corners[2];
        const int axis = plane.type == SgySliceType::Inline ? 2 : (plane.type == SgySliceType::Xline ? 0 : 1);
        const float dirA = rayDir[axis];
        if (std::abs(dirA) < 1e-8f)
            continue;
        const float t = (c0[axis] - rayOrigin[axis]) / dirA;
        if (!(t > 0.0f) || t >= bestT)
            continue;
        const glm::vec3 hit = rayOrigin + rayDir * t;
        bool inside = true;
        for (int k = 0; k < 3; ++k) {
            if (k == axis)
                continue;
            const float lo = std::min(c0[k], c2[k]) - 1e-5f, hi = std::max(c0[k], c2[k]) + 1e-5f;
            if (hit[k] < lo || hit[k] > hi)
                inside = false;
        }
        if (inside) {
            bestT = t;
            best = plane.slot;
        }
    }
    return best;
}

int Seismic3DViewportWidget::draggedSliceIndex(SeismicSliceSlot slot, const QPointF &delta) const {
    if (!volume_ || !volume_->IsLoaded())
        return sliceIndex_[static_cast<int>(slot)];
    const int si = static_cast<int>(slot);
    int index = sliceIndex_[si];
    int minV = 0, maxV = 1;
    // PW-099：把切片法向（索引 +1 对应的世界位移）投影到屏幕，按鼠标位移在
    // 投影方向上的分量换算索引——方向随相机 yaw/pitch 正确，且比例与缩放一致。
    const float hScale = SeismicSliceRenderer::HorizontalScale();
    const float vScale = SeismicSliceRenderer::HeightScale();
    glm::vec3 step(0.0f);
    float fallback = 0.0f;
    if (slot == SeismicSliceSlot::Inline) {
        minV = volume_->InlineMin();
        maxV = volume_->InlineMax();
        step.z = hScale / static_cast<float>(std::max(1, maxV - minV));
        fallback = static_cast<float>(delta.x() * 0.15);
    } else if (slot == SeismicSliceSlot::Crossline) {
        minV = volume_->XlineMin();
        maxV = volume_->XlineMax();
        step.x = hScale / static_cast<float>(std::max(1, maxV - minV));
        fallback = static_cast<float>(-delta.x() * 0.15);
    } else {
        maxV = volume_->SampleMax();
        step.y = -vScale / std::max(1.0f, static_cast<float>(maxV));
        fallback = static_cast<float>(-delta.y() * 0.1);
    }
    float dIndex = fallback;
    if (width() > 0 && height() > 0) {
        const float aspect = static_cast<float>(width()) / static_cast<float>(height());
        const glm::mat4 mvp = camera_.BuildProjectionMatrix(aspect) * camera_.BuildViewMatrix();
        const auto toScreen = [&](const glm::vec3 &p, bool *ok) {
            const glm::vec4 c = mvp * glm::vec4(p, 1.0f);
            *ok = c.w > 1e-4f;
            return glm::vec2((c.x / c.w * 0.5f + 0.5f) * width(), (1.0f - (c.y / c.w * 0.5f + 0.5f)) * height());
        };
        bool ok0 = false, ok1 = false;
        const glm::vec2 s0 = toScreen(glm::vec3(0.0f), &ok0);
        const glm::vec2 s1 = toScreen(step, &ok1);
        const glm::vec2 sv = s1 - s0;
        const float len2 = glm::dot(sv, sv);
        if (ok0 && ok1 && len2 > 1e-6f) // 法向几乎正对视线时退回旧启发
            dIndex = glm::dot(glm::vec2(static_cast<float>(delta.x()), static_cast<float>(delta.y())), sv) / len2;
    }
    // 累积小数部分，避免慢速拖动时每帧 round 为 0 而卡住
    dragRemainder_ += dIndex;
    const int whole = static_cast<int>(std::trunc(dragRemainder_));
    dragRemainder_ -= static_cast<float>(whole);
    index += whole;
    return std::clamp(index, minV, std::max(minV, maxV));
}

// ---- D3.1 / D3.3 / D3.4 / D3.7 / D3.12 ----

bool Seismic3DViewportWidget::updateStackLayer(int layerIdx, int sampleIndex, const SgySliceImage &image) {
    if (!volume_)
        return false;
    if (!glInitialized_) // D.14：GL 未就绪 → 渲染器内暂存，initializeGL 后补传
        return sliceRenderer_.UpdateStackLayer(this, layerIdx, *volume_, sampleIndex, image);
    makeCurrent();
    const bool ok = sliceRenderer_.UpdateStackLayer(this, layerIdx, *volume_, sampleIndex, image);
    doneCurrent();
    if (ok)
        update();
    return ok;
}


} // namespace seismic

void seismic::Seismic3DViewportWidget::rememberPropertyReplay(PendingPropertySlice rec) {
    replayProperty_.erase(std::remove_if(replayProperty_.begin(), replayProperty_.end(),
                                         [&](const PendingPropertySlice &item) {
                                             return item.stack == rec.stack &&
                                                    (rec.stack ? item.stackLayer == rec.stackLayer : item.slot == rec.slot);
                                         }),
                          replayProperty_.end());
    replayProperty_.push_back(std::move(rec));
}
