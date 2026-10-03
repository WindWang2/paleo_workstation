// 层：视图
#pragma once

#include <QWidget>

class QComboBox;
class QDoubleSpinBox;
class QSpinBox;
class QToolButton;
class QProgressBar;
class QLabel;
class QLineEdit;

namespace seismic {

// goal/seismic-inversion 反演参数面板：方法/频带/正则化/迭代上限/子波 →
// 意图信号（视图只发信号，编排归 dock→InversionWorkflow）。busy/进度/结果
// 由 dock 回填 slots。诚实口径：面板明示「带限 + 低频」，不标「高分辨率」。
struct InversionPanelParams {
    QString method = QStringLiteral("bandlimited"); // bandlimited | sparse
    double lowCutHz = 8.0;
    double lambda = 0.0;      // sparse；0 = 自动
    int maxIterations = 200;  // sparse
    QString waveletPath;      // wavelet.json（DERIVED 资产或临时文件）
};

class InversionPanel : public QWidget {
    Q_OBJECT

public:
    explicit InversionPanel(QWidget *parent = nullptr);

    InversionPanelParams currentParams() const;
    void setWaveletPath(const QString &path); // 提取子波后自动回填
    void setBusy(bool busy);
    bool isBusy() const { return m_busy; }

signals:
    void inversionRequested(const seismic::InversionPanelParams &params);
    void cancelRequested(); // 取消在途反演任务
    void extractWaveletRequested(); // 入口挂「提取子波」（井震标定侧）

public slots:
    void updateProgress(int percent, const QString &stageLabel);
    void showResult(bool ok, const QString &summary);

private:
    void buildUi();
    void syncEnabledState();

    QComboBox *m_cboMethod = nullptr;
    QDoubleSpinBox *m_spinLowCut = nullptr;
    QDoubleSpinBox *m_spinLambda = nullptr;
    QSpinBox *m_spinIterations = nullptr;
    QLineEdit *m_editWavelet = nullptr;
    QToolButton *m_btnWaveletBrowse = nullptr;
    QToolButton *m_btnExtractWavelet = nullptr;
    QToolButton *m_btnRun = nullptr;
    QToolButton *m_btnCancel = nullptr;
    QProgressBar *m_progress = nullptr;
    QLabel *m_lblStatus = nullptr;
    bool m_busy = false;
};

} // namespace seismic
