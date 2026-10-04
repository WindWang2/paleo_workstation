// 层：数据
#pragma once

// domain/seismic/sectionaxis — 剖面「列 ↔ 测线号」与「样点 ↔ TWT」的唯一换算（header-only）。
//
// #147/#129：SBM 剖面的列按体的实际线号表（XlineValues()/InlineValues()）排列，
// 线号步长可以 >1，所以列号 col 对应 lines[col]，不是 colMin + col。
// #146：剖面时间轴带记录延迟 t0（survey startTimeMs）：twt = t0 + sample·dt，
// sample = round((twt − t0)/dt)。拾取、追踪种子、追踪结果回写都必须走这里。
//
// lines 为空时退回 colMin + col（旧调用方/单测夹具的单位线距语义）。

#include <algorithm>
#include <cmath>
#include <vector>

namespace seismic {

// 列号 → 测线号。越界 → false（不夹取）。
inline bool sectionLineForColumn(const std::vector<int> &lines, int colMin, int col, int *lineNo)
{
    if (col < 0)
        return false;
    if (lines.empty()) {
        if (lineNo)
            *lineNo = colMin + col;
        return true;
    }
    if (col >= int(lines.size()))
        return false;
    if (lineNo)
        *lineNo = lines[std::size_t(col)];
    return true;
}

// 测线号 → 列号（精确匹配；lines 升序）。不在轴上 → -1。
inline int sectionColumnForLine(const std::vector<int> &lines, int colMin, int lineNo)
{
    if (lines.empty())
        return lineNo - colMin;
    const auto it = std::lower_bound(lines.begin(), lines.end(), lineNo);
    if (it == lines.end() || *it != lineNo)
        return -1;
    return int(it - lines.begin());
}

// TWT(ms) → 剖面样点号（含记录延迟 t0）。
inline int sectionSampleForTwt(double twtMs, double t0Ms, double dtMs)
{
    if (!(dtMs > 0.0) || !std::isfinite(twtMs))
        return -1;
    return int(std::lround((twtMs - t0Ms) / dtMs));
}

// 剖面样点号 → TWT(ms)（含记录延迟 t0）。
inline double sectionTwtForSample(int sample, double t0Ms, double dtMs)
{
    return t0Ms + double(sample) * dtMs;
}

} // namespace seismic
