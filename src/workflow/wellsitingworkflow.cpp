// 层：功能
#include "wellsitingworkflow.h"

#include "../catalog/datacatalog.h"
#include "../domain/wellsitingplan.h"
#include "../metadata/paleoprojectstore.h"
#include "../metadata/wellsitingstore.h"
#include "../qgis/qgislayerservice.h"
#include "../qgis/qgisstyleservice.h"
#include "../services/projectdata.h"

#include <qgsgeometry.h>
#include <qgsvectorlayer.h>

#include <QDateTime>
#include <QDir>
#include <QFile>
#include <QFileInfo>
#include <QHash>
#include <QImage>
#include <QJsonArray>
#include <QJsonDocument>
#include <QJsonObject>
#include <QPainter>
#include <QSaveFile>
#include <QSet>
#include <QTextStream>

#include <algorithm>
#include <cmath>

namespace
{

using paleo::wellsiting::Point2;

// RFC4180 简易转义：含逗号/引号/换行的字段包引号、内嵌引号翻倍。
QString csvField( const QString &value )
{
  if ( value.contains( QLatin1Char( ',' ) ) || value.contains( QLatin1Char( '"' ) ) ||
       value.contains( QLatin1Char( '\n' ) ) || value.contains( QLatin1Char( '\r' ) ) )
    return QStringLiteral( "\"%1\"" ).arg( QString( value ).replace( QLatin1Char( '"' ),
                                                               QLatin1String( "\"\"" ) ) );
  return value;
}

QVariantMap metricsToMap( const paleo::wellsiting::ScenarioMetrics &metrics )
{
  QVariantMap m;
  m.insert( QStringLiteral( "hole_area_total" ), metrics.holeAreaTotal );
  m.insert( QStringLiteral( "hole_count" ), metrics.holeCount );
  m.insert( QStringLiteral( "coverage_ratio" ), metrics.coverageRatio );
  m.insert( QStringLiteral( "domain_mean_distance" ), metrics.domainMeanDistance );
  m.insert( QStringLiteral( "inter_well_mean_spacing" ), metrics.interWellMeanSpacing );
  m.insert( QStringLiteral( "weighted_density" ), metrics.weightedDensity );
  if ( !metrics.note.empty() )
    m.insert( QStringLiteral( "note" ), QString::fromStdString( metrics.note ) );
  return m;
}

QString coordsText( double value )
{
  return QString::number( value, 'f', 1 );
}

QString ringToWkt( const std::vector<Point2> &ring )
{
  if ( ring.size() < 3 )
    return QString();
  QString wkt = QStringLiteral( "(" );
  for ( std::size_t i = 0; i < ring.size(); ++i )
    wkt += QStringLiteral( "%1 %2%3" )
               .arg( coordsText( ring[i].x ), coordsText( ring[i].y ),
                     i + 1 < ring.size() ? QStringLiteral( ", " ) : QString() );
  wkt += QStringLiteral( ")" );
  return wkt;
}

// 空洞边界 WKT：单段 POLYGON，多段 MULTIPOLYGON（8 连通域角点相接时）。
QString boundaryWkt( const std::vector<std::vector<Point2>> &parts )
{
  QStringList polys;
  for ( const std::vector<Point2> &ring : parts )
  {
    const QString one = ringToWkt( ring );
    if ( !one.isEmpty() )
      polys.append( QStringLiteral( "(%1)" ).arg( one ) );
  }
  if ( polys.isEmpty() )
    return QString();
  if ( polys.size() == 1 )
    return QStringLiteral( "POLYGON%1" ).arg( polys.first() );
  return QStringLiteral( "MULTIPOLYGON(%1)" ).arg( polys.join( QStringLiteral( "," ) ) );
}

QJsonArray ringToJson( const std::vector<Point2> &ring )
{
  QJsonArray coords;
  for ( const Point2 &p : ring )
    coords.append( QJsonArray{ p.x, p.y } );
  if ( !ring.empty() )
    coords.append( QJsonArray{ ring.front().x, ring.front().y } ); // GeoJSON 闭合
  return coords;
}

paleo::wellsiting::Polygon ringFromQgs( const QgsPolylineXY &pts )
{
  paleo::wellsiting::Polygon polygon;
  polygon.exterior.points.reserve( pts.size() );
  for ( const QgsPointXY &p : pts )
    polygon.exterior.points.push_back( Point2{ p.x(), p.y() } );
  return polygon;
}

std::vector<Point2> lineFromQgs( const QgsPolylineXY &pts )
{
  std::vector<Point2> out;
  out.reserve( pts.size() );
  for ( const QgsPointXY &p : pts )
    out.push_back( Point2{ p.x(), p.y() } );
  return out;
}

paleo::wellsiting::Polygon rectDomain( double minX, double minY, double maxX, double maxY )
{
  paleo::wellsiting::Polygon domain;
  domain.exterior.points = { Point2{ minX, minY }, Point2{ maxX, minY }, Point2{ maxX, maxY },
                             Point2{ minX, maxY } };
  return domain;
}

std::vector<Point2> wellPoints( const QList<QVariantMap> &wells )
{
  std::vector<Point2> pts;
  pts.reserve( wells.size() );
  for ( const QVariantMap &w : wells )
    pts.push_back( Point2{ w.value( QStringLiteral( "x" ) ).toDouble(),
                           w.value( QStringLiteral( "y" ) ).toDouble() } );
  return pts;
}

// QGIS 几何 → 核多边形外环集（multipart 一并展开）。
std::vector<paleo::wellsiting::Polygon> polygonsFromWkt( const QString &wkt )
{
  std::vector<paleo::wellsiting::Polygon> out;
  const QgsGeometry geometry = QgsGeometry::fromWkt( wkt );
  if ( geometry.isNull() || geometry.isEmpty() )
    return out;
  const QgsMultiPolygonXY multi =
      geometry.isMultipart() ? geometry.asMultiPolygon()
                             : QgsMultiPolygonXY{ geometry.asPolygon() };
  for ( const QgsPolygonXY &poly : multi )
  {
    if ( poly.isEmpty() )
      continue;
    paleo::wellsiting::Polygon kernel = ringFromQgs( poly.front() );
    if ( kernel.exterior.points.size() >= 3 )
      out.push_back( std::move( kernel ) );
  }
  return out;
}

std::vector<std::vector<Point2>> linesFromWkt( const QString &wkt )
{
  std::vector<std::vector<Point2>> out;
  const QgsGeometry geometry = QgsGeometry::fromWkt( wkt );
  if ( geometry.isNull() || geometry.isEmpty() )
    return out;
  const QgsMultiPolylineXY multi =
      geometry.isMultipart() ? geometry.asMultiPolyline()
                             : QgsMultiPolylineXY{ geometry.asPolyline() };
  for ( const QgsPolylineXY &line : multi )
    if ( line.size() >= 2 )
      out.push_back( lineFromQgs( line ) );
  return out;
}

paleo::wellsiting::SitingOptions kernelOptions( const WellSitingParams &params )
{
  paleo::wellsiting::SitingOptions options;
  options.controlRadius = params.controlRadius;
  options.cellSize = params.cellSize;
  return options;
}

} // namespace

struct WellSitingWorkflow::Impl
{
  QPointer<QgisLayerService> layers;
  QPointer<PaleoProjectStore> store;
  QPointer<ProjectDataFacade> projectData;
  QPointer<DataCatalog> catalog;
  WellSitingStore *sitingStore = nullptr; // 非 QObject——裸指针（不持有）
  QString projectDir;

  std::function<QList<SitingConstraintGeometry>()> constraintProvider;
  std::function<QStringList()> faultCutsProvider;

  paleo::siting::ScenarioSet scenarios;
  bool scenariosLoaded = false;
};

WellSitingParams WellSitingParams::fromMap( const QVariantMap &map )
{
  WellSitingParams params;
  params.controlRadius = map.value( QStringLiteral( "control_radius" ), 2500.0 ).toDouble();
  params.cellSize = map.value( QStringLiteral( "cell_size" ), 100.0 ).toDouble();
  params.gridSpacing = map.value( QStringLiteral( "grid_spacing" ), 0.0 ).toDouble();
  params.lineBuffer = map.value( QStringLiteral( "line_buffer" ), 300.0 ).toDouble();
  params.boundaryMargin = map.value( QStringLiteral( "boundary_margin" ), 200.0 ).toDouble();
  params.minWellDistance = map.value( QStringLiteral( "min_well_distance" ), 0.0 ).toDouble();
  params.perHoleLimit = map.value( QStringLiteral( "per_hole_limit" ), 0 ).toInt();
  return params;
}

QVariantMap WellSitingParams::toMap() const
{
  QVariantMap map;
  map.insert( QStringLiteral( "control_radius" ), controlRadius );
  map.insert( QStringLiteral( "cell_size" ), cellSize );
  map.insert( QStringLiteral( "grid_spacing" ), gridSpacing );
  map.insert( QStringLiteral( "line_buffer" ), lineBuffer );
  map.insert( QStringLiteral( "boundary_margin" ), boundaryMargin );
  map.insert( QStringLiteral( "min_well_distance" ), minWellDistance );
  map.insert( QStringLiteral( "per_hole_limit" ), perHoleLimit );
  return map;
}

WellSitingWorkflow::WellSitingWorkflow( QgisLayerService *layers, PaleoProjectStore *store,
                                        QObject *parent )
  : QObject( parent ), d( new Impl )
{
  d->layers = layers;
  d->store = store;
}

WellSitingWorkflow::~WellSitingWorkflow()
{
  delete d;
}

void WellSitingWorkflow::setProjectData( ProjectDataFacade *projectData )
{
  d->projectData = projectData;
}

void WellSitingWorkflow::setCatalog( DataCatalog *catalog, const QString &projectDir )
{
  d->catalog = catalog;
  if ( !projectDir.isEmpty() )
    d->projectDir = projectDir;
  d->scenariosLoaded = false; // 换工程重载
  m_lastDiagnosis.clear();
  m_lastHoles.clear();
  m_lastCandidates.clear();
  m_lastEvaluation.clear();
}

void WellSitingWorkflow::setSitingStore( WellSitingStore *store )
{
  d->sitingStore = store;
  d->scenariosLoaded = false;
}

void WellSitingWorkflow::setConstraintProvider(
    std::function<QList<SitingConstraintGeometry>()> provider )
{
  d->constraintProvider = std::move( provider );
}

void WellSitingWorkflow::setFaultCutsProvider( std::function<QStringList()> wktProvider )
{
  d->faultCutsProvider = std::move( wktProvider );
}

// ---------------------------------------------------------------------------
// 输入归集与域解析
// ---------------------------------------------------------------------------

QList<QVariantMap> WellSitingWorkflow::realWellsForSiting() const
{
  QList<QVariantMap> out;
  // planned 隔离红线：实井只认 entityType=="well"。catalog 直查按
  // writeWellsGeoJson 同口径过滤（hasSurface 且坐标有限）；catalog 缺席时
  // 走 ProjectDataFacade::wells()（内部同样只查 "well"）并按坐标状态过滤。
  if ( d->catalog )
  {
    for ( const CatalogEntity &e : d->catalog->entities( QStringLiteral( "well" ) ) )
    {
      if ( !e.hasSurface || !std::isfinite( e.surfaceX ) || !std::isfinite( e.surfaceY ) )
        continue;
      QVariantMap m;
      m.insert( QStringLiteral( "id" ), e.id );
      m.insert( QStringLiteral( "name" ), e.name );
      m.insert( QStringLiteral( "x" ), e.surfaceX );
      m.insert( QStringLiteral( "y" ), e.surfaceY );
      out.append( m );
    }
    return out;
  }
  if ( d->projectData )
  {
    for ( const ProjectWell &w : d->projectData->wells() )
    {
      if ( w.coordinateStatus == QLatin1String( "invalid" ) ||
           w.coordinateStatus == QLatin1String( "missing" ) )
        continue;
      if ( !std::isfinite( w.surfaceX ) || !std::isfinite( w.surfaceY ) )
        continue;
      QVariantMap m;
      m.insert( QStringLiteral( "id" ), w.id );
      m.insert( QStringLiteral( "name" ), w.name );
      m.insert( QStringLiteral( "x" ), w.surfaceX );
      m.insert( QStringLiteral( "y" ), w.surfaceY );
      out.append( m );
    }
  }
  return out;
}

std::vector<paleo::wellsiting::Polygon>
WellSitingWorkflow::resolveDomain( const WellSitingParams &params,
                                   const QList<QVariantMap> &wells, QString *source ) const
{
  std::vector<paleo::wellsiting::Polygon> domain;
  if ( d->constraintProvider )
  {
    for ( const SitingConstraintGeometry &constraint : d->constraintProvider() )
    {
      if ( constraint.type != QLatin1String( "polygon" ) )
        continue;
      domain = polygonsFromWkt( constraint.wkt );
      if ( !domain.empty() )
      {
        if ( source )
          *source = tr( "工区边界约束" );
        return domain; // 第一个 polygon 约束即工区域
      }
    }
  }
  if ( d->catalog )
  {
    for ( const CatalogEntity &e : d->catalog->entities( QStringLiteral( "seismic_survey" ) ) )
    {
      paleo::wellsiting::Polygon survey;
      for ( const auto &corner : e.corners )
        survey.exterior.points.push_back( Point2{ corner.first, corner.second } );
      if ( survey.exterior.points.size() >= 3 )
      {
        domain.push_back( std::move( survey ) );
        if ( source )
          *source = tr( "地震工区角点（%1）" ).arg( e.name );
        return domain;
      }
    }
  }
  if ( source )
    *source =
        tr( "井位包围盒外扩 %1 m（无边界约束、无地震工区角点）" ).arg( params.controlRadius, 0, 'f', 0 );
  double minX = 1e18, minY = 1e18, maxX = -1e18, maxY = -1e18;
  for ( const QVariantMap &w : wells )
  {
    minX = std::min( minX, w.value( QStringLiteral( "x" ) ).toDouble() );
    minY = std::min( minY, w.value( QStringLiteral( "y" ) ).toDouble() );
    maxX = std::max( maxX, w.value( QStringLiteral( "x" ) ).toDouble() );
    maxY = std::max( maxY, w.value( QStringLiteral( "y" ) ).toDouble() );
  }
  domain.push_back( rectDomain( minX - params.controlRadius, minY - params.controlRadius,
                                maxX + params.controlRadius, maxY + params.controlRadius ) );
  return domain;
}

// ---------------------------------------------------------------------------
// 覆盖诊断
// ---------------------------------------------------------------------------

bool WellSitingWorkflow::runDiagnosis( const WellSitingParams &params, QString *error )
{
  const QList<QVariantMap> wells = realWellsForSiting();
  QString domainSource;
  const std::vector<paleo::wellsiting::Polygon> domain = resolveDomain( params, wells, &domainSource );
  if ( wells.isEmpty() )
  {
    if ( error )
      *error = tr( "没有可定位的实井——覆盖诊断需要实井集" );
    writeHolesLayer( paleo::wellsiting::CoverageReport{} ); // 不残留旧参数空洞
    return false;
  }

  const paleo::wellsiting::SitingOptions options = kernelOptions( params );
  const paleo::wellsiting::SitingField field =
      paleo::wellsiting::sampleField( domain, wellPoints( wells ), options );
  if ( field.status != paleo::wellsiting::Status::Ok )
  {
    if ( error )
      *error = QString::fromStdString( field.message );
    // 旧参数的空洞图层不残留：覆写空 FeatureCollection（声明保留）。
    writeHolesLayer( paleo::wellsiting::CoverageReport{} );
    return false;
  }
  const paleo::wellsiting::CoverageReport report =
      paleo::wellsiting::describeField( field );

  QVariantMap summary;
  summary.insert( QStringLiteral( "ok" ), true );
  summary.insert( QStringLiteral( "well_count" ), wells.size() );
  summary.insert( QStringLiteral( "hole_count" ), report.holeCount );
  summary.insert( QStringLiteral( "hole_area_total" ), report.holeAreaTotal );
  summary.insert( QStringLiteral( "hole_area_km2" ), report.holeAreaTotal / 1.0e6 );
  summary.insert( QStringLiteral( "coverage_ratio" ), report.coverageRatio );
  summary.insert( QStringLiteral( "domain_area_km2" ), field.domainArea / 1.0e6 );
  const bool finiteMean = std::isfinite( report.domainMeanDistance );
  summary.insert( QStringLiteral( "domain_mean_distance" ),
                  finiteMean ? QVariant( report.domainMeanDistance ) : QVariant() );
  summary.insert( QStringLiteral( "domain_source" ), domainSource );
  if ( report.spacing.valid )
  {
    summary.insert( QStringLiteral( "spacing_mean" ), report.spacing.mean );
    summary.insert( QStringLiteral( "spacing_median" ), report.spacing.median );
    summary.insert( QStringLiteral( "spacing_p90" ), report.spacing.p90 );
  }
  summary.insert( QStringLiteral( "note" ), QString::fromStdString( report.note ) );
  m_lastDiagnosis = summary;

  m_lastHoles.clear();
  for ( const paleo::wellsiting::HoleRegion &region : report.regions )
  {
    QVariantMap hole;
    hole.insert( QStringLiteral( "id" ), region.id );
    hole.insert( QStringLiteral( "area_m2" ), region.area );
    hole.insert( QStringLiteral( "area_km2" ), region.area / 1.0e6 );
    hole.insert( QStringLiteral( "cell_count" ), region.cellCount );
    hole.insert( QStringLiteral( "max_distance_m" ),
                 std::isfinite( region.maxDistance ) ? QVariant( region.maxDistance )
                                                     : QVariant() );
    hole.insert( QStringLiteral( "deepest_x" ), region.deepest.x );
    hole.insert( QStringLiteral( "deepest_y" ), region.deepest.y );
    hole.insert( QStringLiteral( "wkt" ), boundaryWkt( region.boundaryParts ) );
    m_lastHoles.append( hole );
  }

  writeHolesLayer( report );
  emit diagnosisDone( summary );
  return true;
}

void WellSitingWorkflow::writeHolesLayer( const paleo::wellsiting::CoverageReport &report )
{
  if ( d->projectDir.isEmpty() || !d->layers )
    return; // 无工程落图面（纯计算用法）——报告仍有效
  const QString path = QDir( d->projectDir ).filePath(
      QStringLiteral( "artifacts/layers/wellsiting_holes.geojson" ) );
  const bool hadFile = QFile::exists( path );
  if ( report.regions.empty() && !hadFile )
    return; // 无空洞且从未声明——不造空图层（wells 层同一约定）

  QJsonArray features;
  for ( const paleo::wellsiting::HoleRegion &region : report.regions )
  {
    if ( region.boundaryParts.empty() )
      continue;
    QJsonObject geometry;
    if ( region.boundaryParts.size() == 1 )
    {
      geometry.insert( QStringLiteral( "type" ), QStringLiteral( "Polygon" ) );
      geometry.insert( QStringLiteral( "coordinates" ), ringToJson( region.boundaryParts.front() ) );
    }
    else
    {
      QJsonArray polys;
      for ( const std::vector<Point2> &ring : region.boundaryParts )
      {
        QJsonArray one;
        one.append( ringToJson( ring ) );
        polys.append( one );
      }
      geometry.insert( QStringLiteral( "type" ), QStringLiteral( "MultiPolygon" ) );
      geometry.insert( QStringLiteral( "coordinates" ), polys );
    }
    QJsonObject props;
    props.insert( QStringLiteral( "hole_id" ), region.id );
    props.insert( QStringLiteral( "area_m2" ), region.area );
    props.insert( QStringLiteral( "max_distance_m" ),
                  std::isfinite( region.maxDistance ) ? QJsonValue( region.maxDistance )
                                                      : QJsonValue() );
    QJsonObject feature;
    feature.insert( QStringLiteral( "type" ), QStringLiteral( "Feature" ) );
    feature.insert( QStringLiteral( "properties" ), props );
    feature.insert( QStringLiteral( "geometry" ), geometry );
    features.append( feature );
  }

  QJsonObject root;
  root.insert( QStringLiteral( "type" ), QStringLiteral( "FeatureCollection" ) );
  // 与 wells.geojson 同口径：legacy "crs" 成员写工程米制 WKT（OGR 认它，
  // 不落到 4326）。
  QJsonObject crsProps;
  crsProps.insert( QStringLiteral( "name" ), DataCatalog::localGridCrsWkt() );
  QJsonObject crs;
  crs.insert( QStringLiteral( "type" ), QStringLiteral( "name" ) );
  crs.insert( QStringLiteral( "properties" ), crsProps );
  root.insert( QStringLiteral( "crs" ), crs );
  root.insert( QStringLiteral( "features" ), features );

  QDir().mkpath( QFileInfo( path ).absolutePath() );
  {
    QSaveFile file( path );
    file.setDirectWriteFallback( false );
    if ( !file.open( QIODevice::WriteOnly | QIODevice::Truncate ) )
      return;
    const QByteArray bytes = QJsonDocument( root ).toJson();
    if ( !( file.write( bytes ) == bytes.size() && file.commit() ) )
      return;
  }

  LayerDeclaration decl;
  decl.layerId = QStringLiteral( "wellsiting_holes" );
  decl.type = QStringLiteral( "vector" );
  decl.source = path;
  decl.group = QStringLiteral( "07_Validation" ); // canonical 组：验证页档案可见
  decl.title = tr( "井网覆盖空洞" );
  QString declareError;
  if ( !d->layers->declare( decl, &declareError ) )
  {
    qWarning() << "WellSitingWorkflow: holes layer declare failed:" << declareError;
    return;
  }
  QString instantiateError;
  auto *layer = qobject_cast<QgsVectorLayer *>(
      d->layers->instantiate( QStringLiteral( "wellsiting_holes" ), &instantiateError ) );
  if ( layer )
  {
    QgisStyleService::applyHoleLayerStyle( layer );
    layer->triggerRepaint();
  }
}

// ---------------------------------------------------------------------------
// 候选点位
// ---------------------------------------------------------------------------

bool WellSitingWorkflow::generateCandidates( const WellSitingParams &params, QString *error )
{
  const QList<QVariantMap> wells = realWellsForSiting();
  if ( wells.isEmpty() )
  {
    if ( error )
      *error = tr( "没有可定位的实井——候选生成需要实井集" );
    return false;
  }
  const std::vector<paleo::wellsiting::Polygon> domain = resolveDomain( params, wells );
  const paleo::wellsiting::SitingOptions options = kernelOptions( params );
  const paleo::wellsiting::SitingField field =
      paleo::wellsiting::sampleField( domain, wellPoints( wells ), options );
  if ( field.status != paleo::wellsiting::Status::Ok )
  {
    if ( error )
      *error = QString::fromStdString( field.message );
    return false;
  }

  paleo::wellsiting::AvoidSurfaces avoid;
  avoid.lineBuffer = params.lineBuffer;
  if ( d->constraintProvider )
    for ( const SitingConstraintGeometry &constraint : d->constraintProvider() )
    {
      if ( constraint.type != QLatin1String( "line" ) )
        continue;
      for ( const std::vector<Point2> &line : linesFromWkt( constraint.wkt ) )
        avoid.lines.push_back( line );
    }
  if ( d->faultCutsProvider )
    for ( const QString &wkt : d->faultCutsProvider() )
      for ( const paleo::wellsiting::Polygon &polygon : polygonsFromWkt( wkt ) )
        avoid.polygons.push_back( polygon );

  paleo::wellsiting::CandidateOptions candidateOptions;
  candidateOptions.gridSpacing = params.gridSpacing;
  candidateOptions.minWellDistance = params.minWellDistance;
  candidateOptions.boundaryMargin = params.boundaryMargin;
  candidateOptions.perHoleLimit = params.perHoleLimit;
  const std::vector<paleo::wellsiting::CandidatePoint> candidates =
      paleo::wellsiting::generateCandidates( field, avoid, candidateOptions, options );

  m_lastCandidates.clear();
  int index = 0;
  for ( const paleo::wellsiting::CandidatePoint &candidate : candidates )
  {
    QVariantMap m;
    m.insert( QStringLiteral( "index" ), index++ );
    m.insert( QStringLiteral( "x" ), candidate.pos.x );
    m.insert( QStringLiteral( "y" ), candidate.pos.y );
    m.insert( QStringLiteral( "strategy" ), QString::fromStdString( candidate.strategy ) );
    m.insert( QStringLiteral( "hole_id" ), candidate.holeId );
    m.insert( QStringLiteral( "hole_distance_m" ), candidate.holeDistance );
    const paleo::wellsiting::CandidateContribution contribution =
        paleo::wellsiting::contributionOf( field, candidate.pos, options );
    m.insert( QStringLiteral( "contribution_hole_area_m2" ), contribution.holeAreaReduction );
    m.insert( QStringLiteral( "contribution_mean_distance_m" ),
              std::isfinite( contribution.meanDistanceReduction )
                  ? QVariant( contribution.meanDistanceReduction )
                  : QVariant() );
    m_lastCandidates.append( m );
  }
  emit candidatesChanged();
  return true;
}

void WellSitingWorkflow::clearCandidates()
{
  m_lastCandidates.clear();
  emit candidatesChanged();
}

// ---------------------------------------------------------------------------
// planned 计划井 CRUD（catalog 实体 + siting 文档可见性面）
// ---------------------------------------------------------------------------

QList<QVariantMap> WellSitingWorkflow::plannedWells() const
{
  QList<QVariantMap> out;
  if ( !d->catalog )
    return out;
  ensureScenariosLoaded();
  for ( const CatalogEntity &e : d->catalog->entities( QStringLiteral( "planned" ) ) )
  {
    if ( !e.hasSurface || !std::isfinite( e.surfaceX ) || !std::isfinite( e.surfaceY ) )
      continue;
    if ( d->scenarios.isRetired( e.id ) )
      continue;
    QVariantMap m;
    m.insert( QStringLiteral( "id" ), e.id );
    m.insert( QStringLiteral( "name" ), d->scenarios.displayName( e.id, e.name ) );
    m.insert( QStringLiteral( "x" ), e.surfaceX );
    m.insert( QStringLiteral( "y" ), e.surfaceY );
    out.append( m );
  }
  return out;
}

QString WellSitingWorkflow::addPlannedWell( const QString &name, double x, double y,
                                            QString *error )
{
  if ( !d->catalog )
  {
    if ( error )
      *error = tr( "catalog 未绑定——无法创建计划井" );
    return QString();
  }
  if ( name.trimmed().isEmpty() || !std::isfinite( x ) || !std::isfinite( y ) )
  {
    if ( error )
      *error = tr( "计划井需要非空名称与有限坐标" );
    return QString();
  }
  CatalogEntity entity;
  entity.id = d->catalog->nextEntityId( QStringLiteral( "planned" ) );
  entity.entityType = QStringLiteral( "planned" );
  entity.name = name.trimmed();
  entity.hasSurface = true;
  entity.surfaceX = x;
  entity.surfaceY = y;
  // 与实井同口径：坐标是局部测网米原始值，真投影参数出现前一直用它。
  entity.coordinateStatus = QStringLiteral( "untransformed" );
  QString addError;
  if ( !d->catalog->addEntity( entity, &addError ) )
  {
    if ( error )
      *error = addError;
    return QString();
  }
  refreshPlannedLayer();
  emit plannedWellsChanged();
  return entity.id;
}

bool WellSitingWorkflow::movePlannedWell( const QString &entityId, double x, double y,
                                          QString *error )
{
  // catalog 实体不可变（无 update 面）——移动 = 弃旧建新（新 id）。
  // 方案引用按 id 存：旧 id 由 retired 面吸收，重存方案时用新 id。
  const CatalogEntity old = d->catalog ? d->catalog->entityById( entityId ) : CatalogEntity();
  if ( old.id.isEmpty() || old.entityType != QLatin1String( "planned" ) )
  {
    if ( error )
      *error = tr( "计划井 %1 不存在" ).arg( entityId );
    return false;
  }
  if ( !std::isfinite( x ) || !std::isfinite( y ) )
  {
    if ( error )
      *error = tr( "坐标必须有限" );
    return false;
  }
  ensureScenariosLoaded();
  const QString newName = d->scenarios.displayName( entityId, old.name );
  const QString newId = addPlannedWell( newName, x, y, error );
  if ( newId.isEmpty() )
    return false;
  d->scenarios.wellRenames.remove( entityId );
  retirePlannedWell( entityId );
  if ( !saveScenarioSet( error ) )
    return false;
  refreshPlannedLayer();
  emit plannedWellsChanged();
  return true;
}

bool WellSitingWorkflow::renamePlannedWell( const QString &entityId, const QString &name,
                                            QString *error )
{
  if ( name.trimmed().isEmpty() )
  {
    if ( error )
      *error = tr( "名称不能为空" );
    return false;
  }
  ensureScenariosLoaded();
  if ( !d->catalog || d->catalog->entityById( entityId ).id.isEmpty() )
  {
    if ( error )
      *error = tr( "计划井 %1 不存在" ).arg( entityId );
    return false;
  }
  // 实体名不可变——显示名覆盖落在 siting 文档（catalog 保持审计痕迹）。
  d->scenarios.wellRenames.insert( entityId, name.trimmed() );
  if ( !saveScenarioSet( error ) )
    return false;
  refreshPlannedLayer(); // 图层 name 属性随显示名走
  emit plannedWellsChanged();
  return true;
}

bool WellSitingWorkflow::removePlannedWell( const QString &entityId, QString *error )
{
  const CatalogEntity entity =
      d->catalog ? d->catalog->entityById( entityId ) : CatalogEntity();
  if ( entity.id.isEmpty() || entity.entityType != QLatin1String( "planned" ) )
  {
    if ( error )
      *error = tr( "计划井 %1 不存在" ).arg( entityId );
    return false;
  }
  ensureScenariosLoaded();
  retirePlannedWell( entityId );
  if ( !saveScenarioSet( error ) )
    return false;
  refreshPlannedLayer();
  emit plannedWellsChanged();
  return true;
}

bool WellSitingWorkflow::isPlannedRetired( const QString &entityId ) const
{
  ensureScenariosLoaded();
  return d->scenarios.isRetired( entityId );
}

void WellSitingWorkflow::retirePlannedWell( const QString &entityId )
{
  if ( !d->scenarios.retiredWellIds.contains( entityId ) )
    d->scenarios.retiredWellIds.append( entityId );
}

void WellSitingWorkflow::ensureScenariosLoaded() const
{
  if ( d->scenariosLoaded || !d->sitingStore )
    return;
  paleo::siting::ScenarioSet set;
  QString loadError;
  if ( d->sitingStore->load( set, &loadError ) )
  {
    d->scenarios = set;
    d->scenariosLoaded = true;
  }
}

bool WellSitingWorkflow::saveScenarioSet( QString *error ) const
{
  if ( !d->sitingStore )
  {
    if ( error )
      *error = tr( "方案存储未绑定（工程未打开？）" );
    return false;
  }
  if ( !d->sitingStore->save( d->scenarios, error ) )
    return false;
  d->scenariosLoaded = true;
  return true;
}

void WellSitingWorkflow::refreshPlannedLayer()
{
  if ( d->projectDir.isEmpty() || !d->layers || !d->catalog )
    return;
  const QString path = QDir( d->projectDir ).filePath(
      QStringLiteral( "artifacts/layers/planned_wells.geojson" ) );
  const QList<QVariantMap> wells = plannedWells();
  const bool hadFile = QFile::exists( path );
  if ( wells.isEmpty() && !hadFile )
    return;

  QJsonArray features;
  for ( const QVariantMap &w : wells )
  {
    QJsonObject props;
    props.insert( QStringLiteral( "id" ), w.value( QStringLiteral( "id" ) ).toString() );
    props.insert( QStringLiteral( "name" ), w.value( QStringLiteral( "name" ) ).toString() );
    QJsonObject geometry;
    geometry.insert( QStringLiteral( "type" ), QStringLiteral( "Point" ) );
    geometry.insert( QStringLiteral( "coordinates" ),
                     QJsonArray{ w.value( QStringLiteral( "x" ) ).toDouble(),
                                 w.value( QStringLiteral( "y" ) ).toDouble() } );
    QJsonObject feature;
    feature.insert( QStringLiteral( "type" ), QStringLiteral( "Feature" ) );
    feature.insert( QStringLiteral( "properties" ), props );
    feature.insert( QStringLiteral( "geometry" ), geometry );
    features.append( feature );
  }
  QJsonObject root;
  root.insert( QStringLiteral( "type" ), QStringLiteral( "FeatureCollection" ) );
  QJsonObject crsProps;
  crsProps.insert( QStringLiteral( "name" ), DataCatalog::localGridCrsWkt() );
  QJsonObject crs;
  crs.insert( QStringLiteral( "type" ), QStringLiteral( "name" ) );
  crs.insert( QStringLiteral( "properties" ), crsProps );
  root.insert( QStringLiteral( "crs" ), crs );
  root.insert( QStringLiteral( "features" ), features );

  QDir().mkpath( QFileInfo( path ).absolutePath() );
  {
    QSaveFile file( path );
    file.setDirectWriteFallback( false );
    if ( !file.open( QIODevice::WriteOnly | QIODevice::Truncate ) )
      return;
    const QByteArray bytes = QJsonDocument( root ).toJson();
    if ( !( file.write( bytes ) == bytes.size() && file.commit() ) )
      return;
  }

  LayerDeclaration decl;
  decl.layerId = QStringLiteral( "planned_wells" );
  decl.type = QStringLiteral( "vector" );
  decl.source = path;
  decl.group = QStringLiteral( "00_Data" ); // 与 wells 同组：所有编图页档案可见
  decl.title = tr( "计划井（布井候选）" );
  QString declareError;
  if ( !d->layers->declare( decl, &declareError ) )
  {
    qWarning() << "WellSitingWorkflow: planned wells layer declare failed:" << declareError;
    return;
  }
  QString instantiateError;
  auto *layer = qobject_cast<QgsVectorLayer *>(
      d->layers->instantiate( QStringLiteral( "planned_wells" ), &instantiateError ) );
  if ( layer )
  {
    QgisStyleService::applyPlannedWellLayerStyle( layer );
    layer->triggerRepaint();
  }
}

// ---------------------------------------------------------------------------
// 方案评估与持久化
// ---------------------------------------------------------------------------

QVariantMap WellSitingWorkflow::evaluateScenario( const WellSitingParams &params,
                                                  const QStringList &plannedIds,
                                                  QString *error )
{
  const QList<QVariantMap> wells = realWellsForSiting();
  if ( wells.isEmpty() )
  {
    if ( error )
      *error = tr( "没有可定位的实井——评估需要实井基线" );
    return QVariantMap();
  }
  const std::vector<paleo::wellsiting::Polygon> domain = resolveDomain( params, wells );
  const paleo::wellsiting::SitingOptions options = kernelOptions( params );
  const std::vector<Point2> wellPts = wellPoints( wells );

  // 层位井控密度：tops 有分层的层才算被该井控制（无 facade/无 tops → 空）。
  std::vector<paleo::wellsiting::LayerWells> layers;
  if ( d->projectData )
  {
    QHash<QString, std::vector<Point2>> byHorizon;
    for ( const QVariantMap &w : wells )
    {
      const QVector<WellTop> tops =
          d->projectData->topsFor( w.value( QStringLiteral( "id" ) ).toString() );
      QSet<QString> seen;
      for ( const WellTop &top : tops )
      {
        if ( top.horizon.isEmpty() || seen.contains( top.horizon ) )
          continue;
        seen.insert( top.horizon );
        byHorizon[top.horizon].push_back(
            Point2{ w.value( QStringLiteral( "x" ) ).toDouble(),
                    w.value( QStringLiteral( "y" ) ).toDouble() } );
      }
    }
    for ( auto it = byHorizon.constBegin(); it != byHorizon.constEnd(); ++it )
    {
      paleo::wellsiting::LayerWells layer;
      layer.horizon = it.key().toStdString();
      layer.points = it.value();
      layers.push_back( std::move( layer ) );
    }
  }

  const paleo::wellsiting::ScenarioMetrics before =
      paleo::wellsiting::scenarioMetrics( domain, wellPts, {}, layers, options );

  // planned 井按 id 解析为候选点；不存在的 id 如实记录（不虚造点位）。
  std::vector<Point2> candidates;
  QStringList resolved, missing;
  const QList<QVariantMap> planned = plannedWells();
  for ( const QString &id : plannedIds )
  {
    const auto it = std::find_if(
        planned.begin(), planned.end(),
        [&id]( const QVariantMap &w ) {
          return w.value( QStringLiteral( "id" ) ).toString() == id;
        } );
    if ( it == planned.end() )
    {
      missing.append( id );
      continue;
    }
    resolved.append( id );
    candidates.push_back( Point2{ it->value( QStringLiteral( "x" ) ).toDouble(),
                                  it->value( QStringLiteral( "y" ) ).toDouble() } );
  }

  const paleo::wellsiting::ScenarioMetrics after =
      paleo::wellsiting::scenarioMetrics( domain, wellPts, candidates, layers, options );

  QVariantMap evaluation;
  evaluation.insert( QStringLiteral( "before" ), metricsToMap( before ) );
  evaluation.insert( QStringLiteral( "after" ), metricsToMap( after ) );
  evaluation.insert( QStringLiteral( "well_count" ), wells.size() );
  evaluation.insert( QStringLiteral( "candidate_count" ), int( candidates.size() ) );
  if ( !missing.isEmpty() )
    evaluation.insert( QStringLiteral( "missing_planned_ids" ), missing );

  // 逐井贡献分（基线场上单候选边际，栅格口径）。
  const paleo::wellsiting::SitingField baseField =
      paleo::wellsiting::sampleField( domain, wellPts, options );
  QVariantMap contributions;
  for ( std::size_t i = 0; i < candidates.size(); ++i )
  {
    const paleo::wellsiting::CandidateContribution contribution =
        paleo::wellsiting::contributionOf( baseField, candidates[i], options );
    QVariantMap c;
    c.insert( QStringLiteral( "hole_area_m2" ), contribution.holeAreaReduction );
    c.insert( QStringLiteral( "mean_distance_m" ),
              std::isfinite( contribution.meanDistanceReduction )
                  ? QVariant( contribution.meanDistanceReduction )
                  : QVariant() );
    contributions.insert( resolved.at( int( i ) ), c );
  }
  evaluation.insert( QStringLiteral( "contributions" ), contributions );
  evaluation.insert( QStringLiteral( "params" ), params.toMap() );

  m_lastEvaluation = evaluation;
  emit evaluationDone( evaluation );
  return evaluation;
}

bool WellSitingWorkflow::saveScenario( const QString &name, const QStringList &plannedIds,
                                       const WellSitingParams &params, QString *error )
{
  if ( !d->sitingStore )
  {
    if ( error )
      *error = tr( "方案存储未绑定（工程未打开？）" );
    return false;
  }
  ensureScenariosLoaded();

  const QVariantMap evaluation = evaluateScenario( params, plannedIds, error );
  if ( evaluation.isEmpty() )
    return false;

  int maxSeq = 0;
  for ( const paleo::siting::SitingScenario &existing : d->scenarios.scenarios )
  {
    const int seq = existing.id.startsWith( QLatin1String( "scenario-" ) )
                        ? existing.id.mid( QStringLiteral( "scenario-" ).size() ).toInt()
                        : 0;
    maxSeq = std::max( maxSeq, seq );
  }
  paleo::siting::SitingScenario scenario;
  scenario.id = QStringLiteral( "scenario-%1" ).arg( maxSeq + 1 );
  scenario.name =
      name.trimmed().isEmpty() ? tr( "方案 %1" ).arg( maxSeq + 1 ) : name.trimmed();
  scenario.plannedWellIds = plannedIds;
  scenario.params = params.toMap();
  scenario.metricsBefore = evaluation.value( QStringLiteral( "before" ) ).toMap();
  scenario.metricsAfter = evaluation.value( QStringLiteral( "after" ) ).toMap();
  scenario.contributions = evaluation.value( QStringLiteral( "contributions" ) ).toMap();
  scenario.updatedMs = QDateTime::currentMSecsSinceEpoch();

  d->scenarios.upsert( scenario );
  if ( !saveScenarioSet( error ) )
    return false;
  emit scenariosChanged();
  return true;
}

QList<QVariantMap> WellSitingWorkflow::scenarios() const
{
  ensureScenariosLoaded();
  QList<QVariantMap> out;
  for ( const paleo::siting::SitingScenario &scenario : d->scenarios.scenarios )
  {
    QVariantMap m;
    m.insert( QStringLiteral( "id" ), scenario.id );
    m.insert( QStringLiteral( "name" ), scenario.name );
    m.insert( QStringLiteral( "well_count" ), scenario.plannedWellIds.size() );
    m.insert( QStringLiteral( "updated_ms" ), scenario.updatedMs );
    m.insert( QStringLiteral( "planned_well_ids" ), scenario.plannedWellIds );
    m.insert( QStringLiteral( "metrics_before" ), scenario.metricsBefore );
    m.insert( QStringLiteral( "metrics_after" ), scenario.metricsAfter );
    m.insert( QStringLiteral( "contributions" ), scenario.contributions );
    m.insert( QStringLiteral( "params" ), scenario.params );
    out.append( m );
  }
  return out;
}

bool WellSitingWorkflow::deleteScenario( const QString &scenarioId, QString *error )
{
  ensureScenariosLoaded();
  if ( !d->scenarios.remove( scenarioId ) )
  {
    if ( error )
      *error = tr( "方案 %1 不存在" ).arg( scenarioId );
    return false;
  }
  if ( !saveScenarioSet( error ) )
    return false;
  emit scenariosChanged();
  return true;
}

// ---------------------------------------------------------------------------
// 导出
// ---------------------------------------------------------------------------

bool WellSitingWorkflow::exportScenarioCsv( const QString &scenarioId, const QString &path,
                                            QString *error )
{
  ensureScenariosLoaded();
  const int at = d->scenarios.indexOf( scenarioId );
  if ( at < 0 )
  {
    if ( error )
      *error = tr( "方案 %1 不存在" ).arg( scenarioId );
    return false;
  }
  const paleo::siting::SitingScenario &scenario = d->scenarios.scenarios.at( at );
  if ( !d->catalog )
  {
    if ( error )
      *error = tr( "catalog 未绑定——无法解析计划井坐标" );
    return false;
  }

  QSaveFile file( path );
  file.setDirectWriteFallback( false );
  if ( !file.open( QIODevice::WriteOnly | QIODevice::Truncate | QIODevice::Text ) )
  {
    if ( error )
      *error = file.errorString();
    return false;
  }
  QTextStream stream( &file );
  stream.setEncoding( QStringConverter::Utf8 );
  stream << QStringLiteral(
      "scenario_name,well_id,well_name,x_m,y_m,contribution_hole_area_m2,contribution_mean_distance_m\n" );
  for ( const QString &wellId : scenario.plannedWellIds )
  {
    const CatalogEntity entity = d->catalog->entityById( wellId );
    if ( entity.id.isEmpty() || entity.entityType != QLatin1String( "planned" ) )
      continue; // 引用悬空如实跳过
    if ( d->scenarios.isRetired( wellId ) )
      continue; // 已弃用（删除/移动换新 id）——不带陈旧坐标出表
    const QVariantMap contribution = scenario.contributions.value( wellId ).toMap();
    stream << QStringLiteral( "%1,%2,%3,%4,%5,%6,%7\n" )
                  .arg( csvField( scenario.name ), csvField( wellId ),
                        csvField( d->scenarios.displayName( wellId, entity.name ) ),
                        coordsText( entity.surfaceX ), coordsText( entity.surfaceY ),
                        QString::number(
                            contribution.value( QStringLiteral( "hole_area_m2" ) ).toDouble(), 'f', 1 ),
                        QString::number(
                            contribution.value( QStringLiteral( "mean_distance_m" ) ).toDouble(), 'f', 1 ) );
  }
  if ( !file.commit() )
  {
    if ( error )
      *error = file.errorString();
    return false;
  }
  return true;
}

bool WellSitingWorkflow::exportComparisonChart( const QString &path, QString *error )
{
  ensureScenariosLoaded();
  if ( d->scenarios.scenarios.isEmpty() )
  {
    if ( error )
      *error = tr( "没有已保存的方案——先保存至少一个方案再导出对比图" );
    return false;
  }

  // 图表是导出文档产物，非 UI 部件：色值与 DESIGN.md token 同源（蓝=
  // 主数据、中性灰=基线），走 QPainter 直绘。
  const QColor kBar( 0x1B, 0x73, 0xD0 );
  const QColor kBaseBar( 0x5D, 0x6E, 0x80 );
  const QColor kText( 0x24, 0x30, 0x3E ); // DESIGN.md colors.text
  const QColor kMuted( 0x5D, 0x6E, 0x80 );

  struct Row
  {
    QString label;
    double holeArea = 0;     // km²
    double coverage = 0;     // 0..1
    double meanDistance = 0; // m
    bool base = false;
  };
  QList<Row> rows;
  {
    const paleo::siting::SitingScenario &first = d->scenarios.scenarios.first();
    Row base;
    base.label = tr( "基线（现状）" );
    base.holeArea =
        first.metricsBefore.value( QStringLiteral( "hole_area_total" ) ).toDouble() / 1e6;
    base.coverage = first.metricsBefore.value( QStringLiteral( "coverage_ratio" ) ).toDouble();
    base.meanDistance =
        first.metricsBefore.value( QStringLiteral( "domain_mean_distance" ) ).toDouble();
    base.base = true;
    rows.append( base );
  }
  double maxArea = rows.first().holeArea;
  for ( const paleo::siting::SitingScenario &scenario : d->scenarios.scenarios )
  {
    Row row;
    row.label = scenario.name;
    row.holeArea = scenario.metricsAfter.value( QStringLiteral( "hole_area_total" ) ).toDouble() / 1e6;
    row.coverage = scenario.metricsAfter.value( QStringLiteral( "coverage_ratio" ) ).toDouble();
    row.meanDistance =
        scenario.metricsAfter.value( QStringLiteral( "domain_mean_distance" ) ).toDouble();
    rows.append( row );
    maxArea = std::max( maxArea, row.holeArea );
  }
  if ( maxArea <= 0 )
    maxArea = 1;

  const int rowH = 64;
  const int left = 190;
  const int width = 920;
  const int height = 96 + rows.size() * rowH + 64;
  QImage image( width, height, QImage::Format_ARGB32_Premultiplied );
  image.fill( Qt::white );
  QPainter painter( &image );
  painter.setRenderHint( QPainter::Antialiasing );

  QFont titleFont = painter.font();
  titleFont.setPointSize( 15 );
  painter.setFont( titleFont );
  painter.setPen( kText );
  painter.drawText( QRect( 24, 16, width - 48, 32 ), Qt::AlignLeft | Qt::AlignVCenter,
                    tr( "井网覆盖方案对比" ) );
  QFont bodyFont = painter.font();
  bodyFont.setPointSize( 9 );
  painter.setFont( bodyFont );

  const int barX = left;
  const int barW = width - left - 260;
  int y = 80;
  for ( const Row &row : rows )
  {
    painter.setPen( kText );
    painter.drawText( QRect( 24, y, left - 32, 22 ), Qt::AlignRight | Qt::AlignVCenter,
                      row.label );
    const QRect bar( barX, y, barW, 22 );
    painter.setPen( QPen( kMuted, 1 ) );
    painter.setBrush( Qt::NoBrush );
    painter.drawRect( bar );
    const int fill = int( barW * row.holeArea / maxArea );
    if ( fill > 0 )
    {
      painter.setPen( Qt::NoPen );
      painter.setBrush( row.base ? kBaseBar : kBar );
      painter.drawRect( barX, y, fill, 22 );
    }
    painter.setPen( kMuted );
    painter.drawText( QRect( barX + barW + 12, y, 250, 22 ), Qt::AlignLeft | Qt::AlignVCenter,
                      tr( "空洞 %1 km² · 覆盖率 %2% · 均距 %3 m" )
                          .arg( row.holeArea, 0, 'f', 2 )
                          .arg( row.coverage * 100.0, 0, 'f', 1 )
                          .arg( row.meanDistance, 0, 'f', 0 ) );
    y += rowH;
  }

  // 口径注记（诚实面）：随首方案基线指标落图。
  painter.setPen( kMuted );
  const QString note =
      d->scenarios.scenarios.first().metricsBefore.value( QStringLiteral( "note" ) ).toString();
  const QString disclosure =
      note.isEmpty()
          ? tr( "空洞面积/覆盖率为采样格口径（欧氏距离近似）；评估只衡量几何"
                "覆盖，不代表地质最优。" )
          : note;
  painter.drawText( QRect( 24, height - 56, width - 48, 44 ),
                    Qt::AlignLeft | Qt::AlignTop | Qt::TextWordWrap,
                    disclosure +
                        tr( " 各方案柱在其保存时参数（井控半径/格宽/域）下计算，"
                            "柱间对比仅供参考。" ) );
  painter.end();

  if ( !image.save( path, "PNG" ) )
  {
    if ( error )
      *error = tr( "对比图写入失败：%1" ).arg( path );
    return false;
  }
  return true;
}
