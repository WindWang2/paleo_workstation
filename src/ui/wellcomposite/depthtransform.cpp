// 层：视图
#include "depthtransform.h"

#include <algorithm>
#include <cmath>

namespace WellComposite
{

namespace {

// 实际最小曲率 TVD 增量闭合式：ΔTVD = ΔMD/2 · (cosI1+cosI2) · RF
double tvdIncrement(double md1, double inc1Deg, double md2, double inc2Deg)
{
  const double dMd = md2 - md1;
  if (dMd <= 0.0)
    return 0.0;

  const double i1 = inc1Deg * M_PI / 180.0;
  const double i2 = inc2Deg * M_PI / 180.0;

  double cosDog = std::cos(i2 - i1);
  cosDog = std::min(1.0, std::max(-1.0, cosDog));
  const double dogleg = std::acos(cosDog);

  if (dogleg < 1e-9)
    return dMd * 0.5 * (std::cos(i1) + std::cos(i2));

  const double rf = (2.0 / dogleg) * std::tan(dogleg * 0.5);
  return dMd * 0.5 * (std::cos(i1) + std::cos(i2)) * rf;
}

} // namespace

void DepthTransform::setDeviationSurvey(const QVector<DeviationStation> &stations)
{
  m_tvdStations.clear();
  if (stations.isEmpty())
    return;

  auto sorted = stations;
  std::sort(sorted.begin(), sorted.end(),
            [](const DeviationStation &a, const DeviationStation &b) { return a.md < b.md; });

  // 首站前视为垂直井段：TVD 基准取地表（与 MD 同基准，非相对首站）
  double tvd = sorted.first().md * std::cos(sorted.first().inclinationDeg * M_PI / 180.0);
  m_tvdStations.append({sorted.first().md, tvd});
  for (int i = 1; i < sorted.size(); ++i)
  {
    tvd += tvdIncrement(sorted.at(i - 1).md, sorted.at(i - 1).inclinationDeg,
                        sorted.at(i).md, sorted.at(i).inclinationDeg);
    m_tvdStations.append({sorted.at(i).md, tvd});
  }
}

QString DepthTransform::deviationUnavailableReason() const
{
  if (hasDeviationSurvey())
    return QString();
  return QStringLiteral("井斜测量表缺失：TVD 换算需 .clw/井斜数据（当前井无 deviates 表）");
}

double DepthTransform::mdToTvd(double md) const
{
  if (m_tvdStations.isEmpty())
    return md; // 无井斜 = 垂直井，TVD ≡ MD
  if (md <= m_tvdStations.first().first)
    return md - m_tvdStations.first().first + m_tvdStations.first().second;
  if (md >= m_tvdStations.last().first)
  {
    // 末段线性外延
    const int n = m_tvdStations.size();
    const double dm = m_tvdStations.at(n - 1).first - m_tvdStations.at(n - 2).first;
    const double dt = m_tvdStations.at(n - 1).second - m_tvdStations.at(n - 2).second;
    const double ratio = dm > 1e-9 ? dt / dm : 1.0;
    return m_tvdStations.last().second + (md - m_tvdStations.last().first) * ratio;
  }
  for (int i = 1; i < m_tvdStations.size(); ++i)
  {
    if (md <= m_tvdStations.at(i).first)
    {
      const double dm = m_tvdStations.at(i).first - m_tvdStations.at(i - 1).first;
      const double dt = m_tvdStations.at(i).second - m_tvdStations.at(i - 1).second;
      if (dm <= 1e-9)
        return m_tvdStations.at(i).second;
      const double t = (md - m_tvdStations.at(i - 1).first) / dm;
      return m_tvdStations.at(i - 1).second + t * dt;
    }
  }
  return md;
}

double DepthTransform::tvdToMd(double tvd) const
{
  if (m_tvdStations.isEmpty())
    return tvd;
  // 二分站间反插
  for (int i = 1; i < m_tvdStations.size(); ++i)
  {
    if (tvd <= m_tvdStations.at(i).second)
    {
      const double dm = m_tvdStations.at(i).first - m_tvdStations.at(i - 1).first;
      const double dt = m_tvdStations.at(i).second - m_tvdStations.at(i - 1).second;
      if (dt <= 1e-9)
        return m_tvdStations.at(i).first;
      const double t = (tvd - m_tvdStations.at(i - 1).second) / dt;
      return m_tvdStations.at(i - 1).first + t * dm;
    }
  }
  return tvd;
}

void DepthTransform::setKbElevation(double kbMeters)
{
  m_hasKb = true;
  m_kb = kbMeters;
}

double DepthTransform::mdToTvdss(double md) const
{
  return m_hasKb ? m_kb - mdToTvd(md) : mdToTvd(md);
}

void DepthTransform::setTimeDepthTable(const QVector<QPair<double, double>> &tvdTwtPairs)
{
  m_twtStations = tvdTwtPairs;
  std::sort(m_twtStations.begin(), m_twtStations.end(),
            [](const QPair<double, double> &a, const QPair<double, double> &b) {
              return a.first < b.first;
            });
}

QString DepthTransform::twtUnavailableReason() const
{
  if (hasTimeDepthTable())
    return QString();
  return QStringLiteral("时深表缺失：TWT 显示需 VSP/checkshot 时深数据");
}

double DepthTransform::twtAtTvd(double tvd) const
{
  if (m_twtStations.isEmpty())
    return -1.0;
  if (tvd <= m_twtStations.first().first)
    return m_twtStations.first().second;
  for (int i = 1; i < m_twtStations.size(); ++i)
  {
    if (tvd <= m_twtStations.at(i).first)
    {
      const double d0 = m_twtStations.at(i - 1).first;
      const double d1 = m_twtStations.at(i).first;
      const double t0 = m_twtStations.at(i - 1).second;
      const double t1 = m_twtStations.at(i).second;
      if (d1 - d0 <= 1e-9)
        return t1;
      const double f = (tvd - d0) / (d1 - d0);
      return t0 + f * (t1 - t0);
    }
  }
  return m_twtStations.last().second;
}

// ----------------------------------------------------------------------------
// D6.4 采样
// ----------------------------------------------------------------------------
DepthTransform::DepthSample DepthTransform::sampleAt(const CurveData &curve, double depth)
{
  DepthSample s;
  s.depth = depth;
  if (curve.depths.isEmpty())
    return s;

  // 二分最近样本
  int lo = 0, hi = curve.depths.size() - 1;
  while (lo < hi)
  {
    const int mid = (lo + hi) / 2;
    if (curve.depths.at(mid) < depth)
      lo = mid + 1;
    else
      hi = mid;
  }
  int idx = lo;
  if (idx > 0 &&
      std::abs(curve.depths.at(idx) - depth) > std::abs(curve.depths.at(idx - 1) - depth))
    --idx;

  s.value = curve.values.at(idx);
  s.valid = std::isfinite(s.value) && s.value > -900.0f;
  return s;
}

QVector<DepthTransform::DepthSample> DepthTransform::sampleRange(const CurveData &curve,
                                                                 double fromDepth, double toDepth,
                                                                 double stepDepth)
{
  QVector<DepthSample> out;
  if (stepDepth <= 0.0 || toDepth <= fromDepth)
    return out;
  const int n = static_cast<int>((toDepth - fromDepth) / stepDepth) + 1;
  out.reserve(n);
  for (int i = 0; i < n; ++i)
    out << sampleAt(curve, fromDepth + i * stepDepth);
  return out;
}

// ----------------------------------------------------------------------------
// D6.6 LOD 抽稀（min-max 桶）
// ----------------------------------------------------------------------------
QVector<QPair<float, float>> DepthTransform::decimateForLod(const QVector<float> &depths,
                                                            const QVector<float> &values,
                                                            int targetPoints)
{
  QVector<QPair<float, float>> out;
  const int n = qMin(depths.size(), values.size());
  if (n == 0 || targetPoints <= 0 || n <= targetPoints)
  {
    for (int i = 0; i < n; ++i)
      out << qMakePair(depths.at(i), values.at(i));
    return out;
  }

  const int bucketSize = std::max(1, n / targetPoints);
  out.reserve(targetPoints * 3);
  for (int start = 0; start < n; start += bucketSize)
  {
    const int end = std::min(n, start + bucketSize);
    // 桶内保留：首点、末点、最小值点、最大值点（去重）
    int minIdx = -1, maxIdx = -1;
    float minV = std::numeric_limits<float>::max();
    float maxV = std::numeric_limits<float>::lowest();
    for (int i = start; i < end; ++i)
    {
      const float v = values.at(i);
      if (!std::isfinite(v))
        continue;
      if (v < minV)
      {
        minV = v;
        minIdx = i;
      }
      if (v > maxV)
      {
        maxV = v;
        maxIdx = i;
      }
    }
    QList<int> keep;
    if (minIdx >= 0 && maxIdx >= 0)
    {
      if (minIdx <= maxIdx)
        keep << start << minIdx << maxIdx << end - 1;
      else
        keep << start << maxIdx << minIdx << end - 1;
    }
    else
    {
      keep << start << end - 1;
    }
    int last = -1;
    for (int idx : keep)
    {
      if (idx != last)
      {
        out << qMakePair(depths.at(idx), values.at(idx));
        last = idx;
      }
    }
  }
  return out;
}

} // namespace WellComposite
