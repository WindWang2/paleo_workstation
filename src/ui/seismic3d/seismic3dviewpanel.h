// 层：视图
#pragma once

#include <QWidget>
#include <QPointer>
#include <QString>

#include <array>
#include <memory>

#include "seismic3dcolormap.h"
#include "seismic3dfallback.h"
#include "seismic3dviewportwidget.h"
#include "../../services/seismictaskservice.h"

class QCheckBox;
class QComboBox;
class QDoubleSpinBox;
class QSlider;
class QSpinBox;
class QLabel;
class QToolButton;
class QTimer;
class QShowEvent;

namespace seismic {

class Seismic3DViewPanel : public QWidget {
    Q_OBJECT
public:
    explicit Seismic3DViewPanel(QWidget *parent = nullptr);
    ~Seismic3DViewPanel() override = default;

    void setTaskService(SeismicTaskService *taskSvc);
    void setVolume(std::shared_ptr<SgyVolume> volume);
    [[nodiscard]] std::shared_ptr<SgyVolume> volume() const;

    // 渐进 LOD（wave/seismic-engine-deep 主线4）：显式 .sf3p 分页工作区。
    // 拖动滑杆期间切到最粗层（交互跟手），静止 ~350ms 后升回 L0 并重取三槽
    // 切片；仅 paged 通道生效，未设置时保持 Auto 直读/工作区行为。
    void setPagedWorkspace(const QString &sf3pPath);
    [[nodiscard]] QString pagedWorkspace() const { return pagedPath_; }
    [[nodiscard]] int activeLodLevel() const { return activeLod_; }

    [[nodiscard]] Seismic3DViewportWidget *viewport() const { return viewport_; }

    void setInline(int inlineNo);
    void setCrossline(int xlineNo);
    void setTimeSample(int sampleIdx);

    [[nodiscard]] int currentInline() const;
    [[nodiscard]] int currentCrossline() const;
    [[nodiscard]] int currentTimeSample() const;

    // D3.4 井位标记（调用者换算到 inline/xline + sampleFrac）
    void setWells(const std::vector<Seismic3DWell> &wells);
    // D3.12 多体叠加：第二工区轮廓
    void setSecondaryVolume(std::shared_ptr<const SgyVolume> secondary);
    [[nodiscard]] bool hasSecondaryVolume() const;

    // D3.5 当前 colormap（预设名或"自定义"）
    void setColorMap(const Seismic3DColorMap &cmap);
    [[nodiscard]] Seismic3DColorMap colorMap() const { return cmap_; }
    // D3.3 透明度与值域裁剪（0..1 归一化带）
    void setSliceAlpha(float alpha);
    void setValueRange(float minFrac, float maxFrac);

    // D3.9 回退态查询（GL 不可用时视口被 2D 拼接件替换）
    [[nodiscard]] bool isFallbackActive() const { return fallbackActive_; }

    // D3.1 体渲染（切片堆叠）开关
    void setStackModeEnabled(bool enabled);
    [[nodiscard]] bool isStackModeEnabled() const { return stackMode_; }

    // D3.6 相机书签
    void saveCameraBookmark(const QString &name);
    void applyCameraBookmark(int index);
    [[nodiscard]] QStringList cameraBookmarkNames() const;

signals:
    void inlineChanged(int inlineNo);
    void crosslineChanged(int xlineNo);
    void timeChanged(int sampleIdx);
    void lodChanged(const QString &quality); // 质量标签变化（含直读/工作区态）

private slots:
    void onInlineSliderChanged(int val);
    void onCrosslineSliderChanged(int val);
    void onTimeSliderChanged(int val);
    void onSliderPressed();
    void onSliderReleased();

protected:
    // D3.9：GL 看门狗在首个 show 才武装（dock 构造即隐藏，提前计时必误判）。
    void showEvent(QShowEvent *event) override;

private:
    void buildUi();
    void buildDisplayBar();          // D3.x 显示控制行
    void requestSliceUpdate(SeismicSliceSlot slot, SgySliceType type, int index);
    void requestStackLayers();       // D3.1 堆叠层提取（A1/A2：体窗合并通道 + 逐层回落）
    void requestStackLayersPerLayer(const std::vector<int> &samples); // A1 回落：每层一次窄读
    void recolorizeSlice(SeismicSliceSlot slot); // D3.5/D3.3 重着色+上传
    void updateTimeMsLabel(int sampleIdx);
    void switchLod(int level, bool refreshAfter);
    // A2：精化重取。onlyStale=true 时跳过「内容已是 L0 且索引未变」的槽位
    // （拖动只动了一个滑杆——未动的两个槽位整组重取是冗余请求）。
    void refreshVisibleSlices(bool onlyStale = false);
    // A1：体窗堆叠通道的体量估算（paged 按激活 LOD 的 IL/XL 面积因子缩减；
    // 直读无 LOD 全量）。超预算 → 逐层切片回落（窄读）。
    [[nodiscard]] qint64 estimateStackWindowBytes() const;
    // A2：顶替在途切片读（协作取消 + 回调免告警标记）
    void supersedeInFlightSlice(std::size_t slotIndex);
    void updateQualityLabel(const QString &quality);
    void activateFallback();         // D3.9
    void checkMemoryBudget();        // D3.8

    Seismic3DViewportWidget *viewport_ = nullptr;
    QPointer<SeismicTaskService> taskSvc_;

    // Toolbar buttons
    QToolButton *btnIso_ = nullptr;
    QToolButton *btnTop_ = nullptr;
    QToolButton *btnFront_ = nullptr;
    QToolButton *btnSide_ = nullptr;
    QToolButton *btnFit_ = nullptr;
    QToolButton *btnFrame_ = nullptr;
    QToolButton *btnInline_ = nullptr;
    QToolButton *btnCrossline_ = nullptr;
    QToolButton *btnTime_ = nullptr;
    QLabel *qualityLabel_ = nullptr;   // LOD/后端质量标签

    // Sliders & Spinboxes
    QSlider *inlineSlider_ = nullptr;
    QSpinBox *inlineSpin_ = nullptr;
    QSlider *xlineSlider_ = nullptr;
    QSpinBox *xlineSpin_ = nullptr;
    QSlider *timeSlider_ = nullptr;
    QSpinBox *timeSpin_ = nullptr;
    QLabel *timeMsLabel_ = nullptr;

    // Paged/LOD state
    QString pagedPath_;
    int coarsestLod_ = 0;         // progressive 打开后的最粗层级（0 = 无）
    int activeLod_ = 0;
    bool lodSwitchInFlight_ = false;
    QTimer *lodRefineTimer_ = nullptr;

    // Track active requests to avoid queue explosion
    int pendingInline_ = -1;
    int pendingCrossline_ = -1;
    int pendingTime_ = -1;
    bool inlineExtracting_ = false;
    bool crosslineExtracting_ = false;
    bool timeExtracting_ = false;

    // A2（wave/deepen-perf）拖动链路取数合并：
    // slotTasks_     —— 在途任务句柄：新请求顶替时 requestCancel（引擎协作
    //                  中止，不再为已被拖过的索引跑完全程）
    // slotSuperseded_—— 本端主动取消标记（回调据此免打「提取失败」告警）
    // slotCoarse_    —— 槽位内容取自粗 LOD（松手精化只需重取这些槽位）
    std::array<QPointer<PaleoTask>, 3> slotTasks_{};
    std::array<bool, 3> slotSuperseded_{};
    std::array<bool, 3> slotCoarse_{};

    // D3.x 显示控制行控件
    QComboBox *cboColorMap_ = nullptr;
    QToolButton *btnCmapEdit_ = nullptr;       // D3.5 自定义控制点编辑
    QSlider *sliderAlpha_ = nullptr;           // D3.3 透明度
    QDoubleSpinBox *spinRangeMin_ = nullptr;   // D3.3 值域裁剪
    QDoubleSpinBox *spinRangeMax_ = nullptr;
    QToolButton *btnStack_ = nullptr;          // D3.1 体渲染堆叠
    QToolButton *btnShot_ = nullptr;           // D3.7 截图
    QCheckBox *chkFps_ = nullptr;              // D3.10 帧率
    QComboBox *cboCamBookmark_ = nullptr;      // D3.6 相机书签
    QToolButton *btnCamSave_ = nullptr;
    QToolButton *btnCamDel_ = nullptr;
    QLabel *memoryHintLabel_ = nullptr;        // D3.8

    // D3.5/D3.3 渲染状态
    Seismic3DColorMap cmap_;
    bool customCmapActive_ = false;  // false = 引擎预烘焙 rgba 直传
    float alpha_ = 1.0f;
    float rangeMinFrac_ = 0.0f;
    float rangeMaxFrac_ = 1.0f;
    std::array<SgySliceImage, 3> cachedSlices_;   // values 保真缓存（重着色用）
    std::array<int, 3> cachedIndex_{};
    std::array<bool, 3> cachedReady_{};

    // D3.1 堆叠层
    bool stackMode_ = false;
    bool stackExtracting_ = false;
    int stackTargetLayers_ = 0;

    // D3.9 回退
    bool fallbackActive_ = false;
    Seismic3DFallbackWidget *fallback_ = nullptr;
    QTimer *glWatchTimer_ = nullptr;

    // D3.6 相机书签（内存态；QSettings 持久化在 save/apply 中）
    struct CamBookmark {
        QString name;
        SeismicCameraController::CameraState state;
    };
    QList<CamBookmark> camBookmarks_;
};

} // namespace seismic
