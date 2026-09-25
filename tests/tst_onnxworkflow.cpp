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
// on one temp dir; the toy graph (y = x + 40.0) is copied into the fixture's
// own model root so the test is hermetic.
class TestOnnxWorkflow : public QObject
{
  Q_OBJECT

private:
  // Declaration order matters: `dir` must precede `manifest` (its path feeds
  // the ctor), `projectSvc` precedes `layers`/`proc`, and `onnx` is rebound to
  // the fixture-local model root by initFixture.
  struct Fixture
  {
    QTemporaryDir dir;
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
    if ( !f.projectSvc.createProject( f.dir.filePath( QStringLiteral( "proj.qgz" ) ) ) )
      return QString();
    if ( !f.manifest.open() )
      return QString();

    const QString src = sourceModelDir();
    if ( src.isEmpty() )
      return QString();
    const QString modelDir = f.dir.filePath( QStringLiteral( "models" ) );
    if ( !QDir().mkpath( modelDir ) ||
         !QFile::copy( src + QStringLiteral( "/toy.onnx" ),
                       modelDir + QStringLiteral( "/toy.onnx" ) ) )
      return QString();
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
  static QString makeRaster( const QString &path, int w, int h, const QVector<float> &px )
  {
    GDALDriverH drv = GDALGetDriverByName( "GTiff" );
    GDALDatasetH ds = GDALCreate( drv, path.toUtf8().constData(), w, h, 1, GDT_Float32, nullptr );
    if ( !ds )
      return QString();
    const double gt[6] = { 0.0, 1.0, 0.0, static_cast<double>( h ), 0.0, -1.0 };
    GDALSetGeoTransform( ds, const_cast<double *>( gt ) );
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
    const QStringList ids = wf.availableAlgorithms();
    QVERIFY( ids.contains( QStringLiteral( "paleo:paleo_geological_smoothing" ) ) );
    QVERIFY2( ids.contains( QStringLiteral( "onnx:toy" ) ),
              qPrintable( ids.join( QLatin1Char( ',' ) ) ) );
    QVERIFY( !ids.contains( QStringLiteral( "onnx:" ) ) );
  }

  // onnx:toy end to end: load + infer + declare "pred.T1.onnx.toy".
  void onnxPredictionRunsAndDeclares()
  {
    if ( !PaleoOnnxService::runtimeAvailable() )
      QSKIP( "libonnxruntime not found under vendor/onnxruntime" );

    Fixture f;
    if ( initFixture( f ).isEmpty() )
      QSKIP( "spikes/onnx/toy.onnx not locatable" );

    PredictionWorkflow wf( &f.proc, &f.layers );
    wf.setOnnxService( &f.onnx );
    QSignalSpy doneSpy( &wf, &PredictionWorkflow::predictionDone );
    QSignalSpy failSpy( &wf, &PredictionWorkflow::predictionFailed );

    QVariantMap params;
    params.insert( QStringLiteral( "input" ), QVariantList{ 2.0 } );
    params.insert( QStringLiteral( "shape" ), QVariantList{ QVariant::fromValue<qint64>( 1 ) } );
    params.insert( QStringLiteral( "inputName" ), QStringLiteral( "x" ) );

    QString err;
    QVERIFY2( wf.runPrediction( QStringLiteral( "T1" ), QStringLiteral( "onnx:toy" ),
                                params, &err ), qPrintable( err ) );
    QCOMPARE( failSpy.count(), 0 );
    QCOMPARE( doneSpy.count(), 1 );
    QCOMPARE( doneSpy.at( 0 ).at( 0 ).toString(), QStringLiteral( "T1" ) );
    QCOMPARE( doneSpy.at( 0 ).at( 1 ).toString(), QStringLiteral( "pred.T1.onnx.toy" ) );

    const LayerDeclaration *d = findDecl( f.layers, QStringLiteral( "pred.T1.onnx.toy" ) );
    QVERIFY( d != nullptr );
    QCOMPARE( d->horizon, QStringLiteral( "T1" ) );
    QCOMPARE( d->type, QStringLiteral( "raster" ) );
    QCOMPARE( d->group, QStringLiteral( "03_Predict" ) );
    QVERIFY2( QFile::exists( d->source ), qPrintable( d->source ) );
    QVERIFY( !d->source.startsWith( QStringLiteral( "memory" ) ) );
    GDALDatasetH ds = GDALOpen( d->source.toUtf8().constData(), GA_ReadOnly );
    QVERIFY2( ds != nullptr, qPrintable( d->source ) );
    QCOMPARE( GDALGetRasterXSize( ds ), 1 );
    QCOMPARE( GDALGetRasterYSize( ds ), 1 );
    float px = 0.0f;
    QVERIFY( GDALRasterIO( GDALGetRasterBand( ds, 1 ), GF_Read, 0, 0, 1, 1, &px, 1, 1,
                           GDT_Float32, 0, 0 ) == CE_None );
    QCOMPARE( px, 42.0f );
    const char *modelItem = GDALGetMetadataItem( ds, "PALEO_MODEL", nullptr );
    QCOMPARE( QString::fromUtf8( modelItem ? modelItem : "" ), QStringLiteral( "toy" ) );
    GDALClose( ds );
    delete d;
  }

  // rows*cols that do not match the tensor length fail before a layer is declared.
  void onnxGridShapeMismatchFails()
  {
    if ( !PaleoOnnxService::runtimeAvailable() )
      QSKIP( "libonnxruntime not found under vendor/onnxruntime" );
    Fixture f;
    if ( initFixture( f ).isEmpty() )
      QSKIP( "spikes/onnx/toy.onnx not locatable" );

    PredictionWorkflow wf( &f.proc, &f.layers );
    wf.setOnnxService( &f.onnx );
    QVariantMap params;
    params.insert( QStringLiteral( "input" ), QVariantList{ 2.0 } );
    params.insert( QStringLiteral( "shape" ), QVariantList{ QVariant::fromValue<qint64>( 1 ) } );
    params.insert( QStringLiteral( "rows" ), 2 );
    params.insert( QStringLiteral( "cols" ), 2 );
    QString err;
    QVERIFY( !wf.runPrediction( QStringLiteral( "T1" ), QStringLiteral( "onnx:toy" ), params, &err ) );
    QVERIFY2( err.contains( QStringLiteral( "rows" ) ), qPrintable( err ) );
    QVERIFY( findDecl( f.layers, QStringLiteral( "pred.T1.onnx.toy" ) ) == nullptr );
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
    wf.setOnnxService( &f.onnx ); // bound, but must stay out of the way
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
