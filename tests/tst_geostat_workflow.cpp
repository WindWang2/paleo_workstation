// 层：测试壳（真实栈：临时 catalog/manifest/QGIS 引导，被测对象为克里金/SGS 编排链）
#include <QtTest>
#include <QDir>
#include <QFile>
#include <QFileInfo>
#include <QSignalSpy>
#include <QTemporaryDir>

#include <qgsapplication.h>
#include <qgsmaplayer.h>
#include <qgsproject.h>

#include <QJsonDocument>
#include <QJsonObject>

#include <gdal.h>
#include <ogr_api.h>
#include <ogr_srs_api.h>

#include "helpers/workflowfixture.h"
#include "../src/catalog/datacatalog.h"
#include "../src/metadata/layermanifest.h"
#include "../src/metadata/paleoprojectstore.h"
#include "../src/qgis/qgislayerservice.h"
#include "../src/qgis/qgisprocessingservice.h"
#include "../src/qgis/qgisprojectservice.h"
#include "../src/services/singlefactordef.h"
#include "../src/workflow/workflows.h"
#include "../src/io/constraintstore.h"
#include <QJsonArray>

// 方向18 oracle 5/6/7（编排面）：
// - kriging/sgs 三段式跑通：DERIVED 登记 + factor 声明 + 产物存在；
// - 择优诚实面：样本不足阈值 → extra["method_actual"]="idw"；不超阈 → "kriging"；
// - 参数经 QC/参数指纹/extra 往返（读回断言）；
// - 取消诚实：compute 取消即失败、无输出。

using paleo::tests::initFixture;

class GeostatWorkflowTests : public QObject
{
  Q_OBJECT

  private:
    using Fixture = paleo::tests::WorkflowFixture;

    // 井点 GeoJSON：n×n 规则井网 + z 值（对角梯度），供克里金全链。
    static bool writeWells( const QString &path, int side )
    {
      QFile f( path );
      if ( !f.open( QIODevice::WriteOnly ) )
        return false;
      QString features;
      for ( int row = 0; row < side; ++row )
        for ( int column = 0; column < side; ++column )
      {
        const double x = 10.0 * column;
        const double y = 10.0 * row;
        const double z = std::sin( 0.35 * column ) + 0.25 * row + 12.0;
        if ( !features.isEmpty() )
          features += QLatin1Char( ',' );
        features += QStringLiteral( "{\"type\":\"Feature\",\"properties\":{\"z\":%1},"
                                    "\"geometry\":{\"type\":\"Point\",\"coordinates\":[%2,%3]}}" )
                        .arg( z )
                        .arg( x )
                        .arg( y );
      }
      f.write( QStringLiteral( "{\"type\":\"FeatureCollection\",\"features\":[%1]}" ).arg( features ).toUtf8() );
      f.close();
      return QFile::exists( path ) && QFileInfo( path ).size() > 0;
    }

    static bool setupWells( Fixture &f, int side, QString *err )
    {
      const QString ptsPath = f.dir.filePath( QStringLiteral( "wells.geojson" ) );
      if ( !writeWells( ptsPath, side ) )
        return false;
      LayerDeclaration d;
      d.layerId = QStringLiteral( "wells.T1" );
      d.horizon = QStringLiteral( "T1" );
      d.type = QStringLiteral( "vector" );
      d.source = ptsPath;
      return f.layers.declare( d, err );
    }

    // 从 catalog 找本测试登记的 single_factor_raster 版本（按 algorithm_id 定位，
    // fixture 各用例独立，id 唯一）。
    static QVariantMap findExtraByAlgorithm( DataCatalog &catalog, const QString &algorithmId )
    {
      for ( const CatalogAsset &asset : catalog.assets() )
      {
        if ( asset.type != QStringLiteral( "single_factor_raster" ) )
          continue;
        for ( const CatalogVersion &version : catalog.versionsForAsset( asset.id ) )
        {
          if ( version.extra.value( QStringLiteral( "algorithm_id" ) ).toString() == algorithmId )
            return version.extra;
        }
      }
      return QVariantMap();
    }

    static bool hasDeclaredLayer( QgisLayerService &layers, const QString &layerId )
    {
      for ( const LayerDeclaration &d : layers.declared() )
        if ( d.layerId == layerId )
          return true;
      return false;
    }

  private slots:
    void initTestCase()
    {
      QVERIFY( QgsApplication::instance() != nullptr );
      Fixture f;
      QVERIFY( initFixture( f ) );
      QVERIFY( f.proc.algorithmIds().contains( QStringLiteral( "paleo:paleo_constraint_idw" ) ) );
    }

    void cleanup()
    {
      QgsProject::instance()->removeAllMapLayers();
    }

    // Oracle 6 前半 + Oracle 7：克里金全链 + DERIVED + method_actual="kriging"
    void krigingFullChainRegistersDerived();
    // Oracle 5：样本不足阈值 → 自动降级 IDW + method_actual="idw"
    void insufficientSamplesDegradeToIdwHonestly();
    // Oracle 6 后半：SGS 全链 + realizations/seed 入 extra + QC 参数往返
    void sgsFullChainAndParamRoundTrip();
    // 取消诚实：compute 取消 → 失败无输出，publish 拒绝
    void cancelledComputeIsHonest();
    // 方向67 Oracle 5：策略包 id 进血缘；未知 id 拒绝不回退
    void strategyIdLandsInLineage();
    void krigingQcLineageEndToEnd();
    void idwLineageCarriesReceipts();
    void searchRadiusIsHonoured();
    void cokrigingRhoProvenance();
    void explicitVariogramClampProvenance();
    void unknownStrategyIdRejected();
};

void GeostatWorkflowTests::krigingFullChainRegistersDerived()
{
  Fixture f;
  QVERIFY( initFixture( f ) );
  QString err;
  QVERIFY2( setupWells( f, 5, &err ), qPrintable( err ) ); // 25 口井 ≥ 阈值 8

  ConstraintWorkflow wf( &f.proc, &f.layers );
  wf.setCatalog( &f.catalog, f.projectDir() );
  QSignalSpy generated( &wf, &ConstraintWorkflow::factorGenerated );

  QVariantMap params;
  params.insert( QStringLiteral( "field" ), QStringLiteral( "z" ) );
  params.insert( QStringLiteral( "cellSize" ), 2.0 );
  params.insert( QStringLiteral( "method" ), QStringLiteral( "kriging" ) );
  params.insert( QStringLiteral( "variogramModel" ), QStringLiteral( "spherical" ) );
  params.insert( QStringLiteral( "azimuth" ), 0.0 ); // 走向南北 → 各向异性拟合路径
  QVERIFY2( wf.generateFactor( QStringLiteral( "T1" ), QStringLiteral( "sandthick" ), params, &err ),
            qPrintable( err ) );
  QCOMPARE( generated.count(), 1 );

  // DERIVED 版本登记 + factor 声明
  QVERIFY2( hasDeclaredLayer( f.layers, QStringLiteral( "factor.T1.sandthick" ) ),
            "factor layer must be declared" );
  const QVariantMap extra = findExtraByAlgorithm( f.catalog, QStringLiteral( "paleo:geostat_kriging" ) );
  QVERIFY2( !extra.isEmpty(), "derived single_factor_raster version must be registered" );
  QCOMPARE( extra.value( QStringLiteral( "method_actual" ) ).toString(), QStringLiteral( "kriging" ) );
  QCOMPARE( extra.value( QStringLiteral( "method" ) ).toString(), QStringLiteral( "kriging" ) );
  QCOMPARE( extra.value( QStringLiteral( "algorithm_id" ) ).toString(), QStringLiteral( "paleo:geostat_kriging" ) );
  QVERIFY( extra.contains( QStringLiteral( "parameter_hash" ) ) );
  QVERIFY( extra.contains( QStringLiteral( "qc_sha256" ) ) );
  QVERIFY( extra.contains( QStringLiteral( "support_sha256" ) ) ); // 估计方差场旁路
  const QVariantMap variogram = extra.value( QStringLiteral( "variogram" ) ).toMap();
  QVERIFY( variogram.value( QStringLiteral( "range" ) ).toDouble() > 0 );
  QVERIFY( variogram.value( QStringLiteral( "sill" ) ).toDouble() > 0 );
  QVERIFY( variogram.value( QStringLiteral( "anisotropy_ratio" ) ).toDouble() >= 1.0 );
  QVERIFY( extra.value( QStringLiteral( "finite_cells" ) ).toInt() > 0 );

  // QC json 落盘且参数节点完整（参数往返的持久化面）
  const QString qcRel = extra.value( QStringLiteral( "qc_path" ) ).toString();
  QFile qcFile( QDir( f.projectDir() ).filePath( qcRel ) );
  QVERIFY2( qcFile.open( QIODevice::ReadOnly ), qPrintable( qcFile.fileName() ) );
  const QJsonObject qc = QJsonDocument::fromJson( qcFile.readAll() ).object();
  QCOMPARE( qc.value( QStringLiteral( "method_actual" ) ).toString(), QStringLiteral( "kriging" ) );
  const QJsonObject parameters = qc.value( QStringLiteral( "parameters" ) ).toObject();
  QCOMPARE( parameters.value( QStringLiteral( "method" ) ).toString(), QStringLiteral( "kriging" ) );
  QCOMPARE( parameters.value( QStringLiteral( "azimuth" ) ).toDouble(), 0.0 );
  QCOMPARE( parameters.value( QStringLiteral( "cell_size" ) ).toDouble(), 2.0 );
}

void GeostatWorkflowTests::insufficientSamplesDegradeToIdwHonestly()
{
  Fixture f;
  QVERIFY( initFixture( f ) );
  QString err;
  QVERIFY2( setupWells( f, 2, &err ), qPrintable( err ) ); // 4 口井 < 阈值 8

  ConstraintWorkflow wf( &f.proc, &f.layers );
  wf.setCatalog( &f.catalog, f.projectDir() );
  QVariantMap params;
  params.insert( QStringLiteral( "field" ), QStringLiteral( "z" ) );
  params.insert( QStringLiteral( "cellSize" ), 2.0 );
  params.insert( QStringLiteral( "method" ), QStringLiteral( "kriging" ) );
  QVERIFY2( wf.generateFactor( QStringLiteral( "T1" ), QStringLiteral( "sandthick" ), params, &err ),
            qPrintable( err ) );
  const QVariantMap extra = findExtraByAlgorithm( f.catalog, QStringLiteral( "paleo:paleo_constraint_idw" ) );
  QVERIFY( !extra.isEmpty() );
  // 原罪条款：UI 选了克里金，产物必须如实说这次跑的是 idw
  QCOMPARE( extra.value( QStringLiteral( "method_actual" ) ).toString(), QStringLiteral( "idw" ) );
  QCOMPARE( extra.value( QStringLiteral( "algorithm_id" ) ).toString(), QStringLiteral( "paleo:paleo_constraint_idw" ) );
  QVERIFY( !extra.contains( QStringLiteral( "support_sha256" ) ) ); // IDW 无方差旁路
}

void GeostatWorkflowTests::sgsFullChainAndParamRoundTrip()
{
  Fixture f;
  QVERIFY( initFixture( f ) );
  QString err;
  QVERIFY2( setupWells( f, 5, &err ), qPrintable( err ) );

  ConstraintWorkflow wf( &f.proc, &f.layers );
  wf.setCatalog( &f.catalog, f.projectDir() );
  QVariantMap params;
  params.insert( QStringLiteral( "field" ), QStringLiteral( "z" ) );
  params.insert( QStringLiteral( "cellSize" ), 2.5 );
  params.insert( QStringLiteral( "method" ), QStringLiteral( "sgs" ) );
  params.insert( QStringLiteral( "variogramModel" ), QStringLiteral( "gaussian" ) );
  params.insert( QStringLiteral( "realizations" ), 2 );
  params.insert( QStringLiteral( "seed" ), 7 );
  QVERIFY2( wf.generateFactor( QStringLiteral( "T1" ), QStringLiteral( "poro" ), params, &err ),
            qPrintable( err ) );
  const QVariantMap extra = findExtraByAlgorithm( f.catalog, QStringLiteral( "paleo:geostat_sgs" ) );
  QVERIFY( !extra.isEmpty() );
  QCOMPARE( extra.value( QStringLiteral( "method_actual" ) ).toString(), QStringLiteral( "sgs" ) );
  QCOMPARE( extra.value( QStringLiteral( "algorithm_id" ) ).toString(), QStringLiteral( "paleo:geostat_sgs" ) );
  const QVariantMap sgs = extra.value( QStringLiteral( "sgs" ) ).toMap();
  QCOMPARE( sgs.value( QStringLiteral( "realizations" ) ).toInt(), 2 );
  QCOMPARE( sgs.value( QStringLiteral( "seed" ) ).toLongLong(), qint64( 7 ) );
  QVERIFY( sgs.value( QStringLiteral( "sample_std" ) ).toDouble() > 0 );

  // 参数往返：QC parameters 与 extra.sgs 与请求参数一致
  const QString qcRel = extra.value( QStringLiteral( "qc_path" ) ).toString();
  QFile qcFile( QDir( f.projectDir() ).filePath( qcRel ) );
  QVERIFY( qcFile.open( QIODevice::ReadOnly ) );
  const QJsonObject qc = QJsonDocument::fromJson( qcFile.readAll() ).object();
  const QJsonObject parameters = qc.value( QStringLiteral( "parameters" ) ).toObject();
  QCOMPARE( parameters.value( QStringLiteral( "variogramModel" ) ).toString(), QStringLiteral( "gaussian" ) );
  QCOMPARE( parameters.value( QStringLiteral( "seed" ) ).toInt(), 7 );
  QCOMPARE( parameters.value( QStringLiteral( "realizations" ) ).toInt(), 2 );
}

void GeostatWorkflowTests::cancelledComputeIsHonest()
{
  Fixture f;
  QVERIFY( initFixture( f ) );
  QString err;
  QVERIFY2( setupWells( f, 5, &err ), qPrintable( err ) );

  ConstraintWorkflow wf( &f.proc, &f.layers );
  wf.setCatalog( &f.catalog, f.projectDir() );
  ConstraintWorkflow::GeostatJob job;
  QVariantMap params;
  params.insert( QStringLiteral( "field" ), QStringLiteral( "z" ) );
  params.insert( QStringLiteral( "cellSize" ), 0.5 );
  params.insert( QStringLiteral( "method" ), QStringLiteral( "kriging" ) );
  QVERIFY( wf.prepareGeostatJob( QStringLiteral( "T1" ), QStringLiteral( "sandthick" ),
                                 QStringLiteral( "kriging" ), params, &job, &err ) );
  QVERIFY( !wf.computeGeostatJob( &job, [] { return true; } ) );
  QVERIFY( !job.error.isEmpty() );
  QVERIFY( job.outputPath.isEmpty() ); // 半成品不外泄
  QVERIFY( !wf.publishGeostatJob( job, &err ) );
  QVERIFY( !err.isEmpty() );
}

// 方向67 Oracle 5：所选策略包 id 进血缘 extra（词表 id 原样落 strategy_id）。
void GeostatWorkflowTests::strategyIdLandsInLineage()
{
  Fixture f;
  QVERIFY( initFixture( f ) );
  QString err;
  QVERIFY2( setupWells( f, 5, &err ), qPrintable( err ) );

  ConstraintWorkflow wf( &f.proc, &f.layers );
  wf.setCatalog( &f.catalog, f.projectDir() );
  QVariantMap params;
  params.insert( QStringLiteral( "field" ), QStringLiteral( "z" ) );
  params.insert( QStringLiteral( "cellSize" ), 2.0 );
  params.insert( QStringLiteral( "method" ), QStringLiteral( "kriging" ) );
  params.insert( QStringLiteral( "variogramModel" ), QStringLiteral( "spherical" ) );
  params.insert( QStringLiteral( "strategy_id" ), QStringLiteral( "kriging" ) ); // 词表 id
  QVERIFY2( wf.generateFactor( QStringLiteral( "T1" ), QStringLiteral( "sandthick" ), params, &err ),
            qPrintable( err ) );
  const QVariantMap extra = findExtraByAlgorithm( f.catalog, QStringLiteral( "paleo:geostat_kriging" ) );
  QVERIFY( !extra.isEmpty() );
  QCOMPARE( extra.value( QStringLiteral( "strategy_id" ) ).toString(), QStringLiteral( "kriging" ) );
}

// 方向67 Oracle 5：未知策略 id 在分派入口拒绝（词表外不回退，不冒名落血缘）。
void GeostatWorkflowTests::unknownStrategyIdRejected()
{
  Fixture f;
  QVERIFY( initFixture( f ) );
  QString err;
  QVERIFY2( setupWells( f, 5, &err ), qPrintable( err ) );

  ConstraintWorkflow wf( &f.proc, &f.layers );
  wf.setCatalog( &f.catalog, f.projectDir() );
  QVariantMap params;
  params.insert( QStringLiteral( "field" ), QStringLiteral( "z" ) );
  params.insert( QStringLiteral( "cellSize" ), 2.0 );
  params.insert( QStringLiteral( "method" ), QStringLiteral( "kriging" ) );
  params.insert( QStringLiteral( "strategy_id" ), QStringLiteral( "no_such_strategy" ) );
  QVERIFY( !wf.generateFactor( QStringLiteral( "T1" ), QStringLiteral( "sandthick" ), params, &err ) );
  QVERIFY2( err.contains( QStringLiteral( "未知制图策略" ) ), qPrintable( err ) );
  // 没有任何 single_factor_raster 版本被登记（拒绝发生在分派前）。
  QVERIFY( findExtraByAlgorithm( f.catalog, QStringLiteral( "paleo:geostat_kriging" ) ).isEmpty() );
}

int main( int argc, char *argv[] )
{
  QgsApplication app( argc, argv, false );
  app.setPrefixPath( qEnvironmentVariable( "QGIS_PREFIX_PATH", QStringLiteral( "/usr" ) ), true );
  app.initQgis();
  QgsApplication::processingRegistry();
  GDALAllRegister();
  GeostatWorkflowTests tc;
  const int rc = QTest::qExec( &tc, argc, argv );
  QgsApplication::exitQgis();
  return rc;
}


// P2：地统 QC/血缘写入面端到端——约束未用、被忽略输入、模型来源、LOO 交叉验证
// 同时落进 qc.json 与 catalog extra，且两边一致。
static QJsonObject readQc( const QString &projectDir, const QVariantMap &extra )
{
  QFile file( QDir( projectDir ).filePath( extra.value( QStringLiteral( "qc_path" ) ).toString() ) );
  if ( !file.open( QIODevice::ReadOnly ) )
    return {};
  return QJsonDocument::fromJson( file.readAll() ).object();
}

static QStringList ignoredKeys( const QVariantMap &extra )
{
  QStringList keys;
  for ( const QVariant &item : extra.value( QStringLiteral( "ignored_inputs" ) ).toList() )
    keys << item.toMap().value( QStringLiteral( "key" ) ).toString();
  return keys;
}

void GeostatWorkflowTests::krigingQcLineageEndToEnd()
{
  Fixture f;
  QVERIFY( initFixture( f ) );
  QString err;
  QVERIFY2( setupWells( f, 5, &err ), qPrintable( err ) );
  ConstraintStore constraints( f.dir.filePath( QStringLiteral( "constraints.gpkg" ) ) );
  QVERIFY2( constraints.appendExtended( QStringLiteral( "T1" ), QStringLiteral( "wall" ), QStringLiteral( "LINESTRING(20 -5, 20 45)" ),
                                        QStringLiteral( "break_line" ), 0,
                                        QStringLiteral( R"({"semantic":"hard_barrier","blockMode":"full_block"})" ), 1, &err ),
            qPrintable( err ) );
  QVERIFY2( constraints.appendExtended( QStringLiteral( "T1" ), QStringLiteral( "guide" ), QStringLiteral( "LINESTRING(0 0, 40 40)" ),
                                        QStringLiteral( "direction_line" ), 0,
                                        QStringLiteral( R"({"semantic":"direction_guide","ratio":4})" ), 1, &err ),
            qPrintable( err ) );
  ConstraintWorkflow wf( &f.proc, &f.layers );
  wf.setCatalog( &f.catalog, f.projectDir() );
  wf.setConstraintStore( &constraints );

  QVariantMap params;
  params.insert( QStringLiteral( "field" ), QStringLiteral( "z" ) );
  params.insert( QStringLiteral( "cellSize" ), 2.0 );
  params.insert( QStringLiteral( "method" ), QStringLiteral( "kriging" ) );
  params.insert( QStringLiteral( "variogramModel" ), QStringLiteral( "spherical" ) );
  // 页面会一并发送的量：自动拟合下块金/变程比不被读取，IDW 幂次与 SGS 参数也不被读取。
  params.insert( QStringLiteral( "nugget" ), 0.3 );
  params.insert( QStringLiteral( "sill" ), 0.0 );
  params.insert( QStringLiteral( "range" ), 0.0 );
  params.insert( QStringLiteral( "anisotropyRatio" ), 3.0 );
  params.insert( QStringLiteral( "power" ), 2.0 );
  params.insert( QStringLiteral( "minPoints" ), 3 );
  params.insert( QStringLiteral( "searchRadius" ), 0.0 );
  params.insert( QStringLiteral( "realizations" ), 4 );
  QVERIFY2( wf.generateFactor( QStringLiteral( "T1" ), QStringLiteral( "sandthick" ), params, &err ), qPrintable( err ) );
  const QVariantMap extra = findExtraByAlgorithm( f.catalog, QStringLiteral( "paleo:geostat_kriging" ) );
  QVERIFY( !extra.isEmpty() );
  const QJsonObject qc = readQc( f.projectDir(), extra );
  QVERIFY( !qc.isEmpty() );

  // 约束：克里金不读约束 → 逐条未用、零消费，QC 与血缘一致。
  QVERIFY( extra.value( QStringLiteral( "constraints_consumed" ) ).toList().isEmpty() );
  QStringList unused;
  for ( const QVariant &item : extra.value( QStringLiteral( "constraints_unused" ) ).toList() )
    unused << item.toMap().value( QStringLiteral( "id" ) ).toString();
  unused.sort();
  QCOMPARE( unused, QStringList( { QStringLiteral( "guide" ), QStringLiteral( "wall" ) } ) );
  QCOMPARE( qc.value( QStringLiteral( "constraints_unused" ) ).toArray().size(), 2 );
  QVERIFY( !extra.value( QStringLiteral( "constraint_capability" ) ).toString().isEmpty() );

  // 被忽略输入逐条留因。
  const QStringList ignored = ignoredKeys( extra );
  for ( const char *key : { "nugget", "sill", "range", "anisotropyRatio", "power", "minPoints", "realizations" } )
    QVERIFY2( ignored.contains( QString::fromLatin1( key ) ), key );
  QCOMPARE( qc.value( QStringLiteral( "ignored_inputs" ) ).toArray().size(), ignored.size() );
  QCOMPARE( extra.value( QStringLiteral( "model_provenance" ) ).toMap().value( QStringLiteral( "variogram_source" ) ).toString(),
            QStringLiteral( "auto_fit_omnidirectional" ) );

  // LOO 交叉验证：每口井都估到，误差有限；平滑场上克里金应优于样本均值。
  const QVariantMap cv = extra.value( QStringLiteral( "cross_validation" ) ).toMap();
  QCOMPARE( cv.value( QStringLiteral( "status" ) ).toString(), QStringLiteral( "computed" ) );
  QCOMPARE( cv.value( QStringLiteral( "estimator" ) ).toString(), QStringLiteral( "ordinary_kriging" ) );
  QCOMPARE( cv.value( QStringLiteral( "attempted" ) ).toInt(), 25 );
  QCOMPARE( cv.value( QStringLiteral( "estimated" ) ).toInt(), 25 );
  double mean = 0, var = 0;
  for ( int row = 0; row < 5; ++row )
    for ( int column = 0; column < 5; ++column )
      mean += std::sin( 0.35 * column ) + 0.25 * row + 12.0;
  mean /= 25;
  for ( int row = 0; row < 5; ++row )
    for ( int column = 0; column < 5; ++column )
      var += std::pow( std::sin( 0.35 * column ) + 0.25 * row + 12.0 - mean, 2 );
  const double sampleStd = std::sqrt( var / 25 );
  const double rmse = cv.value( QStringLiteral( "rmse" ) ).toDouble();
  QVERIFY( std::isfinite( rmse ) && rmse > 0 );
  QVERIFY2( rmse < sampleStd, qPrintable( QStringLiteral( "rmse %1 vs std %2" ).arg( rmse ).arg( sampleStd ) ) );
  QVERIFY( cv.value( QStringLiteral( "mean_standardized_squared_error" ) ).toDouble() > 0 );
  QCOMPARE( qc.value( QStringLiteral( "cross_validation" ) ).toObject().value( QStringLiteral( "rmse" ) ).toDouble(), rmse );
}

void GeostatWorkflowTests::searchRadiusIsHonoured()
{
  Fixture f;
  QVERIFY( initFixture( f ) );
  QString err;
  QVERIFY2( setupWells( f, 5, &err ), qPrintable( err ) );
  ConstraintWorkflow wf( &f.proc, &f.layers );
  wf.setCatalog( &f.catalog, f.projectDir() );
  QVariantMap params;
  params.insert( QStringLiteral( "field" ), QStringLiteral( "z" ) );
  params.insert( QStringLiteral( "cellSize" ), 2.0 );
  params.insert( QStringLiteral( "method" ), QStringLiteral( "kriging" ) );
  QVERIFY2( wf.generateFactor( QStringLiteral( "T1" ), QStringLiteral( "sandthick" ), params, &err ), qPrintable( err ) );
  const int baseline = findExtraByAlgorithm( f.catalog, QStringLiteral( "paleo:geostat_kriging" ) )
                           .value( QStringLiteral( "finite_cells" ) ).toInt();
  QVERIFY( baseline > 0 );
  // 井距 10：半径 6 + 至少 4 点 → 只有少数格能凑够邻域，其余格如实 nodata。
  params.insert( QStringLiteral( "searchRadius" ), 6.0 );
  params.insert( QStringLiteral( "minPoints" ), 4 );
  QVERIFY2( wf.generateFactor( QStringLiteral( "T1" ), QStringLiteral( "poro" ), params, &err ), qPrintable( err ) );
  QVariantMap radiusExtra;
  for ( const CatalogAsset &asset : f.catalog.assets() )
    for ( const CatalogVersion &version : f.catalog.versionsForAsset( asset.id ) )
      if ( version.extra.value( QStringLiteral( "factor_id" ) ).toString() == QStringLiteral( "poro" ) )
        radiusExtra = version.extra;
  QVERIFY( !radiusExtra.isEmpty() );
  const QVariantMap search = radiusExtra.value( QStringLiteral( "search" ) ).toMap();
  QCOMPARE( search.value( QStringLiteral( "search_radius" ) ).toDouble(), 6.0 );
  QCOMPARE( search.value( QStringLiteral( "min_points" ) ).toInt(), 4 );
  QVERIFY( !ignoredKeys( radiusExtra ).contains( QStringLiteral( "minPoints" ) ) );
  QVERIFY2( radiusExtra.value( QStringLiteral( "finite_cells" ) ).toInt() < baseline,
            "搜索半径必须真正收窄邻域（此前被静默忽略）" );
}

void GeostatWorkflowTests::cokrigingRhoProvenance()
{
  Fixture f;
  QVERIFY( initFixture( f ) );
  QString err;
  QVERIFY2( setupWells( f, 5, &err ), qPrintable( err ) );
  const QString rasterPath = f.dir.filePath( QStringLiteral( "cov.tif" ) );
  {
    GDALDatasetH ds = GDALCreate( GDALGetDriverByName( "GTiff" ), rasterPath.toUtf8().constData(), 60, 60, 1, GDT_Float32, nullptr );
    QVERIFY( ds );
    double gt[6] = { -10.0, 1.0, 0.0, 50.0, 0.0, -1.0 };
    GDALSetGeoTransform( ds, gt );
    QVector<float> px( 60 * 60 );
    for ( int r = 0; r < 60; ++r )
      for ( int c = 0; c < 60; ++c )
        px[r * 60 + c] = static_cast<float>( 0.4 * c - 0.2 * r + 3.0 );
    QCOMPARE( GDALRasterIO( GDALGetRasterBand( ds, 1 ), GF_Write, 0, 0, 60, 60, px.data(), 60, 60, GDT_Float32, 0, 0 ), CE_None );
    GDALClose( ds );
  }
  LayerDeclaration d;
  d.layerId = QStringLiteral( "seismic.cov" );
  d.horizon = QStringLiteral( "T1" );
  d.type = QStringLiteral( "raster" );
  d.source = rasterPath;
  QVERIFY2( f.layers.declare( d, &err ), qPrintable( err ) );
  ConstraintWorkflow wf( &f.proc, &f.layers );
  wf.setCatalog( &f.catalog, f.projectDir() );

  const auto run = [&]( const QString &factorId, const QVariant &rho ) -> QVariantMap {
    QVariantMap params;
    params.insert( QStringLiteral( "field" ), QStringLiteral( "z" ) );
    params.insert( QStringLiteral( "cellSize" ), 2.0 );
    params.insert( QStringLiteral( "method" ), QStringLiteral( "cokriging" ) );
    params.insert( QStringLiteral( "covariateLayerId" ), QStringLiteral( "seismic.cov" ) );
    if ( rho.isValid() )
      params.insert( QStringLiteral( "crossCorrelation" ), rho );
    QString error;
    if ( !wf.generateFactor( QStringLiteral( "T1" ), factorId, params, &error ) )
      return { { QStringLiteral( "error" ), error } };
    for ( const CatalogAsset &asset : f.catalog.assets() )
      for ( const CatalogVersion &version : f.catalog.versionsForAsset( asset.id ) )
        if ( version.extra.value( QStringLiteral( "factor_id" ) ).toString() == factorId )
          return version.extra;
    return {};
  };
  const QVariantMap zero = run( QStringLiteral( "sandthick" ), 0.0 );
  QVERIFY2( !zero.contains( QStringLiteral( "error" ) ), qPrintable( zero.value( QStringLiteral( "error" ) ).toString() ) );
  const QVariantMap zeroNode = zero.value( QStringLiteral( "cokriging" ) ).toMap();
  QCOMPARE( zeroNode.value( QStringLiteral( "rho_source" ) ).toString(), QStringLiteral( "explicit" ) );
  QCOMPARE( zeroNode.value( QStringLiteral( "covariate_effective" ) ).toBool(), false );
  QCOMPARE( zero.value( QStringLiteral( "cross_validation" ) ).toMap().value( QStringLiteral( "estimator" ) ).toString(),
            QStringLiteral( "ordinary_cokriging" ) );

  const QVariantMap clamped = run( QStringLiteral( "poro" ), 1.5 );
  QVERIFY( !clamped.contains( QStringLiteral( "error" ) ) );
  const QVariantMap clampedNode = clamped.value( QStringLiteral( "cokriging" ) ).toMap();
  QCOMPARE( clampedNode.value( QStringLiteral( "rho_clamped" ) ).toBool(), true );
  QCOMPARE( clampedNode.value( QStringLiteral( "rho_effective" ) ).toDouble(), 0.99 );

  const QVariantMap estimated = run( QStringLiteral( "sandratio" ), QVariant() );
  QVERIFY2( !estimated.contains( QStringLiteral( "error" ) ), qPrintable( estimated.value( QStringLiteral( "error" ) ).toString() ) );
  const QVariantMap estimatedNode = estimated.value( QStringLiteral( "cokriging" ) ).toMap();
  QCOMPARE( estimatedNode.value( QStringLiteral( "rho_source" ) ).toString(), QStringLiteral( "estimated_colocated" ) );
  QCOMPARE( estimatedNode.value( QStringLiteral( "colocated_samples" ) ).toInt(), 25 );
  QCOMPARE( estimatedNode.value( QStringLiteral( "covariate_effective" ) ).toBool(), true );
  QVERIFY( !ignoredKeys( estimated ).contains( QStringLiteral( "covariateLayerId" ) ) );
}


// P2：IDW 单因素结果血缘——约束逐条消费/未用 + 逐井参与/跳过（任务D）。
void GeostatWorkflowTests::idwLineageCarriesReceipts()
{
  Fixture f;
  QVERIFY( initFixture( f ) );
  QString err;
  // 井点与约束 store 同处工程局部坐标（DataCatalog::localGridCrsWkt）：IDW 不跨 CRS 混算。
  const QString ptsPath = f.dir.filePath( QStringLiteral( "wells-idw.gpkg" ) );
  {
    GDALDatasetH ds = GDALCreate( GDALGetDriverByName( "GPKG" ), ptsPath.toUtf8().constData(), 0, 0, 0, GDT_Unknown, nullptr );
    QVERIFY( ds );
    OGRSpatialReferenceH srs = OSRNewSpatialReference( DataCatalog::localGridCrsWkt().toUtf8().constData() );
    OGRLayerH layer = GDALDatasetCreateLayer( ds, "wells", srs, wkbPoint, nullptr );
    OSRDestroySpatialReference( srs );
    QVERIFY( layer );
    OGRFieldDefnH idField = OGR_Fld_Create( "well_id", OFTString );
    OGRFieldDefnH zField = OGR_Fld_Create( "z", OFTReal );
    QCOMPARE( OGR_L_CreateField( layer, idField, TRUE ), OGRERR_NONE );
    QCOMPARE( OGR_L_CreateField( layer, zField, TRUE ), OGRERR_NONE );
    OGR_Fld_Destroy( idField );
    OGR_Fld_Destroy( zField );
    int k = 0;
    for ( int row = 0; row < 3; ++row )
      for ( int column = 0; column < 3; ++column, ++k )
      {
        OGRFeatureH feature = OGR_F_Create( OGR_L_GetLayerDefn( layer ) );
        OGR_F_SetFieldString( feature, 0, QStringLiteral( "W%1" ).arg( k ).toUtf8().constData() );
        if ( k == 4 )
          OGR_F_SetFieldNull( feature, 1 );
        else
          OGR_F_SetFieldDouble( feature, 1, 10 + row + 0.5 * column );
        OGRGeometryH point = OGR_G_CreateGeometry( wkbPoint );
        OGR_G_SetPoint_2D( point, 0, 10.0 * column, 10.0 * row );
        OGR_F_SetGeometryDirectly( feature, point );
        QCOMPARE( OGR_L_CreateFeature( layer, feature ), OGRERR_NONE );
        OGR_F_Destroy( feature );
      }
    GDALClose( ds );
  }
  LayerDeclaration d;
  d.layerId = QStringLiteral( "wells.T1" );
  d.horizon = QStringLiteral( "T1" );
  d.type = QStringLiteral( "vector" );
  d.source = ptsPath;
  QVERIFY2( f.layers.declare( d, &err ), qPrintable( err ) );
  ConstraintStore constraints( f.dir.filePath( QStringLiteral( "constraints.gpkg" ) ) );
  QVERIFY2( constraints.appendExtended( QStringLiteral( "T1" ), QStringLiteral( "wall" ), QStringLiteral( "LINESTRING(15 -5, 15 25)" ),
                                        QStringLiteral( "break_line" ), 0,
                                        QStringLiteral( R"({"semantic":"hard_barrier","blockMode":"full_block"})" ), 1, &err ),
            qPrintable( err ) );
  QVERIFY2( constraints.appendExtended( QStringLiteral( "T1" ), QStringLiteral( "shown" ), QStringLiteral( "LINESTRING(5 -5, 5 25)" ),
                                        QStringLiteral( "break_line" ), 0,
                                        QStringLiteral( R"({"semantic":"hard_barrier","blockMode":"display_only"})" ), 1, &err ),
            qPrintable( err ) );
  ConstraintWorkflow wf( &f.proc, &f.layers );
  wf.setCatalog( &f.catalog, f.projectDir() );
  wf.setConstraintStore( &constraints );
  wf.loadConstraints( QStringLiteral( "T1" ) );
  QVariantMap params;
  params.insert( QStringLiteral( "field" ), QStringLiteral( "z" ) );
  params.insert( QStringLiteral( "cellSize" ), 1.0 );
  QVERIFY2( wf.generateFactor( QStringLiteral( "T1" ), QStringLiteral( "sandthick" ), params, &err ), qPrintable( err ) );
  QVariantMap extra;
  for ( const CatalogAsset &asset : f.catalog.assets() )
    for ( const CatalogVersion &version : f.catalog.versionsForAsset( asset.id ) )
      if ( version.extra.value( QStringLiteral( "factor_id" ) ).toString() == QStringLiteral( "sandthick" ) )
        extra = version.extra;
  QVERIFY( !extra.isEmpty() );
  QMap<QString, QString> consumed, unused;
  for ( const QVariant &item : extra.value( QStringLiteral( "constraints_consumed" ) ).toList() )
    consumed.insert( item.toMap().value( QStringLiteral( "id" ) ).toString(), item.toMap().value( QStringLiteral( "role" ) ).toString() );
  for ( const QVariant &item : extra.value( QStringLiteral( "constraints_unused" ) ).toList() )
    unused.insert( item.toMap().value( QStringLiteral( "id" ) ).toString(), item.toMap().value( QStringLiteral( "reason" ) ).toString() );
  QCOMPARE( consumed.value( QStringLiteral( "wall" ) ), QStringLiteral( "hard_barrier" ) );
  QVERIFY( !consumed.contains( QStringLiteral( "shown" ) ) );
  QVERIFY( unused.contains( QStringLiteral( "shown" ) ) );
  QCOMPARE( extra.value( QStringLiteral( "engine_samples_used" ) ).toList().size(), 8 );
  const QVariantList skipped = extra.value( QStringLiteral( "engine_samples_skipped" ) ).toList();
  QCOMPARE( skipped.size(), 1 );
  QCOMPARE( skipped.first().toMap().value( QStringLiteral( "well_id" ) ).toString(), QStringLiteral( "W4" ) );
  QCOMPARE( skipped.first().toMap().value( QStringLiteral( "reason" ) ).toString(), QStringLiteral( "field_missing_or_non_finite" ) );
}

// U4b：显式变差参数（range/sill>0）下负块金→0、各向异性比<1→1 是静默钳制——
// model_provenance 必须留痕（nugget_clamped/nugget_effective、
// anisotropy_clamped/anisotropy_effective，仅发生钳制时插入；正常值无新键）。
void GeostatWorkflowTests::explicitVariogramClampProvenance()
{
  Fixture f;
  QVERIFY( initFixture( f ) );
  QString err;
  QVERIFY2( setupWells( f, 5, &err ), qPrintable( err ) ); // 25 口 ≥ 阈值 8
  ConstraintWorkflow wf( &f.proc, &f.layers );
  wf.setCatalog( &f.catalog, f.projectDir() );

  const auto extraOfFactor = [&f]( const QString &factorId ) {
    QVariantMap found;
    for ( const CatalogAsset &asset : f.catalog.assets() )
      for ( const CatalogVersion &version : f.catalog.versionsForAsset( asset.id ) )
        if ( version.extra.value( QStringLiteral( "factor_id" ) ).toString() == factorId )
          found = version.extra;
    return found;
  };

  const auto run = [&]( const QString &factorId, double nugget, double anisotropy ) {
    QVariantMap params;
    params.insert( QStringLiteral( "field" ), QStringLiteral( "z" ) );
    params.insert( QStringLiteral( "cellSize" ), 2.0 );
    params.insert( QStringLiteral( "method" ), QStringLiteral( "kriging" ) );
    params.insert( QStringLiteral( "nugget" ), nugget );
    params.insert( QStringLiteral( "sill" ), 1.0 );
    params.insert( QStringLiteral( "range" ), 40.0 ); // 显式 range/sill → 显式模型分支
    params.insert( QStringLiteral( "anisotropyRatio" ), anisotropy );
    QString error;
    if ( !wf.generateFactor( QStringLiteral( "T1" ), factorId, params, &error ) )
      return QVariantMap{ { QStringLiteral( "error" ), error } };
    return extraOfFactor( factorId );
  };

  const QVariantMap clamped = run( QStringLiteral( "sandthick" ), -0.5, 0.5 );
  QVERIFY2( !clamped.contains( QStringLiteral( "error" ) ),
            qPrintable( clamped.value( QStringLiteral( "error" ) ).toString() ) );
  QVERIFY( !clamped.isEmpty() );
  const QVariantMap provenance = clamped.value( QStringLiteral( "model_provenance" ) ).toMap();
  QCOMPARE( provenance.value( QStringLiteral( "variogram_source" ) ).toString(),
            QStringLiteral( "explicit" ) );
  QCOMPARE( provenance.value( QStringLiteral( "nugget_clamped" ) ).toBool(), true );
  QCOMPARE( provenance.value( QStringLiteral( "nugget_effective" ) ).toDouble(), 0.0 );
  QCOMPARE( provenance.value( QStringLiteral( "anisotropy_clamped" ) ).toBool(), true );
  QCOMPARE( provenance.value( QStringLiteral( "anisotropy_effective" ) ).toDouble(), 1.0 );
  // QC json 与血缘同源（model_provenance 整节点复制）。
  const QJsonObject qc = readQc( f.projectDir(), clamped );
  const QJsonObject qcProvenance = qc.value( QStringLiteral( "model_provenance" ) ).toObject();
  QCOMPARE( qcProvenance.value( QStringLiteral( "nugget_clamped" ) ).toBool(), true );
  QCOMPARE( qcProvenance.value( QStringLiteral( "anisotropy_effective" ) ).toDouble(), 1.0 );

  // 回归：正常值（nugget>=0、ratio>=1）不新增 clamped 键。
  const QVariantMap honest = run( QStringLiteral( "poro" ), 0.2, 2.0 );
  QVERIFY2( !honest.contains( QStringLiteral( "error" ) ),
            qPrintable( honest.value( QStringLiteral( "error" ) ).toString() ) );
  const QVariantMap cleanProvenance = honest.value( QStringLiteral( "model_provenance" ) ).toMap();
  QCOMPARE( cleanProvenance.value( QStringLiteral( "variogram_source" ) ).toString(),
            QStringLiteral( "explicit" ) );
  QVERIFY( !cleanProvenance.contains( QStringLiteral( "nugget_clamped" ) ) );
  QVERIFY( !cleanProvenance.contains( QStringLiteral( "anisotropy_clamped" ) ) );
}

#include "tst_geostat_workflow.moc"
