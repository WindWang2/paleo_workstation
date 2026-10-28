// 层：视图
#include "depthtransform.h"

#include <algorithm>
#include <cmath>
#include <numbers> // std::numbers::pi——std::numbers::pi 在 MSVC <cmath> 下不定义
#include <vector>

namespace WellComposite
{

namespace {
} // namespace


void DepthTransform::setDeviationSurvey(const QVector<DeviationStation> &stations)
{
  m_survey.reset();
  m_deviationInvalidReason.clear();
  if (stations.isEmpty())
    return;
  QVector<paleo::DeviationStation> conv;
  conv.reserve(stations.size());
  for (const DeviationStation &s : stations)
    conv.append({s.md, s.inclinationDeg, s.azimuthDeg});
  QString err;
  auto survey = paleo::WellDeviationSurvey::fromStations(conv, &err);
  if (survey)
    m_survey = std::move(*survey);
  else
    m_deviationInvalidReason = err; // 站表在但坏：禁用 + 如实原因，不静默降级
}

QString DepthTransform::deviationUnavailableReason() const
{
  if (hasDeviationSurvey())
    return QString();
  if (!m_deviationInvalidReason.isEmpty())
    return QStringLiteral("井斜站表无效：") + m_deviationInvalidReason;
  return QStringLiteral("井斜测量表缺失：TVD 换算需 .clw/井斜数据（当前井无 deviates 表）");
}

double DepthTransform::mdToTvd(double md) const
{
  return m_survey ? m_survey->tvdAt(md) : md; // 无井斜 = 垂直井，TVD ≡ MD
}

double DepthTransform::tvdToMd(double tvd) const
{
  return m_survey ? m_survey->tvdToMd(tvd) : tvd;
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
  m_twtStations.clear();
  m_twtInvalidReason.clear();
  if (tvdTwtPairs.size() < 2)
    return; // 表缺失/点数不足：保持缺失态（twtUnavailableReason 报「缺失」）

  // 先按深度稳定排序 + 同深去重再校验：上游表乱序或同深重复不再是「整表被拒」
  // （校验器要求深度严格递增，未经排序的合法表会被误判为无效）。
  QVector<QPair<double, double>> sorted = tvdTwtPairs;
  std::stable_sort(sorted.begin(), sorted.end(),
                   [](const QPair<double, double> &a, const QPair<double, double> &b) {
                     return a.first < b.first;
                   });
  sorted.erase(std::unique(sorted.begin(), sorted.end(),
                           [](const QPair<double, double> &a, const QPair<double, double> &b) {
                             return std::abs(a.first - b.first) < 1e-4;
                           }),
               sorted.end());
  if (sorted.size() < 2)
  {
    m_twtInvalidReason = QStringLiteral("去重后不足两个时深点");
    return;
  }

  seismic::TimeDepthModel validated;
  std::vector<seismic::TdPoint> points;
  points.reserve(static_cast<std::size_t>(sorted.size()));
  for (const auto &pair : sorted)
    points.push_back({pair.first, pair.second});
  if (!validated.setCheckshots(points))
  {
    // 站表在但坏：与井斜同口径——记原因、保持禁用态，面板据此区分「表被拒」
    // 与「表缺失」，不再把被拒的表误报成缺失。
    m_twtInvalidReason = QStringLiteral("TVD/TWT 需非负且随时间单调递增（已按深度排序去重）");
    return;
  }
  m_twtStations = sorted;
}

void DepthTransform::setMdTimeDepth(const std::optional<seismic::TimeDepthModel> &model, double shiftMs)
{
  m_mdTimeDepth = model;
  m_timeShiftMs = shiftMs;
}

double DepthTransform::twtAtMd(double md) const
{
  return (m_mdTimeDepth ? m_mdTimeDepth->DepthToTwtMs(md) : twtAtTvd(mdToTvd(md))) + m_timeShiftMs;
}

QString DepthTransform::twtUnavailableReason() const
{
  if (hasTimeDepthTable())
    return QString();
  if (!m_twtInvalidReason.isEmpty())
    return QStringLiteral("时深表无效：") + m_twtInvalidReason;
  return QStringLiteral("时深表缺失：TWT 显示需 VSP/checkshot 时深数据");
}

double DepthTransform::twtAtTvd(double tvd) const
{
  if (m_twtStations.size() < 2 || !std::isfinite(tvd) ||
      tvd < m_twtStations.first().first || tvd > m_twtStations.last().first)
    return qQNaN();
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
  return qQNaN();
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
