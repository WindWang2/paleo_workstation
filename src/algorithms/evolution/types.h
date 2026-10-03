// 层：数据
#pragma once

#include "../singlefactor/types.h"

#include <string>
#include <vector>

// algorithms/evolution — 沉积体系多期演化的纯数值 DTO（方向35）。
// 复用 singlefactor 的 Point2/Ring/Polygon/GridSpec/Status/Control 口径；
// 无 QtWidgets、无 QGIS 图层、无文件发布。
namespace paleo::evolution
{

using Point2 = singlefactor::Point2;
using Ring = singlefactor::Ring;
using Polygon = singlefactor::Polygon;
using GridSpec = singlefactor::GridSpec;
using Status = singlefactor::Status;
using Control = singlefactor::Control;

// 一期相覆盖中的一个相面片（同一相可由多个面片组成）。
struct FaciesPatch
{
  int faciesCode = 0;
  Polygon geometry;
};

// 一期相覆盖：层位名 + 采样格网口径（同源校验基准）+ 相面片集合。
// grid 是「这期相图从哪个格网来」的声明——两期对比要求同格网/同坐标域，
// 不同源直接拒算，不做重采样或平移近似。
struct FaciesCoverage
{
  std::string horizon;
  GridSpec grid;
  std::vector<FaciesPatch> patches;
};

// 同源校验结果：same=false 时 reason 给出人可读拒算原因（含两侧数值）。
struct DomainCheck
{
  bool same = true;
  std::string reason;
};

// 边界位移矢量（前缘线位移场的一个采样）。
// from = 早期边界采样点；to = 对期（晚期）同相边界的最近点；
// advance = from 落在晚期同相多边形内（进积近似判别）。
struct BoundaryVector
{
  Point2 from;
  Point2 to;
  bool advance = false;
  int faciesCode = 0;
};

// 逐相演化指标。消失相（早期有晚期无）/ 新生相（早期无晚期有）的
// 对侧指标为 NaN，centroid 双侧齐全时才有效。
struct FaciesChange
{
  int faciesCode = 0;
  double areaEarlier = 0; // m²（格网坐标域单位）
  double areaLater = 0;
  double areaChange = 0;    // later − earlier；消失相为负的 areaEarlier
  double areaChangeRatio = 0; // areaChange / areaEarlier；早期为零 → NaN（新生相）
  bool centroidsValid = false;
  Point2 centroidEarlier;
  Point2 centroidLater;
  double centroidDisplacement = 0; // 欧氏距离；双侧不全 → NaN
  // 边界进退（几何近似口径，见 EvolutionResult::methodNote）：
  double boundaryMedianShift = 0; // 采样位移幅度的中位数；无可评边界 → NaN
  double boundaryAdvanceRatio = 0; // 采样点落入晚期同相内的比例
  double boundaryMedianAzimuthDeg = 0; // 位移方向圆均值方位（北起顺时针）；NaN = 无采样
  double boundaryResultant = 0;        // 单位矢平均合长度 [0,1]——低值示方向发散
  int boundarySamples = 0;
};

// 重叠矩阵的一个格：早期相 earlierCode 与晚期相 laterCode 的交叠面积。
struct OverlapCell
{
  int earlierCode = 0;
  int laterCode = 0;
  double area = 0;
};

struct CompareOptions
{
  // 边界采样间距（坐标域单位）。<=0 → 自动 = max(|pixelWidth|, |pixelHeight|)。
  double boundarySampleSpacing = 0;
  // 采样上限：超出时沿边界均匀抽稀（口径记入 methodNote）。
  int maxBoundarySamples = 4000;
};

struct EvolutionResult
{
  Status status = Status::InvalidInput;
  std::string message;
  // 指标口径注记（如实标注：几何近似/无井控加权等），随报告资产透出。
  std::string methodNote;
  std::string earlierHorizon;
  std::string laterHorizon;
  std::vector<int> faciesCodes; // 两期并集，升序
  std::vector<FaciesChange> changes; // 与 faciesCodes 同序
  // 稀疏重叠矩阵（仅非零格）；行列序 = faciesCodes。
  std::vector<OverlapCell> overlap;
  double totalAreaEarlier = 0;
  double totalAreaLater = 0;
  double unchangedArea = 0; // Σ overlap[i][i]，同期同相稳定面积
  // 优势相变更率 = 1 − unchangedArea / totalAreaEarlier；早期总面积为零 → NaN。
  double faciesTurnoverRatio = 0;
  // 前缘线位移矢量场（按 faciesCode 分组，含方向与进退号）。
  std::vector<BoundaryVector> boundaryField;
};

} // namespace paleo::evolution
