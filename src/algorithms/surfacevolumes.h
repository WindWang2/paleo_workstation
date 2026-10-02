// 层：数据
#pragma once
#include <QString>
#include <vector>

// surfacevolumes — 层位面间体积/面积量算核（纯数值）。
// 积分口径（钉死，与 GDAL pixel-is-area 同约定）：每像元值代表整个像元
// （像元中心矩形法则），体积 = Σ thickness·dx·dy。对分片线性面（棱柱/
// 楔形）该法则精确；对曲面为 O(h) 误差（锥形测试用容差断言）。
// 单位：体积 = z 单位 × 面积单位（z 为米 → m³；z 为 ms → ms·m²，换算
// 成立方米需乘速度场——调用方负责并在报告里如实标注）。
// NaN 语义：任一面 NaN 的像元不计入体积/面积（计入 nullCells，不伪造 0）。
// 符号：volumeBetween = top − base 带符号（底高于顶 → 负，如实报告，
// 不静默取绝对值）；absVolume 为 |thickness| 累计。

namespace paleo::surfacevolumes
{

struct VolumeReport
{
  double cellArea = 0;      // dx·dy
  long long cells = 0;      // 有效格（两输入均非 NaN）
  long long nullCells = 0;  // 任一输入 NaN 的格
  long long positiveCells = 0, negativeCells = 0;
  double volume = 0;        // Σ thickness·A（带符号；aboveDatum 只累计 >0 部分）
  double absVolume = 0;     // Σ |thickness|·A
  double area = 0;          // 有效格数 × cellArea
  double minThickness = 0, maxThickness = 0, meanThickness = 0; // 有效格统计
};

// 层位面间体积：thickness = top − base。两栅格同网格（cols/rows/dx/dy
// 由调用方保证一致——本核只收一份几何）。
bool volumeBetween(const float *top, const float *base, int cols, int rows, double dx,
                   double dy, VolumeReport *out, QString *error = nullptr);

// 基准面上方体积：thickness = max(z − datum, 0)（仅累计正部分；面低于
// 基准的格计入 cells/面积但不贡献体积——与「挖方/填方」的挖方侧语义
// 一致，如实不抵消）。
bool volumeAboveDatum(const float *z, int cols, int rows, double dx, double dy,
                      double datum, VolumeReport *out, QString *error = nullptr);

} // namespace paleo::surfacevolumes
