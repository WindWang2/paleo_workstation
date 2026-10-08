// 层：视图
#pragma once

#include <QWidget>
#include "services/seismictaskservice.h"

class QComboBox;
class QSpinBox;
class QSlider;
class QToolButton;
class QProgressBar;
class QLabel;

namespace seismic {

// goal/seismic-attributes 属性计算面板：参数表单 → 意图信号（视图只发信号，
// 任务编排归 dock→SeismicTaskService）。busy/进度/结果由 dock 回填 slots。
// 状态语义：计算成功后「登记资产」按钮可用（登记上下文由 app 层注入 dock）。
class SeismicAttrPanel : public QWidget {
    Q_OBJECT

public:
    explicit SeismicAttrPanel(QWidget *parent = nullptr);

    // 表单当前值（offscreen 驱动/测试读数）
    SeismicTaskService::SeismicAttrKind currentKind() const;
    SeismicTaskService::SeismicAttrParams currentParams() const;
    double currentAlpha() const;
    // goal/attr-volume：扫描范围（0=本剖面 1=时间切片 2=属性体）与
    // 时间切片采样位（0=首样；dock 在体加载时回填范围/中位缺省）
    int currentScope() const;
    int currentSampleIndex() const;
    void setVolumeSampleRange(int sampleCount);

    // 叠加透明度变化（画布 setAttrOverlayAlpha 直通）
    void setBusy(bool busy);
    bool isBusy() const { return m_busy; }

signals:
    // 「计算」：对 dock 当前剖面发起属性任务
    void computeRequested(seismic::SeismicTaskService::SeismicAttrKind kind,
                          const seismic::SeismicTaskService::SeismicAttrParams &params,
                          double overlayAlpha);
    // goal/attr-volume：扫描意图（视图只发信号，编排归 dock→服务）
    void timeSliceScanRequested(seismic::SeismicTaskService::SeismicAttrKind kind,
                                const seismic::SeismicTaskService::SeismicAttrParams &params,
                                int sampleIndex);
    void volumeScanRequested(seismic::SeismicTaskService::SeismicAttrKind kind,
                             const seismic::SeismicTaskService::SeismicAttrParams &params);
    void cancelRequested();          // 取消在途任务
    void registerRequested();        // 最近一次成功结果 → catalog 派生资产
    void alphaChanged(double alpha); // 叠加透明度实时调整

public slots:
    void updateProgress(int percent, const QString &stageLabel);
    // registrable=false：扫描类结果（自动登记，不点亮剖面登记按钮）
    void showResult(bool ok, const QString &summary, bool registrable = true);
    // #236：工程边界复位——清忙态/可登记态/状态行。切工程后旧工程的结果
    // 不得留在「可登记」态（接线后会把旧属性登记进新工程 catalog）。
    void clearResult();

private:
    void buildUi();
    void syncEnabledState();

    QComboBox *m_cboKind = nullptr;
    QComboBox *m_cboScope = nullptr;
    QComboBox *m_cboWeight = nullptr;
    QSpinBox *m_spinTimeSample = nullptr;
    QLabel *m_lblTimeSample = nullptr;
    QSpinBox *m_spinWindowHalf = nullptr;
    QSpinBox *m_spinIlHalf = nullptr;
    QSpinBox *m_spinXlHalf = nullptr;
    QSpinBox *m_spinTimeHalf = nullptr;
    QSlider *m_sliderAlpha = nullptr;
    QToolButton *m_btnCompute = nullptr;
    QToolButton *m_btnCancel = nullptr;
    QToolButton *m_btnRegister = nullptr;
    QProgressBar *m_progress = nullptr;
    QLabel *m_lblStatus = nullptr;
    bool m_busy = false;
    bool m_hasResult = false;
    bool m_sampleTouched = false; // 用户显式选过采样位（换体不重置）
};

} // namespace seismic
