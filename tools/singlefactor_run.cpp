// tools/singlefactor_run.cpp — ad-hoc runner for the Enping-style single-factor
// data package. Reuses the production pipeline verbatim:
//   readConstraintImportFeatures (方向线/打断线 → ConstraintStore 契约)
//   → paleo:paleo_structural_idw Processing 算法（上游 structural_idw 移植）
//   → FactorContourService::generateStructuralContours（上游 field_contours 移植）
// 旁路对照通道：
//   buildPartition（interpretation：打断线自由端延界 + 洪泛分区）
//   → regionalContours（上游 regional_contours 移植：分区独立井控 IDW + masked
//     marching squares），另写 MS 等值线 GPKG 与对比图件。
// 输出：GeoTIFF 等值面 + .qc.json/.structural.json 侧卡 + GPKG 等值线×2 + PNG 图件×2。
//
// Usage: singlefactor_run <data_dir> <out_dir>
//   data_dir needs: well.shp 成图范围.shp 打断线.shp 方向线_已配准.shp
#include <QDir>
#include <QFile>
#include <QFileInfo>
#include <QFont>
#include <QJsonArray>
#include <QJsonDocument>
#include <QJsonObject>
#include <QPainter>
#include <QVariantMap>

#include <cstdio>
#include <cmath>
#include <memory>
#include <vector>

#include <gdal.h>
#include <ogr_api.h>
#include <ogr_srs_api.h>

#include <qgsapplication.h>
#include <qgscolorrampshader.h>
#include <qgscoordinatereferencesystem.h>
#include <qgsfeature.h>
#include <qgsfield.h>
#include <qgsfillsymbol.h>
#include <qgsfillsymbollayer.h>
#include <qgsgeometry.h>
#include <qgslinesymbol.h>
#include <qgslinesymbollayer.h>
#include <qgsmaprendererparalleljob.h>
#include <qgsmapsettings.h>
#include <qgsmarkersymbol.h>
#include <qgspallabeling.h>
#include <qgsprocessingalgorithm.h>
#include <qgsprocessingcontext.h>
#include <qgsprocessingfeedback.h>
#include <qgsprocessingregistry.h>
#include <qgsrasterlayer.h>
#include <qgsrastershader.h>
#include <qgssinglesymbolrenderer.h>
#include <qgssinglebandpseudocolorrenderer.h>
#include <qgstextformat.h>
#include <qgsvectorlayer.h>
#include <qgsvectorlayerlabeling.h>

#include "algorithms/paleoalgorithms.h"
#include "algorithms/singlefactor/partition.h"
#include "algorithms/singlefactor/regionalcontours.h"
#include "algorithms/singlefactor/support.h"
#include "algorithms/singlefactor/types.h"
#include "qgis/factorcontour.h"
#include "workflow/constraintimport.h"

namespace sf = paleo::singlefactor;

namespace
{

int fail( const QString &message )
{
  std::fprintf( stderr, "ERROR: %s\n", qPrintable( message ) );
  return 1;
}

QJsonObject readJson( const QString &path )
{
  QFile file( path );
  if ( !file.open( QIODevice::ReadOnly ) )
    return {};
  return QJsonDocument::fromJson( file.readAll() ).object();
}

// tests/tst_singlefactor_fieldcontours.cpp 同款：真实导入路径 → 内存约束图层
// （id/type/params_json/schema_version 列，与 ConstraintStore 快照同构）。
std::unique_ptr<QgsVectorLayer> constraintLayer( const QString &directionsPath,
                                               const QString &barriersPath,
                                               QString *error )
{
  auto layer = std::make_unique<QgsVectorLayer>( QStringLiteral( "LineString" ),
                                                 QStringLiteral( "constraints" ),
                                                 QStringLiteral( "memory" ) );
  if ( !layer->isValid() )
  {
    if ( error )
      *error = QStringLiteral( "memory constraint layer invalid" );
    return nullptr;
  }
  layer->startEditing();
  layer->addAttribute( QgsField( QStringLiteral( "id" ), QMetaType::Type::QString ) );
  layer->addAttribute( QgsField( QStringLiteral( "type" ), QMetaType::Type::QString ) );
  layer->addAttribute( QgsField( QStringLiteral( "params_json" ), QMetaType::Type::QString ) );
  layer->addAttribute( QgsField( QStringLiteral( "schema_version" ), QMetaType::Type::Int ) );
  layer->updateFields();

  const auto append = [&layer, error]( const QString &path, const QString &role ) {
    QString resolved;
    const QVector<paleo::ImportedConstraintRecord> records =
        paleo::readConstraintImportFeatures( path, role, &resolved, error );
    if ( records.isEmpty() )
      return false;
    for ( const paleo::ImportedConstraintRecord &record : records )
    {
      QgsFeature feature( layer->fields() );
      feature.setGeometry( QgsGeometry::fromWkt( record.wkt ) );
      feature.setAttribute( QStringLiteral( "id" ),
                            record.params.value( QStringLiteral( "sourceId" ) ).toString() );
      feature.setAttribute( QStringLiteral( "type" ), record.type );
      QVariantMap params = record.params;
      params.insert( QStringLiteral( "schemaVersion" ), 1 );
      feature.setAttribute( QStringLiteral( "params_json" ),
                            QString::fromUtf8( QJsonDocument( QJsonObject::fromVariantMap( params ) )
                                                   .toJson( QJsonDocument::Compact ) ) );
      feature.setAttribute( QStringLiteral( "schema_version" ), 1 );
      if ( !layer->addFeature( feature ) )
      {
        if ( error )
          *error = QStringLiteral( "memory layer addFeature failed" );
        return false;
      }
    }
    return true;
  };
  bool ok = true;
  if ( !directionsPath.isEmpty() )
    ok = append( directionsPath, QStringLiteral( "direction" ) ) && ok;
  if ( ok && !barriersPath.isEmpty() )
    ok = append( barriersPath, QStringLiteral( "barrier" ) ) && ok;
  if ( !ok )
    return nullptr;
  layer->commitChanges();
  return layer;
}

// structuralalgorithm.cpp::appendGeometryBoundary 同款：面要素外环+孔转
// sf::Polygon，闭合线要素按外环兜底。
std::vector<sf::Polygon> layerBoundaryPolygons( QgsVectorLayer *layer )
{
  std::vector<sf::Polygon> out;
  QgsFeature feature;
  QgsFeatureIterator it = layer->getFeatures();
  while ( it.nextFeature( feature ) )
  {
    const QgsGeometry geometry = feature.geometry();
    const Qgis::GeometryType type = QgsWkbTypes::geometryType( geometry.wkbType() );
    if ( type == Qgis::GeometryType::Polygon )
    {
      const QList<QgsPolygonXY> parts =
          geometry.isMultipart() ? geometry.asMultiPolygon()
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
        out.push_back( std::move( boundary ) );
      }
    }
    else if ( type == Qgis::GeometryType::Line )
    {
      const QgsPolylineXY line = geometry.asPolyline();
      if ( line.size() < 3 )
        continue;
      sf::Polygon boundary;
      for ( const QgsPointXY &p : line )
        boundary.exterior.points.push_back( sf::Point2{ p.x(), p.y() } );
      const sf::Point2 first = boundary.exterior.points.front();
      const sf::Point2 last = boundary.exterior.points.back();
      if ( first.x != last.x || first.y != last.y )
        boundary.exterior.points.push_back( first );
      out.push_back( std::move( boundary ) );
    }
  }
  return out;
}

// 每条折线部件 → 一个 BarrierSpec（解释分区把它们当隔断墙，自由端由
// buildPartition 沿切向延到域边界）。
std::vector<sf::BarrierSpec> layerBarrierSpecs( QgsVectorLayer *layer )
{
  std::vector<sf::BarrierSpec> out;
  QgsFeature feature;
  QgsFeatureIterator it = layer->getFeatures();
  while ( it.nextFeature( feature ) )
  {
    const QgsGeometry geometry = feature.geometry();
    const QgsMultiPolylineXY parts =
        geometry.isMultipart() ? geometry.asMultiPolyline()
                               : QgsMultiPolylineXY{ geometry.asPolyline() };
    int part = 0;
    for ( const QgsPolylineXY &line : parts )
    {
      if ( line.size() < 2 )
        continue;
      sf::BarrierSpec spec;
      spec.lineId = QStringLiteral( "barrier_%1_%2" )
                        .arg( feature.id() )
                        .arg( part++ )
                        .toStdString();
      for ( const QgsPointXY &p : line )
        spec.points.push_back( sf::Point2{ p.x(), p.y() } );
      out.push_back( std::move( spec ) );
    }
  }
  return out;
}

// 复用已写出的 GeoTIFF 像元网格作 MS 采样网格（与等值面逐格对齐）。
sf::GridSpec rasterGridSpec( const QString &rasterPath, bool *ok )
{
  sf::GridSpec grid;
  *ok = false;
  GDALDatasetH ds = GDALOpen( rasterPath.toUtf8().constData(), GA_ReadOnly );
  if ( !ds )
    return grid;
  double gt[6] = { 0 };
  const bool hasGt = GDALGetGeoTransform( ds, gt ) == CE_None;
  grid.cols = GDALGetRasterXSize( ds );
  grid.rows = GDALGetRasterYSize( ds );
  GDALClose( ds );
  if ( !hasGt || grid.cols <= 0 || grid.rows <= 0 || !( gt[1] > 0 ) || !( gt[5] < 0 ) )
    return grid;
  grid.originX = gt[0];
  grid.originY = gt[3];
  grid.pixelWidth = gt[1];
  grid.pixelHeight = gt[5];
  *ok = true;
  return grid;
}

// MS 等值线落盘：与 generateStructuralContours 同一 GPKG 写法（GDAL C API）。
bool writeMsContoursGpkg( const QString &path, OGRSpatialReferenceH srs,
                          const sf::RegionalContourResult &result, QString *error )
{
  QFile::remove( path );
  GDALDriverH drv = GDALGetDriverByName( "GPKG" );
  if ( !drv )
    return !( error && ( *error = QStringLiteral( "GDAL GPKG driver unavailable" ),
                          true ) );
  GDALDatasetH ds =
      GDALCreate( drv, path.toUtf8().constData(), 0, 0, 0, GDT_Unknown, nullptr );
  if ( !ds )
    return !( error && ( *error = QStringLiteral( "cannot create '%1'" ).arg( path ),
                          true ) );
  OGRLayerH layer = GDALDatasetCreateLayer( ds, "contours", srs, wkbLineString, nullptr );
  if ( !layer )
  {
    if ( error )
      *error = QStringLiteral( "cannot create layer 'contours' in '%1'" ).arg( path );
    GDALClose( ds );
    return false;
  }
  const auto addField = [layer]( const char *name, OGRFieldType type ) {
    OGRFieldDefnH field = OGR_Fld_Create( name, type );
    OGR_L_CreateField( layer, field, true );
    OGR_Fld_Destroy( field );
  };
  addField( "level", OFTReal );
  addField( "region", OFTInteger );
  addField( "closed", OFTInteger );
  addField( "engine", OFTString );

  bool wrote = true;
  for ( std::size_t i = 0; i < result.contours.size(); ++i )
  {
    const sf::ContourPolyline &line = result.contours[i];
    if ( line.points.size() < 2 )
      continue;
    OGRGeometryH geometry = OGR_G_CreateGeometry( wkbLineString );
    for ( const sf::Point2 &pt : line.points )
      OGR_G_SetPoint_2D( geometry, OGR_G_GetPointCount( geometry ), pt.x, pt.y );
    const bool closed =
        line.points.size() > 2 && line.points.front().x == line.points.back().x &&
        line.points.front().y == line.points.back().y;
    OGRFeatureH feature = OGR_F_Create( OGR_L_GetLayerDefn( layer ) );
    OGR_F_SetGeometryDirectly( feature, geometry );
    OGR_F_SetFieldDouble( feature, OGR_F_GetFieldIndex( feature, "level" ), line.level );
    OGR_F_SetFieldInteger( feature, OGR_F_GetFieldIndex( feature, "region" ),
                           result.regionOfContour[i] );
    OGR_F_SetFieldInteger( feature, OGR_F_GetFieldIndex( feature, "closed" ),
                           closed ? 1 : 0 );
    OGR_F_SetFieldString( feature, OGR_F_GetFieldIndex( feature, "engine" ),
                          "regional_ms" );
    if ( OGR_L_CreateFeature( layer, feature ) != OGRERR_NONE )
      wrote = false;
    OGR_F_Destroy( feature );
    if ( !wrote )
      break;
  }
  GDALClose( ds );
  if ( !wrote && error )
    *error = QStringLiteral( "等值线要素写入失败" );
  return wrote;
}

struct RegionalMsOutcome
{
  bool ok = false;
  QString message;
  int regions = 0;
  int featureCount = 0;
  int extensions = 0;
  QStringList conflicts;
  QStringList issues;
  QString strictStatus;               // 上游严格口径（空区即整体拒算）的结果
  std::vector<int> droppedRegions;    // 空井区：出图时降级为域外（harness 宽松档）
  std::vector<sf::BarrierSpec> walls; // 延界后的分区墙（画对比图用）
  std::vector<int> wellsPerRegion;    // 井→区归属直方图（诊断空区拒算）
  std::vector<int> cellsPerRegion;
};

// 分区 MS 旁路：打断线当硬隔断 → interpretation 洪泛分区 → 各区独立井控 IDW →
// masked marching squares。参数与 structural 通道同口径（power=2、extend 到边界）。
RegionalMsOutcome runRegionalMs( QgsVectorLayer *wells, QgsVectorLayer *boundary,
                                 QgsVectorLayer *barriers, const QString &rasterPath,
                                 const std::vector<double> &levels,
                                 const QString &gpkgPath,
                                 const QgsCoordinateReferenceSystem &crs )
{
  RegionalMsOutcome out;
  const int idIdx = wells->fields().indexOf( QStringLiteral( "well_id" ) );
  const int valIdx = wells->fields().indexOf( QStringLiteral( "sand_thk" ) );
  if ( idIdx < 0 || valIdx < 0 )
  {
    out.message = QStringLiteral( "wells layer missing well_id/sand_thk" );
    return out;
  }
  sf::PreparedInput input;
  QgsFeature feature;
  QgsFeatureIterator it = wells->getFeatures();
  while ( it.nextFeature( feature ) )
  {
    const QgsPointXY pt = feature.geometry().asPoint();
    sf::Sample sample;
    sample.stableRowId = std::to_string( feature.id() );
    sample.wellId = feature.attribute( idIdx ).toString().toStdString();
    sample.x = pt.x();
    sample.y = pt.y();
    sample.value = feature.attribute( valIdx ).toDouble();
    input.samples.push_back( std::move( sample ) );
  }
  input.validCount = static_cast<int>( input.samples.size() );
  input.originalCount = input.validCount;
  input.domain = layerBoundaryPolygons( boundary );
  if ( input.domain.empty() )
  {
    out.message = QStringLiteral( "boundary layer produced no polygons" );
    return out;
  }

  bool gridOk = false;
  const sf::GridSpec grid = rasterGridSpec( rasterPath, &gridOk );
  if ( !gridOk )
  {
    out.message = QStringLiteral( "cannot derive grid from %1" ).arg( rasterPath );
    return out;
  }
  const std::size_t cells =
      static_cast<std::size_t>( grid.cols ) * static_cast<std::size_t>( grid.rows );
  const double tol =
      std::min( std::abs( grid.pixelWidth ), std::abs( grid.pixelHeight ) ) * 1e-6;
  std::vector<std::uint8_t> domainMask( cells, std::uint8_t{ 0 } );
  for ( int row = 0; row < grid.rows; ++row )
    for ( int col = 0; col < grid.cols; ++col )
      if ( sf::pointInDomain( input.domain, sf::cellCenter( grid, col, row ), tol ) )
        domainMask[static_cast<std::size_t>( row ) * grid.cols + col] = 1;

  sf::ResolvedParameters params;
  params.power = 2.0;
  params.coverage = sf::CoverageMode::DomainExtrapolation;
  if ( const std::string err = sf::resolveParameters( input, grid, &params );
       !err.empty() )
  {
    out.message = QString::fromStdString( err );
    return out;
  }

  const std::vector<sf::BarrierSpec> barrierSpecs = layerBarrierSpecs( barriers );
  std::vector<sf::Point2> wellXy;
  wellXy.reserve( input.samples.size() );
  for ( const sf::Sample &sample : input.samples )
    wellXy.push_back( sf::Point2{ sample.x, sample.y } );
  const sf::PartitionResult partition =
      sf::buildPartition( grid, domainMask, barrierSpecs, wellXy, input.domain,
                          /*interpretation=*/true );
  out.regions = partition.regionCount;
  out.extensions = static_cast<int>( partition.extensions.size() );
  for ( const std::string &conflict : partition.conflicts )
    out.conflicts << QString::fromStdString( conflict );
  out.walls = partition.partitionBarriers;
  out.wellsPerRegion.assign( static_cast<std::size_t>( std::max( partition.regionCount, 0 ) ), 0 );
  for ( const int wellRegion : partition.wellRegionIds )
    if ( wellRegion >= 0 &&
         wellRegion < static_cast<int>( out.wellsPerRegion.size() ) )
      ++out.wellsPerRegion[static_cast<std::size_t>( wellRegion )];
  out.cellsPerRegion.assign( out.wellsPerRegion.size(), 0 );
  for ( const int region : partition.regionIds )
    if ( region >= 0 && region < static_cast<int>( out.cellsPerRegion.size() ) )
      ++out.cellsPerRegion[static_cast<std::size_t>( region )];

  // 上游严格口径：任一提线分区无井即整体拒算（不跨隔断借值）。先如实跑一遍
  // 拿拒算回执，再把空井区降为域外重编号跑宽松档——库语义不动，宽松只在本
  // harness 的对比通道里生效。
  const sf::RegionalContourResult strict =
      sf::regionalContours( input, grid, params, levels, partition );
  out.strictStatus = QString::fromStdString(
      strict.status == sf::Status::Ok ? "ok" : strict.message );

  sf::PartitionResult runPartition = partition;
  if ( partition.regionCount > 0 )
  {
    std::vector<int> remap( static_cast<std::size_t>( partition.regionCount ), -1 );
    int next = 0;
    for ( int rid = 0; rid < partition.regionCount; ++rid )
    {
      if ( out.wellsPerRegion[static_cast<std::size_t>( rid )] > 0 )
        remap[static_cast<std::size_t>( rid )] = next++;
      else
        out.droppedRegions.push_back( rid );
    }
    if ( next < partition.regionCount )
    {
      for ( int &region : runPartition.regionIds )
        region = region >= 0 ? remap[static_cast<std::size_t>( region )]
                             : sf::kRegionOutside;
      for ( int &wellRegion : runPartition.wellRegionIds )
        if ( wellRegion >= 0 )
          wellRegion = remap[static_cast<std::size_t>( wellRegion )];
      runPartition.regionCount = next;
    }
  }

  const sf::RegionalContourResult result =
      sf::regionalContours( input, grid, params, levels, runPartition );
  for ( const std::string &issue : result.issues )
    out.issues << QString::fromStdString( issue );
  if ( result.status != sf::Status::Ok )
  {
    out.message = QString::fromStdString( result.message );
    return out;
  }

  const QByteArray wkt =
      crs.toWkt( Qgis::CrsWktVariant::Wkt2_2019 ).toUtf8();
  OGRSpatialReferenceH srs = OSRNewSpatialReference( wkt.constData() );
  QString writeErr;
  const bool wrote = writeMsContoursGpkg( gpkgPath, srs, result, &writeErr );
  if ( srs )
    OSRDestroySpatialReference( srs );
  if ( !wrote )
  {
    out.message = writeErr;
    return out;
  }
  out.featureCount = static_cast<int>( result.contours.size() );
  out.ok = true;
  return out;
}

// 分区墙（延界后）画成内存线图层，供对比图定位分区边界。
std::unique_ptr<QgsVectorLayer> wallLayer( const std::vector<sf::BarrierSpec> &walls,
                                           const QgsCoordinateReferenceSystem &crs )
{
  auto layer = std::make_unique<QgsVectorLayer>( QStringLiteral( "LineString" ),
                                                 QStringLiteral( "partition_walls" ),
                                                 QStringLiteral( "memory" ) );
  if ( !layer->isValid() )
    return nullptr;
  layer->setCrs( crs );
  layer->startEditing();
  for ( const sf::BarrierSpec &wall : walls )
  {
    QgsPolylineXY line;
    for ( const sf::Point2 &p : wall.points )
      line << QgsPointXY( p.x, p.y );
    if ( line.size() < 2 )
      continue;
    QgsFeature feature( layer->fields() );
    feature.setGeometry( QgsGeometry::fromPolylineXY( line ) );
    layer->addFeature( feature );
  }
  layer->commitChanges();
  return layer;
}

QgsRectangle fitAspect( QgsRectangle rect, double aspect )
{
  const double w = rect.width();
  const double h = rect.height();
  if ( w <= 0 || h <= 0 )
    return rect;
  const double current = w / h;
  if ( current < aspect )
    rect.setXMinimum( rect.center().x() - h * aspect * 0.5 ),
        rect.setXMaximum( rect.center().x() + h * aspect * 0.5 );
  else
    rect.setYMinimum( rect.center().y() - w / aspect * 0.5 ),
        rect.setYMaximum( rect.center().y() + w / aspect * 0.5 );
  return rect;
}

struct RampStop
{
  double value;
  QColor color;
};

const RampStop kThicknessRamp[] = {
    { 0.0, QColor( "#3288bd" ) },   { 60.0, QColor( "#99d594" ) },
    { 120.0, QColor( "#e6f598" ) }, { 180.0, QColor( "#fee08b" ) },
    { 240.0, QColor( "#fc8d59" ) }, { 300.0, QColor( "#d53e4f" ) },
};

QColor rampColor( double value )
{
  const int count = int( sizeof( kThicknessRamp ) / sizeof( kThicknessRamp[0] ) );
  if ( value <= kThicknessRamp[0].value )
    return kThicknessRamp[0].color;
  for ( int i = 1; i < count; ++i )
  {
    if ( value <= kThicknessRamp[i].value )
    {
      const double t = ( value - kThicknessRamp[i - 1].value ) /
                       ( kThicknessRamp[i].value - kThicknessRamp[i - 1].value );
      const QColor a = kThicknessRamp[i - 1].color;
      const QColor b = kThicknessRamp[i].color;
      return QColor( a.red() + int( t * ( b.red() - a.red() ) ),
                     a.green() + int( t * ( b.green() - a.green() ) ),
                     a.blue() + int( t * ( b.blue() - a.blue() ) ) );
    }
  }
  return kThicknessRamp[count - 1].color;
}

void styleRaster( QgsRasterLayer *layer )
{
  QList<QgsColorRampShader::ColorRampItem> items;
  for ( const RampStop &stop : kThicknessRamp )
    items << QgsColorRampShader::ColorRampItem( stop.value, stop.color,
                                              QString::number( stop.value ) );
  auto *colorRamp = new QgsColorRampShader( 0.0, 300.0 );
  colorRamp->setColorRampType( Qgis::ShaderInterpolationMethod::Linear );
  colorRamp->setColorRampItemList( items );
  colorRamp->setClip( true );
  auto *shader = new QgsRasterShader( 0.0, 300.0 );
  shader->setRasterShaderFunction( colorRamp );
  layer->setRenderer(
      new QgsSingleBandPseudoColorRenderer( layer->dataProvider(), 1, shader ) );
}

void styleLineLayer( QgsVectorLayer *layer, const QColor &color, double widthMm,
                     Qt::PenStyle pen = Qt::SolidLine )
{
  auto *symbol = static_cast<QgsLineSymbol *>(
      QgsSymbol::defaultSymbol( Qgis::GeometryType::Line ) );
  symbol->setColor( color );
  symbol->setWidth( widthMm );
  for ( int i = 0; i < symbol->symbolLayerCount(); ++i )
    if ( auto *lineLayer = dynamic_cast<QgsSimpleLineSymbolLayer *>( symbol->symbolLayer( i ) ) )
      lineLayer->setPenStyle( pen );
  layer->setRenderer( new QgsSingleSymbolRenderer( symbol ) );
}

void enableLabels( QgsVectorLayer *layer, const QString &field, double sizePt,
                   const QColor &color, bool curved )
{
  QgsPalLayerSettings settings;
  settings.fieldName = field;
  settings.placement = curved ? Qgis::LabelPlacement::Curved : Qgis::LabelPlacement::AroundPoint;
  settings.dist = 1.2;
  QgsTextFormat format;
  format.setFont( QFont( QStringLiteral( "Noto Sans SC" ) ) );
  format.setSize( sizePt );
  format.setSizeUnit( Qgis::RenderUnit::Points );
  format.setColor( color );
  QgsTextBufferSettings buffer;
  buffer.setEnabled( true );
  buffer.setColor( Qt::white );
  buffer.setSize( 0.8 );
  format.setBuffer( buffer );
  settings.setFormat( format );
  layer->setLabeling( new QgsVectorLayerSimpleLabeling( settings ) );
  layer->setLabelsEnabled( true );
}

// 渲染 4800×3000 图件：等值面 + 等值线（含注记）+ 约束线 + 边界 + 井点/井名，
// QPainter 叠加标题与色条。extraLines 为可选额外线层（如分区墙，虚线）。
bool renderMap( const QString &rasterTif, const QString &contourUri,
                QgsVectorLayer *boundaryLayer, QgsVectorLayer *wellsLayer,
                const QString &directionsPath, const QString &barriersPath,
                QgsVectorLayer *extraLines, const QString &title,
                const QString &mapPath, const QgsCoordinateReferenceSystem &crs,
                QString *error )
{
  auto rasterLayer =
      std::make_unique<QgsRasterLayer>( rasterTif, QStringLiteral( "sand_thk" ) );
  std::unique_ptr<QgsVectorLayer> contourLayer;
  if ( !contourUri.isEmpty() )
    contourLayer = std::make_unique<QgsVectorLayer>(
        contourUri, QStringLiteral( "contours" ), QStringLiteral( "ogr" ) );
  auto directionLayer = std::make_unique<QgsVectorLayer>(
      directionsPath, QStringLiteral( "directions" ), QStringLiteral( "ogr" ) );
  auto barrierLayer = std::make_unique<QgsVectorLayer>(
      barriersPath, QStringLiteral( "barriers" ), QStringLiteral( "ogr" ) );
  if ( !rasterLayer->isValid() ||
       ( contourLayer && !contourLayer->isValid() ) )
  {
    if ( error )
      *error = QStringLiteral( "output layers failed to load for render" );
    return false;
  }
  rasterLayer->setCrs( crs );
  if ( contourLayer )
    contourLayer->setCrs( crs );
  directionLayer->setCrs( crs );
  barrierLayer->setCrs( crs );

  styleRaster( rasterLayer.get() );
  if ( auto *fill = static_cast<QgsFillSymbol *>(
           QgsSymbol::defaultSymbol( Qgis::GeometryType::Polygon ) ) )
  {
    fill->setColor( Qt::transparent );
    for ( int i = 0; i < fill->symbolLayerCount(); ++i )
      if ( auto *fillLayer =
               dynamic_cast<QgsSimpleFillSymbolLayer *>( fill->symbolLayer( i ) ) )
      {
        fillLayer->setBrushStyle( Qt::NoBrush );
        fillLayer->setStrokeColor( QColor( "#24303E" ) );
        fillLayer->setStrokeStyle( Qt::DashLine );
        fillLayer->setStrokeWidth( 0.5 );
      }
    boundaryLayer->setRenderer( new QgsSingleSymbolRenderer( fill ) );
  }
  styleLineLayer( directionLayer.get(), QColor( "#1B73D0" ), 0.4 );
  styleLineLayer( barrierLayer.get(), QColor( "#E53935" ), 0.55 );
  if ( contourLayer )
    styleLineLayer( contourLayer.get(), QColor( "#5D3A1A" ), 0.35 );

  std::unique_ptr<QgsSymbol> wellSymbol( QgsMarkerSymbol::createSimple(
      { { QStringLiteral( "name" ), QStringLiteral( "circle" ) },
        { QStringLiteral( "color" ), QStringLiteral( "#24303E" ) },
        { QStringLiteral( "size" ), QStringLiteral( "2.0" ) },
        { QStringLiteral( "outline_color" ), QStringLiteral( "#FFFFFF" ) },
        { QStringLiteral( "outline_width" ), QStringLiteral( "0.3" ) } } ) );
  wellsLayer->setRenderer( new QgsSingleSymbolRenderer( wellSymbol.release() ) );
  wellsLayer->setCrs( crs );
  enableLabels( wellsLayer, QStringLiteral( "well_id" ), 8.0, QColor( "#24303E" ),
                false );
  if ( contourLayer )
    enableLabels( contourLayer.get(), QStringLiteral( "level" ), 7.5,
                  QColor( "#5D3A1A" ), true );

  QList<QgsMapLayer *> stack{ wellsLayer };
  if ( contourLayer )
    stack << contourLayer.get();
  stack << directionLayer.get() << barrierLayer.get();
  if ( extraLines )
    stack << extraLines;
  stack << boundaryLayer << rasterLayer.get();

  const int mapW = 4800, mapH = 3000;
  QgsRectangle extent = boundaryLayer->extent();
  extent.scale( 1.03 );
  extent = fitAspect( extent, double( mapW ) / mapH );

  QgsMapSettings ms;
  ms.setLayers( stack );
  ms.setExtent( extent );
  ms.setOutputSize( QSize( mapW, mapH ) );
  ms.setOutputDpi( 300 );
  ms.setDestinationCrs( crs );
  ms.setBackgroundColor( QColor( "#EDF1F5" ) );
  std::fprintf( stderr, "rendering map %dx%d...\n", mapW, mapH );
  QgsMapRendererParallelJob job( ms );
  job.start();
  job.waitForFinished();
  QImage image = job.renderedImage();
  if ( image.isNull() )
  {
    if ( error )
      *error = QStringLiteral( "renderer produced no output" );
    return false;
  }

  // 标题 + 图例色条（QPainter 后处理）。
  {
    QPainter painter( &image );
    painter.setRenderHint( QPainter::Antialiasing );
    QFont titleFont( QStringLiteral( "Noto Sans SC" ) );
    titleFont.setPixelSize( 120 );
    titleFont.setWeight( QFont::Medium );
    painter.setFont( titleFont );
    const QRect titleRect = painter.fontMetrics().boundingRect( title );
    QRect box( mapW / 2 - titleRect.width() / 2 - 24, 24, titleRect.width() + 48,
               titleRect.height() + 20 );
    painter.fillRect( box, QColor( 255, 255, 255, 210 ) );
    painter.setPen( QColor( "#24303E" ) );
    painter.drawText( box, Qt::AlignCenter, title );

    // 色条：右侧中部内嵌，0–300 m 厚度。
    const int barH = 720, barW = 44;
    const int barX = mapW - 300, barY = ( mapH - barH ) / 2;
    QFont tickFont( QStringLiteral( "Noto Sans SC" ) );
    tickFont.setPixelSize( 40 );
    QFont unitFont( QStringLiteral( "Noto Sans SC" ) );
    unitFont.setPixelSize( 42 );
    const int boxX = barX - 30, boxW = 250;
    painter.fillRect( QRect( boxX, barY - 90, boxW, barH + 180 ),
                      QColor( 255, 255, 255, 210 ) );
    for ( int y = 0; y < barH; ++y )
    {
      const double v = 300.0 * ( 1.0 - double( y ) / barH );
      painter.setPen( rampColor( v ) );
      painter.drawLine( barX, barY + y, barX + barW, barY + y );
    }
    painter.setPen( QColor( "#24303E" ) );
    painter.drawRect( QRect( barX, barY, barW, barH ) );
    painter.setFont( tickFont );
    for ( int v = 0; v <= 300; v += 50 )
    {
      const int y = barY + int( barH * ( 1.0 - v / 300.0 ) );
      painter.drawLine( barX + barW, y, barX + barW + 10, y );
      painter.drawText( QRect( barX + barW + 16, y - 30, 110, 60 ),
                        Qt::AlignVCenter | Qt::AlignLeft, QString::number( v ) );
    }
    painter.setFont( unitFont );
    painter.drawText( QRect( boxX + 12, barY - 82, boxW - 24, 60 ),
                      Qt::AlignLeft | Qt::AlignVCenter,
                      QStringLiteral( "厚度 (m)" ) );
    painter.end();
  }
  if ( !image.save( mapPath ) )
  {
    if ( error )
      *error = QStringLiteral( "map save failed: %1" ).arg( mapPath );
    return false;
  }
  std::fprintf( stderr, "map written: %s\n", qPrintable( mapPath ) );
  return true;
}

} // namespace

int main( int argc, char *argv[] )
{
  QgsApplication app( argc, argv, false );
  app.setPrefixPath( qEnvironmentVariable( "QGIS_PREFIX_PATH", QStringLiteral( "/usr" ) ), true );
  app.initQgis();
  GDALAllRegister();
  QgsApplication::processingRegistry()->addProvider( new PaleoProvider() );

  if ( argc < 3 )
    return fail( QStringLiteral( "usage: %1 <data_dir> <out_dir>" ).arg( argv[0] ) );
  const QDir dataDir( QString::fromLocal8Bit( argv[1] ) );
  const QDir outDir( QString::fromLocal8Bit( argv[2] ) );
  const QString wellsPath = dataDir.filePath( QStringLiteral( "well.shp" ) );
  const QString boundaryPath = dataDir.filePath( QStringLiteral( "成图范围.shp" ) );
  const QString barriersPath = dataDir.filePath( QStringLiteral( "打断线.shp" ) );
  const QString directionsPath =
      dataDir.filePath( QStringLiteral( "方向线_已配准.shp" ) );
  for ( const QString &path : { wellsPath, boundaryPath, barriersPath, directionsPath } )
    if ( !QFileInfo::exists( path ) )
      return fail( QStringLiteral( "missing input: %1" ).arg( path ) );

  QString rasterRel = QStringLiteral( "raster/sand_thickness_digitized.tif" );
  QString contourRel = QStringLiteral( "contour/sand_thickness_contour_digitized.gpkg" );
  QString mapRel = QStringLiteral( "map/sand_thickness_digitized.png" );
  QString msContourRel =
      QStringLiteral( "contour/sand_thickness_contour_regional_ms.gpkg" );
  QString msMapRel = QStringLiteral( "map/sand_thickness_digitized_regional_ms.png" );
  QString reportRel = QStringLiteral( "logs/data_validation_report.json" );
  const QString rasterPath = outDir.filePath( rasterRel );
  const QString contourPath = outDir.filePath( contourRel );
  const QString mapPath = outDir.filePath( mapRel );
  const QString msContourPath = outDir.filePath( msContourRel );
  const QString msMapPath = outDir.filePath( msMapRel );
  const QString reportPath = outDir.filePath( reportRel );
  for ( const QString &path :
        { rasterPath, contourPath, mapPath, msContourPath, msMapPath, reportPath } )
    QFileInfo( path ).dir().mkpath( QStringLiteral( "." ) );

  // ---- 图层：井点 + 成图边界 ----
  auto wells = std::make_unique<QgsVectorLayer>( wellsPath, QStringLiteral( "wells" ),
                                                 QStringLiteral( "ogr" ) );
  auto boundary = std::make_unique<QgsVectorLayer>( boundaryPath,
                                                  QStringLiteral( "boundary" ),
                                                  QStringLiteral( "ogr" ) );
  if ( !wells->isValid() || !boundary->isValid() )
    return fail( QStringLiteral( "wells/boundary layer failed to load" ) );
  const QgsCoordinateReferenceSystem crs = wells->crs();
  // 成图范围.shp 无 .prj —— 统一到井点 CRS，避免「已知/未知 CRS 混用」拒算。
  boundary->setCrs( crs );
  std::fprintf( stderr, "wells: %lld features, crs valid=%d\n", wells->featureCount(),
                int( crs.isValid() ) );

  // ---- 约束线：方向线 + 打断线（真实导入链）----
  QString importErr;
  std::unique_ptr<QgsVectorLayer> constraints =
      constraintLayer( directionsPath, barriersPath, &importErr );
  if ( !constraints )
    return fail( QStringLiteral( "constraint import failed: %1" ).arg( importErr ) );
  constraints->setCrs( crs );
  std::fprintf( stderr, "constraints: %lld features\n", constraints->featureCount() );

  // ---- 单因素等值面：paleo:paleo_structural_idw ----
  QVariantMap params;
  params.insert( QStringLiteral( "WELLS" ),
                 QVariant::fromValue( static_cast<QgsMapLayer *>( wells.get() ) ) );
  params.insert( QStringLiteral( "WELL_ID_FIELD" ), QStringLiteral( "well_id" ) );
  params.insert( QStringLiteral( "VALUE_FIELD" ), QStringLiteral( "sand_thk" ) );
  params.insert( QStringLiteral( "FACTOR_MODE" ), QStringLiteral( "direct" ) );
  params.insert( QStringLiteral( "VALUE_RANGE" ), QStringLiteral( "data" ) );
  params.insert( QStringLiteral( "BOUNDARY" ),
                 QVariant::fromValue( static_cast<QgsMapLayer *>( boundary.get() ) ) );
  params.insert( QStringLiteral( "CONSTRAINTS" ),
                 QVariant::fromValue( static_cast<QgsMapLayer *>( constraints.get() ) ) );
  params.insert( QStringLiteral( "GRID_RESOLUTION" ), 339 ); // ≈60 m/格（成图长轴 ~20.3 km）
  params.insert( QStringLiteral( "POWER" ), 2.0 );
  params.insert( QStringLiteral( "EXTEND_TO_BOUNDARY" ), true );
  params.insert( QStringLiteral( "ENABLE_BARRIERS" ), true );
  params.insert( QStringLiteral( "ENABLE_DIRECTIONS" ), true );
  params.insert( QStringLiteral( "LOCAL_GRID" ), true ); // 局部工区坐标
  params.insert( QStringLiteral( "OUTPUT" ), rasterPath );

  QgsProcessingContext ctx;
  QgsProcessingFeedback fb;
  const QgsProcessingAlgorithm *alg = QgsApplication::processingRegistry()->algorithmById(
      QStringLiteral( "paleo:paleo_structural_idw" ) );
  if ( !alg )
    return fail( QStringLiteral( "paleo:paleo_structural_idw not registered" ) );
  std::fprintf( stderr, "running paleo_structural_idw...\n" );
  const QVariantMap results = alg->run( params, ctx, &fb );
  if ( results.isEmpty() )
    return fail( QStringLiteral( "structural run failed: %1" ).arg( fb.textLog() ) );
  const QString stem = results.value( QStringLiteral( "OUTPUT" ) ).toString();
  const QString base = stem.left( stem.lastIndexOf( QLatin1Char( '.' ) ) );
  const QString structuralPath = base + QStringLiteral( ".structural.json" );
  const QJsonObject qc = readJson( base + QStringLiteral( ".qc.json" ) );
  const QJsonObject model = readJson( structuralPath );
  if ( qc.isEmpty() || model.isEmpty() )
    return fail( QStringLiteral( "structural sidecars missing" ) );
  std::fprintf( stderr, "surface written: %s\n", qPrintable( stem ) );

  // ---- 等值线：FactorContourService::generateStructuralContours ----
  const QVector<double> levels = { 50, 80, 110, 140, 170, 200, 230, 260 };
  QVector<double> levelsUsed;
  QString contourErr;
  std::fprintf( stderr, "extracting field contours...\n" );
  if ( !FactorContourService::generateStructuralContours(
           stem, structuralPath, contourPath, levels, 0.0, &levelsUsed, &contourErr ) )
    return fail( QStringLiteral( "contour generation failed: %1" ).arg( contourErr ) );
  std::fprintf( stderr, "contours written: %s\n", qPrintable( contourPath ) );

  // ---- 分区 MS 旁路：regionalContours（regional_contours 移植）----
  std::fprintf( stderr, "running partitioned marching squares (regional)...\n" );
  auto barrierSource = std::make_unique<QgsVectorLayer>(
      barriersPath, QStringLiteral( "barriers_src" ), QStringLiteral( "ogr" ) );
  if ( !barrierSource->isValid() )
    return fail( QStringLiteral( "barrier layer failed to load" ) );
  barrierSource->setCrs( crs );
  std::vector<double> msLevels( levels.begin(), levels.end() );
  const RegionalMsOutcome ms =
      runRegionalMs( wells.get(), boundary.get(), barrierSource.get(), stem,
                     msLevels, msContourPath, crs );
  std::unique_ptr<QgsVectorLayer> msWallLayer;
  if ( ms.ok )
    std::fprintf( stderr, "regional ms contours: %d features, %d regions, "
                          "%d wall extensions\n",
                  ms.featureCount, ms.regions, ms.extensions );
  else
    std::fprintf( stderr, "regional ms skipped: %s\n", qPrintable( ms.message ) );
  msWallLayer = wallLayer( ms.walls, crs );
  if ( msWallLayer )
    styleLineLayer( msWallLayer.get(), QColor( "#C218B2" ), 0.45, Qt::DashLine );

  // ---- 图件渲染 ×2 ----
  QString renderErr;
  if ( !renderMap( stem, contourPath + QStringLiteral( "|layername=contours" ),
                   boundary.get(), wells.get(), directionsPath, barriersPath,
                   nullptr, QStringLiteral( "恩平组1-2段砂体厚度等值线图" ), mapPath,
                   crs, &renderErr ) )
    return fail( renderErr );
  // MS 拒算时也出一张分区墙诊断图：虚线为延界后的实际隔断。
  if ( !renderMap( stem,
                   ms.ok ? msContourPath + QStringLiteral( "|layername=contours" )
                         : QString(),
                   boundary.get(), wells.get(), directionsPath, barriersPath,
                   msWallLayer.get(),
                   ms.ok ? QStringLiteral( "恩平组1-2段砂体厚度等值线图（分区MS）" )
                         : QStringLiteral( "恩平组1-2段砂体厚度等值线图（分区MS·分区诊断）" ),
                   msMapPath, crs, &renderErr ) )
    return fail( renderErr );

  // ---- 验证/血缘简表 ----
  QJsonArray usedLevels;
  for ( double level : levelsUsed )
    usedLevels << level;
  QJsonObject report;
  report.insert( QStringLiteral( "algorithm" ), QStringLiteral( "paleo:paleo_structural_idw" ) );
  report.insert( QStringLiteral( "contour_engine" ), QStringLiteral( "paleo_field_contours" ) );
  report.insert( QStringLiteral( "wells_total" ), wells->featureCount() );
  report.insert( QStringLiteral( "wells_used" ), qc.value( QStringLiteral( "wells" ) )
                                                      .toArray()
                                                      .size() );
  report.insert( QStringLiteral( "wells_skipped" ), qc.value( QStringLiteral( "skipped" ) )
                                                        .toArray() );
  {
    const QJsonObject grid = model.value( QStringLiteral( "grid" ) ).toObject();
    QJsonObject dims;
    dims.insert( QStringLiteral( "nx" ),
                 grid.value( QStringLiteral( "x" ) ).toArray().size() );
    dims.insert( QStringLiteral( "ny" ),
                 grid.value( QStringLiteral( "y" ) ).toArray().size() );
    report.insert( QStringLiteral( "grid" ), dims );
  }
  report.insert( QStringLiteral( "levels_used" ), usedLevels );
  QgsVectorLayer reportContours(
      contourPath + QStringLiteral( "|layername=contours" ), QStringLiteral( "c" ),
      QStringLiteral( "ogr" ) );
  const long long contourCount =
      reportContours.isValid() ? reportContours.featureCount() : -1;
  report.insert( QStringLiteral( "contour_feature_count" ), contourCount );
  {
    long long detourCount = 0;
    const int detourIdx =
        reportContours.fields().indexOf( QStringLiteral( "detour_applied" ) );
    QgsFeature feature;
    QgsFeatureIterator it = reportContours.getFeatures();
    while ( it.nextFeature( feature ) )
      if ( detourIdx >= 0 && feature.attribute( detourIdx ).toInt() == 1 )
        ++detourCount;
    report.insert( QStringLiteral( "detour_applied_features" ), detourCount );
  }
  {
    QJsonObject msReport;
    msReport.insert( QStringLiteral( "engine" ),
                     QStringLiteral( "regional_contours_marching_squares" ) );
    msReport.insert( QStringLiteral( "status" ), ms.ok ? QStringLiteral( "ok" )
                                                       : ms.message );
    msReport.insert( QStringLiteral( "strict_mode_status" ), ms.strictStatus );
    msReport.insert( QStringLiteral( "regions" ), ms.regions );
    QJsonArray dropped;
    for ( const int region : ms.droppedRegions )
      dropped << region;
    msReport.insert( QStringLiteral( "dropped_empty_regions" ), dropped );
    msReport.insert( QStringLiteral( "contour_feature_count" ), ms.featureCount );
    msReport.insert( QStringLiteral( "wall_extensions" ), ms.extensions );
    QJsonArray wellsPerRegion;
    for ( const int count : ms.wellsPerRegion )
      wellsPerRegion << count;
    msReport.insert( QStringLiteral( "wells_per_region" ), wellsPerRegion );
    QJsonArray cellsPerRegion;
    for ( const int count : ms.cellsPerRegion )
      cellsPerRegion << count;
    msReport.insert( QStringLiteral( "cells_per_region" ), cellsPerRegion );
    QJsonArray conflicts;
    for ( const QString &conflict : ms.conflicts )
      conflicts << conflict;
    msReport.insert( QStringLiteral( "partition_conflicts" ), conflicts );
    QJsonArray issues;
    for ( const QString &issue : ms.issues )
      issues << issue;
    msReport.insert( QStringLiteral( "issues" ), issues );
    report.insert( QStringLiteral( "regional_ms" ), msReport );
  }
  QJsonObject outputs;
  outputs.insert( QStringLiteral( "raster" ), rasterRel );
  outputs.insert( QStringLiteral( "structural_sidecar" ),
                  QFileInfo( structuralPath ).fileName() );
  outputs.insert( QStringLiteral( "qc_sidecar" ),
                  QFileInfo( base + QStringLiteral( ".qc.json" ) ).fileName() );
  outputs.insert( QStringLiteral( "contour" ), contourRel );
  outputs.insert( QStringLiteral( "contour_regional_ms" ), msContourRel );
  outputs.insert( QStringLiteral( "map_png" ), mapRel );
  outputs.insert( QStringLiteral( "map_regional_ms_png" ), msMapRel );
  report.insert( QStringLiteral( "outputs" ), outputs );
  QFile reportFile( reportPath );
  if ( reportFile.open( QIODevice::WriteOnly ) )
    reportFile.write( QJsonDocument( report ).toJson() );
  std::fprintf( stderr, "report written: %s\n", qPrintable( reportPath ) );

  QgsApplication::exitQgis();
  return 0;
}
