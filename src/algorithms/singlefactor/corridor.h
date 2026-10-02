// 层：数据
#pragma once

#include "types.h"

#include <map>
#include <span>
#include <string>
#include <vector>

// direction_corridor 移植（haiyou-visualization 27fdb99）。
// Surfer 式方向走廊：折线 (s,n) 曲线坐标、core/influence 双半径指数衰减、
// 多方向线分区独立控制（网格单元归属最强影响线）、搜索邻域按局部轴拉伸椭圆。
// 纯数值核：无 QtWidgets、无 QGIS、无栅格发布。
// 层：数据
namespace paleo::singlefactor
{

struct DirectionLineSpec
{
  std::string lineId;
  std::vector<Point2> points;
  bool active = true;
  double ratio = 12.0; // >=1，沿轴拉伸比
  double influenceRadius = 0; // <=0 → 由井距/搜索半径/线长解析
  double priority = 1; // >0，小值优先
  double coreRadius = 0; // <=0 → 基础搜索半径推导
  std::string zoneId; // 非空时不同区方向线不做切向混合
  std::string extendMode = "auto"; // auto|none|tangent
  double transition = 0; // <=0 → influence-core
};

struct PolylineGeometry
{
  std::string lineId;
  std::vector<Point2> points; // 可含端部切向延长点
  std::vector<double> cumlen; // 各顶点弧长
  double totalLength = 0;
  double ratio = 1;
  double coreRadius = 0;
  double influenceRadius = 0;
  int priority = 1;
  std::string zoneId;
  std::string extendMode = "auto";
  double transition = 0;
  double sStart = 0; // 延长后原始起点所在弧长
  double sEnd = 0; // 延长后原始终点所在弧长
  int index = 0;
};

struct PointCurveCoord
{
  int dirIndex = -1;
  double s = 0;
  double n = 0; // 带符号法距，行进方向左侧为正
  double tx = 1;
  double ty = 0; // 最近段单位切向
  double g = 0; // [0,1] 组合影响强度
  double ratio = 1;
  double distance = 0; // 到折线欧氏距离
};

// buildGridDirectionCache 产物：rows*cols 平铺（行主序，同 GridSpec）。
struct DirectionFieldCache
{
  int cells = 0;
  std::vector<int> dirIndex; // -1 无控制线
  std::vector<double> s;
  std::vector<double> n;
  std::vector<double> tx;
  std::vector<double> ty;
  std::vector<double> g;
  std::vector<double> ratio;
  std::vector<double> stretch; // 1+(ratio-1)*g
  // 竞争线双角混合时的次强线
  std::vector<int> dirIndex2;
  std::vector<double> g2;
  std::vector<double> tx2;
  std::vector<double> ty2;
  std::vector<double> ratio2;
  std::vector<double> s2;
  std::vector<double> n2;
};

// 每条方向线的井投影表。
struct WellCurveTable
{
  std::vector<double> s;
  std::vector<double> n;
  std::vector<double> g;
  std::vector<double> tx;
  std::vector<double> ty;
  std::vector<std::uint8_t> valid;
  double ratio = 1;
  std::string zoneId;
  double lineLength = 0;
};

struct PolylineProjection
{
  double s = 0;
  double n = 0;
  double tx = 1;
  double ty = 0;
  double distance = 0;
};

// 沿脊一维值剖面：(s, z) 序列，端点锚定方向线两端。
struct RidgeProfile
{
  std::vector<double> s;
  std::vector<double> z;
};

struct AlongTrackStats
{
  int alongTrackCells = 0;
  int alongTrackProfiles = 0;
};

// 解析缺省半径；返回剔除未激活/退化线后的规格。
std::vector<DirectionLineSpec> resolveDirectionParams( std::span<const DirectionLineSpec> specs,
    double searchRadius, double meanWellSpacing, double mapExtent );

PolylineGeometry buildPolylineGeometry( const DirectionLineSpec &spec, int index, double extendDistance );

std::vector<PolylineGeometry> buildDirectionGeometries( std::span<const DirectionLineSpec> specs,
    double searchRadius, double meanWellSpacing, double mapExtent );

PolylineProjection projectPointToPolyline( Point2 point, const PolylineGeometry &geom );

// 垂向包络：core 内 1，influence 外 0，环带内指数衰减 (e^{-kt}-e^{-k})/(1-e^{-k})。
double influenceStrength( double distToLine, double coreRadius, double influenceRadius,
                          double transition = 0.0, double expK = 3.0 );

// 沿线包络：原始线跨 [sStart,sEnd] 内 1，端外仅短尖端锥形收敛。
double alongTrackEnvelope( double s, double sStart, double sEnd, double tipLength = 0.0,
                           const std::string &extendMode = "auto" );

double combinedInfluence( double distToLine, double s, const PolylineGeometry &geom, double tipLength = 0.0 );

// 每像元控制方向属性（含次强线双角混合）。domainMask 为 rows*cols，真值才计算。
DirectionFieldCache buildGridDirectionCache( const GridSpec &grid,
    const std::vector<std::uint8_t> &domainMask, const std::vector<PolylineGeometry> &geoms );

std::map<int, WellCurveTable> precomputeWellCurveCoords( std::span<const Point2> wellXy,
    const std::vector<PolylineGeometry> &geoms );

PointCurveCoord pickControllingDirection( std::span<const PointCurveCoord> candidates,
    const std::vector<PolylineGeometry> &geoms );

// 曲线坐标距离：(Δs/ratio)² + Δn²；expAniso>1 时沿轴再非线性压近。
double curveDistanceSq( double s0, double n0, double s1, double n1, double ratio, double expAniso = 1.0 );

// d_eff = sqrt((1-g)d_euc² + g d_curve²)；g>=0.25 时加压向曲线度量。
double blendEffectiveDistance( double euclidean, double curveDist, double gPair );

// 椭圆接受：曲线坐标 (s,n) 下沿轴半径按 ratio/线长拉伸，垂向保持基础半径。
bool ellipticalSearchAccept( double s0, double n0, double s1, double n1, double ratio,
                             double baseRadius, double gPair, double euclidean, double lineLength = 0.0 );

// 网格-井对有效距离与邻域判定（二者须受同一条控制线约束才用曲线度量）。
struct PairDistanceResult
{
  double dEff = 0;
  double gPair = 0;
};
PairDistanceResult pairEffectiveDistance( double euclidean, int cellDir, double cellS, double cellN,
    double cellG, double cellRatio, std::size_t wellIndex,
    const std::map<int, WellCurveTable> &wellCoords );

bool pairInSearchNeighborhood( double euclidean, double dEff, double gPair, double cellS, double cellN,
    double cellRatio, std::size_t wellIndex, int cellDir,
    const std::map<int, WellCurveTable> &wellCoords, double baseRadius, bool useExtendedSearch );

// 近轴井 → 沿线 (s, z) 剖面，端点锚定方向线两端（低凹与高脊都延伸到线端）。
std::map<int, RidgeProfile> buildAlongTrackWellProfiles( std::span<const Point2> wellXy,
    std::span<const double> wellValues, const std::map<int, WellCurveTable> &wellCoords,
    const std::vector<PolylineGeometry> &geoms, double minG = 0.15 );

// 剖面取值：井间线性内插，出井段常值外推；n 的垂向衰减由调用方处理。
double sampleAlongTrackValue( double s, double n, const RidgeProfile &profile,
                              const PolylineGeometry &geom );

// 走廊沿脊混合：IDW 场与一维脊线值按指数 alpha 融合，空井段以脊线值填充。
// 返回新场（原场不动），stats 记录改写像元数。
std::vector<double> blendCorridorAlongTrack( const std::vector<double> &gridValues,
    const DirectionFieldCache &cache, const std::vector<PolylineGeometry> &geoms,
    const std::map<int, RidgeProfile> &profiles, const std::vector<std::uint8_t> &domainMask,
    AlongTrackStats *stats = nullptr, double blendStrength = 0.99, double minCellG = 0.05,
    double expK = 6.0 );

} // namespace paleo::singlefactor
