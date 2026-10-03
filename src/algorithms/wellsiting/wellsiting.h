// 层：数据
#pragma once

#include "../singlefactor/geosutil.h"
#include "../singlefactor/types.h"

#include <cstdint>
#include <string>
#include <vector>

// algorithms/wellsiting — 井网覆盖诊断/候选点位/方案评估的纯数值核
// （方向 34）。无 QtWidgets、无 QGIS 图层、无文件发布；调用方只读结果。
//
// 口径（诚实面，随报告 note 一同返回）：
//   · 距离一律是局部米制网格上的欧氏距离（不做绕障/各向异性）；
//   · 空洞面积 = 采样格计数 × 格面积（栅格口径），边界多边形是格矩形
//     并集的阶梯轮廓，仅用于显示，不参与面积量算；
//   · 采样只在工区域多边形内的格中心上进行（点在多边形内判定）；
//   · 本核评估的是几何覆盖，不声称地质最优性。
namespace paleo::wellsiting
{

using Point2 = paleo::singlefactor::Point2;
using Polygon = paleo::singlefactor::Polygon;
using GridSpec = paleo::singlefactor::GridSpec;
using Status = paleo::singlefactor::Status;

// 诊断/评估共用参数。controlRadius = 井控半径（距最近井超过它算空洞）；
// cellSize = 采样格边长；maxCells = 采样预算（cols×rows 上限，防大工区爆格）。
struct SitingOptions
{
  double controlRadius = 2500;
  double cellSize = 100;
  std::uint64_t maxCells = 4000000;
  // 边界提取（GEOS 格矩形并集）的每空洞格数预算：超出预算的空洞不画
  // 边界（量算不受影响），note 如实注记。防大工区 UI 阻塞。
  std::uint64_t maxBoundaryCells = 250000;
};

// 井间最近邻距离分布（每口井到最近邻井的距离，n≥2 才有效）。
struct NearestNeighborStats
{
  std::size_t count = 0;
  double min = 0, mean = 0, median = 0, p90 = 0, max = 0;
  bool valid = false;
};

NearestNeighborStats nearestNeighborStats( const std::vector<Point2> &wells );

// 采样场：工区域上距最近井的欧氏距离场 + 空洞连通域。诊断、候选生成与
// 方案评估共用；携带输入（domain/wells）便于复用同一底座做避让检查。
struct SitingField
{
  Status status = Status::InvalidInput;
  std::string message;
  GridSpec grid;                       // north-up pixel-is-area，格中心采样
  double cellArea = 0;                 // cellSize²（平方米）
  double domainArea = 0;               // 域内格数 × cellArea（栅格口径）
  std::vector<Polygon> domain;         // 输入域（原样保留）
  std::vector<Point2> wells;           // 输入井集（实井；planned 由调用方拼装）
  std::vector<double> dist;            // 距最近井距离；域外为 -1；无井为 +inf
  std::vector<std::uint8_t> inDomain;  // 0/1
  std::vector<std::vector<int>> holes; // 每个空洞连通域的格序号（按面积降序）
  std::uint64_t maxBoundaryCells = 250000; // 边界提取预算（describeField 消费）
  std::string note;                    // 口径注记（调用方如实上屏）
};

// 井集为空是合法退化：全域都是空洞，域内 dist = +inf。
SitingField sampleField( const std::vector<Polygon> &domain,
                         const std::vector<Point2> &wells, const SitingOptions &options );

// 空洞区域的显示/量算摘要。area 用格计数；boundaryParts 是格矩形并集的
// 外环集合（GEOS unary union，阶梯轮廓；8 连通域角点相接时可能多段）。
struct HoleRegion
{
  int id = 0;
  double area = 0;        // m²（cellCount × cellArea）
  int cellCount = 0;
  double maxDistance = 0; // 空洞内最深格的距最近井距离
  Point2 deepest;         // 最大空洞圆心（最深格中心）
  std::vector<std::vector<Point2>> boundaryParts;
};

struct CoverageReport
{
  Status status = Status::InvalidInput;
  std::string message;
  NearestNeighborStats spacing;   // 井间最近邻分布
  double holeAreaTotal = 0;       // m²（栅格口径）
  int holeCount = 0;
  double coverageRatio = 0;       // 1 − 空洞面积/域面积（栅格口径）
  double domainMeanDistance = 0;  // 域内格中心距最近井的均值（米；无井为 inf）
  std::vector<HoleRegion> regions;
  std::string note;               // 口径注记
};

// 从采样场提炼报告。withBoundaries=false 时跳过 GEOS 边界提取（评估/贡献
// 只消费格计量——大工区省时）；GEOS 不可用或空洞超预算时 boundaryParts
// 留空、量算不受影响（超预算如实入 note）。
CoverageReport describeField( const SitingField &field, bool withBoundaries = true );

// 便捷一次调用：采样 + 报告。
CoverageReport diagnoseCoverage( const std::vector<Polygon> &domain,
                                 const std::vector<Point2> &wells,
                                 const SitingOptions &options );

// ---------------------------------------------------------------------------
// 候选点位生成
// ---------------------------------------------------------------------------

// 避让面：折线（约束线/断层迹线）与多边形（禁钻区），各自带缓冲距。
struct AvoidSurfaces
{
  std::vector<std::vector<Point2>> lines;
  std::vector<Polygon> polygons;
  double lineBuffer = 300;  // 距折线的最小距离（<=0 不避让折线）
  double polygonBuffer = 0; // 距多边形边界外的缓冲（<=0 视为 0；内部恒禁钻）
};

struct CandidateOptions
{
  double gridSpacing = 0;      // 候选规则网格间距；<=0 → controlRadius
  double minWellDistance = 0;  // 候选距既有井的最小距离；<=0 → 不限制
  double boundaryMargin = 0;   // 候选距工区边界的最小距离；<=0 → 不限制
  int perHoleLimit = 0;        // 每空洞规则网格候选数上限；0 = 不限
  bool includeDeepest = true;  // 每空洞附「最大空洞圆心」候选
};

struct CandidatePoint
{
  Point2 pos;
  std::string strategy; // "grid" | "deepest"
  int holeId = 0;
  double holeDistance = 0; // 该点位的基础场距离（候选处空洞深度）
};

// 在空洞内生成候选：每空洞先取最大空洞圆心，再取落在空洞格上的规则网格
// 点（网格锚定在采样场原点）；逐点做域内/避让/距井检查。规则网格候选按
// 空洞深度降序截 perHoleLimit（保深不保先）。结果按空洞序（面积降序）。
std::vector<CandidatePoint> generateCandidates( const SitingField &field,
                                                const AvoidSurfaces &avoid,
                                                const CandidateOptions &options,
                                                const SitingOptions &siting );

// ---------------------------------------------------------------------------
// 方案评估
// ---------------------------------------------------------------------------

// 层位井控层：一口井「控制」某层位 = 该层位有该井的输入点（调用方从
// tops/分层归集）；候选井按「控制全部层位」假设计入（部署假设，入 note）。
struct LayerWells
{
  std::string horizon;
  double weight = 1;
  std::vector<Point2> points;
};

struct ScenarioMetrics
{
  double holeAreaTotal = 0;        // m²（栅格口径，与诊断同格）
  int holeCount = 0;
  double coverageRatio = 0;
  double domainMeanDistance = 0;   // 域内格中心距最近（实+候选）井的均值
  double interWellMeanSpacing = 0; // 井间最近邻均值（实+候选井；n<2 为 0）
  double weightedDensity = 0;      // 按层位加权井控密度（口/km²，栅格域面积）
  std::string note;
};

// 基线（现状）指标：只算实井。
ScenarioMetrics baselineMetrics( const std::vector<Polygon> &domain,
                                 const std::vector<Point2> &wells,
                                 const std::vector<LayerWells> &layers,
                                 const SitingOptions &options );

// 方案指标：实井 + 候选井。layers 只含实井点位——候选井对每层各 +1 口。
ScenarioMetrics scenarioMetrics( const std::vector<Polygon> &domain,
                                 const std::vector<Point2> &wells,
                                 const std::vector<Point2> &candidates,
                                 const std::vector<LayerWells> &layers,
                                 const SitingOptions &options );

// 单候选边际贡献（基于基础采样场，栅格口径，解析统计不重采样）：
//   holeAreaReduction     = 只加该候选后空洞面积减少量（m²）
//   meanDistanceReduction = 域内均值距最近井的减少量（米）
struct CandidateContribution
{
  double holeAreaReduction = 0;
  double meanDistanceReduction = 0;
};

CandidateContribution contributionOf( const SitingField &field,
                                      const Point2 &candidate,
                                      const SitingOptions &options );

} // namespace paleo::wellsiting
