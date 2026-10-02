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
#include "horizonsurfacerenderer.h"
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

    // 属性体挂到现成切片槽 / 堆叠层。GL 未就绪时暂存，initializeGL 后补传。
    // values 有、rgba 空时在这里烘焙（NaN = alpha 0）。不要求已加载地震体。
    bool updatePropertySlice(
        SeismicSliceSlot slot,
        SgySliceType type,
        int index,
        const PropertyBrickAxes &axes,
        const SgySliceImage &image);
    bool updatePropertyStackLayer(
        int layerIdx,
        int kIndex,
        const PropertyBrickAxes &axes,
        const SgySliceImage &image);
    [[nodiscard]] bool hasPendingPropertySlice() const { return !pendingProperty_.empty(); }

    // D3.1 体渲染堆叠层贴图
    bool updateStackLayer(int layerIdx, int sampleIndex, const SgySliceImage &image);
    void setStackVisible(bool visible);
    [[nodiscard]] bool isStackVisible() const;
    void setStackLayerCount(int count); // 交互降采样：拖动 4 / 静止 16
    [[nodiscard]] int stackLayerCount() const { return stackLayerCount_; }

    bool updateLineSlice(
        const std::vector<glm::ivec2> &pathPoints,
        const SgySliceImage &image);

    // D7.2 任意剖面拾取：顶面（时间切片平面）射线求交 → (inline, xline)。
    // autoCommitAtTwo=true 两点即提交（斜剖面）；false 多点累积，回车/双击
    // 提交（栅栏），Esc 取消。预览橡皮线走顶面路径线（UpdateLineSection）。
    void setSectionPickMode(bool enabled, bool autoCommitAtTwo = true);
    [[nodiscard]] bool isSectionPickMode() const { return sectionPickActive_; }
    [[nodiscard]] bool isLineSectionReady() const;
    void clearLineSection();

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

    // D7.1 传递函数：lutRgba 256×4（enable=false 关 TF 路径恢复预烘焙色）。
    // GL 未就绪时暂存，initializeGL 后补传；重传仅 LUT，切片值纹理不动。
    void setTransferFunction(const std::vector<unsigned char> &lutRgba, bool enable);
    [[nodiscard]] bool isTransferFunctionActive() const { return tfActive_; }

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

    // D7.3 井名标注（paintGL 后 QPainter 叠绘；跟随井轨迹顶点）
    void setWellLabelsVisible(bool visible);
    [[nodiscard]] bool wellLabelsVisible() const { return wellLabelsVisible_; }

    // D7.3 层位面上图：全量重建 + 逐层位显隐（GL 未就绪暂存，就绪后补传）
    void setHorizons(const std::vector<Seismic3DHorizonSurface> &items);
    void setHorizonVisible(int index, bool visible);
    [[nodiscard]] bool isHorizonVisible(int index) const;
    [[nodiscard]] int horizonCount() const { return int(horizonItems_.size()); }
    [[nodiscard]] bool areHorizonsVisible() const { return horizonRenderer_.IsVisible(); }
    void setHorizonsVisible(bool visible);

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
    // D7.2 拾取完成（两点=斜剖面；N 点=栅栏）——面板接去服务取数
    void sectionPathCommitted(const std::vector<glm::ivec2> &points);
    void sectionPickModeChanged(bool active);

protected:
    void initializeGL() override;
    void resizeGL(int w, int h) override;
    void paintGL() override;
    void showEvent(QShowEvent *event) override;

    void mousePressEvent(QMouseEvent *event) override;
    void mouseMoveEvent(QMouseEvent *event) override;
    void mouseReleaseEvent(QMouseEvent *event) override;
    void wheelEvent(QWheelEvent *event) override;
    void keyPressEvent(QKeyEvent *event) override;
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

    struct PendingPropertySlice {
        SeismicSliceSlot slot = SeismicSliceSlot::Inline;
        SgySliceType type = SgySliceType::Inline;
        int index = 0;
        PropertyBrickAxes axes;
        SgySliceImage image;
        bool stack = false;
        int stackLayer = 0;
    };

    // D3.2：屏幕坐标是否落在某可见切片面内（四角投影 + 点在四边形内测试）
    SeismicSliceSlot pickSliceAt(const QPointF &pos) const;
    // D3.2：拖动增量 → 新切片索引
    int draggedSliceIndex(SeismicSliceSlot slot, const QPointF &delta) const;
    void applyInertia();

    // D7.2：屏幕点 → 顶面射线求交 → (inline, xline)（吸附真实线号）
    bool pickTopPlaneGrid(const QPointF &pos, glm::ivec2 &outGrid) const;
    void updateSectionPreview();          // 已拾点+悬停点 → 顶面橡皮线
    void commitSectionPath();
    void cancelSectionPick();

    SeismicCameraController camera_;
    SeismicSliceRenderer sliceRenderer_;
    VolumeFrameRenderer frameRenderer_;
    HorizonSurfaceRenderer horizonRenderer_;
    std::shared_ptr<SgyVolume> volume_;
    std::shared_ptr<const SgyVolume> secondaryVolume_;

    std::map<SeismicSliceSlot, PendingSlice> pendingSlices_;
    PendingLineSlice pendingLineSlice_;
    std::vector<PendingPropertySlice> pendingProperty_;

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

    // D7.1 TF（GL 前暂存 + initializeGL 补传）
    std::vector<unsigned char> tfLutBytes_;
    bool tfActive_ = false;

    // D7.2 剖面拾取（两点=斜剖面自动提交；多点=栅栏回车/双击提交）
    bool sectionPickActive_ = false;
    bool sectionAutoCommitTwo_ = true;
    std::vector<glm::ivec2> sectionPickPoints_;
    glm::ivec2 sectionHoverPoint_{0, 0};
    bool sectionHoverValid_ = false;
    std::vector<glm::ivec2> activeSectionPath_; // 已贴剖面路径（取消拾取后恢复其顶面线）

    // D7.3 层位/井标注（GL 前暂存 + initializeGL 补传；井列表供标注投影）
    std::vector<Seismic3DHorizonSurface> horizonItems_;
    bool horizonsPending_ = false;
    std::vector<Seismic3DWell> wellsForLabels_;
    bool wellLabelsVisible_ = false;
};
} // namespace seismic
