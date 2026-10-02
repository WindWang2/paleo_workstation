// 层：数据
#pragma once

#include "stratgrid.h"

#include <QString>

#include <vector>

// upscale — 井轨迹 × 层段求交后的曲线粗化（纯数值）。
//
// 每井 × 每层至多一个代表值。井不穿该层、或穿了但曲线在段内没有有限样本
// → hasValue=false，value 为 NaN，不补 0。
//
// 聚合：
//   · Mean：落在层内的有限样本的算术平均（样本点按 layerOfFraction 归属，
//     界面只进更深一层）；
//   · ThicknessWeightedMean（缺省）：轨迹 MD 弧长加权的分段线性积分。
//     直井且 z=MD 时弧长即垂向厚度。NaN 样本断开，不跨缺失插值；
//   · Median：层内有限样本的中位数（偶数个取中间两值的平均）；
//   · Mode：层内有限样本的众数（计数）。并列时取最小数值，结果确定。
// 代表柱 (columnI, columnJ) 取该层内贡献弧长最长的一段所在柱，供充填落种子。

namespace paleo::stratgrid
{

enum class Aggregator
{
  Mean = 0,
  ThicknessWeightedMean = 1,
  Median = 2,
  Mode = 3
};

struct WellStation
{
  double md = 0;
  double x = 0;
  double y = 0;
  double z = 0;
};

struct CurvePoint
{
  double md = 0;
  double value = 0;
};

struct WellCurve
{
  QString wellId;
  QString curveName;
  std::vector<WellStation> stations; // MD 严格递增；至少 1 个
  std::vector<CurvePoint> curve;
};

struct LayerValue
{
  double value = 0; // 无值时为 NaN
  bool hasValue = false;
  double supportLength = 0; // 参与加权的 MD 长度
  int columnI = -1;
  int columnJ = -1;
};

struct UpscaleTable
{
  int nWells = 0;
  int nLayers = 0;
  std::vector<LayerValue> cells; // well * nLayers + k

  LayerValue &at(int well, int layer) { return cells[static_cast<std::size_t>(well * nLayers + layer)]; }
  const LayerValue &at(int well, int layer) const
  {
    return cells[static_cast<std::size_t>(well * nLayers + layer)];
  }
};

bool upscaleWells(const ZoneGrid &grid, const std::vector<WellCurve> &wells, Aggregator aggregator,
                  UpscaleTable *out, QString *error = nullptr);

QString aggregatorId(Aggregator aggregator);

} // namespace paleo::stratgrid
