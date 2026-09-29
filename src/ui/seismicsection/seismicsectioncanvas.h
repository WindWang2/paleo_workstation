// 层：视图
#pragma once

#include <QWidget>
#include <QImage>
#include <vector>
#include <glm/glm.hpp>

#include "domain/seismic/nicestep.h"
#include "domain/seismic/sgyvolume.h"
#include "domain/seismic/timedepthmodel.h"
#include "domain/seismic/sectionwellprojector.h"
#include "services/seismictaskservice.h"

namespace seismic {

enum class SectionColorMapType {
    RedWhiteBlue = 0,   // Standard bipolar seismic (Peak=Red, Trough=Blue, Zero=White)
    Grayscale = 1,      // Black/White density
    Rainbow = 2,        // Spectrum
    // ---- D2.8：预设扩到 8 档 ----
    BlueWhiteRed = 3,   // 极性反转观感（Peak=Blue）
    BlackWhiteBlue = 4, // 经典纸面（Peak=Black, Trough=Blue）
    RedWhiteBlack = 5,  // 经典纸面镜像
    GreenWhiteMagenta = 6,
    CyanWhiteOrange = 7
};

// 显示三模（D2.2）：密度 / wiggle 变面积 / 混合
enum class SectionDisplayMode {
    Density = 0,   // 变面积密度
    WiggleVA = 1,  // 波形变面积（正相位涂黑）
    Mixed = 2      // 密度淡显 + 波形叠加
};

enum class SectionVerticalUnit {
    TwoWayTimeMs = 0,  // TWT (ms)
    DepthMeters = 1    // Depth (m)
};

enum class SectionOrientation {
    Vertical = 0,   // Inline, Crossline, Arbitrary line (X: traces/dist, Y: TWT ms / depth m)
    TimeSlice = 1   // Horizontal time slice (X: Crosslines, Y: Inlines, Constant TWT ms)
};

// 增益曲线控制点（D2.4）：TWT → 增益倍数，分段线性
struct SectionGainNode {
    double twtMs = 0.0;
    double gain = 1.0;
};

// D4.1 解释拾取模式：无 / 种子拾取 / 断层线绘制
enum class SectionPickMode {
    None = 0,
    Seed = 1,   // 点击放层位种子点
    Fault = 2   // 拖拽画断层折线
};

// D4.1 剖面身份（列号 → 测线号换算；IL 剖面列=XL 轴，XL 剖面列=IL 轴）
struct SectionRef
{
    SgySliceType type = SgySliceType::Inline;
    int index = 0;
    int colMin = 0;
    int colMax = 0;
    bool valid = false;
};

// 剖面书签快照（D2.12）：线号 + 视口范围，canvas 与 dock 间往返
struct SectionViewState {
    double zoomX = 1.0;
    double zoomY = 1.0;
    double panX = 0.0;
    double panY = 0.0;
};

class SeismicSectionCanvas : public QWidget {
    Q_OBJECT

public:
    explicit SeismicSectionCanvas(QWidget *parent = nullptr);
    ~SeismicSectionCanvas() override = default;

    // Data feed
    void setSectionData(const SgySliceImage &image,
                        float sampleIntervalMs = 2.0f,
                        double startSampleMs = 0.0f,
                        const std::vector<float> &columnDistancesM = {},
                        const std::vector<glm::dvec2> &mapCoords = {});

    void setTimeSliceData(const SgySliceImage &image,
                          double twtMs,
                          int inlineMin, int inlineMax,
                          int xlineMin, int xlineMax);

    // 瓦片渐进时间片（wave/seismic-engine-deep 主线2）：begin 建立全网格
    // NaN 底图并适应窗口；append 只重绘该瓦片区域（分块贴图，不整图重建）；
    // finish 以完整图整体替换但不重置用户视口。
    // 与 setTimeSliceData 的取舍：paged 后端冷缓存首见走瓦片流（引擎焦点
    // 优先，先出中心再补边角）；直读/热缓存路径一次性整图更省。
    void beginTimeSliceTiled(int xlineCount, int inlineCount, double twtMs,
                             int inlineMin, int inlineMax, int xlineMin, int xlineMax);
    void appendTimeSliceTile(const SgySliceImage &tile, int x, int y);
    void finishTimeSliceTiled(const SgySliceImage &full);

    void setOrientation(SectionOrientation orientation);
    SectionOrientation orientation() const { return m_orientation; }

    double currentTimeMs() const { return m_currentTimeMs; }
    int inlineMin() const { return m_inlineMin; }
    int inlineMax() const { return m_inlineMax; }
    int xlineMin() const { return m_xlineMin; }
    int xlineMax() const { return m_xlineMax; }

    void clearData();
    bool hasData() const { return m_traces > 0 && m_samples > 0; }

    // D2.14：空数据/无效线号的原因态（无数据时的占位文案）
    void setNoDataReason(const QString &reason);
    QString noDataReason() const { return m_noDataReason; }

    // ---- D5 井震与任意线 ----
    // 井轨迹（顶/底剖面位置对；空 = 垂直简化）
    struct WellTrajectory
    {
        QString wellId;
        double topTracePos = 0.0;
        double bottomTracePos = 0.0;
        double topTwtMs = 0.0;
        double bottomTwtMs = 0.0;
    };
    void setWellTrajectories(const std::vector<WellTrajectory> &traj);
    const std::vector<WellTrajectory> &wellTrajectories() const { return m_wellTrajectories; }

    // 合成记录 overlay（每井一份；ok=false 时画降级原因注记）
    struct SyntheticOverlay
    {
        QString wellId;
        bool ok = false;
        QString reason;
        std::vector<double> twtMs;
        std::vector<float> amplitude;
    };
    void setSyntheticOverlays(const std::vector<SyntheticOverlay> &overlays);

    // 多井开关：>0 时只显示离剖面最近的 N 口井（D5.7）
    void setMaxVisibleWells(int n);
    int maxVisibleWells() const { return m_maxVisibleWells; }

    // ---- D4 解释 ----
    void setPickMode(SectionPickMode mode);
    SectionPickMode pickMode() const { return m_pickMode; }
    void setSectionRef(const SectionRef &ref) { m_sectionRef = ref; }
    SectionRef sectionRef() const { return m_sectionRef; }
    // 叠加数据（画布只画不存——会话模型在 dock）
    void setPickOverlays(const QList<SeismicPick> &picks, const QList<SeismicFaultSegment> &faults);

    // Wells and calibration
    void setWells(const std::vector<SectionWellInfo> &wells);
    void setTimeDepthModel(const TimeDepthModel &model);
    const TimeDepthModel& timeDepthModel() const { return m_tdModel; }

    // Visualization parameters
    void setColorMap(SectionColorMapType type);
    SectionColorMapType colorMap() const { return m_colorMap; }

    void setColorMapInverted(bool inverted);      // D2.8 反转
    bool colorMapInverted() const { return m_cmapInverted; }

    void setDisplayMode(SectionDisplayMode mode); // D2.2 显示三模
    SectionDisplayMode displayMode() const { return m_displayMode; }

    void setGain(float gain);
    float gain() const { return m_gain; }

    void setContrast(float contrast);
    float contrast() const { return m_contrast; }

    void setAmplitudeThreshold(float threshold);  // D2.3 密度阈值（0=不裁）
    float amplitudeThreshold() const { return m_threshold; }

    void setPolarityInverted(bool inverted);      // D2.3 极性
    bool polarityInverted() const { return m_polarityInverted; }

    void setAgcEnabled(bool enabled, int windowMs); // D2.4 AGC
    bool agcEnabled() const { return m_agcEnabled; }
    int agcWindowMs() const { return m_agcWindowMs; }

    void setGainCurve(const std::vector<SectionGainNode> &nodes); // D2.4 手动增益曲线
    std::vector<SectionGainNode> gainCurve() const { return m_gainCurve; }

    void setVerticalUnit(SectionVerticalUnit unit);
    SectionVerticalUnit verticalUnit() const { return m_vertUnit; }

    void setDualScaleEnabled(bool enabled);       // D2.5 双刻度（TWT+深度）
    bool dualScaleEnabled() const { return m_dualScale; }

    void setShowWells(bool show);
    bool showWells() const { return m_showWells; }

    void setShowFormationTops(bool show);
    bool showFormationTops() const { return m_showTops; }

    void setShowWellCurves(bool show);
    bool showWellCurves() const { return m_showCurves; }

    void setBufferDistanceM(double bufferM);
    double bufferDistanceM() const { return m_bufferDistanceM; }

    // Navigation and scaling
    void zoomIn();
    void zoomOut();
    void resetZoom();
    void fitToWindow();

    double zoomX() const { return m_zoomX; }
    double zoomY() const { return m_zoomY; }

    void setVerticalExaggeration(double factor);  // D2.7 纵向拉伸
    double verticalExaggeration() const { return m_vExag; }

    // D2.12 书签：视口状态存取
    SectionViewState viewState() const;
    void setViewState(const SectionViewState &state);

    // 直接设定缩放/平移（测试与外部驱动；zy<0 时保持当前纵横向比）
    void setZoom(double zx, double zy = -1.0);
    void panBy(int dx, int dy);

    // D2.10 卷帘对比：B 剖面 + 分割位置（0..1，画布横向比例）
    void setCompareData(const SgySliceImage &image, const QString &label);
    void setCompareEnabled(bool enabled);
    bool compareEnabled() const { return m_compareEnabled; }
    void setCurtainPos(double frac);
    double curtainPos() const { return m_curtainPos; }
    QString compareLabel() const { return m_compareLabel; }

    // D2.9 导出：当前画布（图像+坐标轴+色标）按倍率渲染成 PNG
    bool exportPng(const QString &filePath, double scale = 2.0);

    // D2.13 打印/复制
    QImage grabCanvasImage(double scale = 1.0);

    // D2.4 显示值查询（AGC/增益曲线/极性已应用；NaN=缺失）——读数与测试用
    float displayValueAt(int traceIdx, int sampleIdx) const { return displayValue(traceIdx, sampleIdx); }

    // Geometry queries
    int traceCount() const { return m_traces; }
    int sampleCount() const { return m_samples; }
    double totalDistanceM() const;
    double sampleIntervalMs() const { return m_dtMs; }
    double startSampleMs() const { return m_t0Ms; }

    // Coordinates conversion
    double traceToPixelX(double trace) const;
    double timeToPixelY(double twtMs) const;
    double inlineToPixelY(double inlineNo) const;
    double pixelToTrace(double px) const;
    double pixelToTime(double py) const;
    double pixelToInline(double py) const;

signals:
    void traceHovered(int traceIndex, double twtMs, double depthM, float amplitude, double mapX, double mapY);
    void traceClicked(int traceIndex, double twtMs, double depthM, float amplitude, double mapX, double mapY);
    void zoomChanged(double zoomFactor);
    // D2.10 卷帘拖动（状态栏读数用）
    void curtainMoved(double frac);
    // D4.1/D4.4：拾取（列号+TWT；IL/XL 由 dock 经 SectionRef 解析）/ 断层折线
    void pickPlaced(int traceCol, double twtMs);
    void faultDrawn(const QVector<QPair<double, double>> &points); // (traceFrac, twtMs)

protected:
    void paintEvent(QPaintEvent *event) override;
    void resizeEvent(QResizeEvent *event) override;
    void mousePressEvent(QMouseEvent *event) override;
    void mouseMoveEvent(QMouseEvent *event) override;
    void mouseReleaseEvent(QMouseEvent *event) override;
    void wheelEvent(QWheelEvent *event) override;
    void mouseDoubleClickEvent(QMouseEvent *event) override;
    void leaveEvent(QEvent *event) override;

private:
    void rebuildImage();
    // 把 m_slice.values 的 [x0,x0+w)×[y0+h) 区域按当前色标/增益写入
    // m_cachedImage（瓦片流与整图重建共用一条上色路径）。
    void paintValueRegion(int x0, int y0, int w, int h);
    // D2.2 wiggle 变面积渲染（值取自显示缓冲）
    void paintWiggleOverlay(QPainter &p, const QRect &vp, double alpha);
    // D2.6 LOD 抽稀：深度放大/缩小时按步长抽行渲染（min/max 保幅）
    const QImage &lodImage() const;
    // D2.4 AGC/增益曲线后的显示值（hover 读数仍用原始值）
    float displayValue(int traceIdx, int sampleIdx) const;
    void rebuildDisplayValues();
    // D2.8 256 档 LUT（colormap + 反转）
    void rebuildColorLut();
    float gainAtTwt(double twtMs) const;
    void updateHoverInfo(const QPoint &pos);
    QRect viewportRect() const;
    double curtainPixelX() const; // D2.10 卷帘分割线像素位置

    // Data storage
    SgySliceImage m_slice;
    int m_traces = 0;
    int m_samples = 0;
    float m_dtMs = 2.0f;
    double m_t0Ms = 0.0f;
    std::vector<float> m_columnDistances;
    std::vector<glm::dvec2> m_mapCoords;

    // Time Slice parameters
    SectionOrientation m_orientation = SectionOrientation::Vertical;
    double m_currentTimeMs = 0.0;
    int m_inlineMin = 1;
    int m_inlineMax = 1;
    int m_xlineMin = 1;
    int m_xlineMax = 1;

    // D2.14 原因态
    QString m_noDataReason;

    // Image rendering cache
    QImage m_cachedImage;
    // D2.6 LOD 抽稀缓存（键 = 抽稀步长；0 = 无效）
    mutable QImage m_lodCache;
    mutable int m_lodStride = 0;
    // D2.1 纹理缓存：(线号, 增益, colormap, 范围) 键控 LRU
    struct CacheEntry {
        qint64 key = 0;
        QImage image;
    };
    std::vector<CacheEntry> m_textureCache;
    qint64 m_textureCacheKey = 0;
    int m_currentSliceId = 0;

    // D2.2 显示模式 / D2.3 阈值与极性 / D2.8 colormap 与反转
    SectionDisplayMode m_displayMode = SectionDisplayMode::Density;
    SectionColorMapType m_colorMap = SectionColorMapType::RedWhiteBlue;
    bool m_cmapInverted = false;
    float m_threshold = 0.0f;
    bool m_polarityInverted = false;
    std::vector<QRgb> m_colorLut;

    float m_gain = 1.0f;
    float m_contrast = 1.45f;
    SectionVerticalUnit m_vertUnit = SectionVerticalUnit::TwoWayTimeMs;

    // D2.4 AGC 与手动增益曲线
    bool m_agcEnabled = false;
    int m_agcWindowMs = 200;
    std::vector<SectionGainNode> m_gainCurve;
    std::vector<float> m_displayValues; // AGC 处理后的显示缓冲（空 = 直通原始值）

    // D2.5 双刻度
    bool m_dualScale = false;

    // D2.7 纵向拉伸
    double m_vExag = 1.0;

    // D2.10 卷帘对比
    SgySliceImage m_compareSlice;
    QString m_compareLabel;
    bool m_compareEnabled = false;
    double m_curtainPos = 0.5;
    bool m_draggingCurtain = false;
    QImage m_compareImage;

    // Calibration and wells
    TimeDepthModel m_tdModel;
    std::vector<SectionWellInfo> m_wells;
    bool m_showWells = true;
    bool m_showTops = true;
    bool m_showCurves = true;
    double m_bufferDistanceM = 500.0;

    // Pan & zoom transform
    double m_zoomX = 1.0;
    double m_zoomY = 1.0;
    double m_panX = 0.0;
    double m_panY = 0.0;

    // Ruler margins
    int m_leftMargin = 72;   // Vertical ruler width
    int m_topMargin = 60;    // Horizontal ruler and wellpins height
    int m_rightMargin = 64;  // Color bar width（双刻度时兼作深度轴）

    // D4 解释状态
    SectionPickMode m_pickMode = SectionPickMode::None;
    SectionRef m_sectionRef;
    QList<SeismicPick> m_pickOverlays;
    QList<SeismicFaultSegment> m_faultOverlays;

    // D5.3/D5.4/D5.7
    std::vector<WellTrajectory> m_wellTrajectories;
    std::vector<SyntheticOverlay> m_syntheticOverlays;
    int m_maxVisibleWells = 0; // 0 = 全部
    QVector<QPair<double, double>> m_faultDraft; // 绘制中的断层折线

    // Interaction state
    bool m_isPanning = false;
    QPoint m_lastMousePos;
    QPoint m_currentMousePos;
    QPoint m_pressPos;
    bool m_pressMoved = false;
    bool m_hasHover = false;
};

} // namespace seismic
