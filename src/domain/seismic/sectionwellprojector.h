// 层：数据
#pragma once

#include <QColor>
#include <QString>
#include <vector>
#include <glm/glm.hpp>

#include "domain/seismic/timedepthmodel.h"

namespace seismic {

struct WellTopItem {
    QString topName;
    double md = 0.0;
    double tvd = 0.0;
    double twtMs = 0.0;
    QColor color = QColor(QStringLiteral("#1B73D0"));
};

struct WellCurveItem {
    QString curveName;  // e.g. "GR", "DEN", "AC"
    std::vector<double> depthsM;
    std::vector<double> twtMs;
    std::vector<float> values;
    float minVal = 0.0f;
    float maxVal = 100.0f;
    QColor color = QColor(QStringLiteral("#43A047"));
};

struct SectionWellInfo {
    QString wellId;
    QString wellName;
    double surfaceX = 0.0;
    double surfaceY = 0.0;
    double bottomX = 0.0;
    double bottomY = 0.0;
    double totalDepth = 0.0;

    // Projection geometry along the unfolded section line
    double cumulativeDistanceM = 0.0; // Distance from start of section (meters)
    double offsetDistanceM = 0.0;     // Perpendicular offset distance (meters)
    double tracePosition = 0.0;       // Fractional trace column [0, totalTraces - 1]
    bool isWithinBuffer = false;

    std::vector<WellTopItem> tops;
    std::vector<WellCurveItem> curves;
};

// Projects 2D/3D well locations and trajectory markers onto an arbitrary seismic section polyline.
class SectionWellProjector {
public:
    static std::vector<SectionWellInfo> ProjectWells(
        const std::vector<glm::dvec2> &polylinePoints,
        const std::vector<double> &vertexDistancesM,
        const std::vector<double> &traceDistancesM,
        const std::vector<SectionWellInfo> &wells,
        double bufferDistanceM = 500.0,
        const TimeDepthModel &tdModel = TimeDepthModel());
};

} // namespace seismic
