// 层：数据
#include "domain/seismic/timedepthmodel.h"

#include <algorithm>
#include <cmath>
#include <limits>

namespace seismic {

namespace {
// 单点宽松表由 (depth, timeMs) 反推常速；timeMs <= 0（缺测记 0）推不出
// 有意义速度——std::max(1e-3, 0) 兜底会给出约 2×10⁹ m/s 的常速，
// DepthToTwtMs 随之返回微秒级伪值而不是报错（L1.2 / #592）。合法速度带
// 与构造器/setVelocity/isValid 同界 [100, 20000)。
constexpr double kMinVelocityMPerS = 100.0;
constexpr double kMaxVelocityMPerS = 20000.0;

// 推出单点常速；推不出（时间 <= 0 或落在合法速度带外）→ false，两个方向的
// 换算都返回 NaN（如实报错，不静默给伪值）。
bool singlePointVelocity(const TdPoint &pt, double fallbackVelocity, double *velocityOut)
{
    if (pt.depthM <= 1e-3) {
        *velocityOut = fallbackVelocity;
        return true;
    }
    if (pt.timeMs <= 0.0)
        return false;
    const double v = (pt.depthM * 2000.0) / pt.timeMs;
    if (!(v > kMinVelocityMPerS && v < kMaxVelocityMPerS))
        return false;
    *velocityOut = v;
    return true;
}
} // namespace

TimeDepthModel::TimeDepthModel()
    : m_velocity(2500.0)
{
}

TimeDepthModel::TimeDepthModel(double velocityMPerS)
    // 与 setVelocity/setCalibration 同界 [100,20000)：界外按缺省 2500 回落，
    // 手改时深 JSON 的越界值不再静默生效（opt-02 边界对称）。
    : m_velocity(velocityMPerS > kMinVelocityMPerS && velocityMPerS < kMaxVelocityMPerS
                     ? velocityMPerS
                     : 2500.0)
{
}

void TimeDepthModel::setVelocity(double velocityMPerS) {
    if (velocityMPerS > kMinVelocityMPerS && velocityMPerS < kMaxVelocityMPerS) {
        m_velocity = velocityMPerS;
    }
}

bool TimeDepthModel::setCheckshots(const std::vector<TdPoint> &points) {
  if (points.size() < 2)
    return false;
  for (std::size_t i = 0; i < points.size(); ++i) {
    const auto &p = points[i];
    // #476：首点负深度/负时间也要拒——setPoints 有 >= 0 检查，strict 路径
    // 此前漏了（首点无单调性可查但不等于可为负）。
    if (!std::isfinite(p.depthM) || !std::isfinite(p.timeMs) ||
        p.depthM < 0.0 || p.timeMs < 0.0 ||
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

    // #218：宽松口径同样要求时间随深度严格递增——TwtMsToDepth 按 timeMs
    // 二分，时间回落的脏点会让 lower_bound 在未按时间排序的区间上返回任意
    // 位置（静默错值）。按深度序贪心保留时间严格递增的点，剔除回落/持平点，
    // 保证双向换算互逆、两个方向的二分前置条件同时成立。
    auto keep = m_points.begin();
    for (auto cur = m_points.begin(); cur != m_points.end(); ++cur) {
        if (keep == m_points.begin() || cur->timeMs > (keep - 1)->timeMs)
            *keep++ = *cur;
    }
    m_points.erase(keep, m_points.end());
}

double TimeDepthModel::DepthToTwtMs(double depthM) const {
  if (!std::isfinite(depthM))
    return std::numeric_limits<double>::quiet_NaN();

  if (m_points.empty()) {
    // If strict mode is set without points, cannot interpolate/extrapolate.
    // Otherwise fallback to constant velocity linear formula.
    return m_strict ? std::numeric_limits<double>::quiet_NaN()
                    : (depthM * 2000.0) / m_velocity;
  }

  if (m_strict &&
      (depthM < m_points.front().depthM || depthM > m_points.back().depthM))
    return std::numeric_limits<double>::quiet_NaN();

    if (m_points.size() == 1) {
        // 单点表推不出合法常速（timeMs<=0 的缺测点/速度带外）→ 两个方向都
        // 返回 NaN，不静默给微秒级伪值。
        double v = 0.0;
        if (!singlePointVelocity(m_points[0], m_velocity, &v))
            return std::numeric_limits<double>::quiet_NaN();
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
  if (!std::isfinite(twtMs))
    return std::numeric_limits<double>::quiet_NaN();

  if (m_points.empty()) {
    // depth = (twt * velocity) / 2000.0
    return m_strict ? std::numeric_limits<double>::quiet_NaN()
                    : (twtMs * m_velocity) / 2000.0;
  }

  if (m_strict && (twtMs < m_points.front().timeMs ||
                   twtMs > m_points.back().timeMs))
    return std::numeric_limits<double>::quiet_NaN();

    if (m_points.size() == 1) {
        double v = 0.0;
        if (!singlePointVelocity(m_points[0], m_velocity, &v))
            return std::numeric_limits<double>::quiet_NaN();
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
    return m_velocity > kMinVelocityMPerS && m_velocity < kMaxVelocityMPerS &&
           std::isfinite(m_velocity);
}

} // namespace seismic
