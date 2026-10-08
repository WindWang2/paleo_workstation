// 层：数据
#pragma once

#include "types.h"

#include <QPair>
#include <QString>
#include <QStringList>
#include <QVariant>

#include <initializer_list>
#include <optional>
#include <vector>

// 上游移植：drawing/single_factor/workflow.py
//   _extract_current_wells / _resolve_current_factor_value /
//   _resolve_direct_factor_value / _attr_value / _parse_numeric / _to_bool /
//   _to_float / _to_int / _resolve_value_range_for_wells
//   + constrained_engine.py point_in_boundary/point_in_ring/point_on_segment
// 语义约定（与上游一致，见 TODOS.md）：不实现
//   _first_numeric_factor_value 兜底——井值字段为空时必须跳过，不得借用
//   OCR_CONF 等无关数值列。

namespace paleo::singlefactor
{

// 要素属性表，保持字段声明顺序（SHP 字段序），用于
// _factor_debug_hint 的「可用字段」预览与别名链查找。
using FeatureAttributes = QList<QPair<QString, QVariant>>;

struct WellFeatureRow
{
  FeatureAttributes attributes;
  bool isPoint = false; // 上游 geometry_type == GEOM_POINT 且为 Point2D
  double x = 0.0;       // feature.geometry.x
  double y = 0.0;
};

struct WellAcquisitionRequest
{
  QString wellIdField;    // request.well_id_field（空串 = 上游 None）
  QString factorMode;     // "ratio" 之外一律按 direct 处理
  QString valueField;
  QString factorName;
  QString numeratorField;
  QString denominatorField;
  std::optional<double> valueMin; // request.value_min（None → nullopt）
  std::optional<double> valueMax;
  bool strictFields = false; // 显式提取不回落其它指标，旧请求保持兼容。
};

struct AcquiredWell
{
  std::string wellId;
  double x = 0.0;
  double y = 0.0;
  double value = 0.0;
  bool isControl = false;
};

struct WellAcquisitionResult
{
  std::vector<AcquiredWell> wells; // 严格保留输入要素顺序
  QStringList skipped;             // 上游 skip 文案（含贴边软纳入提示行）
  int softIncluded = 0;
};

// ---- 属性/数值辅助（workflow.py L2264-2307, L3044-3064）----
QVariant attrValue( const FeatureAttributes &attributes,
                    std::initializer_list<QString> names );
std::optional<double> parseNumeric( const QVariant &value );
bool toBool( const QVariant &value, bool defaultValue );
double toFloat( const QVariant &value, double defaultValue );
int toInt( const QVariant &value, int defaultValue );

// ---- 边界几何判定（constrained_engine.py L1928-1978, workflow.py L1502-1544）----
bool pointOnSegment( Point2 point, Point2 a, Point2 b, double tolerance = 1e-9 );
bool pointInRing( Point2 point, const Ring &ring );
bool pointInBoundary( Point2 point, const Polygon &boundary );
bool pointInBoundaryBbox( Point2 point, const Polygon &boundary,
                          double marginRatio = 0.0 );
double pointDistanceToRing( Point2 point, const Ring &ring );

// ---- 因素值解析（workflow.py L1546-1606，无 first-numeric 兜底）----
std::optional<double> resolveDirectFactorValue( const FeatureAttributes &attributes,
                                                const WellAcquisitionRequest &request );
std::optional<double> resolveCurrentFactorValue( const FeatureAttributes &attributes,
                                                 const WellAcquisitionRequest &request );

// ---- 井点采集（workflow.py L1389-1498）----
WellAcquisitionResult acquireWells( const std::vector<WellFeatureRow> &rows,
                                    const std::vector<Polygon> &boundaries,
                                    const WellAcquisitionRequest &request );

// ---- 图例值域（workflow.py L691-742）----
struct ResolvedValueRange
{
  std::optional<double> min;
  std::optional<double> max;
};
ResolvedValueRange resolveValueRangeForWells( const std::vector<AcquiredWell> &wells,
                                              const std::optional<double> &requestMin,
                                              const std::optional<double> &requestMax,
                                              const std::vector<double> &gridValues );

} // namespace paleo::singlefactor
