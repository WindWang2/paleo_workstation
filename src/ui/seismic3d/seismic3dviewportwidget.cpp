#include "seismic3dviewportwidget.h"

static void initSeismicResources() {
    Q_INIT_RESOURCE(seismic_shaders);
}

namespace seismic {

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
}

Seismic3DViewportWidget::~Seismic3DViewportWidget() {
    if (glInitialized_) {
        makeCurrent();
        sliceRenderer_.Cleanup(this);
        frameRenderer_.Cleanup(this);
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
    glInitialized_ = true;

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

    emit glReady();
    update();
}

void Seismic3DViewportWidget::resizeGL(int w, int h) {
    glViewport(0, 0, w, h);
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
    // Elegant dark slate background for scientific 3D seismic display
    glClearColor(0.12f, 0.14f, 0.17f, 1.0f);
    glClear(GL_COLOR_BUFFER_BIT | GL_DEPTH_BUFFER_BIT);

    glEnable(GL_DEPTH_TEST);
    glDepthFunc(GL_LESS);

    const float aspect = height() > 0 ? static_cast<float>(width()) / static_cast<float>(height()) : 1.0f;
    const glm::mat4 proj = camera_.BuildProjectionMatrix(aspect);
    const glm::mat4 view = camera_.BuildViewMatrix();
    const glm::mat4 model = glm::mat4(1.0f);

    sliceRenderer_.Render(this, view, proj, model);
    frameRenderer_.Render(this, view, proj, model);
}

void Seismic3DViewportWidget::setVolume(std::shared_ptr<SgyVolume> volume) {
    volume_ = std::move(volume);
    pendingSlices_.clear();
    pendingLineSlice_.valid = false;
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

bool Seismic3DViewportWidget::updateLineSlice(
    const std::vector<glm::ivec2> &pathPoints,
    const SgySliceImage &image) {
    if (!volume_) {
        return false;
    }
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
    if (event->button() == Qt::LeftButton && !(event->modifiers() & Qt::ShiftModifier)) {
        dragMode_ = DragMode::Rotate;
    } else if (event->button() == Qt::RightButton || event->button() == Qt::MiddleButton ||
               (event->button() == Qt::LeftButton && (event->modifiers() & Qt::ShiftModifier))) {
        dragMode_ = DragMode::Pan;
    } else {
        dragMode_ = DragMode::None;
    }
}

void Seismic3DViewportWidget::mouseMoveEvent(QMouseEvent *event) {
    if (dragMode_ == DragMode::None) {
        return;
    }

    const float dx = static_cast<float>(event->position().x() - lastMousePos_.x());
    const float dy = static_cast<float>(event->position().y() - lastMousePos_.y());
    lastMousePos_ = event->position();

    if (dragMode_ == DragMode::Rotate) {
        camera_.Rotate(dx, dy);
    } else if (dragMode_ == DragMode::Pan) {
        camera_.Pan(dx, dy);
    }

    update();
    emit cameraChanged();
}

void Seismic3DViewportWidget::mouseReleaseEvent(QMouseEvent * /*event*/) {
    dragMode_ = DragMode::None;
}

void Seismic3DViewportWidget::wheelEvent(QWheelEvent *event) {
    const float delta = static_cast<float>(event->angleDelta().y()) / 120.0f;
    camera_.Zoom(delta);
    update();
    emit cameraChanged();
}

void Seismic3DViewportWidget::mouseDoubleClickEvent(QMouseEvent * /*event*/) {
    fitToBounds();
}

} // namespace seismic
