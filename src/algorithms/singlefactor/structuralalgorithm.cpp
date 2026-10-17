// 层：数据
// Processing 包装：读图层、调用结构面核、写 Float64 栅格与 sidecar。
// 上游语义移植见 structural.cpp / wellacquisition.cpp。
#include "structuralalgorithm.h"
#include "singlefactor_internal.h"
#include "../rasterout.h"
#include "constraintparse.h"
#include "structural.h"
#include "wellacquisition.h"

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
#include <qgswkbtypes.h>

#include <gdal.h>

#include <QFile>
#include <QFileInfo>
#include <QJsonDocument>
#include <QJsonObject>
#include <QVariantList>

#include <algorithm>
#include <cmath>
#include <limits>
#include <memory>
#include <vector>

namespace sf = paleo::singlefactor;

namespace
{
using paleo::singlefactor::utf8;
using paleo::singlefactor::OutputGuard;
using paleo::singlefactor::sidecarPath;
using paleo::singlefactor::writeJson;
using paleo::singlefactor::pointsJson;

constexpr double kStructNodata = -9999.0;

QVariantMap polygonJson( const sf::Polygon &polygon )
{
  QVariantMap item;
  item.insert( QStringLiteral( "exterior" ), pointsJson( polygon.exterior.points ) );
  QVariantList holes;
  for ( const sf::Ring &hole : polygon.holes )
    holes << QVariant( pointsJson( hole.points ) );
  item.insert( QStringLiteral( "holes" ), holes );
  return item;
}

// 上游 _extract_current_boundaries：面要素外环+孔；闭合线要素按外环处理。
void appendGeometryBoundary( const QgsGeometry &geometry, std::vector<sf::Polygon> *out )
{
  const Qgis::GeometryType type = QgsWkbTypes::geometryType( geometry.wkbType() );
  if ( type == Qgis::GeometryType::Polygon )
  {
    const QList<QgsPolygonXY> parts = geometry.isMultipart() ? geometry.asMultiPolygon()
                                                           : QList<QgsPolygonXY>{ geometry.asPolygon() };
    for ( const QgsPolygonXY &polygon : parts )
    {
      if ( polygon.isEmpty() || polygon.first().size() < 3 )
        continue;
      sf::Polygon boundary;
      for ( const QgsPointXY &p : polygon.first() )
        boundary.exterior.points.push_back( sf::Point2{ p.x(), p.y() } );
      for ( int ringIndex = 1; ringIndex < polygon.size(); ++ringIndex )
      {
        const QgsPolylineXY &ring = polygon.at( ringIndex );
        if ( ring.size() < 3 )
          continue;
        sf::Ring hole;
        for ( const QgsPointXY &p : ring )
          hole.points.push_back( sf::Point2{ p.x(), p.y() } );
        boundary.holes.push_back( std::move( hole ) );
      }
      out->push_back( std::move( boundary ) );
    }
    return;
  }
  if ( type == Qgis::GeometryType::Line )
  {
    const QgsPolylineXY line = geometry.asPolyline();
    if ( line.size() < 3 )
      return;
    sf::Polygon boundary;
    for ( const QgsPointXY &p : line )
      boundary.exterior.points.push_back( sf::Point2{ p.x(), p.y() } );
    const sf::Point2 first = boundary.exterior.points.front();
    const sf::Point2 last = boundary.exterior.points.back();
    if ( first.x != last.x || first.y != last.y )
      boundary.exterior.points.push_back( first );
    out->push_back( std::move( boundary ) );
  }
}

std::vector<sf::Polygon> readBoundarySource( QgsProcessingFeatureSource *source )
{
  std::vector<sf::Polygon> boundaries;
  if ( !source )
    return boundaries;
  QgsFeatureIterator it = source->getFeatures( QgsFeatureRequest() );
  QgsFeature feature;
  while ( it.nextFeature( feature ) )
  {
    if ( !feature.hasGeometry() || feature.geometry().isEmpty() )
      continue;
    appendGeometryBoundary( feature.geometry(), &boundaries );
  }
  return boundaries;
}

// 井点在局部米网、边界在投影坐标（或反过来）时，直接点-in-面会把全部井判成
// 边界外。能转换就先把边界变到井点坐标系；没有坐标操作则保持原坐标。
void transformBoundariesTo( std::vector<sf::Polygon> &boundaries,
                            const QgsCoordinateReferenceSystem &from,
                            const QgsCoordinateReferenceSystem &to,
                            const QgsCoordinateTransformContext &context )
{
  if ( boundaries.empty() || !from.isValid() || !to.isValid() || from == to )
    return;
  const QgsCoordinateTransform transform( from, to, context );
  if ( !transform.isValid() )
    return;
  try
  {
    const auto mapPoint = [&transform]( sf::Point2 &point ) {
      const QgsPointXY xy = transform.transform( QgsPointXY( point.x, point.y ) );
      point.x = xy.x();
      point.y = xy.y();
    };
    for ( sf::Polygon &boundary : boundaries )
    {
      for ( sf::Point2 &point : boundary.exterior.points )
        mapPoint( point );
      for ( sf::Ring &hole : boundary.holes )
        for ( sf::Point2 &point : hole.points )
          mapPoint( point );
    }
  }
  catch ( const QgsCsException &error )
  {
    throw QgsProcessingException(
        QStringLiteral( "成图边界无法转换到井点坐标系：%1" ).arg( error.what() ) );
  }
}

// 上游 _extract_current_wells 的要素视图：属性保持字段声明序。
sf::WellFeatureRow wellRowFromFeature( const QgsFeature &feature )
{
  sf::WellFeatureRow row;
  const QgsFields fields = feature.fields();
  const QVariantList values = feature.attributes();
  for ( int i = 0; i < fields.size() && i < values.size(); ++i )
    row.attributes.append( { fields.at( i ).name(), values.at( i ) } );
  const QgsGeometry geometry = feature.geometry();
  if ( !geometry.isEmpty() &&
       QgsWkbTypes::geometryType( geometry.wkbType() ) == Qgis::GeometryType::Point )
  {
    row.isPoint = true;
    const QgsPointXY point =
        geometry.isMultipart() ? geometry.asMultiPoint().value( 0 ) : geometry.asPoint();
    row.x = point.x();
    row.y = point.y();
  }
  return row;
}

void writeDoubleGrid( const QString &path, int cols, int rows, double originX, double originY,
                      double pixelWidth, double pixelHeight,
                      const std::vector<double> &values,
                      const QgsCoordinateReferenceSystem &crs, const char *algorithmId,
                      const QString &canonicalCrsWkt )
{
  const double geoTransform[6] = { originX, pixelWidth, 0.0, originY, 0.0, pixelHeight };
  GDALDatasetH dataset =
      PaleoRasterOut::createDoubleRaster( path, cols, rows, geoTransform, crs, kStructNodata, canonicalCrsWkt );
  if ( !dataset )
    throw QgsProcessingException( QStringLiteral( "Cannot create output raster %1" ).arg( path ) );
  GDALSetMetadataItem( dataset, "PALEO_VALUE_SOURCE", "analysis", nullptr );
  GDALSetMetadataItem( dataset, "PALEO_ALGORITHM", algorithmId, nullptr );
  GDALRasterBandH band = GDALGetRasterBand( dataset, 1 );
  std::vector<double> row( static_cast<std::size_t>( cols ) );
  for ( int r = 0; r < rows; ++r )
  {
    for ( int c = 0; c < cols; ++c )
    {
      const double value =
          values[static_cast<std::size_t>( r ) * static_cast<std::size_t>( cols ) +
                 static_cast<std::size_t>( c )];
      row[static_cast<std::size_t>( c )] = std::isfinite( value ) ? value : kStructNodata;
    }
    if ( GDALRasterIO( band, GF_Write, 0, r, cols, 1, row.data(), cols, 1, GDT_Float64, 0, 0 ) !=
         CE_None )
    {
      GDALClose( dataset );
      throw QgsProcessingException( QStringLiteral( "GDAL write failed at row %1" ).arg( r ) );
    }
  }
  GDALClose( dataset );
}

} // namespace

QString StructuralIdwAlgorithm::shortHelpString() const
{
  return QStringLiteral(
      "Upstream-equivalent structural IDW (workflow.py structural_idw): boundary-union "
      "domain, direction-guide tangent kernels clipped to the domain, contour-stop lines "
      "demoted to interpretive soft boundaries, single-region local IDW trend. "
      "Well acquisition mirrors _extract_current_wells (soft inclusion, 0..1 clipping, "
      "x/y attribute fallback); empty value cells are skipped rather than borrowing "
      "unrelated numeric fields. Output is a Float64 GeoTIFF (nodata -9999, pixel "
      "centres on the analysis nodes) plus .qc.json and .structural.json sidecars." );
}

void StructuralIdwAlgorithm::initAlgorithm( const QVariantMap & )
{
  addParameter( new QgsProcessingParameterFeatureSource(
      QStringLiteral( "WELLS" ), QStringLiteral( "Wells (point layer)" ),
      QList<int>() << static_cast<int>( Qgis::ProcessingSourceType::VectorPoint ) ) );
  addParameter( new QgsProcessingParameterString(
      QStringLiteral( "WELL_ID_FIELD" ), QStringLiteral( "Well id field name" ),
      QStringLiteral( "well_id" ), false, true ) );
  addParameter( new QgsProcessingParameterField(
      QStringLiteral( "VALUE_FIELD" ), QStringLiteral( "Value field" ), QVariant(),
      QStringLiteral( "WELLS" ), Qgis::ProcessingFieldParameterDataType::Any ) );
  addParameter( new QgsProcessingParameterString(
      QStringLiteral( "FACTOR_NAME" ), QStringLiteral( "Factor name alias (optional)" ),
      QString(), false, true ) );
  addParameter( new QgsProcessingParameterEnum(
      QStringLiteral( "FACTOR_MODE" ), QStringLiteral( "Factor mode" ),
      QStringList() << QStringLiteral( "direct" ) << QStringLiteral( "ratio" ), false, 0 ) );
  addParameter( new QgsProcessingParameterString(
      QStringLiteral( "NUMERATOR_FIELD" ), QStringLiteral( "Ratio numerator field" ),
      QString(), false, true ) );
  addParameter( new QgsProcessingParameterString(
      QStringLiteral( "DENOMINATOR_FIELD" ), QStringLiteral( "Ratio denominator field" ),
      QString(), false, true ) );
  addParameter( new QgsProcessingParameterEnum(
      QStringLiteral( "VALUE_RANGE" ), QStringLiteral( "Value range" ),
      QStringList() << QStringLiteral( "ratio_0_1" ) << QStringLiteral( "data" ), false, 0 ) );
  addParameter( new QgsProcessingParameterFeatureSource(
      QStringLiteral( "BOUNDARY" ), QStringLiteral( "Mapping boundary (polygon/closed line)" ),
      QList<int>() << static_cast<int>( Qgis::ProcessingSourceType::VectorAnyGeometry ) ) );
  addParameter( new QgsProcessingParameterFeatureSource(
      QStringLiteral( "INTERPOLATION_AREA" ), QStringLiteral( "Interpolation area (optional)" ),
      QList<int>() << static_cast<int>( Qgis::ProcessingSourceType::VectorAnyGeometry ),
      QVariant(), true ) );
  addParameter( new QgsProcessingParameterFeatureSource(
      QStringLiteral( "CONSTRAINTS" ), QStringLiteral( "Constraint lines (optional)" ),
      QList<int>() << static_cast<int>( Qgis::ProcessingSourceType::VectorLine ),
      QVariant(), true ) );
  addParameter( new QgsProcessingParameterNumber(
      QStringLiteral( "GRID_RESOLUTION" ), QStringLiteral( "Grid resolution" ),
      Qgis::ProcessingNumberParameterType::Integer, 339 ) );
  addParameter( new QgsProcessingParameterNumber(
      QStringLiteral( "POWER" ), QStringLiteral( "IDW power" ),
      Qgis::ProcessingNumberParameterType::Double, 2.0 ) );
  addParameter( new QgsProcessingParameterNumber(
      QStringLiteral( "MIN_POINTS" ), QStringLiteral( "Minimum samples (non-extend)" ),
      Qgis::ProcessingNumberParameterType::Integer, 3 ) );
  addParameter( new QgsProcessingParameterNumber(
      QStringLiteral( "MAX_POINTS" ), QStringLiteral( "Max samples (non-extend, 0 = all)" ),
      Qgis::ProcessingNumberParameterType::Integer, 12 ) );
  addParameter( new QgsProcessingParameterNumber(
      QStringLiteral( "SEARCH_RADIUS" ), QStringLiteral( "Search radius (non-extend, 0 = auto)" ),
      Qgis::ProcessingNumberParameterType::Double, 0.0 ) );
  addParameter( new QgsProcessingParameterBoolean(
      QStringLiteral( "EXTEND_TO_BOUNDARY" ), QStringLiteral( "Extend trend to boundary" ), true ) );
  addParameter( new QgsProcessingParameterBoolean(
      QStringLiteral( "ENABLE_BARRIERS" ), QStringLiteral( "Enable barriers" ), true ) );
  addParameter( new QgsProcessingParameterBoolean(
      QStringLiteral( "ENABLE_DIRECTIONS" ), QStringLiteral( "Enable direction lines" ), true ) );
  addParameter( new QgsProcessingParameterNumber(
      QStringLiteral( "INTERPRETIVE_STRENGTH" ), QStringLiteral( "Interpretive boundary strength" ),
      Qgis::ProcessingNumberParameterType::Double, 0.35 ) );
  addParameter( new QgsProcessingParameterNumber(
      QStringLiteral( "BARRIER_BUFFER" ), QStringLiteral( "Barrier buffer distance" ),
      Qgis::ProcessingNumberParameterType::Double, 150.0 ) );
  addParameter( new QgsProcessingParameterBoolean(
      QStringLiteral( "BARRIER_BUFFER_AUTO" ), QStringLiteral( "Auto barrier buffer" ), false ) );
  addParameter( new QgsProcessingParameterNumber(
      QStringLiteral( "BARRIER_SHAPE_RADIUS" ), QStringLiteral( "Barrier shape radius" ),
      Qgis::ProcessingNumberParameterType::Double, 0.0 ) );
  addParameter( new QgsProcessingParameterNumber(
      QStringLiteral( "BARRIER_SHAPE_STRENGTH" ), QStringLiteral( "Barrier shape strength" ),
      Qgis::ProcessingNumberParameterType::Double, 1.0 ) );
  addParameter( new QgsProcessingParameterBoolean(
      QStringLiteral( "BARRIER_EXTEND" ), QStringLiteral( "Extend barrier tips" ), true ) );
  addParameter( new QgsProcessingParameterNumber(
      QStringLiteral( "BARRIER_EXTENSION_LIMIT" ),
      QStringLiteral( "Barrier extension limit (-1 = to boundary)" ),
      Qgis::ProcessingNumberParameterType::Double, -1.0 ) );
  addParameter( new QgsProcessingParameterNumber(
      QStringLiteral( "CONTOUR_STOP_BUFFER" ),
      QStringLiteral( "Contour stop buffer (-1 = auto)" ),
      Qgis::ProcessingNumberParameterType::Double, -1.0 ) );
  addParameter( new QgsProcessingParameterBoolean(
      QStringLiteral( "CLUSTER" ), QStringLiteral( "Well-cluster locality" ), true ) );
  addParameter( new QgsProcessingParameterBoolean(
      QStringLiteral( "LOCAL_GRID" ), QStringLiteral( "Allow an empty CRS as a local grid" ),
      false ) );
  addParameter( new QgsProcessingParameterRasterDestination(
      QStringLiteral( "OUTPUT" ), QStringLiteral( "Analysis raster" ) ) );
}

QVariantMap StructuralIdwAlgorithm::processAlgorithm( const QVariantMap &parameters,
                                                      QgsProcessingContext &context,
                                                      QgsProcessingFeedback *feedback )
{
  if ( feedback )
    feedback->setProgress( 0 );
  std::unique_ptr<QgsProcessingFeatureSource> wellsSource(
      parameterAsSource( parameters, QStringLiteral( "WELLS" ), context ) );
  if ( !wellsSource )
    throw QgsProcessingException( invalidSourceError( parameters, QStringLiteral( "WELLS" ) ) );

  const QgsCoordinateReferenceSystem crs = wellsSource->sourceCrs();
  const bool localGrid = parameterAsBool( parameters, QStringLiteral( "LOCAL_GRID" ), context );
  if ( crs.isGeographic() )
    throw QgsProcessingException( QStringLiteral( "经纬度必须先投影" ) );
  if ( !crs.isValid() && !localGrid )
    throw QgsProcessingException(
        QStringLiteral( "缺少投影坐标系。空坐标系只能在显式局部工程网下计算" ) );

  const QString outPath = parameterAsOutputLayer( parameters, QStringLiteral( "OUTPUT" ), context );
  if ( outPath.isEmpty() )
    throw QgsProcessingException( QStringLiteral( "Invalid OUTPUT raster destination" ) );

  // 入口校验（对齐姊妹引擎 localdirectionalgorithm.cpp 的 POWER 文案）：
  // 0/负/NaN 幂次若放进引擎，extend 模式下只会表现为整面缺井控。
  const double power = parameterAsDouble( parameters, QStringLiteral( "POWER" ), context );
  if ( !( power > 0.0 ) || !std::isfinite( power ) )
    throw QgsProcessingException( QStringLiteral( "POWER 必须为有限正数" ) );

  // ---- 边界与插值区（上游 _extract_current_boundaries）----
  std::unique_ptr<QgsProcessingFeatureSource> boundarySource(
      parameterAsSource( parameters, QStringLiteral( "BOUNDARY" ), context ) );
  if ( !boundarySource )
    throw QgsProcessingException( invalidSourceError( parameters, QStringLiteral( "BOUNDARY" ) ) );
  std::vector<sf::Polygon> boundaries = readBoundarySource( boundarySource.get() );
  if ( boundaries.empty() )
    throw QgsProcessingException( QStringLiteral( "边界图层中没有有效 polygon" ) );
  transformBoundariesTo( boundaries, boundarySource->sourceCrs(), crs,
                         context.transformContext() );
  std::unique_ptr<QgsProcessingFeatureSource> areaSource(
      parameterAsSource( parameters, QStringLiteral( "INTERPOLATION_AREA" ), context ) );
  std::vector<sf::Polygon> areas = readBoundarySource( areaSource.get() );
  if ( areaSource )
    transformBoundariesTo( areas, areaSource->sourceCrs(), crs, context.transformContext() );
  if ( feedback )
    feedback->setProgress( 5 );

  // ---- 井点采集（上游 _extract_current_wells）----
  sf::WellAcquisitionRequest acquire;
  acquire.wellIdField =
      parameterAsString( parameters, QStringLiteral( "WELL_ID_FIELD" ), context );
  acquire.factorMode =
      parameterAsEnumString( parameters, QStringLiteral( "FACTOR_MODE" ), context );
  acquire.valueField =
      parameterAsString( parameters, QStringLiteral( "VALUE_FIELD" ), context );
  acquire.factorName =
      parameterAsString( parameters, QStringLiteral( "FACTOR_NAME" ), context );
  acquire.numeratorField =
      parameterAsString( parameters, QStringLiteral( "NUMERATOR_FIELD" ), context );
  acquire.denominatorField =
      parameterAsString( parameters, QStringLiteral( "DENOMINATOR_FIELD" ), context );
  const QString valueRange =
      parameterAsEnumString( parameters, QStringLiteral( "VALUE_RANGE" ), context );
  if ( valueRange == QLatin1String( "ratio_0_1" ) )
  {
    acquire.valueMin = 0.0;
    acquire.valueMax = 1.0;
  }

  std::vector<sf::WellFeatureRow> rows;
  {
    QgsFeatureIterator it = wellsSource->getFeatures( QgsFeatureRequest() );
    QgsFeature feature;
    while ( it.nextFeature( feature ) )
    {
      if ( feedback && feedback->isCanceled() )
        throw QgsProcessingException( QStringLiteral( "Canceled" ) );
      rows.push_back( wellRowFromFeature( feature ) );
    }
  }
  const sf::WellAcquisitionResult acquired = sf::acquireWells( rows, boundaries, acquire );
  if ( acquired.wells.size() < 3 )
  {
    QString suffix;
    const int shown = std::min<int>( 8, acquired.skipped.size() );
    if ( shown > 0 )
      suffix = QStringLiteral( "。前几项原因：%1" )
                   .arg( QStringList( acquired.skipped.mid( 0, shown ) )
                             .join( QStringLiteral( "；" ) ) );
    const bool allOutside =
        !acquired.skipped.isEmpty() &&
        std::all_of( acquired.skipped.cbegin(), acquired.skipped.cend(),
                     []( const QString &reason ) {
                       return reason.contains( QStringLiteral( "边界外" ) );
                     } );
    if ( allOutside )
      suffix += QStringLiteral( "。成图边界与井点范围不相交，请改用「测区范围」" );
    throw QgsProcessingException(
        QStringLiteral( "有效井点不足，至少需要 3 个，当前仅 %1 个%2" )
            .arg( acquired.wells.size() )
            .arg( suffix ) );
  }
  if ( feedback )
    feedback->setProgress( 15 );

  // ---- 约束线 → 上游 barriers / directions / 软边界（语义见 structural.cpp 头注）----
  std::unique_ptr<QgsProcessingFeatureSource> constraintSource(
      parameterAsSource( parameters, QStringLiteral( "CONSTRAINTS" ), context ) );
  sf::ConstraintParseOptions parseOptions;
  parseOptions.keepDisabled = true;
  parseOptions.keepNonBlockingBarriers = true;
  const sf::ParsedConstraints parsed =
      sf::readConstraintLines( constraintSource.get(), crs, context, parseOptions );

  std::vector<sf::StructuralBarrier> barriers;
  std::vector<sf::StructuralDirection> directions;
  std::vector<sf::StructuralSoftBoundary> explicitSoft;
  for ( const sf::ConstraintLine &line : parsed.lines )
  {
    if ( line.semantic == sf::Semantic::HardBarrier )
    {
      sf::StructuralBarrier barrier;
      barrier.lineId = line.stableId;
      barrier.points = line.points;
      barrier.active = line.enabled;
      barrier.blockMode = line.blockMode;
      barriers.push_back( std::move( barrier ) );
    }
    else if ( line.semantic == sf::Semantic::ContourStop ||
              line.semantic == sf::Semantic::CartographicDetour )
    {
      // Paleo 制图停线语义 = 上游 display_only 打断线（仅等值线停线）。
      sf::StructuralBarrier barrier;
      barrier.lineId = line.stableId;
      barrier.points = line.points;
      barrier.active = line.enabled;
      barrier.blockMode =
          line.blockMode.empty() || sf::isFullBlockMode( line.blockMode )
              ? "display_only"
              : line.blockMode;
      barriers.push_back( std::move( barrier ) );
    }
    else if ( line.semantic == sf::Semantic::DirectionGuide )
    {
      sf::StructuralDirection direction;
      direction.lineId = line.stableId;
      direction.points = line.points;
      direction.active = line.enabled;
      direction.ratio = line.ratio;
      direction.influenceRadius = line.influenceRadius;
      direction.coreRadius = line.coreRadius;
      directions.push_back( std::move( direction ) );
    }
    else if ( line.semantic == sf::Semantic::InterpretiveBoundary )
    {
      // 停用的解释边界不得进趋势权重（keepDisabled 只为保留上游清单）。
      if ( !line.enabled )
        continue;
      // softRadius>0 显式半径；否则走与停线一致的自动半径。
      sf::StructuralSoftBoundary soft;
      soft.lineId = line.stableId;
      soft.points = line.points;
      soft.radius = line.softRadius;
      soft.strength = line.softStrength;
      explicitSoft.push_back( std::move( soft ) );
    }
  }
  if ( feedback )
    feedback->setProgress( 20 );

  // ---- 有效分辨率（上游 resolve_performance_grid_resolution）----
  double bx0 = std::numeric_limits<double>::max(), bx1 = std::numeric_limits<double>::lowest();
  double by0 = std::numeric_limits<double>::max(), by1 = std::numeric_limits<double>::lowest();
  for ( const sf::Polygon &boundary : boundaries )
  {
    for ( const sf::Point2 &p : boundary.exterior.points )
    {
      bx0 = std::min( bx0, p.x );
      bx1 = std::max( bx1, p.x );
      by0 = std::min( by0, p.y );
      by1 = std::max( by1, p.y );
    }
  }
  const int resolution = sf::resolvePerformanceGridResolution(
      bx1 - bx0, by1 - by0,
      parameterAsInt( parameters, QStringLiteral( "GRID_RESOLUTION" ), context ) );

  sf::StructuralRequest request;
  request.power = power;
  request.resolution = resolution;
  request.extendTrendToBoundary =
      parameterAsBool( parameters, QStringLiteral( "EXTEND_TO_BOUNDARY" ), context );
  request.enableBarriers =
      parameterAsBool( parameters, QStringLiteral( "ENABLE_BARRIERS" ), context );
  request.enableDirections =
      parameterAsBool( parameters, QStringLiteral( "ENABLE_DIRECTIONS" ), context );
  request.searchRadius =
      parameterAsDouble( parameters, QStringLiteral( "SEARCH_RADIUS" ), context );
  request.minPoints = parameterAsInt( parameters, QStringLiteral( "MIN_POINTS" ), context );
  request.maxPoints = parameterAsInt( parameters, QStringLiteral( "MAX_POINTS" ), context );
  request.barrierBufferDistance =
      parameterAsDouble( parameters, QStringLiteral( "BARRIER_BUFFER" ), context );
  request.barrierBufferAuto =
      parameterAsBool( parameters, QStringLiteral( "BARRIER_BUFFER_AUTO" ), context );
  request.barrierShapeRadius =
      parameterAsDouble( parameters, QStringLiteral( "BARRIER_SHAPE_RADIUS" ), context );
  request.barrierShapeStrength =
      parameterAsDouble( parameters, QStringLiteral( "BARRIER_SHAPE_STRENGTH" ), context );
  request.interpretiveBoundaryStrength =
      parameterAsDouble( parameters, QStringLiteral( "INTERPRETIVE_STRENGTH" ), context );
  request.contourStopBufferDistance =
      parameterAsDouble( parameters, QStringLiteral( "CONTOUR_STOP_BUFFER" ), context );
  request.wellClusterLocality =
      parameterAsBool( parameters, QStringLiteral( "CLUSTER" ), context );
  request.valueMin = acquire.valueMin;
  request.valueMax = acquire.valueMax;
  request.explicitSoftBoundaries = std::move( explicitSoft );

  sf::Control control;
  control.cancelled = [feedback]() { return feedback && feedback->isCanceled(); };
  control.progress = [feedback]( double fraction ) {
    if ( feedback )
      feedback->setProgress( 20.0 + 60.0 * std::clamp( fraction, 0.0, 1.0 ) );
  };
  const sf::StructuralResult surface = sf::buildStructuralSurface(
      acquired.wells, boundaries, barriers, directions, areas, request, control );
  if ( surface.status != sf::Status::Ok )
  {
    if ( surface.status == sf::Status::Cancelled )
      throw QgsProcessingException( QStringLiteral( "Canceled" ) );
    throw QgsProcessingException( utf8( surface.message.empty()
                                            ? std::string( sf::statusName( surface.status ) )
                                            : surface.message ) );
  }
  if ( feedback )
    feedback->setProgress( 82 );

  // ---- 输出：Float64 GTiff，像元中心 = 节点（north-up）----
  const int nx = static_cast<int>( surface.xAxis.size() );
  const int ny = static_cast<int>( surface.yAxis.size() );
  const double dx = nx > 1 ? surface.xAxis[1] - surface.xAxis[0] : 0.0;
  const double dy = ny > 1 ? surface.yAxis[1] - surface.yAxis[0] : 0.0;
  const double originX = surface.xAxis.front() - dx / 2.0;
  const double originY = surface.yAxis.back() + dy / 2.0;

  OutputGuard guard;
  const QString qcPath = sidecarPath( outPath, QStringLiteral( ".qc.json" ) );
  const QString structuralPath = sidecarPath( outPath, QStringLiteral( ".structural.json" ) );
  guard.paths << outPath << qcPath << structuralPath;

  // 引擎网格行 = 升序 y；栅格行 0 = 最大 y → 写盘时翻转行序。
  std::vector<double> rasterValues( surface.grid.size() );
  for ( int r = 0; r < ny; ++r )
  {
    const std::size_t src = static_cast<std::size_t>( ny - 1 - r ) * static_cast<std::size_t>( nx );
    const std::size_t dst = static_cast<std::size_t>( r ) * static_cast<std::size_t>( nx );
    std::copy( surface.grid.begin() + static_cast<std::ptrdiff_t>( src ),
               surface.grid.begin() + static_cast<std::ptrdiff_t>( src + nx ),
               rasterValues.begin() + static_cast<std::ptrdiff_t>( dst ) );
  }
  writeDoubleGrid( outPath, nx, ny, originX, originY, dx, -dy, rasterValues, crs,
                   "paleo:paleo_structural_idw",
                   PaleoRasterOut::canonicalWktFromParameters( parameters ) );

  // ---- .structural.json：等值线阶段所需的完整面模型 ----
  QVariantList barrierList;
  for ( const sf::StructuralBarrier &barrier : barriers )
  {
    QVariantMap item;
    item.insert( QStringLiteral( "id" ), utf8( barrier.lineId ) );
    item.insert( QStringLiteral( "points" ), pointsJson( barrier.points ) );
    item.insert( QStringLiteral( "active" ), barrier.active );
    item.insert( QStringLiteral( "blockMode" ), utf8( barrier.blockMode ) );
    barrierList << item;
  }
  QVariantList boundaryList;
  for ( const sf::Polygon &boundary : boundaries )
    boundaryList << polygonJson( boundary );
  QVariantList areaList;
  for ( const sf::Polygon &area : areas )
    areaList << polygonJson( area );

  QVariantList directionList;
  for ( const sf::StructuralDirectionInfo &direction : surface.directions )
  {
    QVariantMap item;
    item.insert( QStringLiteral( "line_id" ), utf8( direction.lineId ) );
    item.insert( QStringLiteral( "ratio" ), direction.ratio );
    item.insert( QStringLiteral( "influence_radius" ), direction.influenceRadius );
    item.insert( QStringLiteral( "core_radius" ), direction.coreRadius );
    item.insert( QStringLiteral( "active" ), direction.active );
    QVariantList pieces;
    for ( const std::vector<sf::Point2> &piece : direction.pieces )
      pieces << QVariant( pointsJson( piece ) );
    item.insert( QStringLiteral( "pieces" ), pieces );
    directionList << item;
  }
  QVariantList softList;
  for ( const sf::StructuralSoftBoundary &soft : surface.interpretiveBoundaries )
  {
    QVariantMap item;
    item.insert( QStringLiteral( "points" ), pointsJson( soft.points ) );
    item.insert( QStringLiteral( "radius" ), soft.radius );
    item.insert( QStringLiteral( "strength" ), soft.strength );
    softList << item;
  }

  QVariantMap fieldModel;
  fieldModel.insert( QStringLiteral( "cluster_span" ),
                     request.wellClusterLocality ? surface.span : 0.0 );
  fieldModel.insert( QStringLiteral( "search_radius" ),
                     surface.searchRadius ? QVariant( *surface.searchRadius ) : QVariant() );
  fieldModel.insert( QStringLiteral( "min_points" ), surface.resolvedMinPoints );
  fieldModel.insert( QStringLiteral( "max_points" ), surface.resolvedMaxPoints );
  fieldModel.insert( QStringLiteral( "power" ), request.power );
  fieldModel.insert( QStringLiteral( "directions" ), directionList );
  fieldModel.insert( QStringLiteral( "interpretive_boundaries" ), softList );

  QVariantMap partition;
  partition.insert( QStringLiteral( "version" ), 17 );
  partition.insert( QStringLiteral( "source" ), QStringLiteral( "shared_trend_grid" ) );
  partition.insert( QStringLiteral( "visible" ), false );
  partition.insert( QStringLiteral( "preserve_closed" ), false );
  partition.insert( QStringLiteral( "allow_open_crossing" ), false );
  partition.insert( QStringLiteral( "bridge_tip_gaps" ), false );
  partition.insert( QStringLiteral( "approach_bend" ), true );
  partition.insert( QStringLiteral( "smooth_tip_bridge" ), true );
  partition.insert( QStringLiteral( "close_source_rings" ), true );
  partition.insert( QStringLiteral( "boundary_semantics" ), QStringLiteral( "interpretive_soft" ) );
  partition.insert( QStringLiteral( "buffer_policy" ), QStringLiteral( "soft_interpolation" ) );
  partition.insert( QStringLiteral( "geometry_policy" ), QStringLiteral( "local_interpretive_detour" ) );
  partition.insert( QStringLiteral( "extension_limit" ),
                    parameterAsDouble( parameters, QStringLiteral( "BARRIER_EXTENSION_LIMIT" ), context ) );
  partition.insert( QStringLiteral( "work_transition_distance" ), request.barrierShapeRadius );
  partition.insert( QStringLiteral( "extend_ends" ),
                    parameterAsBool( parameters, QStringLiteral( "BARRIER_EXTEND" ), context ) );
  partition.insert( QStringLiteral( "shape_strength" ), request.barrierShapeStrength );
  partition.insert( QStringLiteral( "shape_radius" ), request.barrierShapeRadius );

  QVariantList xAxis;
  for ( const double v : surface.xAxis )
    xAxis << v;
  QVariantList yAxis;
  for ( const double v : surface.yAxis )
    yAxis << v;
  QVariantList gridRows;
  for ( int r = 0; r < ny; ++r )
  {
    QVariantList row;
    for ( int c = 0; c < nx; ++c )
    {
      const double value =
          surface.grid[static_cast<std::size_t>( r ) * static_cast<std::size_t>( nx ) +
                       static_cast<std::size_t>( c )];
      row << ( std::isfinite( value ) ? QVariant( value ) : QVariant() );
    }
    gridRows << QVariant( row );
  }
  QVariantList maskRows;
  for ( int r = 0; r < ny; ++r )
  {
    QVariantList row;
    for ( int c = 0; c < nx; ++c )
      row << static_cast<bool>( surface.validMask[static_cast<std::size_t>( r ) *
                                                     static_cast<std::size_t>( nx ) +
                                                 static_cast<std::size_t>( c )] );
    maskRows << QVariant( row );
  }

  QVariantMap model;
  model.insert( QStringLiteral( "schema_version" ), 1 );
  model.insert( QStringLiteral( "algorithm" ), QStringLiteral( "paleo:paleo_structural_idw" ) );
  QVariantMap gridMap;
  gridMap.insert( QStringLiteral( "x" ), xAxis );
  gridMap.insert( QStringLiteral( "y" ), yAxis );
  gridMap.insert( QStringLiteral( "z" ), gridRows );
  model.insert( QStringLiteral( "grid" ), gridMap );
  model.insert( QStringLiteral( "valid_mask" ), maskRows );
  model.insert( QStringLiteral( "boundaries" ), boundaryList );
  model.insert( QStringLiteral( "interpolation_areas" ), areaList );
  model.insert( QStringLiteral( "barriers" ), barrierList );
  model.insert( QStringLiteral( "barrier_buffer_distance" ), surface.barrierBufferDistance );
  model.insert( QStringLiteral( "contour_stop_buffer_distance" ),
                surface.contourStopBufferDistance );
  model.insert( QStringLiteral( "contour_partition" ), partition );
  model.insert( QStringLiteral( "field_model" ), fieldModel );
  model.insert( QStringLiteral( "value_min" ),
                surface.valueMin ? QVariant( *surface.valueMin ) : QVariant() );
  model.insert( QStringLiteral( "value_max" ),
                surface.valueMax ? QVariant( *surface.valueMax ) : QVariant() );
  writeJson( structuralPath, model );

  // ---- .qc.json：与 local_direction_idw 同顶层契约 + 采集/解析明细 ----
  QVariantList acceptedWells;
  for ( const sf::AcquiredWell &well : acquired.wells )
  {
    QVariantMap item;
    item.insert( QStringLiteral( "well_id" ), utf8( well.wellId ) );
    item.insert( QStringLiteral( "x" ), well.x );
    item.insert( QStringLiteral( "y" ), well.y );
    item.insert( QStringLiteral( "value" ), well.value );
    item.insert( QStringLiteral( "is_control_point" ), well.isControl );
    acceptedWells << item;
  }
  QVariantMap counts;
  counts.insert( QStringLiteral( "accepted" ), static_cast<int>( acquired.wells.size() ) );
  counts.insert( QStringLiteral( "skipped" ), acquired.skipped.size() );
  counts.insert( QStringLiteral( "soft_included" ), acquired.softIncluded );
  // U5b：finite/nodata 与姊妹引擎同口径（全网格计数）——workflow 血缘读
  // counts.finite/nodata 写 finite_cells/nodata_cells，此前 structural 恒 null。
  int finiteCells = 0;
  for ( const double value : surface.grid )
    if ( std::isfinite( value ) )
      ++finiteCells;
  counts.insert( QStringLiteral( "finite" ), finiteCells );
  counts.insert( QStringLiteral( "nodata" ),
                 static_cast<int>( surface.grid.size() ) - finiteCells );
  QVariantMap qc;
  qc.insert( QStringLiteral( "schema_version" ), 1 );
  qc.insert( QStringLiteral( "value_source" ), QStringLiteral( "analysis" ) );
  qc.insert( QStringLiteral( "extent_source" ), QStringLiteral( "boundary_domain" ) );
  qc.insert( QStringLiteral( "crs_mode" ),
             crs.isValid() ? QStringLiteral( "projected" ) : QStringLiteral( "local_engineering" ) );
  qc.insert( QStringLiteral( "counts" ), counts );
  qc.insert( QStringLiteral( "wells" ), acceptedWells );
  QVariantList skippedList;
  for ( const QString &reason : acquired.skipped )
    skippedList << reason;
  qc.insert( QStringLiteral( "skipped" ), skippedList );
  QVariantMap qcParams;
  qcParams.insert( QStringLiteral( "algorithm_id" ), QStringLiteral( "paleo:paleo_structural_idw" ) );
  qcParams.insert( QStringLiteral( "algorithm_version" ), QStringLiteral( "1.0.0" ) );
  qcParams.insert( QStringLiteral( "grid_resolution" ), resolution );
  qcParams.insert( QStringLiteral( "power" ), request.power );
  qcParams.insert( QStringLiteral( "extend_trend_to_boundary" ), request.extendTrendToBoundary );
  qcParams.insert( QStringLiteral( "well_cluster_locality" ), request.wellClusterLocality );
  qcParams.insert( QStringLiteral( "value_field" ), acquire.valueField );
  qcParams.insert( QStringLiteral( "factor_mode" ), acquire.factorMode );
  qcParams.insert( QStringLiteral( "value_range" ), valueRange );
  // U5a：补齐实际数值生效的参数（workflow 下发但此前不进参数指纹：不同参数
  // 产出不同栅格却共享同一 parameter_hash）。记 resolve 后生效值——extend 模式
  // 下 search/min/max 不参与数值（search_radius=null、min/max=1/0）。
  // 决策：CONTOUR_STOP_BUFFER / BARRIER_BUFFER_AUTO / BARRIER_EXTEND /
  // BARRIER_EXTENSION_LIMIT 只影响制图输出（等值线停线缓冲、分区延端），不改
  // 数值场，不进指纹；数值侧生效的 barrierBuffer 走请求值（softRadiusAuto 消费）。
  qcParams.insert( QStringLiteral( "search_radius" ),
                   surface.searchRadius ? QVariant( *surface.searchRadius ) : QVariant() );
  qcParams.insert( QStringLiteral( "min_points" ), surface.resolvedMinPoints );
  qcParams.insert( QStringLiteral( "max_points" ), surface.resolvedMaxPoints );
  qcParams.insert( QStringLiteral( "interpretive_strength" ),
                   request.interpretiveBoundaryStrength );
  qcParams.insert( QStringLiteral( "barrier_buffer" ), request.barrierBufferDistance );
  qcParams.insert( QStringLiteral( "barrier_shape_radius" ), request.barrierShapeRadius );
  qcParams.insert( QStringLiteral( "barrier_shape_strength" ), request.barrierShapeStrength );
  qcParams.insert( QStringLiteral( "enable_barriers" ), request.enableBarriers );
  qcParams.insert( QStringLiteral( "enable_directions" ), request.enableDirections );
  qc.insert( QStringLiteral( "parameters" ), qcParams );
  qc.insert( QStringLiteral( "directions" ), directionList );
  qc.insert( QStringLiteral( "interpretive_boundaries" ), softList );
  qc.insert( QStringLiteral( "barrier_buffer_distance" ), surface.barrierBufferDistance );
  qc.insert( QStringLiteral( "contour_stop_buffer_distance" ), surface.contourStopBufferDistance );
  qc.insert( QStringLiteral( "value_min" ),
             surface.valueMin ? QVariant( *surface.valueMin ) : QVariant() );
  qc.insert( QStringLiteral( "value_max" ),
             surface.valueMax ? QVariant( *surface.valueMax ) : QVariant() );
  qc.insert( QStringLiteral( "diagnostics" ), surface.diagnostics );
  // 引擎回执（P2）：按 buildStructuralSurface 实际进入趋势权重/方向核的集合产出。
  // contour_only 屏障模型——完全阻断线也只是解释软边界，回执绝不报 hard_barrier。
  {
    std::vector<std::string> directionIds;
    for ( const sf::StructuralDirectionInfo &direction : surface.directions )
      for ( const std::vector<sf::Point2> &piece : direction.pieces )
        if ( piece.size() >= 2 )
        {
          double length = 0;
          for ( std::size_t i = 1; i < piece.size(); ++i )
            length += std::hypot( piece[i].x - piece[i - 1].x, piece[i].y - piece[i - 1].y );
          if ( length > 1e-10 )
          {
            directionIds.push_back( direction.lineId );
            break;
          }
        }
    const auto has = []( const std::vector<std::string> &ids, const std::string &id ) {
      return std::find( ids.begin(), ids.end(), id ) != ids.end();
    };
    qc.insert( QStringLiteral( "constraint_receipts" ),
               sf::buildConstraintReceipts(
                   parsed.lines, parsed.ignored, [&]( const sf::ConstraintLine &line, QString *reason ) {
                     const bool stopLike = line.semantic == sf::Semantic::HardBarrier ||
                                           line.semantic == sf::Semantic::ContourStop ||
                                           line.semantic == sf::Semantic::CartographicDetour;
                     if ( ( stopLike || line.semantic == sf::Semantic::InterpretiveBoundary ) &&
                          has( surface.softBoundaryIds, line.stableId ) )
                       return QStringLiteral( "soft_boundary" );
                     if ( line.semantic == sf::Semantic::DirectionGuide && has( directionIds, line.stableId ) &&
                          line.ratio > 1.0 )
                       return QStringLiteral( "direction_guide" );
                     if ( stopLike )
                     {
                       const std::string mode =
                           line.semantic == sf::Semantic::HardBarrier ? line.blockMode
                           : line.blockMode.empty() || sf::isFullBlockMode( line.blockMode ) ? std::string( "display_only" )
                                                                                             : line.blockMode;
                       *reason = !sf::isContourStopMode( mode ) ? QStringLiteral( "non_blocking" )
                                 : QStringLiteral( "interpretive_strength_zero_or_barriers_disabled" );
                     }
                     else if ( line.semantic == sf::Semantic::DirectionGuide )
                       *reason = !request.enableDirections ? QStringLiteral( "directions_disabled_by_request" )
                                 : !( line.ratio > 1.0 )   ? QStringLiteral( "direction_ratio_not_above_one" )
                                                           : QStringLiteral( "outside_compute_domain" );
                     else if ( line.semantic == sf::Semantic::InterpretiveBoundary )
                       *reason = QStringLiteral( "soft_strength_zero" );
                     return QString();
                   } ) );
  }
  writeJson( qcPath, qc );
  if ( feedback )
    feedback->setProgress( 100 );
  guard.keep = true;

  QVariantMap out;
  out.insert( QStringLiteral( "OUTPUT" ), outPath );
  out.insert( QStringLiteral( "QC" ), qcPath );
  out.insert( QStringLiteral( "STRUCTURAL_MODEL" ), structuralPath );
  return out;
}
