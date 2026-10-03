// 层：数据
#pragma once

#include "types.h"
#include "wellacquisition.h"

#include <QVariantMap>

#include <optional>
#include <string>
#include <vector>

// 上游移植：drawing/single_factor/structural_idw.py::build_structural_surface
// （+ constrained_engine.py::resolve_barrier_buffer_distance、
//    fast_grid.py::rasterize_polygon_mask/build_boundary_union_mask/
//    resolve_performance_grid_resolution、
//    constraint_semantics.py 阻断语义、
//    direction_corridor.py::estimate_mean_well_spacing）。
// 区域模型固定为单区域 = 成图边界并集（与上游 extend/非 extend 两分支一致：
// 上游 build_structural_surface 的 regions 恒为 [domain]，仅搜索/点数参数
// 随 extend_trend_to_boundary 切换）。核函数复用 evaluateAt，不改语义。

namespace paleo::singlefactor
{

// ---- 阻断语义（drawing/constraint_semantics.py 精确移植）----
std::string normalizeBlockMode( const std::string &mode );
bool isFullBlockMode( const std::string &mode );
bool isContourStopMode( const std::string &mode );

struct StructuralBarrier
{
  std::string lineId;
  std::vector<Point2> points;
  bool active = true;
  std::string blockMode = "full_block";
  int priority = 3;
};

struct StructuralDirection
{
  std::string lineId;
  std::vector<Point2> points;
  bool active = true;
  double ratio = 8.0;
  double influenceRadius = 0; // <=0 → 自动
  double coreRadius = 0;      // <=0 → 0.3×影响半径
  int priority = 1;
  std::string zoneId;
  std::string extendMode;
  std::string transition;
};

struct StructuralSoftBoundary
{
  std::vector<Point2> points;
  double radius = 0;
  double strength = 0;
};

struct StructuralRequest
{
  double power = 2.0;
  int resolution = 339; // 已按 resolvePerformanceGridResolution 封顶后的有效分辨率
  bool extendTrendToBoundary = true;
  bool enableBarriers = true;
  bool enableDirections = true;
  double searchRadius = 0;
  int minPoints = 3;
  int maxPoints = 12;
  double barrierBufferDistance = 150;
  double barrierBlankCells = 0;
  bool barrierBufferAuto = false;
  double barrierShapeRadius = 0;
  double barrierShapeStrength = 1.0;
  double interpretiveBoundaryStrength = 0.35;
  double contourStopBufferDistance = -1; // <0 → 自动
  bool wellClusterLocality = true;
  // 声明值域（request.value_min/value_max；ratio_0_1 → 0/1）。
  // 与井+网格值一并进 _resolve_value_range_for_wells。
  std::optional<double> valueMin;
  std::optional<double> valueMax;
  // Paleo interpretive_boundary 语义线：radius<=0 → 与停线同一自动半径。
  std::vector<StructuralSoftBoundary> explicitSoftBoundaries;
};

struct StructuralDirectionInfo
{
  std::string lineId;
  double ratio = 8.0;
  double influenceRadius = 0;
  double coreRadius = 0;
  bool active = true;
  std::vector<std::vector<Point2>> pieces; // 域内裁剪片段（长度>1e-10）
};

struct StructuralResult
{
  Status status = Status::InvalidInput;
  std::string message;
  std::vector<double> xAxis;              // 升序节点轴
  std::vector<double> yAxis;
  std::vector<double> grid;               // 行主序（行=升序 y），域外 NaN
  std::vector<std::uint8_t> validMask;    // 上游 target（射线并集掩膜）
  std::vector<int> regionIds;             // labels 重排为网格（0/-1）
  std::vector<int> wellRegionIds;
  std::vector<StructuralDirectionInfo> directions; // 有效方向线（解析半径后）
  std::vector<StructuralSoftBoundary> interpretiveBoundaries;
  double span = 0;                            // max(domain宽, domain高)
  double spacing = 0;                         // max(井最近邻均值, step)
  std::optional<double> searchRadius;         // 解析后的搜索半径（extend → nullopt）
  int resolvedMinPoints = 1;
  int resolvedMaxPoints = 0;
  double barrierBufferDistance = 0;
  bool barrierBufferAutoApplied = false;
  double contourStopBufferDistance = 0;
  std::optional<double> valueMin;
  std::optional<double> valueMax;
  QVariantMap diagnostics;
};

// fast_grid.py::resolve_performance_grid_resolution（max_cells<=0 时按
// 冻结口径钉 200000——算法层不读 UI 性能滑条）。
int resolvePerformanceGridResolution( double mapWidth, double mapHeight,
                                      int requested, int maxCells = 0 );

// constrained_engine.py::resolve_barrier_buffer_distance
std::pair<double, bool> resolveBarrierBufferDistance( double requestedDistance,
                                                      double legacyCellBuffer,
                                                      double gridStep,
                                                      double mapWidth,
                                                      double mapHeight,
                                                      double searchRadius,
                                                      bool hasBarriers,
                                                      bool autoEnabled );

StructuralResult buildStructuralSurface( const std::vector<AcquiredWell> &wells,
                                         const std::vector<Polygon> &boundaries,
                                         const std::vector<StructuralBarrier> &barriers,
                                         const std::vector<StructuralDirection> &directions,
                                         const std::vector<Polygon> &interpolationAreas,
                                         const StructuralRequest &request,
                                         const Control &control );

} // namespace paleo::singlefactor
