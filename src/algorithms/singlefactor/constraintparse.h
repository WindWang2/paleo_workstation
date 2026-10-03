// 层：数据
#pragma once

#include "types.h"

#include <QString>

#include <optional>
#include <string>
#include <vector>

// 约束线图层 → ConstraintLine 的共享解析（原 localdirectionalgorithm.cpp
// 内部静态函数，结构面引擎复用，无行为变化）。
// params_json schemaVersion=1 契约见 workflows.cpp::lineParamsJson。

class QgsProcessingFeatureSource;
class QgsProcessingContext;
class QgsCoordinateReferenceSystem;

namespace paleo::singlefactor
{

const char *semanticToken( Semantic semantic );
std::optional<Semantic> semanticFromParams( const QString &token );
std::optional<Semantic> semanticFromTypeColumn( const QString &type );

struct ParsedConstraints
{
  std::vector<ConstraintLine> lines;
  std::vector<std::string> ignored;
};

struct ConstraintParseOptions
{
  // 结构面引擎（structural_idw）需要上游 barriers 完整清单：停用行保留
  // enabled=false（上游 active 标记），hard_barrier 的非阻断 blockMode
  // 原文保留不重分类（上游在 build_structural_surface 里按
  // is_full_block_mode/is_contour_stop_mode 自行划分）。其余引擎沿用默认。
  bool keepDisabled = false;
  bool keepNonBlockingBarriers = false;
};

ParsedConstraints readConstraintLines( QgsProcessingFeatureSource *constraints,
                                       const QgsCoordinateReferenceSystem &targetCrs,
                                       QgsProcessingContext &context,
                                       const ConstraintParseOptions &options = {} );

} // namespace paleo::singlefactor
