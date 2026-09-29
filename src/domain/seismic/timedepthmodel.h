// 层：数据
#pragma once

#include <vector>
#include <string>

namespace seismic {

struct TdPoint {
    double depthM = 0.0;  // True vertical depth (TVD) or MD in meters
    double timeMs = 0.0;  // Two-way travel time (TWT) in milliseconds
};

// Pure domain model for time-to-depth and depth-to-time conversion.
// Supports both a simple velocity model (constant v in m/s) and
// piecewise-linear calibrated time-depth checkshot tables.
class TimeDepthModel {
public:
    TimeDepthModel();
    explicit TimeDepthModel(double velocityMPerS);

    void setVelocity(double velocityMPerS);
    double velocity() const { return m_velocity; }

    // Strict checkshots preserve file order and never extrapolate. Rejection
    // leaves the model unchanged.
    bool setCheckshots(const std::vector<TdPoint> &points);
    void setPoints(const std::vector<TdPoint> &points);
    const std::vector<TdPoint>& points() const { return m_points; }
    bool hasCheckshots() const { return !m_points.empty(); }

    // Bidirectional conversion
    double DepthToTwtMs(double depthM) const;
    double TwtMsToDepth(double twtMs) const;

    // Checks whether the model produces valid non-zero finite results
    bool isValid() const;

private:
  bool m_strict = false;
  double m_velocity = 2500.0; // Default seismic velocity: 2500 m/s
  std::vector<TdPoint> m_points;
};

} // namespace seismic
