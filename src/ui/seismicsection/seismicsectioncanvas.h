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

namespace seismic {

enum class SectionColorMapType {
    RedWhiteBlue = 0,  // Standard bipolar seismic (Peak=Red, Trough=Blue, Zero=White)
    Grayscale = 1,     // Black/White density
    Rainbow = 2        // Spectrum
};

enum class SectionVerticalUnit {
    TwoWayTimeMs = 0,  // TWT (ms)
    DepthMeters = 1    // Depth (m)
};

enum class SectionOrientation {
    Vertical = 0,   // Inline, Crossline, Arbitrary line (X: traces/dist, Y: TWT ms / depth m)
    TimeSlice = 1   // Horizontal time slice (X: Crosslines, Y: Inlines, Constant TWT ms)
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

    void setOrientation(SectionOrientation orientation);
    SectionOrientation orientation() const { return m_orientation; }

    double currentTimeMs() const { return m_currentTimeMs; }
    int inlineMin() const { return m_inlineMin; }
    int inlineMax() const { return m_inlineMax; }
    int xlineMin() const { return m_xlineMin; }
    int xlineMax() const { return m_xlineMax; }

    void clearData();
    bool hasData() const { return m_traces > 0 && m_samples > 0; }

    // Wells and calibration
    void setWells(const std::vector<SectionWellInfo> &wells);
    void setTimeDepthModel(const TimeDepthModel &model);
    const TimeDepthModel& timeDepthModel() const { return m_tdModel; }

    // Visualization parameters
    void setColorMap(SectionColorMapType type);
    SectionColorMapType colorMap() const { return m_colorMap; }

    void setGain(float gain);
    float gain() const { return m_gain; }

    void setContrast(float contrast);
    float contrast() const { return m_contrast; }

    void setVerticalUnit(SectionVerticalUnit unit);
    SectionVerticalUnit verticalUnit() const { return m_vertUnit; }

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
    void updateHoverInfo(const QPoint &pos);
    QRect viewportRect() const;

    // Data storage
    SgySliceImage m_slice;
    int m_traces = 0;
    int m_samples = 0;
    float m_dtMs = 2.0f;
    double m_t0Ms = 0.0;
    std::vector<float> m_columnDistances;
    std::vector<glm::dvec2> m_mapCoords;

    // Time Slice parameters
    SectionOrientation m_orientation = SectionOrientation::Vertical;
    double m_currentTimeMs = 0.0;
    int m_inlineMin = 1;
    int m_inlineMax = 1;
    int m_xlineMin = 1;
    int m_xlineMax = 1;

    // Image rendering cache
    QImage m_cachedImage;
    SectionColorMapType m_colorMap = SectionColorMapType::RedWhiteBlue;
    float m_gain = 1.0f;
    float m_contrast = 1.45f;
    SectionVerticalUnit m_vertUnit = SectionVerticalUnit::TwoWayTimeMs;

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
    int m_rightMargin = 64;  // Color bar width

    // Interaction state
    bool m_isPanning = false;
    QPoint m_lastMousePos;
    QPoint m_currentMousePos;
    bool m_hasHover = false;
};

} // namespace seismic
