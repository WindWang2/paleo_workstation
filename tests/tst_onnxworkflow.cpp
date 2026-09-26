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

#include "../src/ai/onnxpredictionservice.h"
#include "../src/catalog/datacatalog.h"
#include "../src/metadata/layermanifest.h"
#include "../src/metadata/paleoprojectstore.h"
#include "../src/qgis/qgislayerservice.h"
#include "../src/qgis/qgisprocessingservice.h"
#include "../src/qgis/qgisprojectservice.h"
#include "../src/qgis/qgisruntime.h"
#include "../src/workflow/workflows.h"

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

  // Locates <repo>/spikes/onnx regardless of the directory the test binary is
  // built into (standalone /tmp builds cannot walk up to the repo, so the
  // working directory is the last resort — run from the repo root).
  static QString sourceModelDir()
  {
    const QString vendor = PaleoOnnxService::vendorRuntimeDir();
    if ( !vendor.isEmpty() )
    {
      QDir d( vendor );
      if ( d.cdUp() && d.cdUp() &&
           QFileInfo::exists( d.absoluteFilePath( QStringLiteral( "spikes/onnx/toy.onnx" ) ) ) )
        return d.absoluteFilePath( QStringLiteral( "spikes/onnx" ) );
    }
    QDir d( QCoreApplication::applicationDirPath() );
    for ( int i = 0; i < 10; ++i )
    {
      if ( QFileInfo::exists( d.absoluteFilePath( QStringLiteral( "spikes/onnx/toy.onnx" ) ) ) )
        return d.absoluteFilePath( QStringLiteral( "spikes/onnx" ) );
      if ( !d.cdUp() )
        break;
    }
    const QString cwd = QDir::current().absoluteFilePath( QStringLiteral( "spikes/onnx" ) );
    if ( QFileInfo::exists( cwd + QStringLiteral( "/toy.onnx" ) ) )
      return cwd;
    return QString();
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

  // D15（pass-2 批准）：端到端 fixture 推理冒烟——模型夹具来自 testdata/onnx
  // （工程内受管夹具，不再依赖 spikes/ 布局）。真 ORT 会话 → PredictionWorkflow
  // → 411×641 GeoTIFF；只证明差异化路径可用：断言尺寸、非空、有限值比例 100%，
  // 不做精度断言。产物按 T26 落 artifacts/derived。
  void onnxEndToEndFixtureSmokeFromTestdata()
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

    QVariantMap params;
    params.insert( QStringLiteral( "input" ), QVariantList{ 2.0f } );
    params.insert( QStringLiteral( "shape" ), QVariantList{ QVariant::fromValue<qint64>( 1 ) } );
    params.insert( QStringLiteral( "inputName" ), QStringLiteral( "x" ) );
    QString err;
    QVERIFY2( wf.runPrediction( QStringLiteral( "D61" ), QStringLiteral( "onnx:grid" ),
                                params, &err ),
              qPrintable( err ) );

    const LayerDeclaration *d = findDecl( f.layers, QStringLiteral( "pred.D61.onnx.grid" ) );
    QVERIFY2( d != nullptr, "prediction layer not declared" );
    QVERIFY( d->source.contains( QStringLiteral( "artifacts/derived/" ) ) );

    GDALDatasetH ds = GDALOpen( d->source.toUtf8().constData(), GA_ReadOnly );
    QVERIFY2( ds != nullptr, qPrintable( d->source ) );
    QCOMPARE( GDALGetRasterXSize( ds ), 641 );
    QCOMPARE( GDALGetRasterYSize( ds ), 411 );
    GDALRasterBandH band = GDALGetRasterBand( ds, 1 );
    QVector<float> px( 411 * 641 );
    QCOMPARE( GDALRasterIO( band, GF_Read, 0, 0, 641, 411, px.data(), 641, 411,
                            GDT_Float32, 0, 0 ),
              CE_None );
    int finite = 0;
    for ( float v : px )
      if ( std::isfinite( v ) )
        ++finite;
    // 非空 + 有限值比例：全格有限（toy 语义是常值广播，非有限即异常）。
    QVERIFY2( finite > 0, "raster is empty" );
    QCOMPARE( finite, 411 * 641 );
    GDALClose( ds );
    delete d;
#else
    QSKIP( "PROJECT_TESTDATA_DIR not defined" );
#endif
  }

  // onnx:grid end to end: 411×641 输出落在 D61 geotransform 上并登记
  // "pred.T1.onnx.grid"。标量输入 2.0 经 Expand+Add 广播出全部 42.0。
  void onnxPredictionRunsAndDeclares()
  {
    if ( !PaleoOnnxService::runtimeAvailable() )
      QSKIP( "libonnxruntime not found under vendor/onnxruntime" );

    Fixture f;
    if ( initFixture( f ).isEmpty() )
      QSKIP( "spikes/onnx models not locatable" );

    PredictionWorkflow wf( &f.proc, &f.layers );
    wf.setOnnxService( &f.onnx );
    wf.setCatalog( &f.catalog, f.dir.path() ); // T26
    QSignalSpy doneSpy( &wf, &PredictionWorkflow::predictionDone );
    QSignalSpy failSpy( &wf, &PredictionWorkflow::predictionFailed );

    QVariantMap params;
    params.insert( QStringLiteral( "input" ), QVariantList{ 2.0 } );
    params.insert( QStringLiteral( "shape" ), QVariantList{ QVariant::fromValue<qint64>( 1 ) } );
    params.insert( QStringLiteral( "inputName" ), QStringLiteral( "x" ) );

    QString err;
    QVERIFY2( wf.runPrediction( QStringLiteral( "T1" ), QStringLiteral( "onnx:grid" ),
                                params, &err ), qPrintable( err ) );
    QCOMPARE( failSpy.count(), 0 );
    QCOMPARE( doneSpy.count(), 1 );
    QCOMPARE( doneSpy.at( 0 ).at( 0 ).toString(), QStringLiteral( "T1" ) );
    QCOMPARE( doneSpy.at( 0 ).at( 1 ).toString(), QStringLiteral( "pred.T1.onnx.grid" ) );

    const LayerDeclaration *d = findDecl( f.layers, QStringLiteral( "pred.T1.onnx.grid" ) );
    QVERIFY( d != nullptr );
    QCOMPARE( d->horizon, QStringLiteral( "T1" ) );
    QCOMPARE( d->type, QStringLiteral( "raster" ) );
    QCOMPARE( d->group, QStringLiteral( "03_Predict" ) );
    QVERIFY2( QFile::exists( d->source ), qPrintable( d->source ) );
    QVERIFY( !d->source.startsWith( QStringLiteral( "memory" ) ) );
    // T26：预测栅格落 artifacts/derived 并登记 DERIVED 版本（不进 QDir::temp）。
    QVERIFY2( d->source.contains( QStringLiteral( "artifacts/derived/" ) ),
              qPrintable( d->source ) );
    bool onnxRegistered = false;
    for ( const CatalogAsset &a : f.catalog.assets() )
    {
      if ( a.type != QLatin1String( "onnx_prediction" ) )
        continue;
      for ( const CatalogVersion &v : f.catalog.versionsForAsset( a.id ) )
        if ( d->source.endsWith( QLatin1Char( '/' ) + v.fileName ) && d->source.contains( v.id ) )
          onnxRegistered = !v.sha256.isEmpty();
    }
    QVERIFY2( onnxRegistered, "ONNX raster has no DERIVED catalog version" );
    GDALDatasetH ds = GDALOpen( d->source.toUtf8().constData(), GA_ReadOnly );
    QVERIFY2( ds != nullptr, qPrintable( d->source ) );
    QCOMPARE( GDALGetRasterXSize( ds ), 641 );
    QCOMPARE( GDALGetRasterYSize( ds ), 411 );
    // 没有 horizon.D61* 声明时用 plan §3 常量：(0, 12793/640, 0, 16406, 0, -16406/410)
    double gt[6] = { 0, 0, 0, 0, 0, 0 };
    QCOMPARE( GDALGetGeoTransform( ds, gt ), CE_None );
    QVERIFY2( qAbs( gt[0] - 0.0 ) < 1e-9, qPrintable( QString::number( gt[0] ) ) );
    QVERIFY2( qAbs( gt[1] - 12793.0 / 640.0 ) < 1e-9, qPrintable( QString::number( gt[1] ) ) );
    QVERIFY2( qAbs( gt[2] - 0.0 ) < 1e-9, qPrintable( QString::number( gt[2] ) ) );
    QVERIFY2( qAbs( gt[3] - 16406.0 ) < 1e-9, qPrintable( QString::number( gt[3] ) ) );
    QVERIFY2( qAbs( gt[4] - 0.0 ) < 1e-9, qPrintable( QString::number( gt[4] ) ) );
    QVERIFY2( qAbs( gt[5] - ( -16406.0 / 410.0 ) ) < 1e-9, qPrintable( QString::number( gt[5] ) ) );
    float px = 0.0f;
    QVERIFY( GDALRasterIO( GDALGetRasterBand( ds, 1 ), GF_Read, 0, 0, 1, 1, &px, 1, 1,
                           GDT_Float32, 0, 0 ) == CE_None );
    QCOMPARE( px, 42.0f );
    const char *modelItem = GDALGetMetadataItem( ds, "PALEO_MODEL", nullptr );
    QCOMPARE( QString::fromUtf8( modelItem ? modelItem : "" ), QStringLiteral( "grid" ) );
    GDALClose( ds );
    delete d;
  }

  // 已声明的 horizon.D61* 栅格优先提供 geotransform（plan §3：ONNX 栅格用同
  // 一套 D61 geotransform）。声明的栅格本身的尺寸不影响 411×641 门禁。
  void onnxDeclaredD61GeotransformWins()
  {
    if ( !PaleoOnnxService::runtimeAvailable() )
      QSKIP( "libonnxruntime not found under vendor/onnxruntime" );

    Fixture f;
    if ( initFixture( f ).isEmpty() )
      QSKIP( "spikes/onnx models not locatable" );

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

    QVariantMap params;
    params.insert( QStringLiteral( "input" ), QVariantList{ 2.0 } );
    params.insert( QStringLiteral( "shape" ), QVariantList{ QVariant::fromValue<qint64>( 1 ) } );
    params.insert( QStringLiteral( "inputName" ), QStringLiteral( "x" ) );

    QVERIFY2( wf.runPrediction( QStringLiteral( "T1" ), QStringLiteral( "onnx:grid" ),
                                params, &err ), qPrintable( err ) );

    const LayerDeclaration *d = findDecl( f.layers, QStringLiteral( "pred.T1.onnx.grid" ) );
    QVERIFY( d != nullptr );
    GDALDatasetH ds = GDALOpen( d->source.toUtf8().constData(), GA_ReadOnly );
    QVERIFY2( ds != nullptr, qPrintable( d->source ) );
    QCOMPARE( GDALGetRasterXSize( ds ), 641 );
    QCOMPARE( GDALGetRasterYSize( ds ), 411 );
    double gt[6] = { 0, 0, 0, 0, 0, 0 };
    QCOMPARE( GDALGetGeoTransform( ds, gt ), CE_None );
    for ( int i = 0; i < 6; ++i )
      QVERIFY2( qAbs( gt[i] - d61gt[i] ) < 1e-9,
                qPrintable( QStringLiteral( "gt[%1]: %2 != %3" ).arg( i ).arg( gt[i] ).arg( d61gt[i] ) ) );
    GDALClose( ds );
    delete d;
  }

  // plan trap：标量/点输出不得落成 1×1 栅格——拒绝并写出钉死的文案与实际行列数。
  void onnxScalarOutputRefused()
  {
    if ( !PaleoOnnxService::runtimeAvailable() )
      QSKIP( "libonnxruntime not found under vendor/onnxruntime" );

    Fixture f;
    if ( initFixture( f ).isEmpty() )
      QSKIP( "spikes/onnx models not locatable" );

    PredictionWorkflow wf( &f.proc, &f.layers );
    wf.setOnnxService( &f.onnx );
    wf.setCatalog( &f.catalog, f.dir.path() ); // T26
    QSignalSpy doneSpy( &wf, &PredictionWorkflow::predictionDone );
    QSignalSpy failSpy( &wf, &PredictionWorkflow::predictionFailed );

    QVariantMap params;
    params.insert( QStringLiteral( "input" ), QVariantList{ 2.0 } );
    params.insert( QStringLiteral( "shape" ), QVariantList{ QVariant::fromValue<qint64>( 1 ) } );
    params.insert( QStringLiteral( "inputName" ), QStringLiteral( "x" ) );

    QString err;
    QVERIFY( !wf.runPrediction( QStringLiteral( "T1" ), QStringLiteral( "onnx:toy" ),
                                params, &err ) );
    QCOMPARE( err, QStringLiteral( "结果不是 411×641，没有写入栅格（实际 1×1）" ) );
    QCOMPARE( doneSpy.count(), 0 );
    QCOMPARE( failSpy.count(), 1 );
    QCOMPARE( failSpy.at( 0 ).at( 0 ).toString(), QStringLiteral( "T1" ) );
    QCOMPARE( failSpy.at( 0 ).at( 1 ).toString(), err );
    QVERIFY( findDecl( f.layers, QStringLiteral( "pred.T1.onnx.toy" ) ) == nullptr );
  }

  // 非 411×641 的二维输出同样拒绝（tile2x2 → 2×2）；params 里的 rows/cols
  // 不再能绕过门禁。
  void onnxNonGridShapeRefused()
  {
    if ( !PaleoOnnxService::runtimeAvailable() )
      QSKIP( "libonnxruntime not found under vendor/onnxruntime" );
    Fixture f;
    if ( initFixture( f ).isEmpty() )
      QSKIP( "spikes/onnx models not locatable" );

    PredictionWorkflow wf( &f.proc, &f.layers );
    wf.setOnnxService( &f.onnx );
    wf.setCatalog( &f.catalog, f.dir.path() ); // T26
    QVariantMap params;
    params.insert( QStringLiteral( "input" ), QVariantList{ 2.0 } );
    params.insert( QStringLiteral( "shape" ), QVariantList{ QVariant::fromValue<qint64>( 1 ) } );
    params.insert( QStringLiteral( "rows" ), 411 ); // 故意填门禁值也不行——形状说了算
    params.insert( QStringLiteral( "cols" ), 641 );
    QString err;
    QVERIFY( !wf.runPrediction( QStringLiteral( "T1" ), QStringLiteral( "onnx:tile2x2" ),
                              params, &err ) );
    QCOMPARE( err, QStringLiteral( "结果不是 411×641，没有写入栅格（实际 2×2）" ) );
    QVERIFY( findDecl( f.layers, QStringLiteral( "pred.T1.onnx.tile2x2" ) ) == nullptr );
  }

  // "onnx:ghost" — no such file under the model root → failed signal + false.
  void onnxMissingModelFails()
  {
    Fixture f;
    const QString modelDir = initFixture( f );
    if ( modelDir.isEmpty() )
      QSKIP( "spikes/onnx/toy.onnx not locatable" );

    PredictionWorkflow wf( &f.proc, &f.layers );
    wf.setOnnxService( &f.onnx );
    wf.setCatalog( &f.catalog, f.dir.path() ); // T26
    QSignalSpy doneSpy( &wf, &PredictionWorkflow::predictionDone );
    QSignalSpy failSpy( &wf, &PredictionWorkflow::predictionFailed );

    QString err;
    QVERIFY( !wf.runPrediction( QStringLiteral( "T1" ), QStringLiteral( "onnx:ghost" ),
                                QVariantMap(), &err ) );
    QVERIFY( !err.isEmpty() );
    QCOMPARE( doneSpy.count(), 0 );
    QCOMPARE( failSpy.count(), 1 );
    QCOMPARE( failSpy.at( 0 ).at( 0 ).toString(), QStringLiteral( "T1" ) );
    // Nothing declared for the failed run.
    QVERIFY( findDecl( f.layers, QStringLiteral( "pred.T1.onnx.ghost" ) ) == nullptr );
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
};

int main( int argc, char *argv[] )
{
  // QgisRuntime owns the QgsApplication lifecycle; initialize() must run
  // before any Q(Core)Application exists — it constructs one itself.
  if ( !QgisRuntime::isInitialized() )
    QgisRuntime::initialize( QStringLiteral( "/usr" ) );
  QgsApplication::processingRegistry(); // ensure registry alive
  GDALAllRegister();
  TestOnnxWorkflow tc;
  const int rc = QTest::qExec( &tc, argc, argv );
  QgisRuntime::shutdown();
  return rc;
}

#include "tst_onnxworkflow.moc"
