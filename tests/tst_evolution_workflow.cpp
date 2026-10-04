// 层：功能（测试壳位于 tests/，被测对象为工作流编排面）
#include "helpers/workflowfixture.h"

#include <QtTest>
#include <QFile>
#include <QSignalSpy>

#include <qgsapplication.h>
#include <qgsmaplayer.h>
#include <qgsproject.h>
#include <qgsrulebasedrenderer.h>
#include <qgsvectorlayer.h>

#include <cpl_conv.h>
#include <gdal.h>
#include <ogr_api.h>

#include "../src/catalog/datacatalog.h"
#include "../src/domain/arearules.h"
#include "../src/domain/mappinghorizons.h"
#include "../src/io/faciescoveragereader.h"
#include "../src/metadata/layermanifest.h"
#include "../src/workflow/evolutionworkflow.h"
#include "../src/workflow/workflows.h"

// 方向35 编排验收（真实服务端到端）：
//   · 两期分类栅格 → deriveFaciesPolygons（带格网口径戳）→ analyzePair →
//     evolution.vectors.<层位> 进 05_PaleoMap 层位组 + 箭头样式 + 指标表 CSV 资产；
//   · 不同格网（像元/尺寸不同）→ 拒算且原因可读；
//   · 无口径戳的旧产物 → 读取器如实拒绝；
//   · analyzeSequence 按 mappingHorizons() 相邻对（对内 earlier=更深）。

using paleo::tests::WorkflowFixture;
using paleo::tests::initFixture;

namespace
{

LayerDeclaration decl( const QString &layerId, const QString &horizon, const QString &type,
                       const QString &source, const QString &group )
{
  LayerDeclaration d;
  d.layerId = layerId;
  d.horizon = horizon;
  d.type = type;
  d.source = source;
  d.group = group;
  return d;
}

const LayerDeclaration *findDecl( QgisLayerService &layers, const QString &layerId )
{
  static LayerDeclaration found;
  const QVector<LayerDeclaration> decls = layers.declared();
  for ( const LayerDeclaration &d : decls )
    if ( d.layerId == layerId )
    {
      found = d;
      return &found;
    }
  return nullptr;
}

// Float32 GTiff，像元 cell（正方北向上）。
QString makeRaster( const QString &path, int w, int h, const QVector<float> &px, double cell )
{
  GDALDriverH drv = GDALGetDriverByName( "GTiff" );
  GDALDatasetH ds = GDALCreate( drv, path.toUtf8().constData(), w, h, 1, GDT_Float32, nullptr );
  if ( !ds )
    return QString();
  const double gt[6] = { 0.0, cell, 0.0, static_cast<double>( h ) * cell, 0.0, -cell };
  GDALSetGeoTransform( ds, const_cast<double *>( gt ) );
  GDALRasterBandH band = GDALGetRasterBand( ds, 1 );
  GDALSetRasterNoDataValue( band, -9999.0 );
  const CPLErr err = GDALRasterIO( band, GF_Write, 0, 0, w, h,
                                   const_cast<float *>( px.constData() ), w, h, GDT_Float32, 0, 0 );
  GDALClose( ds );
  return err == CE_None ? path : QString();
}

// 4×4 两相栅格（北向上，行 0 = 顶行）：early = 上半 1 / 下半 2；
// late = 1 只剩顶行（2 向北进积一行）。
QVector<float> earlyPixels()
{
  return { 1, 1, 1, 1, 1, 1, 1, 1, 2, 2, 2, 2, 2, 2, 2, 2 };
}

QVector<float> latePixels()
{
  return { 1, 1, 1, 1, 2, 2, 2, 2, 2, 2, 2, 2, 2, 2, 2, 2 };
}

// 走真实 deriveFaciesPolygons 产两期相多边形（含格网口径戳），返回是否成功。
bool deriveBothPeriods( WorkflowFixture &f, double earlyCell, double lateCell, QString *error )
{
  const QString earlyRaster = makeRaster( f.dir.filePath( QStringLiteral( "coded_H2.tif" ) ),
                                          4, 4, earlyPixels(), earlyCell );
  const QString lateRaster = makeRaster( f.dir.filePath( QStringLiteral( "coded_H1.tif" ) ),
                                         4, 4, latePixels(), lateCell );
  if ( earlyRaster.isEmpty() || lateRaster.isEmpty() )
    return false;

  QString err;
  if ( !f.layers.declare( decl( QStringLiteral( "composite.H2" ), QStringLiteral( "H2" ),
                                QStringLiteral( "raster" ), earlyRaster,
                                QStringLiteral( "05_PaleoMap" ) ),
                          &err ) )
  {
    *error = err;
    return false;
  }
  if ( !f.layers.declare( decl( QStringLiteral( "composite.H1" ), QStringLiteral( "H1" ),
                                QStringLiteral( "raster" ), lateRaster,
                                QStringLiteral( "05_PaleoMap" ) ),
                          &err ) )
  {
    *error = err;
    return false;
  }

  CompositionWorkflow compose( &f.proc, &f.layers );
  compose.setCatalog( &f.catalog, f.dir.path() );
  if ( !compose.deriveFaciesPolygons( QStringLiteral( "H2" ), QStringLiteral( "composite.H2" ),
                                      QVariantMap(), &err ) )
  {
    *error = err;
    return false;
  }
  if ( !compose.deriveFaciesPolygons( QStringLiteral( "H1" ), QStringLiteral( "composite.H1" ),
                                      QVariantMap(), &err ) )
  {
    *error = err;
    return false;
  }
  return findDecl( f.layers, QStringLiteral( "facies.H1" ) ) != nullptr &&
         findDecl( f.layers, QStringLiteral( "facies.H2" ) ) != nullptr;
}

// 无口径戳的旧式相多边形 GPKG（模拟方向35 之前的产物）。
QString makeLegacyGpkg( const QString &path )
{
  GDALDriverH drv = GDALGetDriverByName( "GPKG" );
  if ( !drv )
    return QString();
  GDALDatasetH ds = GDALCreate( drv, path.toUtf8().constData(), 0, 0, 0, GDT_Unknown, nullptr );
  if ( !ds )
    return QString();
  OGRLayerH layer = GDALDatasetCreateLayer( ds, "facies_polygons", nullptr, wkbPolygon, nullptr );
  OGRFieldDefnH codeFld = OGR_Fld_Create( "facies_code", OFTInteger );
  OGR_L_CreateField( layer, codeFld, TRUE );
  OGR_Fld_Destroy( codeFld );
  OGRGeometryH rect = OGR_G_CreateGeometry( wkbPolygon );
  OGRGeometryH ring = OGR_G_CreateGeometry( wkbLinearRing );
  OGR_G_AddPoint_2D( ring, 0, 0 );
  OGR_G_AddPoint_2D( ring, 4, 0 );
  OGR_G_AddPoint_2D( ring, 4, 4 );
  OGR_G_AddPoint_2D( ring, 0, 4 );
  OGR_G_AddPoint_2D( ring, 0, 0 );
  OGR_G_AddGeometryDirectly( rect, ring );
  OGRFeatureH feat = OGR_F_Create( OGR_L_GetLayerDefn( layer ) );
  OGR_F_SetFieldInteger( feat, 0, 1 );
  OGR_F_SetGeometry( feat, rect );
  OGR_L_CreateFeature( layer, feat );
  OGR_F_Destroy( feat );
  OGR_G_DestroyGeometry( rect );
  GDALClose( ds );
  return path;
}

bool hasDerivedAsset( DataCatalog &catalog, const QString &assetType )
{
  for ( const CatalogAsset &a : catalog.assets() )
  {
    if ( a.type != assetType )
      continue;
    for ( const CatalogVersion &v : catalog.versionsForAsset( a.id ) )
      if ( !v.sha256.isEmpty() )
        return true;
  }
  return false;
}

} // namespace

class TestEvolutionWorkflow : public QObject
{
  Q_OBJECT
  private slots:
    void initTestCase()
    {
      QVERIFY( QgsApplication::instance() != nullptr );
      QVERIFY2( mappingHorizons().contains( QStringLiteral( "D61" ) ),
                "默认层位序应在场（无需工程配置）" );
    }

    void cleanup()
    {
      QgsProject::instance()->removeAllMapLayers();
      AreaRules::reset();
    }

    // 端到端：两期分相 → 演化对比 → 矢量图层进层位组 + 指标资产。
    void pairAnalysisDeclaresVectorLayerAndMetrics();

    // Oracle 1：格网不同源（像元尺寸不同）→ 拒算且原因可读。
    void refusesWhenGridsDiffer();

    // 旧产物无口径戳 → 拒绝，不猜格网。
    void refusesLegacyProductWithoutStamp();

    // analyzeSequence：按 mappingHorizons() 取相邻对，对内 earlier=更深。
    void sequenceAnalyzesAdjacentPairs();
};

void TestEvolutionWorkflow::pairAnalysisDeclaresVectorLayerAndMetrics()
{
  WorkflowFixture f;
  QVERIFY( initFixture( f ) );
  QString err;
  QVERIFY2( deriveBothPeriods( f, 1.0, 1.0, &err ), qPrintable( err ) );

  EvolutionWorkflow wf( &f.layers );
  wf.setCatalog( &f.catalog, f.dir.path() );
  QSignalSpy analyzed( &wf, &EvolutionWorkflow::pairAnalyzed );
  QSignalSpy failed( &wf, &EvolutionWorkflow::analysisFailed );

  QVERIFY2( wf.analyzePair( QStringLiteral( "H2" ), QStringLiteral( "H1" ), QVariantMap(), &err ),
            qPrintable( err ) );
  QCOMPARE( failed.count(), 0 );
  QCOMPARE( analyzed.count(), 1 );
  QCOMPARE( analyzed.at( 0 ).at( 0 ).toString(), QStringLiteral( "H2" ) );
  QCOMPARE( analyzed.at( 0 ).at( 1 ).toString(), QStringLiteral( "H1" ) );
  QCOMPARE( analyzed.at( 0 ).at( 2 ).toString(), QStringLiteral( "evolution.vectors.H1" ) );

  // 声明契约：05_PaleoMap 层位组、horizon=晚期、受管派生路径。
  const LayerDeclaration *d = findDecl( f.layers, QStringLiteral( "evolution.vectors.H1" ) );
  QVERIFY( d != nullptr );
  QCOMPARE( d->type, QStringLiteral( "vector" ) );
  QCOMPARE( d->group, QStringLiteral( "05_PaleoMap" ) );
  QCOMPARE( d->horizon, QStringLiteral( "H1" ) );
  QVERIFY( d->source.contains( QStringLiteral( "artifacts/derived/" ) ) );

  // GPKG 内容：evolution_vectors 图层 + 语义字段 + 矢量要素。
  QgsVectorLayer vl( d->source, QStringLiteral( "vectors" ), QStringLiteral( "ogr" ) );
  QVERIFY2( vl.isValid(), qPrintable( vl.error().message() ) );
  QVERIFY( vl.fields().lookupField( QStringLiteral( "vector_kind" ) ) >= 0 );
  QVERIFY( vl.fields().lookupField( QStringLiteral( "advance" ) ) >= 0 );
  QVERIFY( vl.fields().lookupField( QStringLiteral( "facies_code" ) ) >= 0 );
  QVERIFY( vl.featureCount() > 0 );

  // 箭头样式：规则渲染器接管（进/退 × 前缘/质心）。
  QgsMapLayer *layer = f.layers.instantiate( QStringLiteral( "evolution.vectors.H1" ), &err );
  QVERIFY( layer != nullptr );
  auto *vector = qobject_cast<QgsVectorLayer *>( layer );
  QVERIFY( vector != nullptr );
  QVERIFY( dynamic_cast<QgsRuleBasedRenderer *>( vector->renderer() ) != nullptr );

  // 指标：口径可读 + 数值与合成输入一致（像元 1m：1=8→4、2=8→12）。
  const paleo::evolution::EvolutionResult metrics = wf.lastMetrics();
  QCOMPARE( metrics.status, paleo::evolution::Status::Ok );
  QVERIFY( !metrics.methodNote.empty() );
  QCOMPARE( metrics.faciesCodes.size(), static_cast<std::size_t>( 2 ) );
  for ( const paleo::evolution::FaciesChange &change : metrics.changes )
  {
    if ( change.faciesCode == 1 )
    {
      QCOMPARE( change.areaEarlier, 8.0 );
      QCOMPARE( change.areaLater, 4.0 );
      QVERIFY( std::fabs( change.areaChange + 4.0 ) < 1e-9 );
    }
    else if ( change.faciesCode == 2 )
    {
      QCOMPARE( change.areaEarlier, 8.0 );
      QCOMPARE( change.areaLater, 12.0 );
      QVERIFY( std::fabs( change.areaChange - 4.0 ) < 1e-9 );
    }
    else
    {
      QFAIL( "合成输入只有相 1/2" );
    }
  }
  QVERIFY( std::fabs( metrics.faciesTurnoverRatio - 0.25 ) < 1e-9 ); // 4/16 换相
  QVERIFY( wf.metricsRows().size() == 2 );
  QVERIFY( EvolutionWorkflow::metricsCsvHeader().size() == 16 );

  // 资产登记：矢量 GPKG 与指标表都进 DERIVED 版本。
  QVERIFY( hasDerivedAsset( f.catalog, QStringLiteral( "evolution_vectors" ) ) );
  QVERIFY( hasDerivedAsset( f.catalog, QStringLiteral( "evolution_metrics" ) ) );
}

void TestEvolutionWorkflow::refusesWhenGridsDiffer()
{
  WorkflowFixture f;
  QVERIFY( initFixture( f ) );
  QString err;
  QVERIFY2( deriveBothPeriods( f, 1.0, 2.0, &err ), qPrintable( err ) ); // 像元 1m vs 2m

  EvolutionWorkflow wf( &f.layers );
  wf.setCatalog( &f.catalog, f.dir.path() );
  QSignalSpy failed( &wf, &EvolutionWorkflow::analysisFailed );

  QVERIFY( !wf.analyzePair( QStringLiteral( "H2" ), QStringLiteral( "H1" ), QVariantMap(), &err ) );
  QVERIFY2( err.contains( QStringLiteral( "拒算" ) ), qPrintable( err ) );
  QVERIFY2( err.contains( QStringLiteral( "像元尺寸" ) ), qPrintable( err ) );
  QCOMPARE( failed.count(), 1 );
  // 拒算不落产物：无矢量图层、无指标资产。
  QVERIFY( findDecl( f.layers, QStringLiteral( "evolution.vectors.H1" ) ) == nullptr );
  QVERIFY( !hasDerivedAsset( f.catalog, QStringLiteral( "evolution_vectors" ) ) );
}

void TestEvolutionWorkflow::refusesLegacyProductWithoutStamp()
{
  WorkflowFixture f;
  QVERIFY( initFixture( f ) );
  QString err;
  QVERIFY2( deriveBothPeriods( f, 1.0, 1.0, &err ), qPrintable( err ) );

  const QString legacy = makeLegacyGpkg( f.dir.filePath( QStringLiteral( "legacy.gpkg" ) ) );
  QVERIFY( !legacy.isEmpty() );
  QVERIFY( f.layers.declare( decl( QStringLiteral( "facies.H3" ), QStringLiteral( "H3" ),
                                   QStringLiteral( "vector" ),
                                   QStringLiteral( "%1|layername=facies_polygons" ).arg( legacy ),
                                   QStringLiteral( "05_PaleoMap" ) ),
                             &err ) );

  // 读取器层：缺戳如实报错（不猜格网）。
  const paleo::evolution::CoverageReadResult read =
    paleo::evolution::readFaciesCoverage( QStringLiteral( "%1|layername=facies_polygons" ).arg( legacy ),
                                          QStringLiteral( "H3" ) );
  QVERIFY( !read.ok );
  QVERIFY2( read.error.contains( QStringLiteral( "PALEO_PROVENANCE" ) ), qPrintable( read.error ) );

  // 编排层：同因拒算。
  EvolutionWorkflow wf( &f.layers );
  wf.setCatalog( &f.catalog, f.dir.path() );
  QVERIFY( !wf.analyzePair( QStringLiteral( "H3" ), QStringLiteral( "H1" ), QVariantMap(), &err ) );
  QVERIFY2( err.contains( QStringLiteral( "口径戳" ) ), qPrintable( err ) );
}

void TestEvolutionWorkflow::sequenceAnalyzesAdjacentPairs()
{
  WorkflowFixture f;
  QVERIFY( initFixture( f ) );
  QString err;
  QVERIFY2( deriveBothPeriods( f, 1.0, 1.0, &err ), qPrintable( err ) );

  // 工程层位序只含两期（浅→深 = H1、H2）：唯一相邻对，对内 earlier=H2。
  QFile config( f.dir.filePath( QStringLiteral( "project_area.json" ) ) );
  QVERIFY( config.open( QIODevice::WriteOnly | QIODevice::Truncate ) );
  config.write( R"({
  "schema_version": 1,
  "sequence_boundaries": ["H1", "H2"],
  "target_horizon": "H1",
  "classifier": {"dat_path_rules": [], "reference_dir_names": [], "fixed_auxiliary_name_stem": "OTHER-1"},
  "segy_indexing": {"inline_word_offset": 189, "crossline_word_offset": 193, "field_record_offset": 9, "cdp_xline_offset": 24},
  "onnx_grid": {"rows": 97, "cols": 129}
})" );
  config.close();
  AreaRules::setProjectDir( f.dir.path() );
  QVERIFY( AreaRules::lastError().isEmpty() );
  QCOMPARE( mappingHorizons(), QStringList( { QStringLiteral( "H1" ), QStringLiteral( "H2" ) } ) );

  EvolutionWorkflow wf( &f.layers );
  wf.setCatalog( &f.catalog, f.dir.path() );
  QSignalSpy analyzed( &wf, &EvolutionWorkflow::pairAnalyzed );
  QSignalSpy done( &wf, &EvolutionWorkflow::sequenceDone );
  QSignalSpy failed( &wf, &EvolutionWorkflow::analysisFailed );

  QStringList failures;
  QCOMPARE( wf.analyzeSequence( &failures, &err ), 1 );
  QCOMPARE( failed.count(), 0 );
  QVERIFY( failures.isEmpty() );
  QCOMPARE( analyzed.count(), 1 );
  QCOMPARE( analyzed.at( 0 ).at( 0 ).toString(), QStringLiteral( "H2" ) ); // earlier = 更深
  QCOMPARE( analyzed.at( 0 ).at( 1 ).toString(), QStringLiteral( "H1" ) ); // later = 更浅
  QCOMPARE( done.count(), 1 );
  QVERIFY( findDecl( f.layers, QStringLiteral( "evolution.vectors.H1" ) ) != nullptr );
}

#include "tst_evolution_workflow.moc"

int main( int argc, char *argv[] )
{
  QgsApplication app( argc, argv, false );
  app.setPrefixPath( qEnvironmentVariable( "QGIS_PREFIX_PATH", QStringLiteral( "/usr" ) ), true ); // distro install
  app.initQgis();
  QgsApplication::processingRegistry(); // ensure registry alive
  GDALAllRegister();
  TestEvolutionWorkflow tc;
  const int rc = QTest::qExec( &tc, argc, argv );
  QgsApplication::exitQgis();
  return rc;
}


