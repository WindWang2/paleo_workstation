// 层：数据
#include "domain/seismic/timedepthmodel.h"

#include <algorithm>
#include <cmath>
#include <limits>

namespace seismic {

TimeDepthModel::TimeDepthModel()
    : m_velocity(2500.0)
{
}

TimeDepthModel::TimeDepthModel(double velocityMPerS)
    : m_velocity(velocityMPerS > 100.0 ? velocityMPerS : 2500.0)
{
}

void TimeDepthModel::setVelocity(double velocityMPerS) {
    if (velocityMPerS > 100.0 && velocityMPerS < 20000.0) {
        m_velocity = velocityMPerS;
    }
}

bool TimeDepthModel::setCheckshots(const std::vector<TdPoint> &points) {
  if (points.size() < 2)
    return false;
  for (std::size_t i = 0; i < points.size(); ++i) {
    const auto &p = points[i];
    if (!std::isfinite(p.depthM) || !std::isfinite(p.timeMs) ||
        (i && (p.depthM <= points[i - 1].depthM ||
               p.timeMs <= points[i - 1].timeMs)))
      return false;
  }
  m_points = points;
  m_strict = true;
  return true;
}

void TimeDepthModel::setPoints(const std::vector<TdPoint> &points) {
  m_strict = false;
  m_points.clear();
  for (const auto &pt : points) {
    if (std::isfinite(pt.depthM) && std::isfinite(pt.timeMs) &&
        pt.depthM >= 0.0 && pt.timeMs >= 0.0) {
      m_points.push_back(pt);
    }
  }
    std::sort(m_points.begin(), m_points.end(), [](const TdPoint &a, const TdPoint &b) {
        return a.depthM < b.depthM;
    });

    // Remove duplicates
    auto it = std::unique(m_points.begin(), m_points.end(), [](const TdPoint &a, const TdPoint &b) {
        return std::abs(a.depthM - b.depthM) < 1e-4;
    });
    m_points.erase(it, m_points.end());
}

double TimeDepthModel::DepthToTwtMs(double depthM) const {
  if (!std::isfinite(depthM) ||
      (m_strict &&
       (depthM < m_points.front().depthM || depthM > m_points.back().depthM)))
    return std::numeric_limits<double>::quiet_NaN();

  if (m_points.empty()) {
    // Linear velocity formula: twt = 2000.0 * depth / velocity
    return (depthM * 2000.0) / m_velocity;
  }

    if (m_points.size() == 1) {
        const double v = m_points[0].depthM > 1e-3
            ? (m_points[0].depthM * 2000.0) / std::max(1e-3, m_points[0].timeMs)
            : m_velocity;
        return (depthM * 2000.0) / v;
    }

    // Extrapolate below or above
    if (depthM <= m_points.front().depthM) {
        const auto &p0 = m_points[0];
        const auto &p1 = m_points[1];
        const double slope = (p1.timeMs - p0.timeMs) / std::max(1e-4, p1.depthM - p0.depthM);
        return p0.timeMs + (depthM - p0.depthM) * slope;
    }

    if (depthM >= m_points.back().depthM) {
        const auto &p0 = m_points[m_points.size() - 2];
        const auto &p1 = m_points.back();
        const double slope = (p1.timeMs - p0.timeMs) / std::max(1e-4, p1.depthM - p0.depthM);
        return p1.timeMs + (depthM - p1.depthM) * slope;
    }

    // Binary search
    auto it = std::lower_bound(m_points.begin(), m_points.end(), depthM, [](const TdPoint &pt, double d) {
        return pt.depthM < d;
    });

    if (it == m_points.begin())
        return it->timeMs;

    const auto &p1 = *it;
    const auto &p0 = *(it - 1);
    const double t = (depthM - p0.depthM) / std::max(1e-4, p1.depthM - p0.depthM);
    return p0.timeMs + t * (p1.timeMs - p0.timeMs);
}

double TimeDepthModel::TwtMsToDepth(double twtMs) const {
  if (!std::isfinite(twtMs) || (m_strict && (twtMs < m_points.front().timeMs ||
                                             twtMs > m_points.back().timeMs)))
    return std::numeric_limits<double>::quiet_NaN();

  if (m_points.empty()) {
    // depth = (twt * velocity) / 2000.0
    return (twtMs * m_velocity) / 2000.0;
  }

    if (m_points.size() == 1) {
        const double v = m_points[0].depthM > 1e-3
            ? (m_points[0].depthM * 2000.0) / std::max(1e-3, m_points[0].timeMs)
            : m_velocity;
        return (twtMs * v) / 2000.0;
    }

    // Find points sorted by time
    if (twtMs <= m_points.front().timeMs) {
        const auto &p0 = m_points[0];
        const auto &p1 = m_points[1];
        const double slope = (p1.depthM - p0.depthM) / std::max(1e-4, p1.timeMs - p0.timeMs);
        return p0.depthM + (twtMs - p0.timeMs) * slope;
    }

    if (twtMs >= m_points.back().timeMs) {
        const auto &p0 = m_points[m_points.size() - 2];
        const auto &p1 = m_points.back();
        const double slope = (p1.depthM - p0.depthM) / std::max(1e-4, p1.timeMs - p0.timeMs);
        return p1.depthM + (twtMs - p1.timeMs) * slope;
    }

    // Binary search by time
    auto it = std::lower_bound(m_points.begin(), m_points.end(), twtMs, [](const TdPoint &pt, double t) {
        return pt.timeMs < t;
    });

    if (it == m_points.begin())
        return it->depthM;

    const auto &p1 = *it;
    const auto &p0 = *(it - 1);
    const double factor = (twtMs - p0.timeMs) / std::max(1e-4, p1.timeMs - p0.timeMs);
    return p0.depthM + factor * (p1.depthM - p0.depthM);
}

bool TimeDepthModel::isValid() const {
    return m_velocity > 100.0 && std::isfinite(m_velocity);
}

} // namespace seismic
