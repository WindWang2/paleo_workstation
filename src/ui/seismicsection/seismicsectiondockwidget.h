// 层：视图
#pragma once

#include "services/paleotaskservice.h"
#include "services/seismictaskservice.h"
#include <QComboBox>
#include <QDockWidget>
#include <QDoubleSpinBox>
#include <QLabel>
#include <QPointer>
#include <QProgressBar>
#include <QSlider>
#include <QToolButton>
#include <memory>

#include "services/seismictaskservice.h"
#include "ui/seismicsection/seismicsectioncanvas.h"

class DataCatalog;
class QCheckBox;
class QDialog;
class QTableWidget;
class QUndoStack;

namespace seismic {

class SeismicPickPanel;

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
    ~SeismicSectionDockWidget() override;

    SeismicSectionCanvas *canvas() const { return m_canvas; }

    // Direct data feed
    void setSectionData(const SgySliceImage &image,
                        float sampleIntervalMs = 2.0f,
                        double startSampleMs = 0.0f,
                        const std::vector<float> &columnDistancesM = {},
                        const std::vector<glm::dvec2> &mapCoords = {});

    void setTimeOriginMs(double value) { m_timeOriginMs = value; }
    void setTaskService(SeismicTaskService *service) {
      m_taskService = service;
    }
    void refreshWellOverlay(const std::vector<SectionWellInfo> &wells);
    bool hasRoute() const { return !m_route.empty(); }
    void setWells(const std::vector<SectionWellInfo> &wells);
    void setTimeDepthModel(const TimeDepthModel &model);
    void setLineTitle(const QString &title);
    // 清除已提交的剖面路线（地图连线语义）：hasRoute() 复归 false，
    // 「保存剖面新版本」随之失效；画布图像保留至下一次提取。
    void clearRoute();

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

    // ---- D5 井震与任意线 ----
    void setCandidateWells(const std::vector<SectionWellInfo> &wells); // D5.3/D5.7 候选井（地图链路外亦可注入）
    std::vector<SectionWellInfo> candidateWells() const { return m_candidateWells; }
    void showArbitraryLineEditor();                    // D5.1 多段折线路径编辑器
    void showWellSideTrace();                          // D5.6 井旁道小图（最近井）
    // D5.3 井轨迹投影（顶/底到剖面折线的投影）；TD 无实测表 → 返回带原因注记
    void computeWellTrajectories(const std::vector<glm::dvec2> &mapPolyline);
    void computeSyntheticOverlays();                   // D5.4

    // ---- D4 解释工具 ----
    const SeismicInterpretationSession &interpretationSession() const { return m_session; }
    SeismicInterpretationSession &mutableSession();            // undo 命令写入口
    void refreshInterpretationOverlay();                       // 会话 → 画布叠加+面板
    QString sessionFilePath() const;
    void setInterpretationCatalog(DataCatalog *catalog, const QString &assetId,
                                  const QString &versionId, const QString &outputDir);
    // 画布拾取 → 解析测线号入会话（undo 可撤销）
    void addPickFromCanvas(int traceCol, double twtMs);
    void addPicks(const QList<SeismicPick> &picks);   // 批量（追踪结果）
    void removePick(int id);
    void renamePickHorizon(int id, const QString &newName);
    void addFaultFromCanvas(const QVector<QPair<double, double>> &points);
    bool saveInterpretationSession(QString *error = nullptr);
    bool loadInterpretationSession(QString *error = nullptr);
    QString registerCurrentHorizonAsset(QString *error = nullptr);
    QString registerCurrentFaultAsset(QString *error = nullptr);
    void setTrackSeedPick(int pickId) { m_trackSeedPick = pickId; }
    void setTrackOptions(const SeismicTrackOptions &opt) { m_trackOptions = opt; }
    void runTracking();                                // D4.2 种子追踪
    SeismicPickPanel *pickPanel() const { return m_pickPanel; }
    void setPickMode(SectionPickMode mode);

signals:
  void setupRequested();
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
    // IL/XL/Time 切片在途任务（SeismicTaskService 通道）：同型同号去抖 +
    // 新请求 requestCancel 顶替旧在途（被顶替的读取在逐线检查点退出，
    // 不再占并发闸）。完成回调按「世代号+请求号」守卫丢弃陈旧结果。
    QPointer<PaleoTask> m_sliceTask;
    SgySliceType m_sliceType = SgySliceType::Inline;
    int m_sliceIndex = -1;
    quint64 m_sliceRequest = 0;
    SeismicTaskService *m_taskService = nullptr;
    QPointer<PaleoTask> m_extraction;
    quint64 m_generation = 0;
    double m_timeOriginMs = 0;
    std::vector<glm::dvec2> m_route;
    std::vector<float> m_distances;

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
    QToolButton *m_btnPickMode = nullptr;           // D4.1
    QToolButton *m_btnFaultMode = nullptr;          // D4.4
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

    // D2.10 卷帘 B 图提取状态（同一切片通道，独立在途任务 + 请求号守卫）
    QPointer<PaleoTask> m_compareTask;
    quint64 m_compareRequest = 0;

    // ---- D5 ----
    std::vector<SectionWellInfo> m_candidateWells;
    std::vector<glm::dvec2> m_lastMapPolyline;

    // ---- D4 解释 ----
    SeismicInterpretationSession m_session;
    QUndoStack *m_undoStack = nullptr;
    SeismicPickPanel *m_pickPanel = nullptr;
    SgySliceImage m_lastSlice;                 // 追踪原料（最近一次剖面提取）
    int m_trackSeedPick = -1;
    SeismicTrackOptions m_trackOptions;
    DataCatalog *m_catalog = nullptr;          // 资产登记上下文（app 层注入）
    QString m_catalogAssetId;
    QString m_catalogVersionId;
    QString m_interpretationDir;
    void setupInterpretationUi(QWidget *parent);
};

} // namespace seismic
