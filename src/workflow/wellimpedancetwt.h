// 层：功能
#pragma once

// workflow/wellimpedancetwt — 反演/子波井输入的逐井时深口径（#125，纯数值，header-only）。
//
// 井曲线的 TWT 由 SectionWorkbench 按**该井**时深表/常速近似 + 时间平移算好，
// 存在 WellCurveItem::twtMs（与剖面井叠加同一口径）。反演与子波提取必须消费
// 这份逐井时间轴，不能用画布的全局 TimeDepthModel 重新换算（全局模型默认
// 2500 m/s 常速，会抹掉逐井校正与平移，并给缺时深的井虚构 TWT）。
//
// 规则：
// - 时深对只取 (depth, twt) 均有限的样，按深度排序，并要求深度与 TWT 都严格
//   递增（非单调样丢弃）。
// - 阻抗散样只在时深对覆盖的深度范围内线性插值 TWT，不外推；时深对不足 2 个
//   → 该井不参与（调用方据此跳过并如实提示），绝不回退到默认速度。

#include <algorithm>
#include <cmath>
#include <cstddef>
#include <limits>
#include <utility>
#include <vector>

namespace paleo::inv {

// 追加一条曲线自带的 (depth, twt) 对（长度不等 → 忽略整条曲线）。
inline void appendTimeDepthPairs(const std::vector<double> &depthsM,
                                 const std::vector<double> &twtMs,
                                 std::vector<std::pair<double, double>> *pairs)
{
    if (!pairs || depthsM.size() != twtMs.size())
        return;
    for (std::size_t i = 0; i < depthsM.size(); ++i)
        if (std::isfinite(depthsM[i]) && std::isfinite(twtMs[i]))
            pairs->emplace_back(depthsM[i], twtMs[i]);
}

// 合并后的时深对 → 深度、TWT 双严格递增的表。返回表长 ≥2。
inline bool buildTimeDepthTable(std::vector<std::pair<double, double>> pairs,
                                std::vector<double> *depthM, std::vector<double> *twtMs)
{
    if (!depthM || !twtMs)
        return false;
    depthM->clear();
    twtMs->clear();
    std::sort(pairs.begin(), pairs.end());
    for (const auto &p : pairs) {
        if (!depthM->empty() && (p.first <= depthM->back() || p.second <= twtMs->back()))
            continue;
        depthM->push_back(p.first);
        twtMs->push_back(p.second);
    }
    return depthM->size() >= 2;
}

// 表内线性插值；表外 → NaN（不外推）。
inline double interpTwtNoExtrapolate(const std::vector<double> &depthM,
                                     const std::vector<double> &twtMs, double d)
{
    if (depthM.size() < 2 || depthM.size() != twtMs.size() || !std::isfinite(d) ||
        d < depthM.front() || d > depthM.back())
        return std::numeric_limits<double>::quiet_NaN();
    auto it = std::upper_bound(depthM.begin(), depthM.end(), d);
    if (it == depthM.end())
        return twtMs.back();
    const std::size_t hi = std::size_t(it - depthM.begin());
    const std::size_t lo = hi - 1;
    const double f = (d - depthM[lo]) / (depthM[hi] - depthM[lo]);
    return twtMs[lo] + f * (twtMs[hi] - twtMs[lo]);
}

// AC×DEN → TWT 域阻抗散样（公共深度轴并集、Z=ρ·(1e6/DT)），TWT 取逐井时深表。
inline bool wellImpedanceTwt(const std::vector<double> &acDepthsM, const std::vector<float> &acUsPerM,
                             const std::vector<double> &denDepthsM, const std::vector<float> &denValues,
                             const std::vector<double> &tdDepthM, const std::vector<double> &tdTwtMs,
                             std::vector<double> *twtMs, std::vector<float> *impedance)
{
    if (!twtMs || !impedance || acDepthsM.empty() || acDepthsM.size() != acUsPerM.size() ||
        denDepthsM.empty() || denDepthsM.size() != denValues.size())
        return false;
    twtMs->clear();
    impedance->clear();
    std::vector<double> depths = acDepthsM;
    depths.insert(depths.end(), denDepthsM.begin(), denDepthsM.end());
    std::sort(depths.begin(), depths.end());
    depths.erase(std::unique(depths.begin(), depths.end()), depths.end());
    auto interp = [](const std::vector<double> &xs, const std::vector<float> &ys, double x) -> double {
        if (x <= xs.front())
            return ys.front();
        if (x >= xs.back())
            return ys.back();
        const auto it = std::upper_bound(xs.begin(), xs.end(), x);
        const std::size_t hi = std::size_t(it - xs.begin());
        const std::size_t lo = hi - 1;
        const double f = (x - xs[lo]) / (xs[hi] - xs[lo]);
        return double(ys[lo]) + f * (double(ys[hi]) - double(ys[lo]));
    };
    for (double d : depths) {
        const double dt = interp(acDepthsM, acUsPerM, d);
        const double denV = interp(denDepthsM, denValues, d);
        if (!(dt > 1e-6) || !(denV > 0.0))
            continue;
        const double t = interpTwtNoExtrapolate(tdDepthM, tdTwtMs, d);
        if (!std::isfinite(t))
            continue;
        if (!twtMs->empty() && t <= twtMs->back())
            continue; // 保持严格递增（resampleToGrid 要求有序）
        twtMs->push_back(t);
        impedance->push_back(float(denV * (1e6 / dt)));
    }
    return twtMs->size() >= 2;
}

} // namespace paleo::inv
