// 层：数据
#include "horizontrack.h"

#include <algorithm>
#include <cmath>
#include <cstddef>
#include <cstdint>
#include <deque>
#include <limits>
#include <cstring>

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

// 连续道最小二乘斜率（x = 0..n-1，y = 拾取样点；拾取按构造落在连续道/IL 上）。
// n < 2 → NaN（估计失败）；n ≥ 2 时分母恒正（Σi² 项占优）。
double contiguousSlope(const int *y, int n)
{
  if (n < 2)
    return std::numeric_limits<double>::quiet_NaN();
  double sx = 0.0, sy = 0.0, sxx = 0.0, sxy = 0.0;
  for (int i = 0; i < n; ++i)
  {
    sx += i;
    sy += y[i];
    sxx += double(i) * i;
    sxy += double(i) * y[i];
  }
  return (double(n) * sxy - sx * sy) / (double(n) * sxx - sx * sx);
}

// 倾角引导搜索窗中心（窗顶位）：hist 为本方向最近拾取样点（末位 = 前沿），
// 逻辑序 = 道序。返回 prevTop + round(slope)；估计失败（引导关闭 / 可用
// 拾取 < 2 / 斜率 NaN / |斜率| > 2·maxSearchSamples——预测窗 ±maxSearch 的
// 单步可达上界之外，趋势无从验证）如实回落 prevTop（= 现行隐式行为，斜率 0）。
int guidedCenter(int prevTop, const std::deque<int> &hist,
                 const TrackOptions &options)
{
  if (options.dipHistoryPicks < 2 || hist.size() < 2)
    return prevTop;
  const int n = int(std::min<std::size_t>(options.dipHistoryPicks, hist.size()));
  // deque 分段存储，末 n 个元素不保证内存连续——拷出后再喂斜率估计
  const std::vector<int> recent(hist.end() - n, hist.end());
  const double slope = contiguousSlope(recent.data(), n);
  if (!std::isfinite(slope) || std::abs(slope) > 2 * options.maxSearchSamples)
    return prevTop;
  return prevTop + int(std::lround(slope));
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
    // 显式倾角引导历史（本方向独立；种子为第 1 点，末位恒为前沿）
    std::deque<int> dipHist(1, seed.sample);
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
      // 窗中心：无引导/估计失败 = prevTop（隐式斜率 0）；有引导 = 趋势预测
      const int searchBase = guidedCenter(prevTop, dipHist, options);
      double bestGated = -2.0;  // 过门候选的最佳相关
      double bestUngated = -2.0; // 无视门的最佳相关（区分停因用）
      int bestSample = -1;
      float bestFactor = 1.0f;
      const int searchLo =
          std::max(0, searchBase - options.maxSearchSamples);
      const int searchHi =
          std::min(nSamples - seedWinLen, searchBase + options.maxSearchSamples);
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
      dipHist.push_back(bestSample);
      if (int(dipHist.size()) > std::max(options.dipHistoryPicks, 1))
        dipHist.pop_front();
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
  if (!volume)
  {
    PropagateResult result;
    result.seedIl = seedIl;
    result.stopReason = StopReason::Invalid;
    return result;
  }
  return propagateVolumeWindowed(
      nIl, nXl, nS,
      [volume, nXl, nS](int il) {
        return volume + static_cast<std::size_t>(il) * nXl * nS;
      },
      seedIl, seeds, options, maxInlineStep, cancelled);
}

PropagateResult propagateVolumeWindowed(
    int nIl, int nXl, int nS, const SectionProvider &sectionAt,
    int seedIl, const std::vector<SeedPoint> &seeds,
    const TrackOptions &options, int maxInlineStep,
    const std::function<bool()> &cancelled)
{
  PropagateResult result;
  result.seedIl = seedIl;
  if (!sectionAt || nIl <= 0 || nXl <= 0 || nS <= 0 || seedIl < 0 ||
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
  const float *seedSection = sectionAt(seedIl);
  if (!seedSection)
  {
    result.stopReason = StopReason::ReadFailure;
    finalizePropagation(result);
    return result;
  }
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

  // 逐列倾角引导历史（IL 向）：colHist 每列 K 项按 IL 升序存最近拾取样点
  // （逻辑序 = IL 序），colCount 记当前列历史长度。K = 0 引导关闭。
  const int K = std::max(options.dipHistoryPicks, 0);
  std::vector<int> colHist;
  std::vector<std::uint8_t> colCount;
  if (K > 0)
  {
    colHist.assign(static_cast<std::size_t>(nXl) * K, 0);
    colCount.assign(static_cast<std::size_t>(nXl), 0);
  }
  const auto histReset = [&]() {
    if (K > 0)
      std::fill(colCount.begin(), colCount.end(), std::uint8_t(0));
  };
  const auto histPush = [&](std::size_t xl, int sample) {
    if (K == 0)
      return;
    int *h = &colHist[xl * static_cast<std::size_t>(K)];
    if (colCount[xl] < K)
    {
      h[colCount[xl]] = sample;
      ++colCount[xl];
    }
    else
    {
      std::memmove(h, h + 1,
                   static_cast<std::size_t>(K - 1) * sizeof(int));
      h[K - 1] = sample;
    }
  };
  // 列搜索窗中心：引导关闭/历史 < 2/斜率钳外 → prevTop（现行隐式行为）
  const auto colCenter = [&](std::size_t xl, int prevTop) {
    if (K < 2 || colCount[xl] < 2)
      return prevTop;
    const double slope = contiguousSlope(&colHist[xl * static_cast<std::size_t>(K)],
                                         int(colCount[xl]));
    if (!std::isfinite(slope) || std::abs(slope) > 2 * options.maxSearchSamples)
      return prevTop;
    return prevTop + int(std::lround(slope));
  };

  // 2. 前沿扫掠：逐 IL 向两侧外推（模板 = 前沿剖面同 xl 道窗）
  const std::vector<int> front0 = frontSample;
  bool sawLoss = false;
  for (int dir : {-1, +1})
  {
    frontSample = front0;
    histReset();
    for (std::size_t xl = 0; xl < front0.size(); ++xl)
      if (front0[xl] >= 0)
        histPush(xl, front0[xl]);
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
      const float *prevSection = sectionAt(il - dir);
      const float *curSection = sectionAt(il);
      if (!prevSection || !curSection)
      {
        // 读错误非数据属性：如实中止（不越错误续扫），已得拾取保留
        result.stopReason = StopReason::ReadFailure;
        finalizePropagation(result);
        return result;
      }
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
        const int searchBase = colCenter(static_cast<std::size_t>(xl), prevTop);
        double bestCorr = -2.0;
        int bestSample = -1;
        const int searchLo =
            std::max(0, searchBase - options.maxSearchSamples);
        const int searchHi =
            std::min(nS - winLen, searchBase + options.maxSearchSamples);
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
        for (int xl = 0; xl < nXl; ++xl)
          if (next[static_cast<std::size_t>(xl)] >= 0)
            histPush(static_cast<std::size_t>(xl),
                     next[static_cast<std::size_t>(xl)]);
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
