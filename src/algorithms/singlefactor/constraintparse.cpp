// 层：数据
#include "constraintparse.h"

#include "structural.h" // isFullBlockMode / normalizeBlockMode（BLK_MODE 语义）

#include <qgscoordinatetransform.h>
#include <qgsfeature.h>
#include <qgsfeatureiterator.h>
#include <qgsfeaturerequest.h>
#include <qgsgeometry.h>
#include <qgsprocessingcontext.h>
#include <qgsprocessingutils.h>
#include <qgswkbtypes.h>

#include <QJsonDocument>
#include <QJsonObject>
#include <QJsonValue>

#include <memory>

namespace paleo::singlefactor
{
namespace
{

double jsonNumber( const QJsonObject &object, const QString &key, double fallback, bool *present )
{
  if ( !object.contains( key ) )
  {
    if ( present )
      *present = false;
    return fallback;
  }
  const QJsonValue value = object.value( key );
  if ( !value.isDouble() )
    throw QgsProcessingException( QStringLiteral( "约束参数 %1 不是数值" ).arg( key ) );
  if ( present )
    *present = true;
  return value.toDouble();
}

} // namespace

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
    QString jsonText;
    if ( jsonIdx >= 0 && !feature.attribute( jsonIdx ).isNull() )
      jsonText = feature.attribute( jsonIdx ).toString().trimmed();
    if ( !jsonText.isEmpty() )
    {
      QJsonParseError parseError;
      const QJsonDocument document = QJsonDocument::fromJson( jsonText.toUtf8(), &parseError );
      if ( parseError.error != QJsonParseError::NoError || !document.isObject() )
        throw QgsProcessingException(
            QStringLiteral( "约束 %1 的 params_json 无法解析" ).arg( featureId ) );
      const QJsonObject object = document.object();
      int schema = 1;
      if ( object.contains( QStringLiteral( "schemaVersion" ) ) )
      {
        const QJsonValue schemaValue = object.value( QStringLiteral( "schemaVersion" ) );
        if ( !schemaValue.isDouble() )
          throw QgsProcessingException( QStringLiteral( "schemaVersion 不是整数" ) );
        schema = schemaValue.toInt();
      }
      else if ( schemaIdx >= 0 && !feature.attribute( schemaIdx ).isNull() )
      {
        schema = feature.attribute( schemaIdx ).toInt();
      }
      if ( schema > 1 )
        throw QgsProcessingException( QStringLiteral( "不认识的约束参数版本 %1" ).arg( schema ) );
      const QJsonValue semanticValue = object.value( QStringLiteral( "semantic" ) );
      if ( !semanticValue.isString() )
        throw QgsProcessingException(
            QStringLiteral( "约束 %1 的 params_json 缺少 semantic" ).arg( featureId ) );
      const std::optional<Semantic> semantic = semanticFromParams( semanticValue.toString() );
      if ( !semantic )
        throw QgsProcessingException(
            QStringLiteral( "不认识的约束语义 %1" ).arg( semanticValue.toString() ) );
      if ( object.contains( QStringLiteral( "enabled" ) ) )
        line.enabled = object.value( QStringLiteral( "enabled" ) ).toBool( true );
      if ( !line.enabled && !options.keepDisabled )
      {
        ignore( QStringLiteral( "disabled" ) );
        continue;
      }
      line.semantic = *semantic;
      // 上游 constraint_semantics.py：hard_barrier 的 blockMode 决定真实
      // 语义——display_only 只停线不动趋势，其余非阻断词一律忽略
      //（旧语义等价的 hard 线仍走 full_block）。
      const QJsonValue blockModeValue = object.value( QStringLiteral( "blockMode" ) );
      if ( blockModeValue.isString() )
        line.blockMode = blockModeValue.toString().trimmed().toUtf8().toStdString();
      if ( line.semantic == Semantic::HardBarrier && !options.keepNonBlockingBarriers &&
           !isFullBlockMode( line.blockMode ) )
      {
        if ( normalizeBlockMode( line.blockMode ) == "display_only" )
        {
          line.semantic = Semantic::ContourStop;
        }
        else
        {
          ignore( QStringLiteral( "non_blocking" ) );
          continue;
        }
      }
      bool present = false;
      line.ratio = jsonNumber( object, QStringLiteral( "ratio" ), line.ratio, &present );
      line.influenceRadius =
          jsonNumber( object, QStringLiteral( "influenceRadius" ), line.influenceRadius, &present );
      line.coreRadius = jsonNumber( object, QStringLiteral( "coreRadius" ), line.coreRadius, &present );
      line.softStrength =
          jsonNumber( object, QStringLiteral( "softStrength" ), line.softStrength, &present );
      line.softRadius = jsonNumber( object, QStringLiteral( "softRadius" ), line.softRadius, &present );
      line.displayBuffer =
          jsonNumber( object, QStringLiteral( "displayBuffer" ), line.displayBuffer, &present );
      line.cartographicBuffer = jsonNumber( object, QStringLiteral( "cartographicBuffer" ),
                                            line.cartographicBuffer, &present );
      if ( object.contains( QStringLiteral( "unit" ) ) &&
           object.value( QStringLiteral( "unit" ) ).isString() )
        line.unit = object.value( QStringLiteral( "unit" ) ).toString().toUtf8().toStdString();
    }
    else
    {
      const QString type = typeIdx >= 0 ? feature.attribute( typeIdx ).toString().trimmed() : QString();
      const std::optional<Semantic> semantic = semanticFromTypeColumn( type );
      if ( !semantic )
      {
        ignore( type.isEmpty() ? QStringLiteral( "unknown_type" )
                               : QStringLiteral( "unknown_type:%1" ).arg( type ) );
        continue;
      }
      line.semantic = *semantic;
    }

    const QgsMultiPolylineXY parts = geometry.isMultipart()
                                         ? geometry.asMultiPolyline()
                                         : QgsMultiPolylineXY{ geometry.asPolyline() };
    int partIndex = 0;
    for ( const QgsPolylineXY &part : parts )
    {
      ConstraintLine piece = line;
      if ( parts.size() > 1 )
        piece.stableId += "#" + std::to_string( partIndex );
      piece.points.reserve( static_cast<std::size_t>( part.size() ) );
      for ( const QgsPointXY &point : part )
        piece.points.push_back( Point2{ point.x(), point.y() } );
      if ( piece.points.size() < 2 )
        parsed.ignored.push_back( piece.stableId + " short_line" );
      else
        parsed.lines.push_back( std::move( piece ) );
      ++partIndex;
    }
  }
  return parsed;
}

} // namespace paleo::singlefactor
