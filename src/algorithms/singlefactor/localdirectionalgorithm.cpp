// 层：数据
// Processing 包装：读图层、调用数值核、写栅格。插值公式在 localidw.cpp。
#include "localdirectionalgorithm.h"
#include "../paleoalgorithms.h"
#include "../rasterout.h"
#include "localidw.h"
#include "samples.h"
#include "support.h"

#include <qgscoordinatereferencesystem.h>
#include <qgscoordinatetransform.h>
#include <qgsexception.h>
#include <qgsfeature.h>
#include <qgsfeatureiterator.h>
#include <qgsfeaturerequest.h>
#include <qgsgeometry.h>
#include <qgsprocessingcontext.h>
#include <qgsprocessingfeedback.h>
#include <qgsprocessingparameters.h>
#include <qgsprocessingutils.h>
#include <qgsrasterlayer.h>
#include <qgsrectangle.h>
#include <qgswkbtypes.h>

#include <cpl_conv.h>
#include <gdal.h>
#include <gdal_alg.h>
#include <ogr_api.h>

#include <QFile>
#include <QFileInfo>
#include <QJsonDocument>
#include <QJsonObject>
#include <QJsonValue>
#include <QTemporaryDir>

#include <algorithm>
#include <cmath>
#include <cstdint>
#include <limits>
#include <memory>
#include <optional>
#include <string>
#include <utility>
#include <vector>

namespace sf = paleo::singlefactor;

namespace
{

// 与既有单因素引擎的文件空值相同。内部 NaN 写成它，数值 0 保持 0。
constexpr float kFileNodata = -9999.0f;

QString utf8( const std::string &text )
{
  return QString::fromUtf8( text.data(), static_cast<qsizetype>( text.size() ) );
}

struct OutputGuard
{
  QStringList paths;
  bool keep = false;
  ~OutputGuard()
  {
    if ( keep )
      return;
    for ( const QString &path : paths )
      QFile::remove( path );
  }
};

struct GdalDataset
{
  GDALDatasetH ds = nullptr;
  ~GdalDataset()
  {
    if ( ds )
      GDALClose( ds );
  }
  void close()
  {
    if ( ds )
    {
      GDALClose( ds );
      ds = nullptr;
    }
  }
};

const char *semanticToken( sf::Semantic semantic )
{
  switch ( semantic )
  {
    case sf::Semantic::HardBarrier:
      return "hard_barrier";
    case sf::Semantic::DirectionGuide:
      return "direction_guide";
    case sf::Semantic::InterpretiveBoundary:
      return "interpretive_boundary";
    case sf::Semantic::ContourStop:
      return "contour_stop";
    case sf::Semantic::CartographicDetour:
      return "cartographic_detour";
    case sf::Semantic::Ignored:
      return "ignored";
  }
  return "ignored";
}

std::optional<sf::Semantic> semanticFromParams( const QString &token )
{
  if ( token == QLatin1String( "hard_barrier" ) )
    return sf::Semantic::HardBarrier;
  if ( token == QLatin1String( "direction_guide" ) )
    return sf::Semantic::DirectionGuide;
  if ( token == QLatin1String( "interpretive_boundary" ) )
    return sf::Semantic::InterpretiveBoundary;
  if ( token == QLatin1String( "contour_stop" ) )
    return sf::Semantic::ContourStop;
  if ( token == QLatin1String( "cartographic_detour" ) )
    return sf::Semantic::CartographicDetour;
  return std::nullopt;
}

std::optional<sf::Semantic> semanticFromTypeColumn( const QString &type )
{
  if ( type == QLatin1String( "break_line" ) )
    return sf::Semantic::HardBarrier;
  if ( type == QLatin1String( "direction_line" ) )
    return sf::Semantic::DirectionGuide;
  if ( type == QLatin1String( "interpretive_boundary" ) )
    return sf::Semantic::InterpretiveBoundary;
  if ( type == QLatin1String( "contour_stop" ) )
    return sf::Semantic::ContourStop;
  if ( type == QLatin1String( "cartographic_detour" ) )
    return sf::Semantic::CartographicDetour;
  return std::nullopt;
}

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

struct ParsedConstraints
{
  std::vector<sf::ConstraintLine> lines;
  std::vector<std::string> ignored;
};

ParsedConstraints readConstraintLines( QgsProcessingFeatureSource *constraints,
                                       const QgsCoordinateReferenceSystem &targetCrs,
                                       QgsProcessingContext &context )
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

    sf::ConstraintLine line;
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
      const std::optional<sf::Semantic> semantic = semanticFromParams( semanticValue.toString() );
      if ( !semantic )
        throw QgsProcessingException(
            QStringLiteral( "不认识的约束语义 %1" ).arg( semanticValue.toString() ) );
      if ( object.contains( QStringLiteral( "enabled" ) ) &&
           !object.value( QStringLiteral( "enabled" ) ).toBool( true ) )
      {
        ignore( QStringLiteral( "disabled" ) );
        continue;
      }
      line.semantic = *semantic;
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
      const std::optional<sf::Semantic> semantic = semanticFromTypeColumn( type );
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
      sf::ConstraintLine piece = line;
      if ( parts.size() > 1 )
        piece.stableId += "#" + std::to_string( partIndex );
      piece.points.reserve( static_cast<std::size_t>( part.size() ) );
      for ( const QgsPointXY &point : part )
        piece.points.push_back( sf::Point2{ point.x(), point.y() } );
      if ( piece.points.size() < 2 )
        parsed.ignored.push_back( piece.stableId + " short_line" );
      else
        parsed.lines.push_back( std::move( piece ) );
      ++partIndex;
    }
  }
  return parsed;
}

QVariantList pointsJson( const std::vector<sf::Point2> &points )
{
  QVariantList list;
  for ( const sf::Point2 &point : points )
  {
    QVariantList pair;
    pair << point.x << point.y;
    list << QVariant( pair );
  }
  return list;
}

QVariantMap parametersForHash( const sf::PreparedInput &input, const sf::GridSpec &grid,
                               const sf::SurfaceResult &result, const QString &field,
                               double cellSize, const QString &extentSource, const QString &crsMode )
{
  const sf::ResolvedParameters &resolved = result.resolved;
  QVariantMap parameters;
  parameters.insert( QStringLiteral( "algorithm_id" ), utf8( resolved.algorithmId ) );
  parameters.insert( QStringLiteral( "algorithm_version" ), utf8( resolved.algorithmVersion ) );
  parameters.insert( QStringLiteral( "semantic_profile" ), utf8( resolved.semanticProfile ) );
  parameters.insert( QStringLiteral( "hard_barrier_model" ), utf8( resolved.hardBarrierModel ) );
  parameters.insert( QStringLiteral( "duplicate_policy" ), utf8( resolved.duplicatePolicy ) );
  parameters.insert( QStringLiteral( "power" ), resolved.power );
  parameters.insert( QStringLiteral( "coverage" ),
                     resolved.coverage == sf::CoverageMode::DomainExtrapolation
                         ? QStringLiteral( "domain_extrapolation" )
                         : QStringLiteral( "well_supported" ) );
  parameters.insert( QStringLiteral( "well_cluster_locality" ), resolved.wellClusterLocality );
  parameters.insert( QStringLiteral( "require_full_coverage" ), resolved.requireFullCoverage );
  parameters.insert( QStringLiteral( "spacing_subsampled" ), resolved.spacingSubsampled );
  parameters.insert( QStringLiteral( "spacing" ), resolved.spacing );
  parameters.insert( QStringLiteral( "step" ), resolved.step );
  parameters.insert( QStringLiteral( "span" ), resolved.span );
  parameters.insert( QStringLiteral( "tolerance" ), resolved.tolerance );
  parameters.insert( QStringLiteral( "search_radius" ),
                     resolved.searchRadius ? QVariant( *resolved.searchRadius ) : QVariant() );
  parameters.insert( QStringLiteral( "supported_radius" ), resolved.supportedRadius );
  parameters.insert( QStringLiteral( "supported_min_points" ), resolved.supportedMinPoints );
  parameters.insert( QStringLiteral( "min_points" ), resolved.minPoints );
  parameters.insert( QStringLiteral( "max_points" ), resolved.maxPoints );
  parameters.insert( QStringLiteral( "value_unit" ), utf8( resolved.valueUnit ) );
  parameters.insert( QStringLiteral( "field" ), field );
  parameters.insert( QStringLiteral( "cell_size" ), cellSize );
  parameters.insert( QStringLiteral( "extent_source" ), extentSource );
  parameters.insert( QStringLiteral( "crs_mode" ), crsMode );
  parameters.insert( QStringLiteral( "crs" ), utf8( grid.crs ) );
  parameters.insert( QStringLiteral( "local_engineering" ), input.localEngineeringGrid );

  QVariantMap gridMap;
  gridMap.insert( QStringLiteral( "cols" ), grid.cols );
  gridMap.insert( QStringLiteral( "rows" ), grid.rows );
  gridMap.insert( QStringLiteral( "origin_x" ), grid.originX );
  gridMap.insert( QStringLiteral( "origin_y" ), grid.originY );
  gridMap.insert( QStringLiteral( "pixel_width" ), grid.pixelWidth );
  gridMap.insert( QStringLiteral( "pixel_height" ), grid.pixelHeight );
  gridMap.insert( QStringLiteral( "convention" ), utf8( grid.convention ) );
  parameters.insert( QStringLiteral( "grid" ), gridMap );

  QVariantList constraints;
  for ( const sf::ConstraintLine &line : input.constraints )
  {
    QVariantMap item;
    item.insert( QStringLiteral( "id" ), utf8( line.stableId ) );
    item.insert( QStringLiteral( "semantic" ), QString::fromLatin1( semanticToken( line.semantic ) ) );
    item.insert( QStringLiteral( "enabled" ), line.enabled );
    item.insert( QStringLiteral( "ratio" ), line.ratio );
    item.insert( QStringLiteral( "influence_radius" ), line.influenceRadius );
    item.insert( QStringLiteral( "core_radius" ), line.coreRadius );
    item.insert( QStringLiteral( "soft_strength" ), line.softStrength );
    item.insert( QStringLiteral( "soft_radius" ), line.softRadius );
    item.insert( QStringLiteral( "display_buffer" ), line.displayBuffer );
    item.insert( QStringLiteral( "cartographic_buffer" ), line.cartographicBuffer );
    item.insert( QStringLiteral( "unit" ), utf8( line.unit ) );
    item.insert( QStringLiteral( "points" ), pointsJson( line.points ) );
    constraints << item;
  }
  parameters.insert( QStringLiteral( "constraints" ), constraints );

  QVariantList directions;
  for ( const sf::ResolvedDirection &direction : resolved.directions )
  {
    QVariantMap item;
    item.insert( QStringLiteral( "id" ), utf8( direction.id ) );
    item.insert( QStringLiteral( "ratio" ), direction.ratio );
    item.insert( QStringLiteral( "influence" ), direction.influence );
    item.insert( QStringLiteral( "core" ), direction.core );
    directions << item;
  }
  parameters.insert( QStringLiteral( "directions" ), directions );

  QVariantList soft;
  for ( const sf::ResolvedSoft &boundary : resolved.soft )
  {
    QVariantMap item;
    item.insert( QStringLiteral( "id" ), utf8( boundary.id ) );
    item.insert( QStringLiteral( "radius" ), boundary.radius );
    item.insert( QStringLiteral( "strength" ), boundary.strength );
    soft << item;
  }
  parameters.insert( QStringLiteral( "soft_boundaries" ), soft );

  QVariantList ignored;
  for ( const std::string &item : input.ignored )
    ignored << utf8( item );
  parameters.insert( QStringLiteral( "ignored" ), ignored );

  QVariantList domain;
  for ( const sf::Polygon &polygon : input.domain )
  {
    QVariantMap item;
    item.insert( QStringLiteral( "exterior" ), pointsJson( polygon.exterior.points ) );
    QVariantList holes;
    for ( const sf::Ring &hole : polygon.holes )
      holes << QVariant( pointsJson( hole.points ) );
    item.insert( QStringLiteral( "holes" ), holes );
    domain << item;
  }
  parameters.insert( QStringLiteral( "domain" ), domain );
  return parameters;
}

sf::Polygon gridExtentPolygon( const sf::GridSpec &grid )
{
  const double left = grid.originX;
  const double right = grid.originX + static_cast<double>( grid.cols ) * grid.pixelWidth;
  const double top = grid.originY;
  const double bottom = grid.originY + static_cast<double>( grid.rows ) * grid.pixelHeight;
  sf::Polygon polygon;
  polygon.exterior.points = {
      sf::Point2{ left, top },
      sf::Point2{ right, top },
      sf::Point2{ right, bottom },
      sf::Point2{ left, bottom },
      sf::Point2{ left, top },
  };
  return polygon;
}

void writeFloatGrid( const QString &path, const sf::GridSpec &grid, const std::vector<double> &values,
                     const QgsCoordinateReferenceSystem &crs, const char *valueSource, const char *algorithmId )
{
  const double geoTransform[6] = { grid.originX, grid.pixelWidth, 0.0, grid.originY, 0.0, grid.pixelHeight };
  GDALDatasetH dataset =
      PaleoRasterOut::createFloatRaster( path, grid.cols, grid.rows, geoTransform, crs, kFileNodata );
  if ( !dataset )
    throw QgsProcessingException( QStringLiteral( "Cannot create output raster %1" ).arg( path ) );
  GDALSetMetadataItem( dataset, "PALEO_VALUE_SOURCE", valueSource, nullptr );
  GDALSetMetadataItem( dataset, "PALEO_ALGORITHM", algorithmId, nullptr );
  GDALRasterBandH band = GDALGetRasterBand( dataset, 1 );
  std::vector<float> row( static_cast<std::size_t>( grid.cols ) );
  for ( int r = 0; r < grid.rows; ++r )
  {
    for ( int c = 0; c < grid.cols; ++c )
    {
      const double value = values[static_cast<std::size_t>( r ) * static_cast<std::size_t>( grid.cols ) +
                                  static_cast<std::size_t>( c )];
      float stored = kFileNodata;
      if ( std::isfinite( value ) )
      {
        stored = static_cast<float>( value );
        if ( !std::isfinite( stored ) )
          stored = kFileNodata;
      }
      row[static_cast<std::size_t>( c )] = stored;
    }
    if ( GDALRasterIO( band, GF_Write, 0, r, grid.cols, 1, row.data(), grid.cols, 1, GDT_Float32, 0, 0 ) !=
         CE_None )
    {
      GDALClose( dataset );
      throw QgsProcessingException( QStringLiteral( "GDAL write failed at row %1" ).arg( r ) );
    }
  }
  GDALClose( dataset );
}

void writeSupportGrid( const QString &path, const sf::GridSpec &grid, const std::vector<std::uint8_t> &marks,
                       const QgsCoordinateReferenceSystem &crs )
{
  const double geoTransform[6] = { grid.originX, grid.pixelWidth, 0.0, grid.originY, 0.0, grid.pixelHeight };
  GDALDatasetH dataset = PaleoRasterOut::createByteRaster( path, grid.cols, grid.rows, geoTransform, crs );
  if ( !dataset )
    throw QgsProcessingException( QStringLiteral( "Cannot create support raster %1" ).arg( path ) );
  GDALRasterBandH band = GDALGetRasterBand( dataset, 1 );
  std::vector<unsigned char> row( static_cast<std::size_t>( grid.cols ) );
  for ( int r = 0; r < grid.rows; ++r )
  {
    for ( int c = 0; c < grid.cols; ++c )
    {
      const std::uint8_t mark = marks[static_cast<std::size_t>( r ) * static_cast<std::size_t>( grid.cols ) +
                                     static_cast<std::size_t>( c )];
      row[static_cast<std::size_t>( c )] = mark;
    }
    if ( GDALRasterIO( band, GF_Write, 0, r, grid.cols, 1, row.data(), grid.cols, 1, GDT_Byte, 0, 0 ) != CE_None )
    {
      GDALClose( dataset );
      throw QgsProcessingException( QStringLiteral( "GDAL support write failed at row %1" ).arg( r ) );
    }
  }
  GDALClose( dataset );
}

QString sidecarPath( const QString &rasterPath, const QString &suffix )
{
  const QFileInfo info( rasterPath );
  return info.absolutePath() + QLatin1Char( '/' ) + info.completeBaseName() + suffix;
}

void writeJson( const QString &path, const QVariantMap &root )
{
  QFile file( path );
  if ( !file.open( QIODevice::WriteOnly | QIODevice::Truncate ) )
    throw QgsProcessingException( QStringLiteral( "Cannot write %1" ).arg( path ) );
  file.write( QJsonDocument::fromVariant( root ).toJson( QJsonDocument::Indented ) );
}

void appendContour( OGRGeometryH geometry, double level, std::vector<sf::ContourPolyline> *out )
{
  if ( !geometry || !out )
    return;
  const OGRwkbGeometryType type = wkbFlatten( OGR_G_GetGeometryType( geometry ) );
  if ( type == wkbLineString )
  {
    sf::ContourPolyline poly;
    poly.level = level;
    const int count = OGR_G_GetPointCount( geometry );
    poly.points.reserve( static_cast<std::size_t>( count ) );
    for ( int i = 0; i < count; ++i )
      poly.points.push_back( sf::Point2{ OGR_G_GetX( geometry, i ), OGR_G_GetY( geometry, i ) } );
    if ( poly.points.size() >= 2 )
      out->push_back( std::move( poly ) );
    return;
  }
  if ( type == wkbMultiLineString || type == wkbGeometryCollection )
  {
    const int count = OGR_G_GetGeometryCount( geometry );
    for ( int i = 0; i < count; ++i )
      appendContour( OGR_G_GetGeometryRef( geometry, i ), level, out );
  }
}

std::vector<sf::ContourPolyline> contourAtLevels( GDALDatasetH source, const std::vector<double> &levels )
{
  GDALRasterBandH band = source ? GDALGetRasterBand( source, 1 ) : nullptr;
  if ( !band )
    throw QgsProcessingException( QStringLiteral( "分析场缺少波段 1" ) );
  QTemporaryDir dir;
  if ( !dir.isValid() )
    throw QgsProcessingException( QStringLiteral( "无法创建等值线临时目录" ) );
  GdalDataset output;
  const QString path = dir.filePath( QStringLiteral( "contours.gpkg" ) );
  GDALDriverH driver = GDALGetDriverByName( "GPKG" );
  if ( !driver )
    throw QgsProcessingException( QStringLiteral( "GDAL GPKG driver unavailable" ) );
  output.ds = GDALCreate( driver, path.toUtf8().constData(), 0, 0, 0, GDT_Unknown, nullptr );
  if ( !output.ds )
    throw QgsProcessingException( QStringLiteral( "无法创建临时等值线" ) );
  OGRSpatialReferenceH srs = const_cast<OGRSpatialReferenceH>( GDALGetSpatialRef( source ) );
  OGRLayerH layer = GDALDatasetCreateLayer( output.ds, "contours", srs, wkbLineString, nullptr );
  if ( !layer )
    throw QgsProcessingException( QStringLiteral( "无法创建临时等值线图层" ) );
  OGRFieldDefnH idField = OGR_Fld_Create( "ID", OFTInteger64 );
  OGR_L_CreateField( layer, idField, true );
  OGR_Fld_Destroy( idField );
  OGRFieldDefnH elevField = OGR_Fld_Create( "ELEV", OFTReal );
  OGR_L_CreateField( layer, elevField, true );
  OGR_Fld_Destroy( elevField );

  QStringList levelText;
  for ( double level : levels )
    levelText << QString::number( level, 'g', 17 );
  const QByteArray fixed = QByteArray( "FIXED_LEVELS=" ) + levelText.join( QLatin1Char( ',' ) ).toUtf8();
  int hasNodata = 0;
  const double nodata = GDALGetRasterNoDataValue( band, &hasNodata );
  const QByteArray nodataOpt = QByteArray( "NODATA=" ) + QByteArray::number( nodata, 'g', 17 );
  const char *options[5] = { fixed.constData(), "ID_FIELD=0", "ELEV_FIELD=1", nullptr, nullptr };
  if ( hasNodata )
  {
    options[3] = nodataOpt.constData();
    options[4] = nullptr;
  }
  const CPLErr rc = GDALContourGenerateEx( band, layer, options, nullptr, nullptr );
  if ( rc != CE_None )
    throw QgsProcessingException( QStringLiteral( "GDALContourGenerateEx failed (rc=%1)" ).arg( int( rc ) ) );

  std::vector<sf::ContourPolyline> lines;
  const int elevIndex = OGR_L_FindFieldIndex( layer, "ELEV", 1 );
  OGR_L_ResetReading( layer );
  while ( OGRFeatureH feature = OGR_L_GetNextFeature( layer ) )
  {
    const double level = elevIndex >= 0 ? OGR_F_GetFieldAsDouble( feature, elevIndex ) : 0.0;
    appendContour( OGR_F_GetGeometryRef( feature ), level, &lines );
    OGR_F_Destroy( feature );
  }
  output.close();
  return lines;
}

} // namespace

QString LocalDirectionIdwAlgorithm::shortHelpString() const
{
  return QStringLiteral(
      "Local-direction IDW of a numeric well field. Hard barriers use grid connectivity. "
      "Direction guides and interpretive boundaries change positive weights only. "
      "Contour stops and cartographic detours are recorded and do not change this analysis raster. "
      "A geographic CRS is rejected. An empty CRS is accepted only when LOCAL_GRID is true. "
      "The float GeoTIFF uses nodata -9999; value 0 is kept. A byte support raster and a QC JSON "
      "are written beside OUTPUT." );
}

void LocalDirectionIdwAlgorithm::initAlgorithm( const QVariantMap & )
{
  addParameter( new QgsProcessingParameterFeatureSource(
      QStringLiteral( "INPUT" ), QStringLiteral( "Wells (point layer)" ),
      QList<int>() << static_cast<int>( Qgis::ProcessingSourceType::VectorPoint ) ) );
  addParameter( new QgsProcessingParameterField(
      QStringLiteral( "FIELD" ), QStringLiteral( "Numeric field" ), QVariant(), QStringLiteral( "INPUT" ),
      Qgis::ProcessingFieldParameterDataType::Numeric ) );
  addParameter( new QgsProcessingParameterFeatureSource(
      QStringLiteral( "CONSTRAINTS" ), QStringLiteral( "Constraint lines (optional)" ),
      QList<int>() << static_cast<int>( Qgis::ProcessingSourceType::VectorLine ), QVariant(), true ) );
  addParameter( new QgsProcessingParameterNumber(
      QStringLiteral( "CELL_SIZE" ), QStringLiteral( "Cell size (map units)" ),
      Qgis::ProcessingNumberParameterType::Double, 1.0 ) );
  addParameter( new QgsProcessingParameterNumber(
      QStringLiteral( "POWER" ), QStringLiteral( "IDW power (finite and > 0)" ),
      Qgis::ProcessingNumberParameterType::Double, 2.0 ) );
  addParameter( new QgsProcessingParameterString(
      QStringLiteral( "COVERAGE" ), QStringLiteral( "well_supported or domain_extrapolation" ),
      QStringLiteral( "well_supported" ) ) );
  addParameter( new QgsProcessingParameterBoolean(
      QStringLiteral( "CLUSTER" ), QStringLiteral( "Well-cluster locality" ), false ) );
  addParameter( new QgsProcessingParameterBoolean(
      QStringLiteral( "LOCAL_GRID" ), QStringLiteral( "Allow an empty CRS as a local engineering grid" ),
      false ) );
  addParameter( new QgsProcessingParameterBoolean(
      QStringLiteral( "REQUIRE_FULL_COVERAGE" ), QStringLiteral( "Fail when a hard compartment has no well" ),
      false ) );
  addParameter( new QgsProcessingParameterBoolean(
      QStringLiteral( "PERCENT_TO_FRACTION" ), QStringLiteral( "Convert percent values to fractions" ), false ) );
  addParameter( new QgsProcessingParameterNumber(
      QStringLiteral( "MIN_POINTS" ), QStringLiteral( "Minimum samples" ),
      Qgis::ProcessingNumberParameterType::Integer, 3 ) );
  addParameter( new QgsProcessingParameterNumber(
      QStringLiteral( "MAX_POINTS" ), QStringLiteral( "Smooth truncation count (0 = no cap)" ),
      Qgis::ProcessingNumberParameterType::Integer, 12 ) );
  addParameter( new QgsProcessingParameterNumber(
      QStringLiteral( "SEARCH_RADIUS" ), QStringLiteral( "Search radius (0 = automatic)" ),
      Qgis::ProcessingNumberParameterType::Double, 0.0 ) );
  addParameter( new QgsProcessingParameterString(
      QStringLiteral( "VALUE_UNIT" ), QStringLiteral( "Value unit (optional)" ), QString(), false, true ) );
  addParameter( new QgsProcessingParameterExtent(
      QStringLiteral( "EXTENT" ), QStringLiteral( "Output extent (optional)" ), QVariant(), true ) );
  addParameter( new QgsProcessingParameterRasterDestination(
      QStringLiteral( "OUTPUT" ), QStringLiteral( "Analysis raster" ) ) );
}

QVariantMap LocalDirectionIdwAlgorithm::processAlgorithm( const QVariantMap &parameters,
                                                         QgsProcessingContext &context,
                                                         QgsProcessingFeedback *feedback )
{
  if ( feedback )
    feedback->setProgress( 0 );
  std::unique_ptr<QgsProcessingFeatureSource> source(
      parameterAsSource( parameters, QStringLiteral( "INPUT" ), context ) );
  if ( !source )
    throw QgsProcessingException( invalidSourceError( parameters, QStringLiteral( "INPUT" ) ) );
  const QString fieldName = parameterAsString( parameters, QStringLiteral( "FIELD" ), context );
  const int fieldIdx = source->fields().lookupField( fieldName );
  if ( fieldIdx < 0 )
    throw QgsProcessingException( QStringLiteral( "Z field '%1' not found on INPUT layer" ).arg( fieldName ) );
  const double cellSize = parameterAsDouble( parameters, QStringLiteral( "CELL_SIZE" ), context );
  if ( !( cellSize > 0.0 ) || !std::isfinite( cellSize ) )
    throw QgsProcessingException( QStringLiteral( "CELL_SIZE must be > 0" ) );
  const double power = parameterAsDouble( parameters, QStringLiteral( "POWER" ), context );
  if ( !( power > 0.0 ) || !std::isfinite( power ) )
    throw QgsProcessingException( QStringLiteral( "POWER 必须为有限正数" ) );
  const QString coverageText = parameterAsString( parameters, QStringLiteral( "COVERAGE" ), context );
  sf::CoverageMode coverage = sf::CoverageMode::WellSupported;
  if ( coverageText == QLatin1String( "well_supported" ) || coverageText.isEmpty() )
    coverage = sf::CoverageMode::WellSupported;
  else if ( coverageText == QLatin1String( "domain_extrapolation" ) )
    coverage = sf::CoverageMode::DomainExtrapolation;
  else
    throw QgsProcessingException( QStringLiteral( "未知 coverageMode：%1" ).arg( coverageText ) );

  const QgsCoordinateReferenceSystem crs = source->sourceCrs();
  const bool localGrid = parameterAsBool( parameters, QStringLiteral( "LOCAL_GRID" ), context );
  sf::CrsMode crsMode = sf::CrsMode::Projected;
  QString crsModeName = QStringLiteral( "projected" );
  if ( crs.isGeographic() )
    throw QgsProcessingException( QStringLiteral( "经纬度必须先投影" ) );
  if ( !crs.isValid() )
  {
    if ( !localGrid )
      throw QgsProcessingException(
          QStringLiteral( "缺少投影坐标系。空坐标系只能在显式局部工程网下计算" ) );
    crsMode = sf::CrsMode::LocalEngineering;
    crsModeName = QStringLiteral( "local_engineering" );
  }
  const QString outPath = parameterAsOutputLayer( parameters, QStringLiteral( "OUTPUT" ), context );
  if ( outPath.isEmpty() )
    throw QgsProcessingException( QStringLiteral( "Invalid OUTPUT raster destination" ) );

  std::vector<sf::Sample> rows;
  {
    const int wellIdx = source->fields().indexOf( QStringLiteral( "well_id" ) );
    const int nameIdx = source->fields().indexOf( QStringLiteral( "name" ) );
    const int overrideIdx = source->fields().indexOf( QStringLiteral( "component_override" ) );
    QgsFeatureIterator it = source->getFeatures( QgsFeatureRequest() );
    QgsFeature feature;
    int sequence = 0;
    while ( it.nextFeature( feature ) )
    {
      if ( feedback && feedback->isCanceled() )
        throw QgsProcessingException( QStringLiteral( "Canceled" ) );
      if ( !feature.hasGeometry() || feature.geometry().isEmpty() )
        continue;
      const QgsGeometry geometry = feature.geometry();
      const QgsPointXY point = geometry.isMultipart() ? geometry.asMultiPoint().value( 0 ) : geometry.asPoint();
      sf::Sample sample;
      sample.stableRowId = feature.id() >= 0 ? std::to_string( static_cast<long long>( feature.id() ) )
                                             : ( "row-" + std::to_string( ++sequence ) );
      QString wellId;
      if ( wellIdx >= 0 )
        wellId = feature.attribute( wellIdx ).toString().trimmed();
      if ( wellId.isEmpty() && nameIdx >= 0 )
        wellId = feature.attribute( nameIdx ).toString().trimmed();
      if ( wellId.isEmpty() )
        wellId = utf8( sample.stableRowId );
      sample.wellId = wellId.toUtf8().toStdString();
      sample.x = point.x();
      sample.y = point.y();
      bool ok = false;
      const double z = feature.attribute( fieldIdx ).toDouble( &ok );
      sample.value = ok ? z : std::numeric_limits<double>::quiet_NaN();
      sample.componentOverride = -1;
      if ( overrideIdx >= 0 && !feature.attribute( overrideIdx ).isNull() )
      {
        bool overrideOk = false;
        const int overrideValue = feature.attribute( overrideIdx ).toInt( &overrideOk );
        if ( overrideOk && overrideValue >= 0 )
          sample.componentOverride = overrideValue;
      }
      rows.push_back( std::move( sample ) );
    }
  }
  if ( rows.empty() )
    throw QgsProcessingException( QStringLiteral( "INPUT contains no usable point features" ) );
  if ( feedback )
    feedback->setProgress( 10 );

  std::unique_ptr<QgsProcessingFeatureSource> constraints(
      parameterAsSource( parameters, QStringLiteral( "CONSTRAINTS" ), context ) );
  const ParsedConstraints parsed = readConstraintLines( constraints.get(), crs, context );
  if ( feedback )
    feedback->setProgress( 20 );

  QString extentSource = QStringLiteral( "input_padded" );
  QgsRectangle extent;
  const QVariant extentValue = parameters.value( QStringLiteral( "EXTENT" ) );
  if ( extentValue.isValid() && !extentValue.isNull() && !extentValue.toString().trimmed().isEmpty() )
  {
    extent = parameterAsExtent( parameters, QStringLiteral( "EXTENT" ), context );
    if ( !extent.isFinite() || extent.isEmpty() )
      throw QgsProcessingException( QStringLiteral( "EXTENT 无效" ) );
    extentSource = QStringLiteral( "explicit" );
  }
  else
  {
    const QgsRectangle raw = source->sourceExtent();
    const double xPad = raw.width() > 0.0 ? raw.width() * 0.1 : cellSize;
    const double yPad = raw.height() > 0.0 ? raw.height() * 0.1 : cellSize;
    extent = QgsRectangle( raw.xMinimum() - xPad, raw.yMinimum() - yPad, raw.xMaximum() + xPad,
                           raw.yMaximum() + yPad );
  }
  const PaleoAlgoGuards::GridDims dims = PaleoAlgoGuards::gridDimsForExtent( extent, cellSize );
  sf::GridSpec grid;
  grid.cols = dims.cols;
  grid.rows = dims.rows;
  grid.originX = extent.xMinimum();
  grid.originY = extent.yMaximum();
  grid.pixelWidth = cellSize;
  grid.pixelHeight = -cellSize;
  if ( crs.isValid() )
  {
    const QString name = crs.authid().isEmpty() ? crs.description() : crs.authid();
    grid.crs = ( name.isEmpty() ? QStringLiteral( "projected" ) : name ).toUtf8().toStdString();
  }
  else
  {
    grid.crs = "local_engineering";
  }

  sf::SamplePrepRequest request;
  request.rows = std::move( rows );
  request.crs = crsMode;
  request.valueUnit = parameterAsString( parameters, QStringLiteral( "VALUE_UNIT" ), context ).toUtf8().toStdString();
  request.percentToFraction = parameterAsBool( parameters, QStringLiteral( "PERCENT_TO_FRACTION" ), context );
  sf::SamplePrepResult prepared = sf::prepareSamples( request );
  if ( prepared.status != sf::Status::Ok )
    throw QgsProcessingException( utf8( prepared.message.empty() ? std::string( "样本准备失败" ) : prepared.message ) );
  prepared.input.constraints = parsed.lines;
  prepared.input.ignored = parsed.ignored;
  // 没有单独的成图多边形时，输出范围就是成图域。空域不能进入网格求值。
  prepared.input.domain = { gridExtentPolygon( grid ) };

  sf::ResolvedParameters resolved;
  resolved.power = power;
  resolved.coverage = coverage;
  resolved.wellClusterLocality = parameterAsBool( parameters, QStringLiteral( "CLUSTER" ), context );
  resolved.requireFullCoverage = parameterAsBool( parameters, QStringLiteral( "REQUIRE_FULL_COVERAGE" ), context );
  resolved.minPoints = parameterAsInt( parameters, QStringLiteral( "MIN_POINTS" ), context );
  resolved.maxPoints = parameterAsInt( parameters, QStringLiteral( "MAX_POINTS" ), context );
  resolved.valueUnit = request.valueUnit;
  const double searchRadius = parameterAsDouble( parameters, QStringLiteral( "SEARCH_RADIUS" ), context );
  if ( searchRadius > 0.0 && std::isfinite( searchRadius ) )
    resolved.searchRadius = searchRadius;
  const std::string resolveError = sf::resolveParameters( prepared.input, grid, &resolved );
  if ( !resolveError.empty() )
    throw QgsProcessingException( utf8( resolveError ) );
  std::string budgetError;
  if ( !sf::gridBudgetOk( grid, static_cast<int>( prepared.input.samples.size() ), &budgetError ) )
    throw QgsProcessingException( utf8( budgetError ) );
  if ( feedback )
    feedback->setProgress( 25 );

  sf::Control control;
  control.cancelled = [feedback]() { return feedback && feedback->isCanceled(); };
  control.progress = [feedback]( double fraction ) {
    if ( feedback )
      feedback->setProgress( 25.0 + 55.0 * std::clamp( fraction, 0.0, 1.0 ) );
  };
  const sf::SurfaceResult surface = sf::evaluateLocalIdw( prepared.input, grid, resolved, control );
  if ( surface.status != sf::Status::Ok )
  {
    if ( surface.status == sf::Status::Cancelled )
      throw QgsProcessingException( QStringLiteral( "Canceled" ) );
    throw QgsProcessingException( utf8( surface.message.empty() ? std::string( sf::statusName( surface.status ) )
                                                               : surface.message ) );
  }
  const std::size_t cellCount = static_cast<std::size_t>( grid.cols ) * static_cast<std::size_t>( grid.rows );
  if ( surface.values.size() != cellCount || surface.marks.size() != cellCount )
    throw QgsProcessingException( QStringLiteral( "结果网格尺寸不一致" ) );

  OutputGuard guard;
  const QString supportPath = sidecarPath( outPath, QStringLiteral( ".support.tif" ) );
  const QString qcPath = sidecarPath( outPath, QStringLiteral( ".qc.json" ) );
  guard.paths << outPath << supportPath << qcPath;
  if ( feedback )
    feedback->setProgress( 80 );
  writeFloatGrid( outPath, grid, surface.values, crs, "analysis", "paleo:paleo_local_direction_idw" );
  writeSupportGrid( supportPath, grid, surface.marks, crs );

  QVariantMap counts;
  counts.insert( QStringLiteral( "finite" ), surface.finiteCells );
  counts.insert( QStringLiteral( "nodata" ), surface.nodataCells );
  counts.insert( QStringLiteral( "extrapolated" ), surface.extrapolatedCells );
  counts.insert( QStringLiteral( "barrier" ), surface.barrierCells );
  counts.insert( QStringLiteral( "original" ), prepared.input.originalCount );
  counts.insert( QStringLiteral( "valid" ), prepared.input.validCount );
  counts.insert( QStringLiteral( "missing" ), prepared.input.missingCount );
  counts.insert( QStringLiteral( "out_of_range" ), prepared.input.outOfRangeCount );
  counts.insert( QStringLiteral( "duplicate_extra" ), prepared.input.duplicateExtraRows );
  counts.insert( QStringLiteral( "conflict" ), prepared.input.conflictRows );
  QVariantList unsupported;
  for ( const sf::UnsupportedRegion &region : surface.unsupported )
  {
    QVariantMap item;
    item.insert( QStringLiteral( "id" ), region.id );
    if ( std::isfinite( region.area ) )
      item.insert( QStringLiteral( "area" ), region.area );
    item.insert( QStringLiteral( "reason" ), utf8( region.reason ) );
    unsupported << item;
  }
  QVariantMap qc;
  qc.insert( QStringLiteral( "schema_version" ), 1 );
  qc.insert( QStringLiteral( "value_source" ), QStringLiteral( "analysis" ) );
  qc.insert( QStringLiteral( "extent_source" ), extentSource );
  qc.insert( QStringLiteral( "crs_mode" ), crsModeName );
  qc.insert( QStringLiteral( "counts" ), counts );
  qc.insert( QStringLiteral( "unsupported" ), unsupported );
  qc.insert( QStringLiteral( "parameters" ),
             parametersForHash( prepared.input, grid, surface, fieldName, cellSize, extentSource, crsModeName ) );
  writeJson( qcPath, qc );
  if ( feedback )
    feedback->setProgress( 100 );
  guard.keep = true;

  QVariantMap out;
  out.insert( QStringLiteral( "OUTPUT" ), outPath );
  out.insert( QStringLiteral( "SUPPORT" ), supportPath );
  out.insert( QStringLiteral( "QC" ), qcPath );
  out.insert( QStringLiteral( "FINITE_CELLS" ), surface.finiteCells );
  out.insert( QStringLiteral( "NODATA_CELLS" ), surface.nodataCells );
  out.insert( QStringLiteral( "EXTRAPOLATED_CELLS" ), surface.extrapolatedCells );
  out.insert( QStringLiteral( "BARRIER_CELLS" ), surface.barrierCells );
  out.insert( QStringLiteral( "EXTENT_SOURCE" ), extentSource );
  out.insert( QStringLiteral( "CRS_MODE" ), crsModeName );
  return out;
}

QString CartographicWorkAlgorithm::shortHelpString() const
{
  return QStringLiteral(
      "Build a cartographic work field from an existing analysis raster. "
      "Enabled contour_stop and cartographic_detour lines that properly cross contours "
      "at LEVELS reshape a copy. The analysis file is opened read-only and is not replaced. "
      "OUTPUT is a separate north-up GeoTIFF." );
}

void CartographicWorkAlgorithm::initAlgorithm( const QVariantMap & )
{
  addParameter( new QgsProcessingParameterRasterLayer(
      QStringLiteral( "INPUT" ), QStringLiteral( "Analysis raster" ) ) );
  addParameter( new QgsProcessingParameterFeatureSource(
      QStringLiteral( "CONSTRAINTS" ), QStringLiteral( "Cartographic lines (optional)" ),
      QList<int>() << static_cast<int>( Qgis::ProcessingSourceType::VectorLine ), QVariant(), true ) );
  addParameter( new QgsProcessingParameterString(
      QStringLiteral( "LEVELS" ), QStringLiteral( "Comma-separated finite contour levels" ) ) );
  addParameter( new QgsProcessingParameterNumber(
      QStringLiteral( "TRANSITION" ), QStringLiteral( "Transition distance (0 = kernel default)" ),
      Qgis::ProcessingNumberParameterType::Double, 0.0, true, 0.0 ) );
  addParameter( new QgsProcessingParameterRasterDestination(
      QStringLiteral( "OUTPUT" ), QStringLiteral( "Cartographic work raster" ) ) );
}

QVariantMap CartographicWorkAlgorithm::processAlgorithm( const QVariantMap &parameters,
                                                        QgsProcessingContext &context,
                                                        QgsProcessingFeedback *feedback )
{
  QgsRasterLayer *raster = parameterAsRasterLayer( parameters, QStringLiteral( "INPUT" ), context );
  if ( !raster || !raster->isValid() )
    throw QgsProcessingException( QStringLiteral( "分析场栅格无法打开" ) );
  const QString inPath = raster->source().section( QLatin1Char( '|' ), 0, 0 );
  const QString outPath = parameterAsOutputLayer( parameters, QStringLiteral( "OUTPUT" ), context );
  if ( outPath.isEmpty() )
    throw QgsProcessingException( QStringLiteral( "Invalid OUTPUT raster destination" ) );
  if ( QFileInfo( inPath ).absoluteFilePath() == QFileInfo( outPath ).absoluteFilePath() )
    throw QgsProcessingException( QStringLiteral( "制图工作场不能覆盖分析场文件" ) );

  QVector<double> levelValues;
  const QString levelText = parameterAsString( parameters, QStringLiteral( "LEVELS" ), context );
  for ( const QString &part : levelText.split( QLatin1Char( ',' ), Qt::SkipEmptyParts ) )
  {
    bool ok = false;
    const double level = part.trimmed().toDouble( &ok );
    if ( !ok || !std::isfinite( level ) )
      throw QgsProcessingException( QStringLiteral( "等值级别必须为有限数值" ) );
    levelValues.push_back( level );
  }
  if ( levelValues.isEmpty() )
    throw QgsProcessingException( QStringLiteral( "等值级别为空" ) );
  const double transition = parameterAsDouble( parameters, QStringLiteral( "TRANSITION" ), context );
  if ( !std::isfinite( transition ) || transition < 0.0 )
    throw QgsProcessingException( QStringLiteral( "过渡宽度无效" ) );

  GdalDataset source;
  source.ds = GDALOpen( inPath.toUtf8().constData(), GA_ReadOnly );
  if ( !source.ds )
    throw QgsProcessingException( QStringLiteral( "cannot open analysis raster '%1'" ).arg( inPath ) );
  double geoTransform[6] = { 0, 0, 0, 0, 0, 0 };
  if ( GDALGetGeoTransform( source.ds, geoTransform ) != CE_None )
    throw QgsProcessingException( QStringLiteral( "分析场缺少 GeoTransform" ) );
  const double axisTol = std::max( std::abs( geoTransform[1] ), std::abs( geoTransform[5] ) ) * 1e-9;
  if ( std::abs( geoTransform[2] ) > axisTol || std::abs( geoTransform[4] ) > axisTol )
    throw QgsProcessingException( QStringLiteral( "首版栅格仅支持 north-up、pixel-is-area" ) );
  sf::GridSpec grid;
  grid.cols = GDALGetRasterXSize( source.ds );
  grid.rows = GDALGetRasterYSize( source.ds );
  grid.originX = geoTransform[0];
  grid.originY = geoTransform[3];
  grid.pixelWidth = geoTransform[1];
  grid.pixelHeight = geoTransform[5];
  const QgsCoordinateReferenceSystem crs = raster->crs();
  if ( crs.isValid() )
  {
    const QString name = crs.authid().isEmpty() ? crs.description() : crs.authid();
    grid.crs = ( name.isEmpty() ? QStringLiteral( "projected" ) : name ).toUtf8().toStdString();
  }
  else
  {
    grid.crs = "local_engineering";
  }
  std::string budgetError;
  if ( !sf::gridBudgetOk( grid, 0, &budgetError ) )
    throw QgsProcessingException( utf8( budgetError ) );

  GDALRasterBandH band = GDALGetRasterBand( source.ds, 1 );
  if ( !band )
    throw QgsProcessingException( QStringLiteral( "分析场缺少波段 1" ) );
  const std::size_t cellCount = static_cast<std::size_t>( grid.cols ) * static_cast<std::size_t>( grid.rows );
  std::vector<float> raw( cellCount );
  if ( GDALRasterIO( band, GF_Read, 0, 0, grid.cols, grid.rows, raw.data(), grid.cols, grid.rows, GDT_Float32, 0,
                     0 ) != CE_None )
    throw QgsProcessingException( QStringLiteral( "无法读取分析场" ) );
  int hasNodata = 0;
  const double nodata = GDALGetRasterNoDataValue( band, &hasNodata );
  std::vector<double> values( cellCount );
  std::vector<std::uint8_t> valid( cellCount, 0 );
  for ( std::size_t i = 0; i < cellCount; ++i )
  {
    const float sample = raw[i];
    if ( !std::isfinite( sample ) || ( hasNodata && static_cast<double>( sample ) == nodata ) )
    {
      values[i] = std::numeric_limits<double>::quiet_NaN();
      continue;
    }
    values[i] = sample;
    valid[i] = 1;
  }

  std::unique_ptr<QgsProcessingFeatureSource> constraints(
      parameterAsSource( parameters, QStringLiteral( "CONSTRAINTS" ), context ) );
  const ParsedConstraints parsed = readConstraintLines( constraints.get(), crs, context );
  std::vector<double> levels( levelValues.begin(), levelValues.end() );
  const std::vector<sf::ContourPolyline> contours = contourAtLevels( source.ds, levels );
  source.close();
  if ( feedback && feedback->isCanceled() )
    throw QgsProcessingException( QStringLiteral( "Canceled" ) );

  const sf::WorkField work = sf::buildCartographicWork( grid, values, valid, parsed.lines, contours, levels, transition );
  if ( work.status != sf::Status::Ok )
    throw QgsProcessingException( utf8( work.message.empty() ? std::string( sf::statusName( work.status ) ) : work.message ) );
  if ( work.values.size() != cellCount )
    throw QgsProcessingException( QStringLiteral( "工作场网格与分析场不一致" ) );

  OutputGuard guard;
  const QString qcPath = sidecarPath( outPath, QStringLiteral( ".qc.json" ) );
  guard.paths << outPath << qcPath;
  writeFloatGrid( outPath, grid, work.values, crs, "cartographic_work", "paleo:paleo_cartographic_work" );

  QVariantList used;
  for ( const std::string &id : work.usedConstraintIds )
    used << utf8( id );
  QVariantList ignored;
  for ( const std::string &item : parsed.ignored )
    ignored << utf8( item );
  QVariantList levelList;
  for ( double level : levels )
    levelList << level;
  QVariantMap qc;
  qc.insert( QStringLiteral( "schema_version" ), 1 );
  qc.insert( QStringLiteral( "value_source" ), QStringLiteral( "cartographic_work" ) );
  qc.insert( QStringLiteral( "modified_cells" ), work.modifiedCells );
  qc.insert( QStringLiteral( "unchanged" ), work.unchanged );
  qc.insert( QStringLiteral( "used_constraints" ), used );
  qc.insert( QStringLiteral( "ignored" ), ignored );
  qc.insert( QStringLiteral( "levels" ), levelList );
  qc.insert( QStringLiteral( "transition_distance" ), work.transitionDistance );
  writeJson( qcPath, qc );
  guard.keep = true;

  QVariantMap out;
  out.insert( QStringLiteral( "OUTPUT" ), outPath );
  out.insert( QStringLiteral( "QC" ), qcPath );
  out.insert( QStringLiteral( "MODIFIED_CELLS" ), work.modifiedCells );
  out.insert( QStringLiteral( "UNCHANGED" ), work.unchanged );
  out.insert( QStringLiteral( "VALUE_SOURCE" ), QStringLiteral( "cartographic_work" ) );
  return out;
}
