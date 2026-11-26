// 层：数据
#pragma once

namespace paleo::petrophys {
struct CurveStats
{
  int n = 0;               // 总样本数（含 null）
  int valid = 0;           // 非 NaN 样本
  int nulls = 0;           // NaN 样本
  double nullRate = 0.0;   // nulls / n（n=0 时 0）
  double min = 0.0, max = 0.0;
  double mean = 0.0;
  double stddev = 0.0;     // 样本标准差（n−1 分母；valid<2 → NaN）
};

struct DepthInterval
{
  double from = 0.0, to = 0.0;  // 起止深度（区间内首末样本深度）
  int fromIndex = -1, toIndex = -1;
  int samples = 0;              // 段内样本数
};

} // namespace paleo::petrophys
