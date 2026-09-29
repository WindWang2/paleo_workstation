// 层：视图
#pragma once

#include <QDockWidget>
#include <QLabel>
#include <QToolButton>
#include <QComboBox>
#include <QSlider>
#include <QDoubleSpinBox>
#include <QProgressBar>
#include <QPointer>
#include <memory>

#include "ui/seismicsection/seismicsectioncanvas.h"

class QCheckBox;
class QDialog;
class QTableWidget;

namespace seismic {

// 剖面书签（D2.12）：命名线号 + 视口范围，QSettings 按体身份持久化
struct SectionBookmark {
    QString name;
    int modeIndex = 0;          // 0=IL 1=XL 2=Time 3=任意线
    int sliceValue = 0;
    SectionViewState view;
};

class SeismicSectionDockWidget : public QDockWidget {
    Q_OBJECT

public:
    explicit SeismicSectionDockWidget(QWidget *parent = nullptr);
    explicit SeismicSectionDockWidget(const QString &title, QWidget *parent = nullptr);
    ~SeismicSectionDockWidget() override = default;

    SeismicSectionCanvas *canvas() const { return m_canvas; }

    // Direct data feed
    void setSectionData(const SgySliceImage &image,
                        float sampleIntervalMs = 2.0f,
                        double startSampleMs = 0.0f,
                        const std::vector<float> &columnDistancesM = {},
                        const std::vector<glm::dvec2> &mapCoords = {});

    void setWells(const std::vector<SectionWellInfo> &wells);
    void setTimeDepthModel(const TimeDepthModel &model);
    void setLineTitle(const QString &title);

    // Volume binding and extraction
    void setVolume(std::shared_ptr<const SgyVolume> volume);
    [[nodiscard]] std::shared_ptr<const SgyVolume> volume() const { return m_volume; }
    void setSectionMode(int modeIndex);
    void extractSliceAsync(SgySliceType type, int index);

    // Asynchronous background extraction of arbitrary or well section path
    void extractSectionFromVolumeAsync(
        std::shared_ptr<const SgyVolume> volume,
        const std::vector<glm::ivec2> &pathPoints,
        const QString &lineTitle = QString(),
        const std::vector<glm::dvec2> &mapPolyline = {},
        const std::vector<SectionWellInfo> &candidateWells = {});

    // D2.12 书签列表（测试与外部驱动用）
    QList<SectionBookmark> bookmarks() const { return m_bookmarks; }
    void addBookmark(const QString &name);   // 存当前线号+视口
    void removeBookmark(int index);
    void applyBookmark(int index);

signals:
    void sectionExtractionFinished(bool success, const QString &message);
    void pointClickedOnMap(double x, double y);

private slots:
    void onZoomChanged(double zoom);
    void onTraceHovered(int traceIndex, double twtMs, double depthM, float amplitude, double mapX, double mapY);
    void onExportSnapshot();
    void onSectionModeChanged(int modeIndex);
    void onSliceSliderChanged(int value);
    void onTraceClicked(int traceIndex, double twtMs, double depthM, float amplitude, double mapX, double mapY);
    void onCopyImage();
    void onPrintImage();

private:
    void setupUi();
    void setupDisplayBar(QWidget *parent);          // D2.x 显示控制行
    void showTraceHeaderCard(int traceIndex);       // D2.11
    void updateCompareSlice();                      // D2.10 卷帘 B 图提取
    void saveBookmarksToSettings() const;           // D2.12
    void loadBookmarksFromSettings();
    QString volumeSettingsKey() const;

    SeismicSectionCanvas *m_canvas = nullptr;
    std::shared_ptr<const SgyVolume> m_volume;
    bool m_isExtractingSlice = false;
    int m_pendingSliceIndex = -1;
    SgySliceType m_pendingSliceType = SgySliceType::Inline;

    // Toolbar widgets
    QLabel *m_lblTitle = nullptr;
    QComboBox *m_cboSectionMode = nullptr;
    QWidget *m_sliceGroup = nullptr;
    QLabel *m_lblSliceIndex = nullptr;
    QSlider *m_sliderSlice = nullptr;
    QSpinBox *m_spinSlice = nullptr;
    QLabel *m_lblTimeMs = nullptr;

    QToolButton *m_btnZoomIn = nullptr;
    QToolButton *m_btnZoomOut = nullptr;
    QToolButton *m_btnFit = nullptr;
    QToolButton *m_btnReset = nullptr;
    QToolButton *m_btnUnitToggle = nullptr;
    QComboBox *m_cboColorMap = nullptr;
    QSlider *m_sliderGain = nullptr;
    QDoubleSpinBox *m_spinGain = nullptr;
    QToolButton *m_btnWellOptions = nullptr;
    QToolButton *m_btnExport = nullptr;
    QToolButton *m_btnCopy = nullptr;               // D2.13
    QToolButton *m_btnPrint = nullptr;              // D2.13

    // D2.x 显示控制行
    QComboBox *m_cboDisplayMode = nullptr;          // D2.2
    QCheckBox *m_chkInvert = nullptr;               // D2.8
    QDoubleSpinBox *m_spinThreshold = nullptr;      // D2.3
    QToolButton *m_btnPolarity = nullptr;           // D2.3
    QToolButton *m_btnAgc = nullptr;                // D2.4
    QSpinBox *m_spinAgcWindow = nullptr;            // D2.4
    QToolButton *m_btnGainCurve = nullptr;          // D2.4
    QDoubleSpinBox *m_spinVExag = nullptr;          // D2.7
    QToolButton *m_btnDualScale = nullptr;          // D2.5
    QToolButton *m_btnCurtain = nullptr;            // D2.10
    QSlider *m_sliderCurtain = nullptr;             // D2.10
    QComboBox *m_cboBookmark = nullptr;             // D2.12
    QToolButton *m_btnBookmarkAdd = nullptr;        // D2.12
    QToolButton *m_btnBookmarkDel = nullptr;        // D2.12

    // Status bar widgets
    QLabel *m_lblCoordinates = nullptr;
    QProgressBar *m_progressBar = nullptr;

    // D2.11 道头卡（非模态）
    QPointer<QDialog> m_traceCard;
    QPointer<QTableWidget> m_traceCardTable;

    // D2.12 书签
    QList<SectionBookmark> m_bookmarks;

    // D2.10 卷帘 B 图提取状态
    bool m_extractingCompare = false;
};

} // namespace seismic
