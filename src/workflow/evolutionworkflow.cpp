// 层：功能
#include "evolutionworkflow.h"
#include "workflows_internal.h"

#include "../algorithms/evolution/compare.h"
#include "../catalog/datacatalog.h"
#include "../domain/mappinghorizons.h"
#include "../domain/singlefactorrequest.h" // 制图工作场不进跨期对比
#include "../io/faciescoveragereader.h"
#include "../metadata/layermanifest.h"
#include "../qgis/qgislayerservice.h"
#include "../qgis/qgisstyleservice.h"
#include "derivedassets.h"

#include <QFile>
#include <QJsonDocument>
#include <QJsonObject>
#include <QMetaType>

#include <cmath>
#include <limits>

#include <cpl_conv.h>
#include <gdal.h>
#include <ogr_api.h>
#include <ogr_srs_api.h>

#include <qgsmaplayer.h>
#include <qgsvectorlayer.h>

// workflow/evolutionworkflow — 方向35 编排体。
// 声明链：facies.<early>/facies.<late>（05_PaleoMap）→ readFaciesCoverage
//（io：几何 + 格网口径戳）→ compareFacies（algorithms：同源拒算门 + 指标）
// → evolution.vectors.<late>（05_PaleoMap，层位组正常管线）+ 指标表 CSV 资产。

namespace
{

using paleo::evolution::BoundaryVector;
using paleo::evolution::FaciesChange;

struct VectorRow
{
  int faciesCode = 0;
  QString kind; // "front" | "centroid"
  bool advance = false;
  double magnitude = 0;
  double dx = 0;
  double dy = 0;
  double x1 = 0;
  double y1 = 0;
  double x2 = 0;
  double y2 = 0;
};

// 迁移矢量 GPKG：一行一个箭头（front = 前缘采样位移，centroid = 质心汇总）。
// CRS 用两期共同的格网口径戳（同源门已保证一致）。
QString writeVectorGpkg( const QString &path, const std::vector<VectorRow> &rows,
                         const std::string &crsWkt, const QString &provenance, QString *error )
{
  if ( QFile::exists( path ) && !QFile::remove( path ) )
  {
    *error = QObject::tr( "无法替换既有迁移矢量文件：%1" ).arg( path );
    return QString();
  }
  GDALDriverH drv = GDALGetDriverByName( "GPKG" );
  if ( !drv )
  {
    *error = QObject::tr( "GPKG 驱动不可用" );
    return QString();
  }
  GDALDatasetH ds = GDALCreate( drv, path.toUtf8().constData(), 0, 0, 0, GDT_Unknown, nullptr );
  if ( !ds )
  {
    *error = QObject::tr( "无法创建迁移矢量文件 %1：%2" )
                 .arg( path, QString::fromUtf8( CPLGetLastErrorMsg() ) );
    return QString();
  }
  OGRSpatialReferenceH srs = nullptr;
  if ( !crsWkt.empty() )
  {
    srs = OSRNewSpatialReference( nullptr );
    QByteArray bytes = QByteArray::fromStdString( crsWkt );
    char *ptr = bytes.data();
    if ( OSRImportFromWkt( srs, &ptr ) != OGRERR_NONE )
    {
      OSRDestroySpatialReference( srs );
      srs = nullptr;
    }
    else
    {
      OSRSetAxisMappingStrategy( srs, OAMS_TRADITIONAL_GIS_ORDER );
    }
  }
  OGRLayerH layer = GDALDatasetCreateLayer( ds, "evolution_vectors", srs, wkbLineString, nullptr );
  if ( srs )
    OSRDestroySpatialReference( srs );
  if ( !layer )
  {
    GDALClose( ds );
    *error = QObject::tr( "无法创建 evolution_vectors 图层" );
    return QString();
  }
  struct FieldSpec
  {
    const char *name;
    OGRFieldType type;
  };
  const FieldSpec fields[] = {
    { "facies_code", OFTInteger }, { "vector_kind", OFTString },   { "advance", OFTInteger },
    { "magnitude", OFTReal },      { "dx", OFTReal },              { "dy", OFTReal },
  };
  for ( const FieldSpec &f : fields )
  {
    OGRFieldDefnH fld = OGR_Fld_Create( f.name, f.type );
    OGR_L_CreateField( layer, fld, TRUE );
    OGR_Fld_Destroy( fld );
  }
  for ( const VectorRow &row : rows )
  {
    OGRGeometryH line = OGR_G_CreateGeometry( wkbLineString );
    OGR_G_AddPoint_2D( line, row.x1, row.y1 );
    OGR_G_AddPoint_2D( line, row.x2, row.y2 );
    OGRFeatureH feat = OGR_F_Create( OGR_L_GetLayerDefn( layer ) );
    OGR_F_SetFieldInteger( feat, 0, row.faciesCode );
    OGR_F_SetFieldString( feat, 1, row.kind.toUtf8().constData() );
    OGR_F_SetFieldInteger( feat, 2, row.advance ? 1 : 0 );
    OGR_F_SetFieldDouble( feat, 3, row.magnitude );
    OGR_F_SetFieldDouble( feat, 4, row.dx );
    OGR_F_SetFieldDouble( feat, 5, row.dy );
    OGR_F_SetGeometry( feat, line );
    OGR_G_DestroyGeometry( line );
    if ( OGR_L_CreateFeature( layer, feat ) != OGRERR_NONE )
    {
      OGR_F_Destroy( feat );
      GDALClose( ds );
      *error = QObject::tr( "迁移矢量要素写入失败" );
      return QString();
    }
    OGR_F_Destroy( feat );
  }
  GDALSetMetadataItem( ds, "PALEO_PROVENANCE", provenance.toUtf8().constData(), nullptr );
  GDALClose( ds );
  return path;
}

QString csvNumber( double value )
{
  if ( std::isnan( value ) )
    return QString();
  return QString::number( value, 'g', 12 );
}

} // namespace

EvolutionWorkflow::EvolutionWorkflow( QgisLayerService *layers, QObject *parent )
  : QObject( parent ), m_layers( layers )
{
}

void EvolutionWorkflow::setCatalog( DataCatalog *catalog, const QString &projectDir )
{
  m_catalog = catalog;
  m_projectDir = projectDir;
}

QgisLayerService *EvolutionWorkflow::layerService() const
{
  return m_layers.data();
}

DataCatalog *EvolutionWorkflow::catalog() const
{
  return m_catalog.data();
}

paleo::evolution::EvolutionResult EvolutionWorkflow::lastMetrics() const
{
  return m_last;
}

QStringList EvolutionWorkflow::metricsCsvHeader()
{
  return { QStringLiteral( "earlier_horizon" ),
           QStringLiteral( "later_horizon" ),
           QStringLiteral( "facies_code" ),
           QStringLiteral( "area_earlier_m2" ),
           QStringLiteral( "area_later_m2" ),
           QStringLiteral( "area_change_m2" ),
           QStringLiteral( "area_change_ratio" ),
           QStringLiteral( "centroid_dx_m" ),
           QStringLiteral( "centroid_dy_m" ),
           QStringLiteral( "centroid_displacement_m" ),
           QStringLiteral( "boundary_median_shift_m" ),
           QStringLiteral( "boundary_advance_ratio" ),
           QStringLiteral( "boundary_median_azimuth_deg" ),
           QStringLiteral( "boundary_resultant" ),
           QStringLiteral( "boundary_samples" ),
           QStringLiteral( "pair_turnover_ratio" ) };
}

QString EvolutionWorkflow::buildMetricsCsv() const
{
  QStringList lines;
  lines << metricsCsvHeader().join( QLatin1Char( ',' ) );
  for ( const QVariant &rowVar : m_metricsRows )
  {
    const QVariantMap row = rowVar.toMap();
    QStringList cells;
    for ( const QString &column : metricsCsvHeader() )
    {
      const QVariant value = row.value( column );
      if ( value.typeId() == QMetaType::Double )
        cells << csvNumber( value.toDouble() );
      else
        cells << value.toString();
    }
    lines << cells.join( QLatin1Char( ',' ) );
  }
  return lines.join( QLatin1Char( '\n' ) ) + QLatin1Char( '\n' );
}

bool EvolutionWorkflow::analyzePair( const QString &earlierHorizon, const QString &laterHorizon,
                                     const QVariantMap &params, QString *error )
{
  const auto fail = [this, &earlierHorizon, &laterHorizon, error]( const QString &msg ) {
    paleo::workflow_detail::setError( error, msg );
    emit analysisFailed( earlierHorizon, laterHorizon, msg );
    return false;
  };

  QgisLayerService *layers = m_layers.data();
  if ( !layers )
    return fail( tr( "演化工作流未绑定图层服务" ) );
  if ( earlierHorizon.isEmpty() || laterHorizon.isEmpty() || earlierHorizon == laterHorizon )
    return fail( tr( "相邻期对比需要两个不同的层位" ) );

  // 解析两期相多边形声明（05_PaleoMap 的 facies.<层位>）。
  QVector<LayerDeclaration> declared;
  QString manifestErr;
  if ( !layers->tryDeclared( &declared, &manifestErr ) )
    return fail( manifestErr.isEmpty() ? tr( "无法读取图层清单" ) : manifestErr );
  QString earlierSource;
  QString laterSource;
  QString earlierGroup;
  QString laterGroup;
  for ( const LayerDeclaration &d : declared )
  {
    if ( d.layerId == QStringLiteral( "facies.%1" ).arg( earlierHorizon ) )
    {
      earlierSource = d.source;
      earlierGroup = d.group;
    }
    else if ( d.layerId == QStringLiteral( "facies.%1" ).arg( laterHorizon ) )
    {
      laterSource = d.source;
      laterGroup = d.group;
    }
  }
  // 解释性制图工作场不是分析相图——同融合/分相的守卫口径。
  if ( paleo::singlefactor::isCartographicProductLayer(
         QStringLiteral( "facies.%1" ).arg( earlierHorizon ), earlierGroup ) ||
       paleo::singlefactor::isCartographicProductLayer(
         QStringLiteral( "facies.%1" ).arg( laterHorizon ), laterGroup ) )
    return fail( tr( "解释性制图工作场不能参与跨期演化对比" ) );
  if ( earlierSource.isEmpty() )
    return fail( tr( "层位 %1 尚无相多边形图层（facies.%1），先完成该期分相" ).arg( earlierHorizon ) );
  if ( laterSource.isEmpty() )
    return fail( tr( "层位 %1 尚无相多边形图层（facies.%1），先完成该期分相" ).arg( laterHorizon ) );

  // 读取两期覆盖（几何 + 格网口径戳）。读取器对缺戳产物如实报错。
  const paleo::evolution::CoverageReadResult earlierRead =
    paleo::evolution::readFaciesCoverage( earlierSource, earlierHorizon );
  if ( !earlierRead.ok )
    return fail( tr( "读取早期（%1）相覆盖失败：%2" ).arg( earlierHorizon, earlierRead.error ) );
  const paleo::evolution::CoverageReadResult laterRead =
    paleo::evolution::readFaciesCoverage( laterSource, laterHorizon );
  if ( !laterRead.ok )
    return fail( tr( "读取晚期（%1）相覆盖失败：%2" ).arg( laterHorizon, laterRead.error ) );

  // 内核对比：不同格网/坐标域在这里拒算（原因可读，不近似）。
  paleo::evolution::CompareOptions options;
  options.boundarySampleSpacing = params.value( QStringLiteral( "boundary_sample_spacing" ) ).toDouble();
  const QVariant maxSamples = params.value( QStringLiteral( "max_boundary_samples" ) );
  options.maxBoundarySamples = maxSamples.isValid() ? maxSamples.toInt() : 4000;
  const paleo::evolution::EvolutionResult metrics =
    paleo::evolution::compareFacies( earlierRead.coverage, laterRead.coverage, options );
  if ( metrics.status != paleo::evolution::Status::Ok )
    return fail( tr( "演化对比拒算（%1 → %2）：%3" )
                   .arg( earlierHorizon, laterHorizon, QString::fromStdString( metrics.message ) ) );
  m_last = metrics;

  // ---- 迁移矢量图层（staged derived → declare → 样式） --------------------
  if ( !m_catalog )
    return fail( tr( "演化工作流未绑定数据目录（catalog）——派生产物无法登记到工程" ) );
  DerivedAssetRegistrar registrar( m_catalog.data(), m_projectDir );
  QString regErr;

  std::vector<VectorRow> rows;
  rows.reserve( metrics.boundaryField.size() + metrics.changes.size() );
  for ( const BoundaryVector &vector : metrics.boundaryField )
  {
    VectorRow row;
    row.faciesCode = vector.faciesCode;
    row.kind = QStringLiteral( "front" );
    row.advance = vector.advance;
    row.dx = vector.to.x - vector.from.x;
    row.dy = vector.to.y - vector.from.y;
    row.magnitude = std::hypot( row.dx, row.dy );
    row.x1 = vector.from.x;
    row.y1 = vector.from.y;
    row.x2 = vector.to.x;
    row.y2 = vector.to.y;
    rows.push_back( std::move( row ) );
  }
  for ( const FaciesChange &change : metrics.changes )
  {
    if ( !change.centroidsValid )
      continue;
    VectorRow row;
    row.faciesCode = change.faciesCode;
    row.kind = QStringLiteral( "centroid" );
    row.advance = change.areaChange >= 0; // 质心箭头配色跟面积增减语义
    row.dx = change.centroidLater.x - change.centroidEarlier.x;
    row.dy = change.centroidLater.y - change.centroidEarlier.y;
    row.magnitude = change.centroidDisplacement;
    row.x1 = change.centroidEarlier.x;
    row.y1 = change.centroidEarlier.y;
    row.x2 = change.centroidLater.x;
    row.y2 = change.centroidLater.y;
    rows.push_back( std::move( row ) );
  }

  const DerivedStaging vectorStaging = registrar.stage(
    QStringLiteral( "evolution_vectors" ),
    tr( "迁移矢量 %1→%2" ).arg( earlierHorizon, laterHorizon ),
    QStringLiteral( "EVOLUTION_VECTORS_%1.gpkg" ).arg( laterHorizon ), &regErr );
  if ( !vectorStaging.isValid() )
    return fail( regErr );

  QJsonObject prov;
  prov.insert( QStringLiteral( "algorithm" ), QStringLiteral( "paleo:evolution_compare_v1" ) );
  prov.insert( QStringLiteral( "earlier" ), earlierHorizon );
  prov.insert( QStringLiteral( "later" ), laterHorizon );
  prov.insert( QStringLiteral( "method_note" ), QString::fromStdString( metrics.methodNote ) );
  const QString provenanceJson =
    QString::fromUtf8( QJsonDocument( prov ).toJson( QJsonDocument::Compact ) );
  QString writeErr;
  if ( writeVectorGpkg( vectorStaging.absolutePath, rows, earlierRead.coverage.grid.crs,
                        provenanceJson, &writeErr )
       .isEmpty() )
    return fail( writeErr );

  QVariantMap vectorExtra;
  vectorExtra.insert( QStringLiteral( "earlier_horizon" ), earlierHorizon );
  vectorExtra.insert( QStringLiteral( "later_horizon" ), laterHorizon );
  vectorExtra.insert( QStringLiteral( "vector_count" ), static_cast<int>( rows.size() ) );
  vectorExtra.insert( QStringLiteral( "method_note" ),
                      QString::fromStdString( metrics.methodNote ) );
  QString commitErr;
  if ( !registrar.commitExternal( vectorStaging, vectorStaging.absolutePath,
                                  registrar.parentVersionIdsFor( { earlierSource.section( QLatin1Char( '|' ), 0, 0 ),
                                                                   laterSource.section( QLatin1Char( '|' ), 0, 0 ) } ),
                                  QStringLiteral( "paleo:evolution_compare_v1" ), vectorExtra,
                                  &commitErr ) )
    return fail( commitErr );

  LayerDeclaration vectorDecl;
  vectorDecl.layerId = QStringLiteral( "evolution.vectors.%1" ).arg( laterHorizon );
  vectorDecl.horizon = laterHorizon;
  vectorDecl.type = QStringLiteral( "vector" );
  vectorDecl.source =
    QStringLiteral( "%1|layername=evolution_vectors" ).arg( vectorStaging.absolutePath );
  vectorDecl.group = QStringLiteral( "05_PaleoMap" );
  vectorDecl.title = tr( "迁移矢量 %1→%2" ).arg( earlierHorizon, laterHorizon );
  if ( !layers->declare( vectorDecl, error ) )
    return fail( error && !error->isEmpty() ? *error
                                            : tr( "迁移矢量图层声明失败：%1" ).arg( vectorDecl.layerId ) );
  paleo::workflow_detail::stampLayerAssetLink( layers, vectorDecl.layerId, vectorStaging.assetId );

  // 样式应用 best-effort：声明已成立，实例化/样式失败不回滚产物（下回
  // 打开工程重挂）。不借 *error 出口——避免「成功但 error 非空」的歧义。
  if ( QgsMapLayer *layer = layers->instantiate( vectorDecl.layerId, nullptr ) )
  {
    if ( auto *vector = qobject_cast<QgsVectorLayer *>( layer ) )
    {
      QgisStyleService::applyMigrationVectorStyle( vector );
      vector->triggerRepaint();
    }
  }

  // ---- 指标表 CSV 资产（版本化：每对分析落一个新版本快照） ----------------
  QVariantList pendingRows;
  for ( const FaciesChange &change : metrics.changes )
  {
    QVariantMap row;
    row.insert( QStringLiteral( "earlier_horizon" ), earlierHorizon );
    row.insert( QStringLiteral( "later_horizon" ), laterHorizon );
    row.insert( QStringLiteral( "facies_code" ), change.faciesCode );
    row.insert( QStringLiteral( "area_earlier_m2" ), change.areaEarlier );
    row.insert( QStringLiteral( "area_later_m2" ), change.areaLater );
    row.insert( QStringLiteral( "area_change_m2" ), change.areaChange );
    row.insert( QStringLiteral( "area_change_ratio" ), change.areaChangeRatio );
    row.insert( QStringLiteral( "centroid_dx_m" ),
                change.centroidsValid
                  ? change.centroidLater.x - change.centroidEarlier.x
                  : std::numeric_limits<double>::quiet_NaN() );
    row.insert( QStringLiteral( "centroid_dy_m" ),
                change.centroidsValid
                  ? change.centroidLater.y - change.centroidEarlier.y
                  : std::numeric_limits<double>::quiet_NaN() );
    row.insert( QStringLiteral( "centroid_displacement_m" ), change.centroidDisplacement );
    row.insert( QStringLiteral( "boundary_median_shift_m" ), change.boundaryMedianShift );
    row.insert( QStringLiteral( "boundary_advance_ratio" ), change.boundaryAdvanceRatio );
    row.insert( QStringLiteral( "boundary_median_azimuth_deg" ), change.boundaryMedianAzimuthDeg );
    row.insert( QStringLiteral( "boundary_resultant" ), change.boundaryResultant );
    row.insert( QStringLiteral( "boundary_samples" ), change.boundarySamples );
    row.insert( QStringLiteral( "pair_turnover_ratio" ), metrics.faciesTurnoverRatio );
    pendingRows.append( row );
  }

  const DerivedStaging metricsStaging =
    registrar.stage( QStringLiteral( "evolution_metrics" ), tr( "演化指标表" ),
                     QStringLiteral( "evolution_metrics.csv" ), &regErr );
  if ( !metricsStaging.isValid() )
    return fail( regErr );
  {
    QFile csv( metricsStaging.absolutePath );
    if ( !csv.open( QIODevice::WriteOnly | QIODevice::Truncate ) )
      return fail( tr( "无法写指标表：%1" ).arg( metricsStaging.absolutePath ) );
    const QByteArray bytes = buildMetricsCsv().toUtf8();
    if ( csv.write( bytes ) != bytes.size() )
      return fail( tr( "指标表写入不完整：%1" ).arg( metricsStaging.absolutePath ) );
  }
  m_metricsRows.append( pendingRows ); // CSV 写出成功后才进会话累计（失败重跑不重复）
  QVariantMap metricsExtra;
  metricsExtra.insert( QStringLiteral( "earlier_horizon" ), earlierHorizon );
  metricsExtra.insert( QStringLiteral( "later_horizon" ), laterHorizon );
  metricsExtra.insert( QStringLiteral( "rows" ), m_metricsRows.size() );
  metricsExtra.insert( QStringLiteral( "method_note" ),
                       QString::fromStdString( metrics.methodNote ) );
  if ( !registrar.commitExternal( metricsStaging, metricsStaging.absolutePath,
                                  registrar.parentVersionIdsFor( { earlierSource.section( QLatin1Char( '|' ), 0, 0 ),
                                                                   laterSource.section( QLatin1Char( '|' ), 0, 0 ) } ),
                                  QStringLiteral( "paleo:evolution_compare_v1" ), metricsExtra,
                                  &commitErr ) )
    return fail( commitErr );

  emit pairAnalyzed( earlierHorizon, laterHorizon, vectorDecl.layerId );
  return true;
}

int EvolutionWorkflow::analyzeSequence( QStringList *failures, QString *error )
{
  QgisLayerService *layers = m_layers.data();
  if ( !layers )
  {
    paleo::workflow_detail::setError( error, tr( "演化工作流未绑定图层服务" ) );
    return -1;
  }
  const QStringList horizons = mappingHorizons(); // 浅 → 深
  int analyzed = 0;
  for ( int i = 0; i + 1 < horizons.size(); ++i )
  {
    const QString &later = horizons.at( i );       // 浅 = 新
    const QString &earlier = horizons.at( i + 1 ); // 深 = 老
    QString pairError;
    if ( !analyzePair( earlier, later, QVariantMap(), &pairError ) )
    {
      if ( failures )
        failures->append( QStringLiteral( "%1→%2：%3" ).arg( earlier, later, pairError ) );
      continue;
    }
    ++analyzed;
  }
  emit sequenceDone( analyzed );
  return analyzed;
}
