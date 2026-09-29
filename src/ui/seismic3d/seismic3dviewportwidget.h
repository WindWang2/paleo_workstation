// 层：视图
#pragma once

#include <QElapsedTimer>
#include <QOpenGLFunctions_3_3_Core>
#include <QOpenGLWidget>
#include <QMouseEvent>
#include <QTimer>
#include <QWheelEvent>

#include <map>
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

    // D3.1 体渲染堆叠层贴图
    bool updateStackLayer(int layerIdx, int sampleIndex, const SgySliceImage &image);
    void setStackVisible(bool visible);
    [[nodiscard]] bool isStackVisible() const;
    void setStackLayerCount(int count); // 交互降采样：拖动 4 / 静止 16
    [[nodiscard]] int stackLayerCount() const { return stackLayerCount_; }

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

    // D3.3 切片透明度（0..1）
    void setSliceAlpha(float alpha);
    [[nodiscard]] float sliceAlpha() const { return sliceRenderer_.sliceAlpha(); }

    // D3.7 截图导出（grabFramebuffer 封装）
    QImage grabViewportImage();

    // D3.10 帧率读数开关（debug）
    void setFpsVisible(bool visible) { fpsVisible_ = visible; update(); }
    [[nodiscard]] bool isFpsVisible() const { return fpsVisible_; }
    [[nodiscard]] double currentFps() const { return fps_; }

    // D3.11 惯性导航开关
    void setInertiaEnabled(bool enabled) { inertiaEnabled_ = enabled; }
    [[nodiscard]] bool isInertiaEnabled() const { return inertiaEnabled_; }

    // D3.4 井位
    void setWells(const std::vector<Seismic3DWell> &wells);

    // D3.12 多体叠加
    void setSecondaryVolume(std::shared_ptr<const SgyVolume> secondary);
    [[nodiscard]] bool hasSecondaryVolume() const { return secondaryVolume_ != nullptr; }

    // D3.9 GL 就绪查询（面板据此决定回退）
    [[nodiscard]] bool isGlReady() const { return glInitialized_; }

    [[nodiscard]] const SeismicCameraController &camera() const { return camera_; }
    [[nodiscard]] SeismicCameraController &camera() { return camera_; }

signals:
    void cameraChanged();
    void volumeLoaded();
    void glReady();
    // D3.2 切片面拖动（联动 2D 剖面：面板更新滑杆并广播 changed 信号）
    void sliceDragged(SeismicSliceSlot slot, int newIndex);
    void sliceHovered(SeismicSliceSlot slot, int index);

protected:
    void initializeGL() override;
    void resizeGL(int w, int h) override;
    void paintGL() override;
    void showEvent(QShowEvent *event) override;

    void mousePressEvent(QMouseEvent *event) override;
    void mouseMoveEvent(QMouseEvent *event) override;
    void mouseReleaseEvent(QMouseEvent *event) override;
    void wheelEvent(QWheelEvent *event) override;
    void mouseDoubleClickEvent(QMouseEvent *event) override;

private:
    enum class DragMode { None, Rotate, Pan, SliceDrag };

    struct PendingSlice {
        SeismicSliceSlot slot;
        SgySliceType type;
        int index;
        SgySliceImage image;
    };

    struct PendingLineSlice {
        std::vector<glm::ivec2> pathPoints;
        SgySliceImage image;
        bool valid = false;
    };

    // D3.2：屏幕坐标是否落在某可见切片面内（四角投影 + 点在四边形内测试）
    SeismicSliceSlot pickSliceAt(const QPointF &pos) const;
    // D3.2：拖动增量 → 新切片索引
    int draggedSliceIndex(SeismicSliceSlot slot, const QPointF &delta) const;
    void applyInertia();

    SeismicCameraController camera_;
    SeismicSliceRenderer sliceRenderer_;
    VolumeFrameRenderer frameRenderer_;
    std::shared_ptr<SgyVolume> volume_;
    std::shared_ptr<const SgyVolume> secondaryVolume_;

    std::map<SeismicSliceSlot, PendingSlice> pendingSlices_;
    PendingLineSlice pendingLineSlice_;

    // 当前三向切片索引（拖动换算基准；面板 setSlice 时同步喂）
    int sliceIndex_[3] = {0, 0, 0};

    DragMode dragMode_ = DragMode::None;
    SeismicSliceSlot dragSlot_ = SeismicSliceSlot::Inline;
    QPointF lastMousePos_;
    bool glInitialized_ = false;
    bool initialFitDone_ = false;

    // D3.10 帧率
    bool fpsVisible_ = false;
    QElapsedTimer fpsClock_;
    int fpsFrames_ = 0;
    double fps_ = 0.0;

    // D3.11 惯性
    bool inertiaEnabled_ = true;
    QPointF rotateVelocity_;
    QTimer inertiaTimer_;

    // D3.1 堆叠层数（交互降采样）
    int stackLayerCount_ = SeismicSliceRenderer::kMaxStackLayers;
};
} // namespace seismic
