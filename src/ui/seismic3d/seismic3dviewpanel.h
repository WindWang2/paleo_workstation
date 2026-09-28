// 层：视图
#pragma once

#include <QWidget>
#include <QPointer>

#include <memory>

#include "seismic3dviewportwidget.h"
#include "../../services/seismictaskservice.h"

class QSlider;
class QSpinBox;
class QLabel;
class QToolButton;

namespace seismic {

class Seismic3DViewPanel : public QWidget {
    Q_OBJECT
public:
    explicit Seismic3DViewPanel(QWidget *parent = nullptr);
    ~Seismic3DViewPanel() override = default;

    void setTaskService(SeismicTaskService *taskSvc);
    void setVolume(std::shared_ptr<SgyVolume> volume);
    [[nodiscard]] std::shared_ptr<SgyVolume> volume() const;

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

private slots:
    void onInlineSliderChanged(int val);
    void onCrosslineSliderChanged(int val);
    void onTimeSliderChanged(int val);

private:
    void buildUi();
    void requestSliceUpdate(SeismicSliceSlot slot, SgySliceType type, int index);
    void updateTimeMsLabel(int sampleIdx);

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

    // Sliders & Spinboxes
    QSlider *inlineSlider_ = nullptr;
    QSpinBox *inlineSpin_ = nullptr;
    QSlider *xlineSlider_ = nullptr;
    QSpinBox *xlineSpin_ = nullptr;
    QSlider *timeSlider_ = nullptr;
    QSpinBox *timeSpin_ = nullptr;
    QLabel *timeMsLabel_ = nullptr;

    // Track active requests to avoid queue explosion
    int pendingInline_ = -1;
    int pendingCrossline_ = -1;
    int pendingTime_ = -1;
    bool inlineExtracting_ = false;
    bool crosslineExtracting_ = false;
    bool timeExtracting_ = false;
};

} // namespace seismic
