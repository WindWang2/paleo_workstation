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

#include "helpers/workflowfixture.h"
#include "../src/catalog/datacatalog.h"
#include "../src/metadata/layermanifest.h"
#include "../src/metadata/paleoprojectstore.h"
#include "../src/qgis/qgislayerservice.h"
#include "../src/qgis/qgisprocessingservice.h"
#include "../src/qgis/qgisprojectservice.h"
#include "../src/services/singlefactordef.h"
#include "../src/workflow/workflows.h"

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

#include "tst_geostat_workflow.moc"
