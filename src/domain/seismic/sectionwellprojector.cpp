#include "domain/seismic/sectionwellprojector.h"

#include <algorithm>
#include <cmath>
#include <limits>

namespace seismic {

std::vector<SectionWellInfo> SectionWellProjector::ProjectWells(
    const std::vector<glm::dvec2> &polylinePoints,
    const std::vector<double> &vertexDistancesM,
    const std::vector<double> &traceDistancesM,
    const std::vector<SectionWellInfo> &wells,
    double bufferDistanceM,
    const TimeDepthModel &tdModel)
{
    std::vector<SectionWellInfo> results;
    if (polylinePoints.size() < 2 || wells.empty())
        return results;

    // Calculate vertex distances if not provided
    std::vector<double> vDist = vertexDistancesM;
    if (vDist.size() != polylinePoints.size()) {
        vDist.resize(polylinePoints.size(), 0.0);
        double cum = 0.0;
        for (std::size_t i = 1; i < polylinePoints.size(); ++i) {
            cum += glm::length(polylinePoints[i] - polylinePoints[i - 1]);
            vDist[i] = cum;
        }
    }

    const double totalLengthM = vDist.back();
    const int totalTraces = static_cast<int>(traceDistancesM.size());

    results.reserve(wells.size());
    for (const auto &w : wells) {
        SectionWellInfo proj = w;
        const glm::dvec2 wellPos(w.surfaceX, w.surfaceY);

        double minDistance = std::numeric_limits<double>::infinity();
        double bestCumDist = 0.0;
        double bestOffsetSigned = 0.0;

        for (std::size_t i = 0; i + 1 < polylinePoints.size(); ++i) {
            const glm::dvec2 p0 = polylinePoints[i];
            const glm::dvec2 p1 = polylinePoints[i + 1];
            const glm::dvec2 v = p1 - p0;
            const double len = glm::length(v);
            if (len < 1e-6)
                continue;

            const glm::dvec2 toWell = wellPos - p0;
            const double t = std::clamp(glm::dot(toWell, v) / (len * len), 0.0, 1.0);
            const glm::dvec2 q = p0 + t * v;
            const double dist = glm::length(wellPos - q);

            if (dist < minDistance) {
                minDistance = dist;
                bestCumDist = vDist[i] + t * (vDist[i + 1] - vDist[i]);
                // Sign: cross product (v.x * toWell.y - v.y * toWell.x)
                const double cross = v.x * toWell.y - v.y * toWell.x;
                bestOffsetSigned = (cross >= 0.0 ? 1.0 : -1.0) * dist;
            }
        }

        proj.cumulativeDistanceM = bestCumDist;
        proj.offsetDistanceM = bestOffsetSigned;
        proj.isWithinBuffer = (std::abs(minDistance) <= bufferDistanceM);

        // Map cumulative distance to trace index
        if (totalTraces > 1 && !traceDistancesM.empty()) {
            auto it = std::lower_bound(traceDistancesM.begin(), traceDistancesM.end(), bestCumDist);
            if (it == traceDistancesM.begin()) {
                proj.tracePosition = 0.0;
            } else if (it == traceDistancesM.end()) {
                proj.tracePosition = static_cast<double>(totalTraces - 1);
            } else {
                const std::size_t idx = std::distance(traceDistancesM.begin(), it);
                const double d0 = traceDistancesM[idx - 1];
                const double d1 = traceDistancesM[idx];
                const double span = std::max(1e-4, d1 - d0);
                const double frac = std::clamp((bestCumDist - d0) / span, 0.0, 1.0);
                proj.tracePosition = static_cast<double>(idx - 1) + frac;
            }
        } else if (totalLengthM > 1e-4 && totalTraces > 0) {
            proj.tracePosition = (bestCumDist / totalLengthM) * static_cast<double>(totalTraces - 1);
        } else {
            proj.tracePosition = 0.0;
        }

        // Calibrate Tops
        for (auto &top : proj.tops) {
            if (top.twtMs <= 0.0) {
                const double depth = top.tvd > 0.0 ? top.tvd : top.md;
                top.twtMs = tdModel.DepthToTwtMs(depth);
            }
        }

        // Calibrate Curves
        for (auto &curve : proj.curves) {
            if (curve.twtMs.size() != curve.depthsM.size()) {
                curve.twtMs.clear();
                curve.twtMs.reserve(curve.depthsM.size());
                for (double d : curve.depthsM) {
                    curve.twtMs.push_back(tdModel.DepthToTwtMs(d));
                }
            }
            if (!curve.values.empty()) {
                float vMin = std::numeric_limits<float>::infinity();
                float vMax = -std::numeric_limits<float>::infinity();
                for (float v : curve.values) {
                    if (std::isfinite(v)) {
                        vMin = std::min(vMin, v);
                        vMax = std::max(vMax, v);
                    }
                }
                if (vMin < vMax) {
                    curve.minVal = vMin;
                    curve.maxVal = vMax;
                }
            }
        }

        results.push_back(proj);
    }

    return results;
}

} // namespace seismic
