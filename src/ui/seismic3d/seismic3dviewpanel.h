// 层：视图
#pragma once

#include <QWidget>
#include <QPointer>
#include <QString>

#include <memory>

#include "seismic3dviewportwidget.h"
#include "../../services/seismictaskservice.h"

class QSlider;
class QSpinBox;
class QLabel;
class QToolButton;
class QTimer;

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

private:
    void buildUi();
    void requestSliceUpdate(SeismicSliceSlot slot, SgySliceType type, int index);
    void updateTimeMsLabel(int sampleIdx);
    void switchLod(int level, bool refreshAfter);
    void refreshVisibleSlices();
    void updateQualityLabel(const QString &quality);

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
};

} // namespace seismic
