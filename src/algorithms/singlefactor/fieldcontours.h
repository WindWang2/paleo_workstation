// 层：数据
#pragma once

#include "types.h"

#include <QString>
#include <QVariantMap>

#include <cstdint>
#include <string>
#include <vector>

// 上游移植：drawing/single_factor/field_contours.py::extract_field_contours
// + contour_work_field.py::local_detour_surface/build_contour_work_surface。
// 输入 = .structural.json 表面模型；输出 = level → polylines 的有序映射。
// 仅覆盖 local_interpretive_detour / field_only 策略路径（上游默认）。

namespace paleo::singlefactor
{

struct FieldContourBarrier
{
  std::vector<Point2> points;
  bool active = true;
  std::string blockMode = "full_block";
};

using LineStringPoints = std::vector<Point2>;

struct ContourPartitionPolicy
{
  std::string geometryPolicy = "field_only";
  std::string bufferPolicy;
  double shapeRadius = 0;
  double shapeStrength = 1.0;
  double workTransitionDistance = 0;
  bool preserveClosed = false;
};

struct FieldContourSurface
{
  std::vector<double> xs;         // grid_x 升序
  std::vector<double> ys;         // grid_y 升序
  std::vector<double> grid;       // 行主序 ny*nx，域外 NaN
  std::vector<std::uint8_t> validMask; // 同尺寸，可空
  std::vector<Polygon> boundaries;
  // 上游 field_model.regions 恒为表面阶段 domain = union(boundaries)
  // ∩ union(interpolation_areas)；与 structural 侧卡一致，存插值范围。
  std::vector<Polygon> interpolationAreas;
  std::vector<FieldContourBarrier> barriers;
  double barrierBufferDistance = 0;
  double contourStopBufferDistance = 0;
  ContourPartitionPolicy partition;
};

struct ContourLevelLines
{
  double level = 0;
  std::vector<std::vector<Point2>> lines; // 上游 dict[level] 顺序
};

struct ContourWorkInfo
{
  double coreValue = 0;
  double bufferHalfWidth = 0;
  double numericalGuard = 0;
  double transitionDistance = 0;
  int modifiedCells = 0;
};

struct FieldContourResult
{
  Status status = Status::InvalidInput;
  std::string message;
  double meshStep = 0;                  // min(dx, dy) —— 制图步长
  std::vector<ContourLevelLines> initial; // field_only 提取（golden initial_contours）
  bool detourApplied = false;
  std::vector<int> crossedBarrierIndices; // surface.barriers 下标
  bool workBuilt = false;
  ContourWorkInfo workInfo;
  std::vector<ContourLevelLines> contours; // 最终（detour 后重提取或 initial）
};

// 上游 extract_global_contours(surface, levels, smooth=True) 的
// local_interpretive_detour 分支。levels 顺序保留（升序传入）。
FieldContourResult extractFieldContours( const FieldContourSurface &surface,
                                         const std::vector<double> &levels,
                                         bool smooth = true );

// 解析 <stem>.structural.json 根对象为 FieldContourSurface（等值线阶段
// 输入 = 该侧卡；grid.z 的 null → NaN）。结构不合法返回 false + error。
bool loadFieldContourSurface( const QVariantMap &model, FieldContourSurface *surface,
                              QString *error );

} // namespace paleo::singlefactor
