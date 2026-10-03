// 层：数据
#pragma once

#include <array>
#include <cstdint>
#include <functional>
#include <optional>
#include <string>
#include <vector>

// algorithms/singlefactor — 局部方向单因素的纯数值 DTO。
// 无 QtWidgets、无 QGIS 图层、无文件发布。调用方只读这些值。
// 层：数据
namespace paleo::singlefactor
{

struct Point2
{
  double x = 0;
  double y = 0;
};

struct Sample
{
  std::string stableRowId;
  std::string wellId;
  double x = 0;
  double y = 0;
  double value = 0;
  // >=0：用户明确指定的硬屏障连通区。-1：未指定。
  int componentOverride = -1;
};

struct Ring
{
  std::vector<Point2> points;
};

struct Polygon
{
  Ring exterior;
  std::vector<Ring> holes;
};

enum class Semantic
{
  HardBarrier,
  DirectionGuide,
  InterpretiveBoundary,
  ContourStop,
  CartographicDetour,
  Ignored
};

struct ConstraintLine
{
  std::string stableId;
  Semantic semantic = Semantic::Ignored;
  std::vector<Point2> points;
  bool enabled = true;
  double ratio = 8;
  double influenceRadius = 0; // <=0 → 解析为自动半径
  double coreRadius = 0;      // <=0 → 0.3×影响半径
  double softStrength = 0.35;
  double softRadius = 0; // <=0 → 自动，且不读取显示缓冲
  double displayBuffer = 0; // 只参与制图显示/停线，不扩大数值缺失
  double cartographicBuffer = 0;
  // 上游 BLK_MODE 原文（constraint_semantics.py 阻断语义）：
  // "full_block"=硬隔断，"display_only"=仅停线，其余非阻断词=忽略。
  std::string blockMode = "full_block";
  std::string unit;
};

enum class CoverageMode
{
  WellSupported,
  DomainExtrapolation
};

enum class Status
{
  Ok,
  Cancelled,
  InvalidInput,
  BudgetExceeded,
  NumericalFailure
};

struct GridSpec
{
  int cols = 0;
  int rows = 0;
  // 左上像元边。north-up 时 pixelHeight < 0，行向南，采样在像元中心。
  double originX = 0;
  double originY = 0;
  double pixelWidth = 1;
  double pixelHeight = -1;
  std::string crs;
  std::string convention = "north_up_pixel_is_area";
};

struct Control
{
  std::function<bool()> cancelled;
  std::function<void( double )> progress; // [0,1]，调用方保证单调
};

struct ResolvedDirection
{
  std::string id;
  double ratio = 1;
  double influence = 0;
  double core = 0;
  std::vector<Point2> points;
};

struct ResolvedSoft
{
  std::string id;
  double radius = 0;
  double strength = 0;
  std::vector<Point2> points;
};

struct ResolvedParameters
{
  bool autosApplied = false;
  double power = 2;
  CoverageMode coverage = CoverageMode::WellSupported;
  bool wellClusterLocality = false;
  bool requireFullCoverage = false;
  // n>400 时本实现仍用全体最近邻均值，不复刻 numpy PCG64 抽样。验收规模不超过 200 口井。
  bool spacingSubsampled = false;
  double clusterSpan = 0;
  double spacing = 0;
  double step = 0;
  double span = 0;
  double tolerance = 0;
  std::optional<double> searchRadius;
  double supportedRadius = 0; // 外推标记用的井控半径，外推时仍记录
  int supportedMinPoints = 3; // 井控规则的最少点数；外推求解会把 minPoints 改为 1
  int minPoints = 3;
  int maxPoints = 12;
  std::vector<ResolvedDirection> directions;
  std::vector<ResolvedSoft> soft;
  std::string hardBarrierModel = "grid_connectivity_v1";
  std::string duplicatePolicy = "preserve_rows_exact_mean";
  std::string algorithmId = "paleo:paleo_local_direction_idw";
  std::string algorithmVersion = "1.0.0";
  std::string semanticProfile = "paleo_local_idw_v1";
  std::string valueUnit;
};

struct PreparedInput
{
  std::vector<Sample> samples;
  std::vector<Polygon> domain;
  std::vector<ConstraintLine> constraints;
  std::vector<std::string> ignored; // "id reason"
  int originalCount = 0;
  int validCount = 0;
  int missingCount = 0;
  int outOfRangeCount = 0;
  int duplicateExtraRows = 0;
  int conflictRows = 0;
  std::string valueUnit;
  bool localEngineeringGrid = false;
};

struct UnsupportedRegion
{
  int id = -1;
  double area = 0;
  std::string reason;
};

struct AmbiguousSample
{
  std::string stableRowId;
  double x = 0;
  double y = 0;
  std::string reason;
};

struct SurfaceResult
{
  Status status = Status::InvalidInput;
  std::string message;
  std::vector<double> values; // NaN = 内部缺失
  std::vector<std::uint8_t> marks; // 0 缺失 1 井控 2 外推 3 屏障格
  std::vector<int> components;
  ResolvedParameters resolved;
  std::vector<UnsupportedRegion> unsupported;
  std::vector<AmbiguousSample> ambiguous;
  std::vector<std::string> issues;
  int finiteCells = 0;
  int nodataCells = 0;
  int extrapolatedCells = 0;
  int barrierCells = 0;
};

struct QueryResult
{
  Status status = Status::InvalidInput;
  std::string message;
  std::vector<double> values;
  std::vector<double> influence;
  ResolvedParameters resolved;
  std::vector<std::string> issues;
};

struct BarrierGrid
{
  std::vector<int> component; // -2 屏障，>=0 连通区
  std::vector<int> sampleComponent; // -1 未归属
  std::vector<AmbiguousSample> ambiguous;
  int componentCount = 0;
  int barrierCells = 0;
};

struct ContourPolyline
{
  std::vector<Point2> points;
  double level = 0;
};

struct WorkField
{
  Status status = Status::Ok;
  std::string message;
  std::vector<double> values;
  double coreValue = 0;
  double bufferHalfWidth = 0;
  double numericalGuard = 0;
  double transitionDistance = 0;
  int modifiedCells = 0;
  std::vector<std::string> usedConstraintIds;
  bool unchanged = true;
};

inline const char *statusName( Status status )
{
  switch ( status )
  {
    case Status::Ok:
      return "ok";
    case Status::Cancelled:
      return "cancelled";
    case Status::InvalidInput:
      return "invalid_input";
    case Status::BudgetExceeded:
      return "budget_exceeded";
    case Status::NumericalFailure:
      return "numerical_failure";
  }
  return "invalid_input";
}

} // namespace paleo::singlefactor
