// 层：数据
#pragma once

#include "types.h"

#include <optional>
#include <span>
#include <string>
#include <vector>

// partition 移植（haiyou-visualization 27fdb99）。
// 解释分区模式：屏障自由端沿端切向延长到域边界 + node-safe 洪泛分区，
// 两侧不共享井、不能绕端回通——比 grid_connectivity_v1 硬屏障更强。
// 层：数据
namespace paleo::singlefactor
{

inline constexpr int kRegionOutside = -1;
inline constexpr int kWellShared = -2;
inline constexpr int kWellPending = -3;
inline constexpr const char *kPartitionVersion = "2026-09-10";

struct BarrierSpec
{
  std::string lineId;
  std::vector<Point2> points;
};

struct BarrierExtension
{
  std::string barrierId;
  std::string end; // "start"|"end"
  Point2 from;
  Point2 to;
  std::string reason = "domain_boundary";
};

struct ExtendResult
{
  std::vector<BarrierSpec> extended;
  std::vector<BarrierExtension> extensions;
  std::vector<std::string> conflicts;
};

struct PartitionResult
{
  std::vector<int> regionIds; // rows*cols；kRegionOutside = 域外或屏障上
  std::vector<int> wellRegionIds; // kWellShared（local）/ kWellPending（interpretation）
  std::vector<BarrierSpec> partitionBarriers;
  std::vector<BarrierExtension> extensions;
  int regionCount = 0;
  bool complete = false;
  std::string mode; // "local"|"interpretation"
  std::vector<std::string> conflicts;
};

// 闭线段相交（含端点接触）。
bool closedSegmentsIntersect( Point2 a, Point2 b, Point2 c, Point2 d, double tol = 1e-12 );
std::optional<Point2> segmentIntersectionClosed( Point2 a, Point2 b, Point2 c, Point2 d );

bool pointOnBarriers( Point2 point, std::span<const BarrierSpec> barriers, double tol );

// 网格邻接判定：在线上的格心与穿线都阻断（区别于允许贴线相切的可见性判定）。
bool partitionEdgeBlocked( Point2 a, Point2 b, std::span<const BarrierSpec> barriers, double onLineTol );

// 自由端延长：沿端切向射线裁到域边界环；命中记入 extensions，未命中记冲突。
ExtendResult extendBarriersToDomain( std::span<const BarrierSpec> barriers,
                                     const std::vector<Polygon> &boundaries,
                                     double snapTol, double maxDist );

// 4 连通洪泛，屏障节点不渗漏；仅贴单区的未标格事后并入该区。
std::vector<int> buildRegionLabelsNodeSafe( const GridSpec &grid,
    const std::vector<std::uint8_t> &domainMask, std::span<const BarrierSpec> barriers,
    const Control *control = nullptr );

// 井→区归属：最近已标格（半径 0..3 环窗）；exclusive 时未配井不给共享值。
std::vector<int> assignWellRegions( std::span<const Point2> wellXy, const GridSpec &grid,
                                    const std::vector<int> &regionIds, bool exclusive );

bool wellAllowedForCell( int cellLabel, int wellLabel, bool exclusive );

PartitionResult buildPartition( const GridSpec &grid, const std::vector<std::uint8_t> &domainMask,
    std::span<const BarrierSpec> barriers, std::span<const Point2> wellXy,
    const std::vector<Polygon> &boundaries, bool interpretation, const Control *control = nullptr );

// 解释分区 → BarrierGrid 适配：未标格（屏障上）记 -2，域边界取网格外接矩形。
// 与 grid_connectivity_v1 同构，供 evaluateLocalIdw 按 hardBarrierModel 分派消费。
BarrierGrid labelInterpretationPartition( const GridSpec &grid,
    const std::vector<ConstraintLine> &barriers, const std::vector<Sample> &samples,
    double tolerance, const Control *control = nullptr );

} // namespace paleo::singlefactor
