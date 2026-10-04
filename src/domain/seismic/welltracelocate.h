// 层：数据
#pragma once

// domain/seismic/welltracelocate — 井口 XY → 井旁道（IL/XL/剖面列）的唯一换算（#129，header-only）。
//
// 链路：道头坐标拟合的测网仿射（SgyCoordinateMapper，#135 中心化最小二乘）把
// 井口 XY 反算成连续 (IL, XL) → 覆盖检查 → 吸附到体的**实际**线号
// （FindNearest*，线号步长可 >1）→ 在 IL 剖面列轴（xlineValues）上找列。
//
// 覆盖口径：每个方向允许越出首/末线半个线距（按该方向实际步长，而不是固定
// 0.5 个线号）——井口落在边缘道的半个面元内仍取边缘道，再远就如实拒绝。
// 线号表为空（极端旧夹具）时按单位线距退化。

#include <cmath>
#include <string>
#include <vector>

#include "sgycoordinatemapper.h" // → vendor/sbm SgyCoordinateMapper（#135 中心化拟合）
#include "sectionaxis.h"

namespace seismic {

struct WellTraceLocation
{
    bool ok = false;
    std::string error;      //!< 失败原因（人读）
    bool outOfCoverage = false;
    double inlineF = 0.0;   //!< 连续解
    double xlineF = 0.0;
    int inlineNo = 0;       //!< 吸附后的实际线号
    int xlineNo = 0;
    int column = -1;        //!< IL 剖面上的列（xlineValues 中的位置）
};

//! 线号表的典型步长（相邻差的最小正值）；空/单值 → 1。
inline int lineStepOf(const std::vector<int> &lines)
{
    int step = 0;
    for (std::size_t k = 1; k < lines.size(); ++k) {
        const int d = lines[k] - lines[k - 1];
        if (d > 0 && (step == 0 || d < step))
            step = d;
    }
    return step > 0 ? step : 1;
}

inline WellTraceLocation locateWellTrace(const SgyIndex &index, const SgyCoordinateMapper &mapper,
                                         double x, double y)
{
    WellTraceLocation out;
    if (!mapper.valid() || !mapper.MapXY(x, y, out.inlineF, out.xlineF)) {
        out.error = mapper.Describe();
        return out;
    }
    const bool haveIl = !index.inlineValues.empty();
    const bool haveXl = !index.xlineValues.empty();
    const double ilLo = haveIl ? index.inlineValues.front() : index.inlineMin;
    const double ilHi = haveIl ? index.inlineValues.back() : index.inlineMax;
    const double xlLo = haveXl ? index.xlineValues.front() : index.xlineMin;
    const double xlHi = haveXl ? index.xlineValues.back() : index.xlineMax;
    const double ilHalf = 0.5 * lineStepOf(index.inlineValues);
    const double xlHalf = 0.5 * lineStepOf(index.xlineValues);
    if (!(out.inlineF >= ilLo - ilHalf && out.inlineF <= ilHi + ilHalf &&
          out.xlineF >= xlLo - xlHalf && out.xlineF <= xlHi + xlHalf)) {
        out.outOfCoverage = true;
        out.error = "well head is outside the survey coverage";
        return out;
    }
    if (haveIl) {
        out.inlineNo = index.FindNearestInlineValue(float(out.inlineF));
    } else {
        out.inlineNo = int(std::lround(out.inlineF));
    }
    if (haveXl) {
        out.xlineNo = index.FindNearestXlineValue(float(out.xlineF));
    } else {
        out.xlineNo = int(std::lround(out.xlineF));
    }
    out.column = sectionColumnForLine(index.xlineValues, index.xlineMin, out.xlineNo);
    if (out.column < 0) {
        out.error = "snapped xline is not on the inline section axis";
        return out;
    }
    out.ok = true;
    return out;
}

} // namespace seismic
