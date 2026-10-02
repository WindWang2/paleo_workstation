// 层：数据
#include "lowfreq.h"

// 低频模型实现。IDW 为 inversion 内薄实现（井距 0 精确命中、无半径截断）；
// 不复用 singlefactor 的 PreparedInput/autos 面——那套携带单因子参数语义，
// 会把反演测试耦合到因子夹具（轮0 口径，见 ledger）。

#include <algorithm>
#include <cmath>
#include <cstddef>
#include <limits>

namespace paleo::inversion
{
constexpr float kNan = std::numeric_limits<float>::quiet_NaN();

// 中心移动平均：见 lowfreq.h 注释（lowCut 频段语义的公共低通核）。
void lowCutMovingAverage(const float *x, int n, int half, float *out)
{
  for (int i = 0; i < n; ++i)
  {
    const int lo = std::max(0, i - half);
    const int hi = std::min(n - 1, i + half);
    double sum = 0.0;
    int cnt = 0;
    for (int k = lo; k <= hi; ++k)
    {
      if (std::isfinite(x[k]))
      {
        sum += double(x[k]);
        ++cnt;
      }
    }
    out[i] = cnt > 0 ? float(sum / double(cnt)) : kNan;
  }
}

namespace
{

double cellCenterX(const LowFreqModelInput &in, int il, int xl)
{
  return in.originX + double(il) * in.ilStepX + double(xl) * in.xlStepX;
}

double cellCenterY(const LowFreqModelInput &in, int il, int xl)
{
  return in.originY + double(il) * in.ilStepY + double(xl) * in.xlStepY;
}

// 离格中心最近的格（暴力一次性的小活：井数 × 格数 ≤ 百万级）。
void nearestCell(const LowFreqModelInput &in, double x, double y, int *il, int *xl)
{
  int bestIl = 0;
  int bestXl = 0;
  double best = std::numeric_limits<double>::infinity();
  for (int i = 0; i < in.nIl; ++i)
  {
    for (int j = 0; j < in.nXl; ++j)
    {
      const double dx = cellCenterX(in, i, j) - x;
      const double dy = cellCenterY(in, i, j) - y;
      const double d2 = dx * dx + dy * dy;
      if (d2 < best)
      {
        best = d2;
        bestIl = i;
        bestXl = j;
      }
    }
  }
  *il = bestIl;
  *xl = bestXl;
}

// 横向 IDW：vals[i] 配 wells[i] 的 xy；query 在 (x,y)。d=0 精确命中；
// 全缺失 → NaN。无有限值时返回 quiet NaN。
float idwAt(const std::vector<LowFreqWellTrace> &wells, const std::vector<float> &vals,
            double x, double y, double power)
{
  double num = 0.0;
  double den = 0.0;
  bool exact = false;
  float exactVal = kNan;
  for (std::size_t m = 0; m < wells.size(); ++m)
  {
    if (!std::isfinite(vals[m]))
      continue;
    const double dx = wells[m].x - x;
    const double dy = wells[m].y - y;
    const double d2 = dx * dx + dy * dy;
    if (d2 == 0.0)
    {
      exact = true;
      exactVal = vals[m];
      break;
    }
    const double w = 1.0 / std::pow(d2, power * 0.5);
    num += w * double(vals[m]);
    den += w;
  }
  if (exact)
    return exactVal;
  return den > 0.0 ? float(num / den) : kNan;
}

// 层位平均 TWT（有限格均值；全缺 → NaN）。
double horizonMeanTwt(const HorizonTwtGrid &h)
{
  double sum = 0.0;
  int cnt = 0;
  for (float v : h.twtMs)
  {
    if (std::isfinite(v))
    {
      sum += double(v);
      ++cnt;
    }
  }
  return cnt > 0 ? sum / double(cnt) : std::numeric_limits<double>::quiet_NaN();
}

} // namespace

LowFreqModelResult lowFreqImpedance(const LowFreqModelInput &input)
{
  LowFreqModelResult r;
  if (input.nIl <= 0 || input.nXl <= 0 || input.nS < 8 || !(input.dtMs > 0.0))
  {
    r.reason = "体网格无效（nIl/nXl/nS/dtMs）";
    return r;
  }
  if (!(input.lowCutHz > 0.0) || !(input.idwPower > 0.0))
  {
    r.reason = "lowCutHz/idwPower 必须 > 0";
    return r;
  }
  if (input.wells.empty())
  {
    r.reason = "无井阻抗曲线";
    return r;
  }
  const std::size_t nCells = std::size_t(input.nIl) * std::size_t(input.nXl);
  for (const LowFreqWellTrace &w : input.wells)
  {
    if (int(w.impedance.size()) != input.nS)
    {
      r.reason = "井曲线长度与体时间网格不一致";
      return r;
    }
  }
  for (const HorizonTwtGrid &h : input.horizons)
  {
    if (h.twtMs.size() != nCells)
    {
      r.reason = "层位网格尺寸与体网格不一致: " + h.name;
      return r;
    }
  }

  // 公共头（回填给逐格合成用）。
  r.t0Ms = input.t0Ms;
  r.dtMs = input.dtMs;
  r.nIl = input.nIl;
  r.nXl = input.nXl;
  r.nS = input.nS;
  r.originX = input.originX;
  r.originY = input.originY;
  r.ilStepX = input.ilStepX;
  r.ilStepY = input.ilStepY;
  r.xlStepX = input.xlStepX;
  r.xlStepY = input.xlStepY;
  r.idwPower = input.idwPower;
  r.lowCutHz = input.lowCutHz;

  const int half = std::max(1, int(std::lround(1000.0 / input.lowCutHz / input.dtMs * 0.5)));
  const int nWin = 2 * half + 1;
  r.smoothingWindowMs = double(nWin - 1) * input.dtMs;

  // 有效层位（平均 TWT 有限）按深度升序；全缺的丢弃。
  std::vector<HorizonTwtGrid> sorted;
  sorted.reserve(input.horizons.size());
  for (const HorizonTwtGrid &h : input.horizons)
  {
    if (std::isfinite(horizonMeanTwt(h)))
      sorted.push_back(h);
  }
  std::sort(sorted.begin(), sorted.end(), [](const HorizonTwtGrid &a, const HorizonTwtGrid &b) {
    return horizonMeanTwt(a) < horizonMeanTwt(b);
  });
  const int nB = int(sorted.size());   // 边界数
  const int nL = nB + 1;               // 层数

  if (nB == 0)
  {
    // ---- 回退：全局趋势（井曲线低通后横向 IDW） ---------------------------
    r.layersUsed = 0;
    r.smoothedWells = input.wells;
    std::vector<float> bufA(std::size_t(input.nS));
    for (LowFreqWellTrace &w : r.smoothedWells)
    {
      lowCutMovingAverage(w.impedance.data(), input.nS, half, bufA.data());
      w.impedance = bufA;
    }
    r.ok = true;
    return r;
  }

  // ---- 层模式 -------------------------------------------------------------
  r.layersUsed = nL;
  r.boundaries = std::move(sorted);
  r.boundaryMeanTwt.assign(std::size_t(nB), 0.0);
  for (int b = 0; b < nB; ++b)
    r.boundaryMeanTwt[std::size_t(b)] = horizonMeanTwt(r.boundaries[std::size_t(b)]);

  // 每口井的所属格（层位 TWT 在该格取层间窗）。
  const std::size_t nW = input.wells.size();
  std::vector<int> wellIl(nW), wellXl(nW);
  for (std::size_t m = 0; m < nW; ++m)
    nearestCell(input, input.wells[m].x, input.wells[m].y, &wellIl[m], &wellXl[m]);

  // 井 × 层均值：窗 = 该井格上的层位边界 TWT（缺格回退层位平均 TWT）。
  std::vector<float> wellLayer(nW * std::size_t(nL), kNan);
  for (std::size_t m = 0; m < nW; ++m)
  {
    const std::size_t cell = std::size_t(wellIl[m]) * std::size_t(input.nXl) +
                             std::size_t(wellXl[m]);
    double tops[64];
    double bots[64];
    if (nL > 64)
    {
      r.reason = "层数超上限 63（层位过多）";
      return r;
    }
    for (int b = 0; b < nB; ++b)
    {
      const float v = r.boundaries[std::size_t(b)].twtMs[cell];
      tops[std::size_t(b + 1)] = std::isfinite(v) ? double(v) : r.boundaryMeanTwt[std::size_t(b)];
    }
    tops[0] = -std::numeric_limits<double>::infinity();
    bots[nL - 1] = std::numeric_limits<double>::infinity();
    for (int l = 0; l + 1 < nL; ++l)
      bots[std::size_t(l)] = tops[std::size_t(l + 1)];
    for (int l = 0; l < nL; ++l)
    {
      double sum = 0.0;
      int cnt = 0;
      for (int i = 0; i < input.nS; ++i)
      {
        const float z = input.wells[m].impedance[std::size_t(i)];
        if (!std::isfinite(z))
          continue;
        const double t = input.t0Ms + double(i) * input.dtMs;
        if (t > tops[std::size_t(l)] && t <= bots[std::size_t(l)])
        {
          sum += double(z);
          ++cnt;
        }
      }
      if (cnt > 0)
        wellLayer[m * std::size_t(nL) + std::size_t(l)] = float(sum / double(cnt));
    }
  }

  // 每格每层横向 IDW（全缺井层的全局回退：所有井该层均值的有限平均）。
  std::vector<float> globalLayer(std::size_t(nL), kNan);
  for (int l = 0; l < nL; ++l)
  {
    double sum = 0.0;
    int cnt = 0;
    for (std::size_t m = 0; m < nW; ++m)
    {
      const float v = wellLayer[m * std::size_t(nL) + std::size_t(l)];
      if (std::isfinite(v))
      {
        sum += double(v);
        ++cnt;
      }
    }
    if (cnt > 0)
      globalLayer[std::size_t(l)] = float(sum / double(cnt));
  }

  r.layerImpedance.assign(nCells * std::size_t(nL), kNan);
  std::vector<float> vals(nW);
  for (int il = 0; il < input.nIl; ++il)
  {
    for (int xl = 0; xl < input.nXl; ++xl)
    {
      const double cx = cellCenterX(input, il, xl);
      const double cy = cellCenterY(input, il, xl);
      const std::size_t cell = std::size_t(il) * std::size_t(input.nXl) + std::size_t(xl);
      for (int l = 0; l < nL; ++l)
      {
        for (std::size_t m = 0; m < nW; ++m)
          vals[m] = wellLayer[m * std::size_t(nL) + std::size_t(l)];
        float v = idwAt(input.wells, vals, cx, cy, input.idwPower);
        if (!std::isfinite(v))
          v = globalLayer[std::size_t(l)]; // 无井控层的全局趋势回退
        r.layerImpedance[cell * std::size_t(nL) + std::size_t(l)] = v;
      }
    }
  }

  r.ok = true;
  return r;
}

void lowFreqTraceAt(const LowFreqModelResult &m, int il, int xl, float *out)
{
  const int n = m.nS;
  if (!m.ok || !out || n <= 0 || il < 0 || il >= m.nIl || xl < 0 || xl >= m.nXl)
  {
    for (int i = 0; i < n; ++i)
      out[i] = kNan;
    return;
  }
  const std::size_t cell = std::size_t(il) * std::size_t(m.nXl) + std::size_t(xl);

  if (m.layersUsed == 0)
  {
    // 回退模式：格心 IDW 低通井曲线。
    const double cx = m.originX + double(il) * m.ilStepX + double(xl) * m.xlStepX;
    const double cy = m.originY + double(il) * m.ilStepY + double(xl) * m.xlStepY;
    for (int i = 0; i < n; ++i)
    {
      double num = 0.0;
      double den = 0.0;
      bool exact = false;
      float exactVal = kNan;
      for (const LowFreqWellTrace &w : m.smoothedWells)
      {
        const float v = w.impedance[std::size_t(i)];
        if (!std::isfinite(v))
          continue;
        const double dx = w.x - cx;
        const double dy = w.y - cy;
        const double d2 = dx * dx + dy * dy;
        if (d2 == 0.0)
        {
          exact = true;
          exactVal = v;
          break;
        }
        const double wt = 1.0 / std::pow(d2, m.idwPower * 0.5);
        num += wt * double(v);
        den += wt;
      }
      out[i] = exact ? exactVal : (den > 0.0 ? float(num / den) : kNan);
    }
    return;
  }

  // 层模式：分段常数 → 平滑（窗 = smoothingWindowMs 折算，边缘缩窗）。
  const int nL = m.layersUsed;
  const int nB = nL - 1;
  std::vector<float> raw(std::size_t(n), kNan);
  double tops[64];
  double bots[64];
  for (int b = 0; b < nB; ++b)
  {
    const float v = m.boundaries[std::size_t(b)].twtMs[cell];
    tops[std::size_t(b + 1)] =
        std::isfinite(v) ? double(v) : m.boundaryMeanTwt[std::size_t(b)];
  }
  tops[0] = -std::numeric_limits<double>::infinity();
  bots[nL - 1] = std::numeric_limits<double>::infinity();
  for (int l = 0; l + 1 < nL; ++l)
    bots[std::size_t(l)] = tops[std::size_t(l + 1)];
  for (int i = 0; i < n; ++i)
  {
    const double t = m.t0Ms + double(i) * m.dtMs;
    for (int l = 0; l < nL; ++l)
    {
      if (t > tops[std::size_t(l)] && t <= bots[std::size_t(l)])
      {
        raw[i] = m.layerImpedance[cell * std::size_t(nL) + std::size_t(l)];
        break;
      }
    }
  }
  // smoothingWindowMs = 2·half·dt（见 lowFreqImpedance），反解 half。
  const int half = std::max(1, int(std::lround(m.smoothingWindowMs / m.dtMs * 0.5)));
  lowCutMovingAverage(raw.data(), n, half, out);
}

} // namespace paleo::inversion
