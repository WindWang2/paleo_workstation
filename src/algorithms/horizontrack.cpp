// 层：数据
#include "horizontrack.h"

#include <algorithm>
#include <cmath>
#include <cstddef>

namespace paleo::hztrack
{
namespace
{

// 零均值 Pearson 相关（NaN 样本对跳过；有效 < 4 → 0——窗破碎即无匹配分）。
// 与服务层 D4.2 实现同式（协方差/方差各自去均值后比值）。
double normalizedCorrelation(const float *w, const float *c, int n)
{
  double sw = 0, sc = 0, sww = 0, scc = 0, swc = 0;
  int valid = 0;
  for (int i = 0; i < n; ++i)
  {
    if (!std::isfinite(w[i]) || !std::isfinite(c[i]))
      continue;
    sw += w[i];
    sc += c[i];
    sww += double(w[i]) * w[i];
    scc += double(c[i]) * c[i];
    swc += double(w[i]) * c[i];
    ++valid;
  }
  if (valid < 4)
    return 0.0;
  const double cov = swc - sw * sc / valid;
  const double varW = sww - sw * sw / valid;
  const double varC = scc - sc * sc / valid;
  const double denom = std::sqrt(varW * varC);
  return denom > 1e-12 ? cov / denom : 0.0;
}

// 窗长钳制；nSamples < 4 时原样返回（调用方以 < 4 判 Invalid，避免
// std::clamp lo>hi 的未定义行为）
int clampWindow(int requested, int nSamples)
{
  if (nSamples < 4)
    return nSamples;
  return std::clamp(requested, 4, nSamples);
}

// 相干门判：返回门值因子（NaN = 弃权 → 1；否则钳 [0,1]）。
// outPass 同时给出门是否放行（阈值 <= 0 时门整体关闭）。
float coherenceFactor(float gate, double threshold, bool *pass)
{
  if (threshold <= 0.0)
  {
    *pass = true;
    return 1.0f;
  }
  if (!std::isfinite(gate))
  {
    *pass = true; // 无信息 ≠ 不合格（相干体边界带语义）
    return 1.0f;
  }
  *pass = double(gate) >= threshold;
  return std::clamp(gate, 0.0f, 1.0f);
}

// 排序成 (il, xl) 字典序并统计覆盖 IL 范围
void finalizePropagation(PropagateResult &r)
{
  std::sort(r.picks.begin(), r.picks.end(),
            [](const VolumePick &a, const VolumePick &b) {
              return a.il != b.il ? a.il < b.il : a.xl < b.xl;
            });
  if (!r.picks.empty())
  {
    r.ilMin = r.picks.front().il;
    r.ilMax = r.picks.back().il;
  }
}

} // namespace

TrackResult trackSection(const float *section, int nTraces, int nSamples,
                         SeedPoint seed, const TrackOptions &options,
                         const float *coherence,
                         const std::function<bool()> &cancelled)
{
  TrackResult result;
  const auto invalidate = [&result]() {
    result.stopLeft.reason = StopReason::Invalid;
    result.stopRight.reason = StopReason::Invalid;
  };
  if (!section || nTraces <= 0 || nSamples <= 0 || seed.trace < 0 ||
      seed.trace >= nTraces || seed.sample < 0 || seed.sample >= nSamples)
  {
    invalidate();
    return result;
  }
  const int winLen = clampWindow(options.windowSamples, nSamples);
  if (options.maxSearchSamples < 0)
  {
    invalidate();
    return result;
  }

  // 种子窗（中心在 seed.sample，向上/向下各半窗，边缘缩窗）
  const int seedTop = std::max(0, seed.sample - winLen / 2);
  const int seedBottom = std::min(nSamples - 1, seedTop + winLen - 1);
  const int seedWinLen = seedBottom - seedTop + 1;
  // 锚点 = 种子在模板窗内的真实偏移（#139）：贴顶/贴底缩窗后不再是窗中心，
  // 拾取样点与下一道窗顶都按锚点换算，否则整条层位被吸向窗中心。
  const int anchor = seed.sample - seedTop;
  if (seedWinLen < 4)
  {
    invalidate();
    return result;
  }
  std::vector<float> seedWave(static_cast<std::size_t>(seedWinLen));
  for (int i = 0; i < seedWinLen; ++i)
    seedWave[static_cast<std::size_t>(i)] =
        section[static_cast<std::size_t>(seed.trace) * nSamples + seedTop + i];

  result.picks.push_back({seed.trace, seed.sample, 1.0f});

  const int dirs[2] = {-1, +1};
  TrackStop *stops[2] = {&result.stopLeft, &result.stopRight};
  bool wasCancelled = false;
  for (int d = 0; d < 2; ++d)
  {
    if (wasCancelled)
    {
      stops[d]->reason = StopReason::Cancelled;
      continue;
    }
    int prevTop = seedTop; // 搜索窗围绕前一道窗口顶（= 隐式倾角引导）
    for (int col = seed.trace + dirs[d]; col >= 0 && col < nTraces; col += dirs[d])
    {
      if (cancelled && cancelled())
      {
        wasCancelled = true;
        stops[d]->reason = StopReason::Cancelled;
        break;
      }
      const float *candTrace =
          section + static_cast<std::size_t>(col) * nSamples;
      double bestGated = -2.0;  // 过门候选的最佳相关
      double bestUngated = -2.0; // 无视门的最佳相关（区分停因用）
      int bestSample = -1;
      float bestFactor = 1.0f;
      const int searchLo =
          std::max(0, prevTop - options.maxSearchSamples);
      const int searchHi =
          std::min(nSamples - seedWinLen, prevTop + options.maxSearchSamples);
      for (int s = searchLo; s <= searchHi; ++s)
      {
        const double corr =
            normalizedCorrelation(seedWave.data(), candTrace + s, seedWinLen);
        if (corr > bestUngated)
          bestUngated = corr;
        const int centerSample = s + anchor;
        bool pass = true;
        const float factor =
            coherence
                ? coherenceFactor(
                      coherence[static_cast<std::size_t>(col) * nSamples +
                                centerSample],
                      options.coherenceThreshold, &pass)
                : 1.0f;
        if (!pass || corr <= bestGated)
          continue;
        bestGated = corr;
        bestSample = centerSample;
        bestFactor = factor;
      }
      if (bestSample < 0 || bestGated < options.correlationThreshold)
      {
        stops[d]->reason =
            bestUngated >= options.correlationThreshold
                ? StopReason::CoherenceGate
                : StopReason::CorrelationLoss;
        stops[d]->trace = col;
        break;
      }
      result.picks.push_back(
          {col, bestSample,
           float(std::clamp(bestGated, 0.0, 1.0)) * bestFactor});
      prevTop = bestSample - anchor;
    }
  }
  std::sort(result.picks.begin(), result.picks.end(),
            [](const TracedPick &a, const TracedPick &b) {
              return a.trace < b.trace;
            });
  return result;
}

std::vector<TracedPick> mergeTraced(const std::vector<TrackResult> &results)
{
  std::vector<TracedPick> merged;
  for (const TrackResult &r : results)
    for (const TracedPick &p : r.picks)
    {
      const auto it =
          std::lower_bound(merged.begin(), merged.end(), p.trace,
                           [](const TracedPick &m, int trace) {
                             return m.trace < trace;
                           });
      if (it != merged.end() && it->trace == p.trace)
      {
        if (p.confidence > it->confidence) // 并列取先入（稳定可重现）
          *it = p;
      }
      else
      {
        merged.insert(it, p);
      }
    }
  return merged;
}

PropagateResult propagateVolume(const float *volume, int nIl, int nXl, int nS,
                                int seedIl, const std::vector<SeedPoint> &seeds,
                                const TrackOptions &options,
                                int maxInlineStep,
                                const std::function<bool()> &cancelled)
{
  PropagateResult result;
  result.seedIl = seedIl;
  if (!volume || nIl <= 0 || nXl <= 0 || nS <= 0 || seedIl < 0 ||
      seedIl >= nIl || seeds.empty())
  {
    result.stopReason = StopReason::Invalid;
    return result;
  }
  const int winLen = clampWindow(options.windowSamples, nS);
  if (winLen < 4 || options.maxSearchSamples < 0)
  {
    result.stopReason = StopReason::Invalid;
    return result;
  }

  // 1. 种子剖面 2D 追踪 + 合并成前沿（trace = xl）
  const float *seedSection =
      volume + static_cast<std::size_t>(seedIl) * nXl * nS;
  std::vector<TrackResult> seedResults;
  seedResults.reserve(seeds.size());
  bool seedCancelled = false;
  for (const SeedPoint &s : seeds)
  {
    seedResults.push_back(
        trackSection(seedSection, nXl, nS, s, options, nullptr, cancelled));
    if (seedResults.back().stopLeft.reason == StopReason::Cancelled ||
        seedResults.back().stopRight.reason == StopReason::Cancelled)
      seedCancelled = true;
  }
  const std::vector<TracedPick> frontPicks = mergeTraced(seedResults);
  if (frontPicks.empty())
  {
    result.stopReason =
        seedCancelled ? StopReason::Cancelled : StopReason::Invalid;
    finalizePropagation(result);
    return result;
  }
  std::vector<int> frontSample(static_cast<std::size_t>(nXl), -1);
  for (const TracedPick &p : frontPicks)
  {
    frontSample[static_cast<std::size_t>(p.trace)] = p.sample;
    result.picks.push_back({seedIl, p.trace, p.sample, p.confidence});
  }
  if (seedCancelled)
  {
    result.stopReason = StopReason::Cancelled; // 种子段已取消：部分结果如实返回
    finalizePropagation(result);
    return result;
  }

  // 2. 前沿扫掠：逐 IL 向两侧外推（模板 = 前沿剖面同 xl 道窗）
  const std::vector<int> front0 = frontSample;
  bool sawLoss = false;
  for (int dir : {-1, +1})
  {
    frontSample = front0;
    bool frontAlive = true;
    for (int step = 1; frontAlive; ++step)
    {
      const int il = seedIl + dir * step;
      if (il < 0 || il >= nIl ||
          (maxInlineStep > 0 && step > maxInlineStep))
        break; // 体边界 / 限步长 = Completed
      if (cancelled && cancelled())
      {
        result.stopReason = StopReason::Cancelled;
        finalizePropagation(result);
        return result;
      }
      const float *prevSection =
          volume + static_cast<std::size_t>(il - dir) * nXl * nS;
      const float *curSection =
          volume + static_cast<std::size_t>(il) * nXl * nS;
      std::vector<int> next(static_cast<std::size_t>(nXl), -1);
      int live = 0;
      for (int xl = 0; xl < nXl; ++xl)
      {
        const int prevSample = frontSample[static_cast<std::size_t>(xl)];
        if (prevSample < 0)
          continue; // 死列不复生（不跨空外推）
        const int prevTop = std::max(
            0, std::min(prevSample - winLen / 2, nS - winLen));
        // 模板窗贴顶/贴底被钳位时，前沿样点在窗内的偏移不再是 winLen/2（#139）。
        const int anchor = prevSample - prevTop;
        const float *tmpl = prevSection +
                            static_cast<std::size_t>(xl) * nS + prevTop;
        double bestCorr = -2.0;
        int bestSample = -1;
        const int searchLo =
            std::max(0, prevTop - options.maxSearchSamples);
        const int searchHi =
            std::min(nS - winLen, prevTop + options.maxSearchSamples);
        for (int s = searchLo; s <= searchHi; ++s)
        {
          const double corr = normalizedCorrelation(
              tmpl, curSection + static_cast<std::size_t>(xl) * nS + s,
              winLen);
          if (corr > bestCorr)
          {
            bestCorr = corr;
            bestSample = s + anchor;
          }
        }
        if (bestSample >= 0 && bestCorr >= options.correlationThreshold)
        {
          next[static_cast<std::size_t>(xl)] = bestSample;
          result.picks.push_back(
              {il, xl, bestSample,
               float(std::clamp(bestCorr, 0.0, 1.0))});
          ++live;
        }
      }
      if (live == 0)
      {
        frontAlive = false;
        sawLoss = true; // 前沿全灭：CorrelationLoss（有边界但同相轴丢了）
      }
      else
      {
        frontSample = std::move(next);
      }
    }
  }
  if (result.stopReason == StopReason::Completed && sawLoss)
    result.stopReason = StopReason::CorrelationLoss;
  finalizePropagation(result);
  return result;
}

} // namespace paleo::hztrack
