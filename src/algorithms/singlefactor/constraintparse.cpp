// 层：数据
#include "constraintparse.h"

#include "domain/constraintsemantics.h" // 统一语义解析 + 部件有效性

#include <qgscoordinatetransform.h>
#include <qgsfeature.h>
#include <qgsfeatureiterator.h>
#include <qgsfeaturerequest.h>
#include <qgsgeometry.h>
#include <qgsprocessingcontext.h>
#include <qgsprocessingutils.h>
#include <qgswkbtypes.h>

#include <memory>

namespace paleo::singlefactor
{

const char *semanticToken( Semantic semantic )
{
  switch ( semantic )
  {
    case Semantic::HardBarrier:
      return "hard_barrier";
    case Semantic::DirectionGuide:
      return "direction_guide";
    case Semantic::InterpretiveBoundary:
      return "interpretive_boundary";
    case Semantic::ContourStop:
      return "contour_stop";
    case Semantic::CartographicDetour:
      return "cartographic_detour";
    case Semantic::Ignored:
      return "ignored";
  }
  return "ignored";
}

std::optional<Semantic> semanticFromParams( const QString &token )
{
  if ( token == QLatin1String( "hard_barrier" ) )
    return Semantic::HardBarrier;
  if ( token == QLatin1String( "direction_guide" ) )
    return Semantic::DirectionGuide;
  if ( token == QLatin1String( "interpretive_boundary" ) )
    return Semantic::InterpretiveBoundary;
  if ( token == QLatin1String( "contour_stop" ) )
    return Semantic::ContourStop;
  if ( token == QLatin1String( "cartographic_detour" ) )
    return Semantic::CartographicDetour;
  return std::nullopt;
}

std::optional<Semantic> semanticFromTypeColumn( const QString &type )
{
  if ( type == QLatin1String( "break_line" ) )
    return Semantic::HardBarrier;
  if ( type == QLatin1String( "direction_line" ) )
    return Semantic::DirectionGuide;
  if ( type == QLatin1String( "interpretive_boundary" ) )
    return Semantic::InterpretiveBoundary;
  if ( type == QLatin1String( "contour_stop" ) )
    return Semantic::ContourStop;
  if ( type == QLatin1String( "cartographic_detour" ) )
    return Semantic::CartographicDetour;
  return std::nullopt;
}

ParsedConstraints readConstraintLines( QgsProcessingFeatureSource *constraints,
                                       const QgsCoordinateReferenceSystem &targetCrs,
                                       QgsProcessingContext &context,
                                       const ConstraintParseOptions &options )
{
  ParsedConstraints parsed;
  if ( !constraints )
    return parsed;
  const bool targetKnown = targetCrs.isValid();
  const bool sourceKnown = constraints->sourceCrs().isValid();
  if ( targetKnown != sourceKnown )
    throw QgsProcessingException( QStringLiteral( "已知 CRS 与未知 CRS 不能混用" ) );

  std::unique_ptr<QgsCoordinateTransform> transform;
  if ( sourceKnown && targetKnown && constraints->sourceCrs() != targetCrs )
  {
    try
    {
      transform = std::make_unique<QgsCoordinateTransform>( constraints->sourceCrs(), targetCrs,
                                                            context.transformContext() );
    }
    catch ( const QgsCsException &e )
    {
      throw QgsProcessingException(
          QStringLiteral( "Cannot transform constraints into the well CRS: %1" ).arg( e.what() ) );
    }
  }

  const QgsFields fields = constraints->fields();
  const int typeIdx = fields.lookupField( QStringLiteral( "type" ) );
  const int jsonIdx = fields.lookupField( QStringLiteral( "params_json" ) );
  const int schemaIdx = fields.lookupField( QStringLiteral( "schema_version" ) );
  const int idIdx = fields.lookupField( QStringLiteral( "id" ) );
  int sequence = 0;
  QgsFeatureIterator it = constraints->getFeatures( QgsFeatureRequest() );
  QgsFeature feature;
  while ( it.nextFeature( feature ) )
  {
    QString featureId;
    if ( idIdx >= 0 )
      featureId = feature.attribute( idIdx ).toString().trimmed();
    if ( featureId.isEmpty() )
      featureId = feature.id() >= 0 ? QString::number( feature.id() )
                                    : QStringLiteral( "line-%1" ).arg( ++sequence );
    const std::string stableId = featureId.toUtf8().toStdString();
    auto ignore = [&]( const QString &reason ) {
      parsed.ignored.push_back( stableId + " " + reason.toUtf8().toStdString() );
    };
    if ( !feature.hasGeometry() || feature.geometry().isEmpty() )
    {
      ignore( QStringLiteral( "empty_geometry" ) );
      continue;
    }
    QgsGeometry geometry = feature.geometry();
    if ( transform )
    {
      const Qgis::GeometryOperationResult transformed = geometry.transform( *transform );
      if ( transformed != Qgis::GeometryOperationResult::Success )
        throw QgsProcessingException(
            QStringLiteral( "Constraint geometry failed to transform into the well CRS" ) );
    }
    if ( QgsWkbTypes::geometryType( geometry.wkbType() ) != Qgis::GeometryType::Line )
    {
      ignore( QStringLiteral( "not_line" ) );
      continue;
    }

    ConstraintLine line;
    line.stableId = stableId;
    // 语义解析统一走 domain/constraintsemantics（UI/工作流计划同一函数），
    // 这里只把 token 映射回引擎枚举。非法 params_json 整次拒绝（旧口径）。
    const QString type = typeIdx >= 0 ? feature.attribute( typeIdx ).toString().trimmed() : QString();
    QString jsonText;
    if ( jsonIdx >= 0 && !feature.attribute( jsonIdx ).isNull() )
      jsonText = feature.attribute( jsonIdx ).toString().trimmed();
    const QVariant schemaColumn = schemaIdx >= 0 ? feature.attribute( schemaIdx ) : QVariant();
    ConstraintSemanticsOptions semanticsOptions;
    semanticsOptions.keepDisabled = options.keepDisabled;
    semanticsOptions.keepNonBlockingBarriers = options.keepNonBlockingBarriers;
    const ConstraintSemantics semantics =
        resolveConstraintSemantics( type, jsonText, schemaColumn, semanticsOptions, featureId );
    if ( !semantics.valid )
      throw QgsProcessingException( semantics.error );
    if ( semantics.semantic.isEmpty() )
    {
      ignore( semantics.ignoredReason );
      continue;
    }
    const std::optional<Semantic> semantic = semanticFromParams( semantics.semantic );
    if ( !semantic )
      throw QgsProcessingException( QStringLiteral( "不认识的约束语义 %1" ).arg( semantics.semantic ) );
    line.semantic = *semantic;
    line.enabled = semantics.enabled;
    line.blockMode = semantics.blockMode.toUtf8().toStdString();
    line.ratio = semantics.ratio;
    line.influenceRadius = semantics.influenceRadius;
    line.coreRadius = semantics.coreRadius;
    line.softStrength = semantics.softStrength;
    line.softRadius = semantics.softRadius;
    line.displayBuffer = semantics.displayBuffer;
    line.cartographicBuffer = semantics.cartographicBuffer;
    line.unit = semantics.unit.toUtf8().toStdString();

    const QgsMultiPolylineXY parts = geometry.isMultipart()
                                         ? geometry.asMultiPolyline()
                                         : QgsMultiPolylineXY{ geometry.asPolyline() };
    int partIndex = 0;
    for ( const QgsPolylineXY &part : parts )
    {
      ConstraintLine piece = line;
      if ( parts.size() > 1 )
        piece.stableId += "#" + std::to_string( partIndex );
      QVector<QPointF> vertices;
      vertices.reserve( part.size() );
      piece.points.reserve( static_cast<std::size_t>( part.size() ) );
      for ( const QgsPointXY &point : part )
      {
        piece.points.push_back( Point2{ point.x(), point.y() } );
        vertices.append( point.toQPointF() );
      }
      // 无效几何不静默参与数值：<2 顶点、零长、自交、非有限坐标逐部件忽略并留因。
      const ConstraintPartCheck check = checkConstraintPart( vertices );
      if ( !check.issue.isEmpty() )
        parsed.ignored.push_back( piece.stableId + " " + check.issue.toUtf8().toStdString() );
      else
        parsed.lines.push_back( std::move( piece ) );
      ++partIndex;
    }
  }
  return parsed;
}

} // namespace paleo::singlefactor
