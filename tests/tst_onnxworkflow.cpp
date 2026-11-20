#include <QtTest>
#include <QCoreApplication>
#include <QDir>
#include <QFile>
#include <QFileInfo>
#include <QSignalSpy>
#include <QTemporaryDir>

#include <qgsapplication.h>
#include <qgsmaplayer.h>
#include <qgsproject.h>
#include <qgsrasterlayer.h>

#include <gdal.h>
#include <cpl_conv.h>

#include "../src/ai/onnxfixture.h"
#include "../src/ai/onnxpredictionservice.h"
#include "../src/catalog/datacatalog.h"
#include "../src/metadata/layermanifest.h"
#include "../src/metadata/paleoprojectstore.h"
#include "../src/qgis/qgislayerservice.h"
#include "../src/qgis/qgisprocessingservice.h"
#include "../src/qgis/qgisprojectservice.h"
#include "../src/qgis/qgisruntime.h"
#include "../src/workflow/workflows.h"
#include "../src/workflow/workflows_internal.h"

// ET4 integration: ONNX models surface in the prediction workflow as
// "onnx:<model>" algorithm ids next to the registry's "paleo:*" set, and a
// run declares a "pred.<horizon>.onnx.<model>" layer through the same
// QgisLayerService::declare path Processing results use. Real service stack
// on one temp dir; the spike graphs (y = x + 40.0 as scalar / 411×641 / 2×2)
// are copied into the fixture's own model root so the test is hermetic.
//
// PROJECT_AREA_PLAN trap gate: a squeezed output that is not the D61 work-area
// grid (411×641) is refused — nothing written, nothing declared — with the
// pinned text 「结果不是 411×641，没有写入栅格」 plus the actual dims. Passing
// rasters carry the declared horizon.D61* geotransform, else the plan
// constants (0, 12793/640, 0, 16406, 0, -16406/410).
class TestOnnxWorkflow : public QObject
{
  Q_OBJECT

private:
  // Declaration order matters: `dir` must precede `manifest` (its path feeds
  // the ctor), `projectSvc` precedes `layers`/`proc`, and `onnx` is rebound to
  // the fixture-local model root by initFixture. T26：catalog 进 fixture——
  // ONNX 预测栅格落 artifacts/derived 并登记 DERIVED 版本。
  struct Fixture
  {
    QTemporaryDir dir;
    DataCatalog catalog;
    QgisProjectService projectSvc;
    PaleoProjectStore store;
    LayerManifest manifest{ dir.filePath( QStringLiteral( "project.sqlite" ) ) };
    QgisLayerService layers{ &projectSvc, &manifest };
    QgisProcessingService proc{ &store };
    PaleoOnnxService onnx;
  };

  // Resolve source fixtures independently of the shared runtime and build directory.
  static QString sourceModelDir()
  {
    const QString model = QFINDTESTDATA( "../spikes/onnx/toy.onnx" );
    return model.isEmpty() ? QString() : QFileInfo( model ).absolutePath();
  }

  // Returns "" when the spike model cannot be located (callers QSKIP).
  QString initFixture( Fixture &f )
  {
    if ( !f.dir.isValid() )
      return QString();
    if ( !f.catalog.open( f.dir.path() ) )
      return QString();
    if ( !f.projectSvc.createProject( f.dir.filePath( QStringLiteral( "proj.qgz" ) ) ) )
      return QString();
    if ( !f.manifest.open() )
      return QString();

    const QString src = sourceModelDir();
    if ( src.isEmpty() )
      return QString();
    const QString modelDir = f.dir.filePath( QStringLiteral( "models" ) );
    if ( !QDir().mkpath( modelDir ) )
      return QString();
    for ( const QString &name : { QStringLiteral( "toy.onnx" ),
                                  QStringLiteral( "grid.onnx" ),
                                  QStringLiteral( "tile2x2.onnx" ) } )
    {
      if ( !QFile::exists( src + QLatin1Char( '/' ) + name ) ||
           !QFile::copy( src + QLatin1Char( '/' ) + name,
                         modelDir + QLatin1Char( '/' ) + name ) )
        return QString();
    }
    f.onnx.setModelRoot( modelDir );
    return modelDir;
  }

  static const LayerDeclaration *findDecl( QgisLayerService &layers, const QString &layerId )
  {
    const QVector<LayerDeclaration> decls = layers.declared();
    for ( const LayerDeclaration &d : decls )
      if ( d.layerId == layerId )
        return new LayerDeclaration( d );
    return nullptr;
  }

  // Write a w x h Float32 GTiff; returns "" on failure (tst_workflows pattern).
  // gt6 overrides the default north-up 1m geotransform when given.
  static QString makeRaster( const QString &path, int w, int h, const QVector<float> &px,
                             const double *gt6 = nullptr )
  {
    GDALDriverH drv = GDALGetDriverByName( "GTiff" );
    GDALDatasetH ds = GDALCreate( drv, path.toUtf8().constData(), w, h, 1, GDT_Float32, nullptr );
    if ( !ds )
      return QString();
    const double fallback[6] = { 0.0, 1.0, 0.0, static_cast<double>( h ), 0.0, -1.0 };
    GDALSetGeoTransform( ds, const_cast<double *>( gt6 ? gt6 : fallback ) );
    GDALRasterBandH band = GDALGetRasterBand( ds, 1 );
    GDALSetRasterNoDataValue( band, -9999.0 );
    const CPLErr err = GDALRasterIO( band, GF_Write, 0, 0, w, h, const_cast<float *>( px.constData() ),
                                     w, h, GDT_Float32, 0, 0 );
    GDALClose( ds );
    return err == CE_None ? path : QString();
  }

  static LayerDeclaration decl( const QString &layerId, const QString &horizon,
                                const QString &type, const QString &source )
  {
    LayerDeclaration d;
    d.layerId = layerId;
    d.horizon = horizon;
    d.type = type;
    d.source = source;
    return d;
  }

private slots:

  void initTestCase()
  {
    QVERIFY( QgisRuntime::isInitialized() );
    // Fixture::proc registers PaleoProvider with the real algorithms.
    Fixture f;
    QVERIFY( f.dir.isValid() );
    QVERIFY( f.projectSvc.createProject( f.dir.filePath( QStringLiteral( "proj.qgz" ) ) ) );
    QVERIFY( f.proc.paleoAlgorithmIds().contains( QStringLiteral( "paleo:paleo_geological_smoothing" ) ) );
  }

  void cleanup()
  {
    // Same safety net as tst_workflows: drop singleton-project leftovers.
    QgsProject::instance()->removeAllMapLayers();
  }

  // Registry ids plus one "onnx:<model>" per file under the model root.
  void onnxModelsListedAsAlgorithms()
  {
    Fixture f;
    const QString modelDir = initFixture( f );
    if ( modelDir.isEmpty() )
      QSKIP( "spikes/onnx/toy.onnx not locatable" );

    PredictionWorkflow wf( &f.proc, &f.layers );

    // Unbound: only the Processing registry ids.
    const QStringList registryOnly = wf.availableAlgorithms();
    QVERIFY( registryOnly.contains( QStringLiteral( "paleo:paleo_geological_smoothing" ) ) );
    QVERIFY( !registryOnly.contains( QStringLiteral( "onnx:toy" ) ) );

    wf.setOnnxService( &f.onnx );
    wf.setCatalog( &f.catalog, f.dir.path() ); // T26
    const QStringList ids = wf.availableAlgorithms();
    QVERIFY( ids.contains( QStringLiteral( "paleo:paleo_geological_smoothing" ) ) );
    QVERIFY2( ids.contains( QStringLiteral( "onnx:toy" ) ),
              qPrintable( ids.join( QLatin1Char( ',' ) ) ) );
    QVERIFY( !ids.contains( QStringLiteral( "onnx:" ) ) );
  }

  // D15（pass-2 批准）→ P4-W2 合同更新：正式 ONNX 预测必须冻结分析单元。
  // 无单元调用（含 testdata/onnx/grid.onnx 夹具路径）在 prepare 前置拒绝，
  // 不贴工区网格、不声明图层、不登记版本。真单元端到端冒烟移至
  // tst_p4_onnx_prediction（真实方案 + 地震体 RMS 输入）；张量数值验证按原
  // 合同留在 tst_onnx/tst_onnxsessions。
  void onnxNoUnitPredictionFromTestdataRejectedAtPrepare()
  {
    if ( !PaleoOnnxService::runtimeAvailable() )
      QSKIP( "libonnxruntime not found under vendor/onnxruntime" );
#ifdef PROJECT_TESTDATA_DIR
    const QString fixtureModel = QStringLiteral( PROJECT_TESTDATA_DIR ) +
                                 QStringLiteral( "/onnx/grid.onnx" );
    if ( !QFile::exists( fixtureModel ) )
      QSKIP( "testdata/onnx/grid.onnx not present" );

    Fixture f;
    if ( !f.dir.isValid() || !f.catalog.open( f.dir.path() ) ||
         !f.projectSvc.createProject( f.dir.filePath( QStringLiteral( "proj.qgz" ) ) ) ||
         !f.manifest.open() )
      QSKIP( "fixture setup failed" );
    const QString modelDir = f.dir.filePath( QStringLiteral( "models" ) );
    QVERIFY( QDir().mkpath( modelDir ) );
    QVERIFY( QFile::copy( fixtureModel, modelDir + QLatin1Char( '/' ) +
                                            QStringLiteral( "grid.onnx" ) ) );
    f.onnx.setModelRoot( modelDir );

    PredictionWorkflow wf( &f.proc, &f.layers );
    wf.setOnnxService( &f.onnx );
    wf.setCatalog( &f.catalog, f.dir.path() );
    QSignalSpy doneSpy( &wf, &PredictionWorkflow::predictionDone );
    QSignalSpy failSpy( &wf, &PredictionWorkflow::predictionFailed );

    QVariantMap params;
    params.insert( QStringLiteral( "input" ), QVariantList{ 2.0f } );
    params.insert( QStringLiteral( "shape" ), QVariantList{ QVariant::fromValue<qint64>( 1 ) } );
    params.insert( QStringLiteral( "inputName" ), QStringLiteral( "x" ) );
    QString err;
    QVERIFY( !wf.runPrediction( QStringLiteral( "D61" ), QStringLiteral( "onnx:grid" ),
                                params, &err ) );
    QVERIFY2( err.contains( QStringLiteral( "冻结分析单元" ) ), qPrintable( err ) );
    QCOMPARE( doneSpy.count(), 0 );
    QCOMPARE( failSpy.count(), 1 );
    QVERIFY( findDecl( f.layers, QStringLiteral( "pred.D61.onnx.grid" ) ) == nullptr );
#else
    QSKIP( "PROJECT_TESTDATA_DIR not defined" );
#endif
  }

  // P4-W2 合同更新：无单元的张量调用（spikes toy/grid/tile2x2 全族）不经
  // 正式发布入口产出「某层预测」；已声明的 horizon.D61* 栅格也不能改变准入。
  // （旧 onnxPredictionRunsAndDeclares / onnxDeclaredD61GeotransformWins /
  // onnxScalarOutputRefused / onnxNonGridShapeRefused / onnxMissingModelFails /
  // onnxMultiClassOutputArgmaxGrid / predictionEvidenceContainsModelSha256…
  // 的无单元发布路径删除；单元路径的形状/血缘/provenance 断言在
  // tst_p4_onnx_prediction，缺模型分类在 tst_onnxsessions。）
  void onnxNoUnitTensorPublishRejected()
  {
    if ( !PaleoOnnxService::runtimeAvailable() )
      QSKIP( "libonnxruntime not found under vendor/onnxruntime" );

    Fixture f;
    if ( initFixture( f ).isEmpty() )
      QSKIP( "spikes/onnx models not locatable" );

    // 已声明的 D61 栅格存在也不构成单元身份。
    const double d61gt[6] = { 123.0, 2.5, 0.0, 4567.0, 0.0, -2.5 };
    const QString d61Path = makeRaster( f.dir.filePath( QStringLiteral( "d61.tif" ) ), 4, 3,
                                        QVector<float>( 12, 7.0f ), d61gt );
    QVERIFY( !d61Path.isEmpty() );
    QString err;
    QVERIFY2( f.layers.declare( decl( QStringLiteral( "horizon.D61" ), QStringLiteral( "D61" ),
                                      QStringLiteral( "raster" ), d61Path ), &err ),
              qPrintable( err ) );

    PredictionWorkflow wf( &f.proc, &f.layers );
    wf.setOnnxService( &f.onnx );
    wf.setCatalog( &f.catalog, f.dir.path() ); // T26
    QSignalSpy doneSpy( &wf, &PredictionWorkflow::predictionDone );
    QSignalSpy failSpy( &wf, &PredictionWorkflow::predictionFailed );

    for ( const QString &model : { QStringLiteral( "toy" ), QStringLiteral( "grid" ),
                                   QStringLiteral( "tile2x2" ), QStringLiteral( "ghost" ) } )
    {
      QVariantMap params;
      params.insert( QStringLiteral( "input" ), QVariantList{ 2.0 } );
      params.insert( QStringLiteral( "shape" ), QVariantList{ QVariant::fromValue<qint64>( 1 ) } );
      params.insert( QStringLiteral( "inputName" ), QStringLiteral( "x" ) );
      params.insert( QStringLiteral( "rows" ), 411 ); // 旧门禁参数不能再绕过准入
      params.insert( QStringLiteral( "cols" ), 641 );
      QString runErr;
      QVERIFY( !wf.runPrediction( QStringLiteral( "T1" ),
                                  QStringLiteral( "onnx:%1" ).arg( model ),
                                  params, &runErr ) );
      QVERIFY2( runErr.contains( QStringLiteral( "冻结分析单元" ) ), qPrintable( runErr ) );
      QVERIFY( findDecl( f.layers,
                         QStringLiteral( "pred.T1.onnx.%1" ).arg( model ) ) == nullptr );
    }
    QCOMPARE( doneSpy.count(), 0 );
    QCOMPARE( failSpy.count(), 4 );
  }

  // P0-3 → P4-W2：无单元 prepare 前置失败（失败先于任何模型文件访问，GUI
  // 线程不加载模型）；有单元时的加载延迟由
  // tst_p4_onnx_prediction::unitOnnxPredictionEndToEndWithPreciseLineage 断言
  //（prepare 后 job.modelSha256 为空）。
  void preparePredictionJobRejectsNoUnitBeforeModelAccess()
  {
    Fixture f;
    const QString modelDir = initFixture( f );
    if ( modelDir.isEmpty() )
      QSKIP( "spikes/onnx/toy.onnx not locatable" );

    PredictionWorkflow wf( &f.proc, &f.layers );
    wf.setOnnxService( &f.onnx );
    wf.setCatalog( &f.catalog, f.dir.path() );

    PredictionWorkflow::PredictionJob job;
    QString err;
    const bool prepOk = wf.preparePredictionJob( QStringLiteral( "T1" ),
                                                 QStringLiteral( "onnx:non_existent_ghost" ),
                                                 {}, &job, &err );
    QVERIFY( !prepOk );
    QVERIFY2( err.contains( QStringLiteral( "冻结分析单元" ) ), qPrintable( err ) );
    QVERIFY( !job.prepared );
    QVERIFY( !job.staging.isValid() );
    QVERIFY( job.modelSha256.isEmpty() ); // 模型从未被加载/哈希
  }

  // The "onnx:" dispatch must not intercept normal registry algorithm ids.
  void processingBranchUnaffected()
  {
    Fixture f;
    const QString modelDir = initFixture( f );
    if ( modelDir.isEmpty() )
      QSKIP( "spikes/onnx/toy.onnx not locatable" );

    const QVector<float> px = { 1, 1, 1,
                                1, 2, 1,
                                1, 1, 1 };
    const QString inPath = makeRaster( f.dir.filePath( QStringLiteral( "coded_in.tif" ) ), 3, 3, px );
    QVERIFY( !inPath.isEmpty() );

    QString err;
    QVERIFY2( f.layers.declare( decl( QStringLiteral( "input.T1" ), QStringLiteral( "T1" ),
                                    QStringLiteral( "raster" ), inPath ), &err ), qPrintable( err ) );
    QgsMapLayer *input = f.layers.instantiate( QStringLiteral( "input.T1" ), &err );
    QVERIFY2( input != nullptr, qPrintable( err ) );

    PredictionWorkflow wf( &f.proc, &f.layers );
    wf.setOnnxService( &f.onnx );
    wf.setCatalog( &f.catalog, f.dir.path() ); // T26 // bound, but must stay out of the way
    QSignalSpy doneSpy( &wf, &PredictionWorkflow::predictionDone );
    QSignalSpy failSpy( &wf, &PredictionWorkflow::predictionFailed );

    QVariantMap params;
    params.insert( QStringLiteral( "INPUT" ), QVariant::fromValue( input ) );
    params.insert( QStringLiteral( "PASSES" ), 1 );

    QVERIFY2( wf.runPrediction( QStringLiteral( "T1" ),
                                QStringLiteral( "paleo:paleo_geological_smoothing" ),
                                params, &err ), qPrintable( err ) );
    QCOMPARE( failSpy.count(), 0 );
    QCOMPARE( doneSpy.count(), 1 );
    const QString resultId = doneSpy.at( 0 ).at( 1 ).toString();
    QVERIFY2( resultId.startsWith( QStringLiteral( "predict.T1." ) ), qPrintable( resultId ) );
    QVERIFY( !resultId.contains( QStringLiteral( "onnx" ) ) );

    // Unknown registry id still fails through the Processing path.
    QVERIFY( !wf.runPrediction( QStringLiteral( "T2" ),
                                QStringLiteral( "paleo:does_not_exist" ),
                                QVariantMap(), &err ) );
    QVERIFY( !err.isEmpty() );
    QCOMPARE( doneSpy.count(), 1 );
    QCOMPARE( failSpy.count(), 1 );
  }

  void onnxAreaGridRespectsRequestedHorizon()
  {
    Fixture f;
    if ( !f.dir.isValid() || !f.projectSvc.createProject( f.dir.filePath( QStringLiteral( "proj.qgz" ) ) ) ||
         !f.manifest.open() )
      QSKIP( "fixture setup failed" );
    const QString d53Path = f.dir.filePath( QStringLiteral( "d53.tif" ) );
    GDALAllRegister();
    auto *driver = GDALGetDriverByName( "GTiff" );
    GDALDatasetH ds = GDALCreate( driver, d53Path.toUtf8().constData(), 10, 10, 1, GDT_Float32, nullptr );
    QVERIFY( ds );
    double customGt[6] = { 5000.0, 20.0, 0.0, 8000.0, 0.0, -20.0 };
    GDALSetGeoTransform( ds, customGt );
    GDALClose( ds );

    LayerDeclaration decl;
    decl.layerId = QStringLiteral( "horizon.D53.time" );
    decl.horizon = QStringLiteral( "D53" );
    decl.type = QStringLiteral( "raster" );
    decl.source = d53Path;
    decl.group = QStringLiteral( "00_Data" );
    decl.title = QStringLiteral( "D53 Time" );
    QVERIFY( f.layers.declare( decl ) );

    const auto grid = paleo::workflow_detail::onnxAreaGrid( &f.layers, QStringLiteral( "D53" ) );
    QVERIFY( grid.fromDecl );
    QCOMPARE( grid.gt[0], 5000.0 );
    QCOMPARE( grid.gt[3], 8000.0 );
  }
};

int main( int argc, char *argv[] )
{
  // QgisRuntime owns the QgsApplication lifecycle; initialize() must run
  // before any Q(Core)Application exists — it constructs one itself.
  if ( !QgisRuntime::isInitialized() )
    QgisRuntime::initialize( QStringLiteral( "/usr" ) );
  QgsApplication::processingRegistry(); // ensure registry alive
  GDALAllRegister();
  // 同上：ctest 无控制台，__fastfail 会吞掉缓冲输出；强制结果落盘。
  const QByteArray logPath = QByteArray( QT_TESTCASE_BUILDDIR ) + "/tst_onnxworkflow-result.txt";
  QList<QByteArray> forwarded;
  forwarded << QByteArray( argv[0] );
  bool hasFileLogger = false;
  for ( int i = 1; i < argc; ++i )
  {
    forwarded << QByteArray( argv[i] );
    // 调用方已自带文件日志时不重复追加，避免两个 -o 的歧义。
    if ( QByteArray( argv[i] ) == "-o" || QByteArray( argv[i] ) == "-output" )
      hasFileLogger = true;
  }
  if ( !hasFileLogger )
    forwarded << QByteArray( "-o" ) << logPath + ",txt";
  QList<char *> cargv;
  cargv.reserve( forwarded.size() );
  for ( QByteArray &a : forwarded )
    cargv << a.data();
  TestOnnxWorkflow tc;
  const int rc = QTest::qExec( &tc, cargv.size(), cargv.data() );
  QgisRuntime::shutdown();
  return rc;
}

#include "tst_onnxworkflow.moc"
