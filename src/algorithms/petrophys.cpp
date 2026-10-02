// 层：数据
#include "algorithms/petrophys.h"

#include <cmath>
#include <limits>

namespace paleo::petrophys
{
namespace
{
constexpr double kNan = std::numeric_limits<double>::quiet_NaN();

// IGR = (GR − grMin)/(grMax − grMin)，钳 [0,1]；NaN 诚实传播；基线非法 → NaN。
double igreIndex(double gr, double grMin, double grMax)
{
  if (std::isnan(gr) || !(grMax > grMin))
    return kNan;
  const double v = (gr - grMin) / (grMax - grMin);
  if (std::isnan(v))
    return kNan;
  if (v < 0.0)
    return 0.0;
  if (v > 1.0)
    return 1.0;
  return v;
}
} // namespace

void vshGrLinear(const double *gr, int n, double grMin, double grMax, double *out)
{
  for (int i = 0; i < n; ++i)
    out[i] = igreIndex(gr[i], grMin, grMax); // 钳过的 IGR 即线性 Vsh
}

void vshGrLarionovYoung(const double *gr, int n, double grMin, double grMax, double *out)
{
  for (int i = 0; i < n; ++i)
  {
    const double igr = igreIndex(gr[i], grMin, grMax);
    out[i] = std::isnan(igr) ? kNan : 0.083 * (std::pow(2.0, 3.7 * igr) - 1.0);
  }
}

void vshGrLarionovOld(const double *gr, int n, double grMin, double grMax, double *out)
{
  for (int i = 0; i < n; ++i)
  {
    const double igr = igreIndex(gr[i], grMin, grMax);
    out[i] = std::isnan(igr) ? kNan : 0.33 * (std::pow(2.0, 2.0 * igr) - 1.0);
  }
}

void vshGrClavier(const double *gr, int n, double grMin, double grMax, double *out)
{
  for (int i = 0; i < n; ++i)
  {
    const double igr = igreIndex(gr[i], grMin, grMax);
    if (std::isnan(igr))
    {
      out[i] = kNan;
      continue;
    }
    // IGR∈[0,1] → (IGR+0.7)² ≤ 2.89 < 3.38，根号内恒正
    out[i] = 1.7 - std::sqrt(3.38 - (igr + 0.7) * (igr + 0.7));
  }
}

bool grExtrema(const double *gr, int n, double *minOut, double *maxOut)
{
  double lo = kNan, hi = kNan;
  for (int i = 0; i < n; ++i)
  {
    if (std::isnan(gr[i]))
      continue;
    if (std::isnan(lo) || gr[i] < lo)
      lo = gr[i];
    if (std::isnan(hi) || gr[i] > hi)
      hi = gr[i];
  }
  if (std::isnan(lo))
    return false;
  *minOut = lo;
  *maxOut = hi;
  return true;
}

void phiDensity(const double *rhob, int n, double rhoMa, double rhoFluid, double *out)
{
  // 前置 ρma > ρf（骨架密度恒大于孔隙流体；违反 = 参数错，全 NaN 不静默）
  const double denom = rhoMa - rhoFluid;
  const bool ok = rhoMa > rhoFluid && std::isfinite(denom) && denom != 0.0;
  for (int i = 0; i < n; ++i)
    out[i] = (std::isnan(rhob[i]) || !ok) ? kNan : (rhoMa - rhob[i]) / denom;
}

void phiNeutron(const double *nphi, int n, bool inPercent, double *out)
{
  for (int i = 0; i < n; ++i)
    out[i] = (std::isnan(nphi[i]) || !inPercent) ? nphi[i] : nphi[i] / 100.0;
}

void phiSonicWyllie(const double *dt, int n, double dtMa, double dtFluid,
                    double cpFactor, double *out)
{
  const double denom = (dtFluid - dtMa) * cpFactor;
  const bool ok = denom != 0.0 && !std::isnan(denom) && cpFactor > 0.0;
  for (int i = 0; i < n; ++i)
    out[i] = (std::isnan(dt[i]) || !ok) ? kNan : (dt[i] - dtMa) / denom;
}

void swArchie(const double *phi, const double *rt, int n,
              double a, double m, double nSat, double rw, double *out)
{
  const bool paramsOk = a > 0.0 && rw > 0.0 && m > 0.0 && nSat > 0.0;
  for (int i = 0; i < n; ++i)
  {
    if (!paramsOk || std::isnan(phi[i]) || std::isnan(rt[i]) || phi[i] <= 0.0
        || phi[i] > 1.0 || rt[i] <= 0.0)
    {
      out[i] = kNan;
      continue;
    }
    out[i] = std::pow(a * rw / (std::pow(phi[i], m) * rt[i]), 1.0 / nSat);
  }
}

bool resampleLinear(const std::vector<double> &refDepths,
                    const std::vector<double> &srcDepths,
                    const std::vector<double> &srcValues,
                    std::vector<double> &out)
{
  out.assign(refDepths.size(), kNan);
  if (srcDepths.empty() || srcDepths.size() != srcValues.size())
    return srcDepths.empty(); // 空源=整源缺失（合法）；等长违规=报错
  for (size_t k = 1; k < srcDepths.size(); ++k)
  {
    if (!(srcDepths[k] > srcDepths[k - 1]))
    {
      out.assign(refDepths.size(), kNan);
      return false; // 须严格递增
    }
  }
  size_t j = 0; // src 游标：ref 单调即可双指针；ref 非单调也逐点二分退化为线性扫
  for (size_t i = 0; i < refDepths.size(); ++i)
  {
    const double d = refDepths[i];
    if (std::isnan(d) || d < srcDepths.front() || d > srcDepths.back())
      continue; // 出界 → NaN（不外推）
    while (j + 1 < srcDepths.size() && srcDepths[j + 1] < d)
      ++j;
    while (j > 0 && srcDepths[j] > d)
      --j;
    const double d0 = srcDepths[j];
    const double d1 = srcDepths[j + 1 < srcDepths.size() ? j + 1 : j];
    const double v0 = srcValues[j];
    const double v1 = srcValues[j + 1 < srcDepths.size() ? j + 1 : j];
    if (d == d0)
    {
      out[i] = v0; // 恰在样本点上（含边界端点约定）
      continue;
    }
    if (std::isnan(v0) || std::isnan(v1))
      continue; // 插值两端缺失 → NaN
    const double t = (d - d0) / (d1 - d0);
    out[i] = v0 + (v1 - v0) * t;
  }
  return true;
}

CurveStats curveStats(const double *x, int n)
{
  CurveStats s;
  s.n = n;
  double sum = 0.0, sumSq = 0.0;
  for (int i = 0; i < n; ++i)
  {
    if (std::isnan(x[i]))
      continue;
    if (s.valid == 0 || x[i] < s.min)
      s.min = x[i];
    if (s.valid == 0 || x[i] > s.max)
      s.max = x[i];
    sum += x[i];
    sumSq += x[i] * x[i];
    ++s.valid;
  }
  s.nulls = n - s.valid;
  s.nullRate = n > 0 ? static_cast<double>(s.nulls) / n : 0.0;
  if (s.valid == 0)
  {
    s.min = s.max = s.mean = s.stddev = kNan;
    return s;
  }
  s.mean = sum / s.valid;
  if (s.valid >= 2)
  {
    const double var = (sumSq - s.valid * s.mean * s.mean) / (s.valid - 1);
    s.stddev = var > 0.0 ? std::sqrt(var) : 0.0;
  }
  else
  {
    s.stddev = kNan;
  }
  return s;
}

std::vector<DepthInterval> anomalyIntervals(const double *depths, const double *x,
                                            int n, double lo, double hi)
{
  std::vector<DepthInterval> out;
  DepthInterval cur;
  for (int i = 0; i < n; ++i)
  {
    if (std::isnan(x[i]))
    {
      if (cur.samples > 0)
      {
        out.push_back(cur);
        cur = DepthInterval{};
      }
      continue;
    }
    const bool bad = x[i] < lo || x[i] > hi;
    if (bad)
    {
      if (cur.samples == 0)
      {
        cur.fromIndex = i;
        cur.from = depths[i];
      }
      cur.toIndex = i;
      cur.to = depths[i];
      ++cur.samples;
    }
    else if (cur.samples > 0)
    {
      out.push_back(cur);
      cur = DepthInterval{};
    }
  }
  if (cur.samples > 0)
    out.push_back(cur);
  return out;
}

} // namespace paleo::petrophys
