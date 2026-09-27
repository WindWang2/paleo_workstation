#pragma once

#include <QOpenGLFunctions_3_3_Core>
#include <QOpenGLWidget>
#include <QMouseEvent>
#include <QWheelEvent>

#include <memory>
#include <vector>

#include "seismiccameracontroller.h"
#include "seismicslicerenderer.h"
#include "volumeframerenderer.h"
#include "../../domain/seismic/sgyvolume.h"

namespace seismic {

class Seismic3DViewportWidget : public QOpenGLWidget, protected QOpenGLFunctions_3_3_Core {
    Q_OBJECT
public:
    explicit Seismic3DViewportWidget(QWidget *parent = nullptr);
    ~Seismic3DViewportWidget() override;

    void setVolume(std::shared_ptr<SgyVolume> volume);
    [[nodiscard]] std::shared_ptr<SgyVolume> volume() const { return volume_; }

    bool updateSlice(
        SeismicSliceSlot slot,
        SgySliceType type,
        int index,
        const SgySliceImage &image);

    bool updateLineSlice(
        const std::vector<glm::ivec2> &pathPoints,
        const SgySliceImage &image);

    void setSlotVisible(SeismicSliceSlot slot, bool visible);
    [[nodiscard]] bool isSlotVisible(SeismicSliceSlot slot) const;
    [[nodiscard]] bool isSlotReady(SeismicSliceSlot slot) const;

    void setFrameVisible(bool visible);
    [[nodiscard]] bool isFrameVisible() const;

    void setPresetView(SeismicCameraController::PresetView preset);
    void fitToBounds();

    [[nodiscard]] const SeismicCameraController &camera() const { return camera_; }
    [[nodiscard]] SeismicCameraController &camera() { return camera_; }

signals:
    void cameraChanged();
    void volumeLoaded();

protected:
    void initializeGL() override;
    void resizeGL(int w, int h) override;
    void paintGL() override;

    void mousePressEvent(QMouseEvent *event) override;
    void mouseMoveEvent(QMouseEvent *event) override;
    void mouseReleaseEvent(QMouseEvent *event) override;
    void wheelEvent(QWheelEvent *event) override;
    void mouseDoubleClickEvent(QMouseEvent *event) override;

private:
    enum class DragMode { None, Rotate, Pan };

    SeismicCameraController camera_;
    SeismicSliceRenderer sliceRenderer_;
    VolumeFrameRenderer frameRenderer_;
    std::shared_ptr<SgyVolume> volume_;

    DragMode dragMode_ = DragMode::None;
    QPointF lastMousePos_;
    bool glInitialized_ = false;
};

} // namespace seismic
