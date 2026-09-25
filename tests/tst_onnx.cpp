#include <QtTest>
#include <QCoreApplication>
#include <QDir>
#include <QFileInfo>
#include <QSignalSpy>

#include "../src/ai/onnxpredictionservice.h"

// ET4 spike -> service: PaleoOnnxService wraps the vendored ONNX Runtime
// (vendor/onnxruntime, official 1.30.0 linux-x64). The toy graph is
// y = x + 40.0 over float32[1] — ort_check proved 2.0 -> 42.0 in-process.
class TestOnnx : public QObject
{
  Q_OBJECT
private slots:
  void runtimeAvailableInVendorTree()
  {
    const QString dir = PaleoOnnxService::vendorRuntimeDir();
    QVERIFY2( !dir.isEmpty(), "vendorRuntimeDir returned empty" );
    QVERIFY2( QDir( dir ).exists(), qPrintable( dir ) );
    QVERIFY2( PaleoOnnxService::runtimeAvailable(),
              qPrintable( QStringLiteral( "no libonnxruntime under %1" ).arg( dir ) ) );
  }

  void listsModelsUnderModelRoot()
  {
    PaleoOnnxService svc;
    svc.setModelRoot( spikesDir() );
    QVERIFY2( svc.availableModels().contains( QStringLiteral( "toy" ) ),
              qPrintable( svc.availableModels().join( QLatin1Char( ',' ) ) ) );
  }

  void loadToyModelAndInfer()
  {
    PaleoOnnxService svc;
    svc.setModelRoot( spikesDir() );

    QString err;
    QVERIFY2( svc.loadModel( QStringLiteral( "toy" ), &err ), qPrintable( err ) );
    QCOMPARE( svc.loadedModel(), QStringLiteral( "toy" ) );

    const QVector<float> out = svc.run( QStringLiteral( "x" ), { 2.0f }, { 1 }, &err );
    QVERIFY2( err.isEmpty(), qPrintable( err ) );
    QCOMPARE( out.size(), qsizetype( 1 ) );
    QVERIFY2( qAbs( out[0] - 42.0f ) < 1e-5f,
              qPrintable( QString::number( static_cast<double>( out[0] ) ) ) );
  }

  void loadEmitsModelLoaded()
  {
    PaleoOnnxService svc;
    svc.setModelRoot( spikesDir() );
    QSignalSpy spy( &svc, &PaleoOnnxService::modelLoaded );
    QVERIFY( svc.loadModel( QStringLiteral( "toy" ) ) );
    QCOMPARE( spy.size(), 1 );
    QCOMPARE( spy.at( 0 ).at( 0 ).toString(), QStringLiteral( "toy" ) );
  }

  void loadMissingModelFails()
  {
    PaleoOnnxService svc;
    svc.setModelRoot( spikesDir() );
    QString err;
    QVERIFY( !svc.loadModel( QStringLiteral( "missing" ), &err ) );
    QVERIFY2( !err.isEmpty(), "error string must describe the failure" );
    QVERIFY( svc.loadedModel().isEmpty() );
  }

  void runWithoutSessionFails()
  {
    PaleoOnnxService svc;
    QString err;
    const QVector<float> out = svc.run( QStringLiteral( "x" ), { 2.0f }, { 1 }, &err );
    QVERIFY( out.isEmpty() );
    QVERIFY2( !err.isEmpty(), "error must be set when no model is loaded" );
  }

  void failedLoadKeepsPreviousSession()
  {
    // A failed load must not clobber a working session (all-or-nothing).
    PaleoOnnxService svc;
    svc.setModelRoot( spikesDir() );
    QVERIFY( svc.loadModel( QStringLiteral( "toy" ) ) );
    QString err;
    QVERIFY( !svc.loadModel( QStringLiteral( "missing" ), &err ) );
    QCOMPARE( svc.loadedModel(), QStringLiteral( "toy" ) );
    err.clear();
    const QVector<float> out = svc.run( QStringLiteral( "x" ), { 2.0f }, { 1 }, &err );
    QVERIFY2( err.isEmpty(), qPrintable( err ) );
    QCOMPARE( out.size(), qsizetype( 1 ) );
    QVERIFY( qAbs( out[0] - 42.0f ) < 1e-5f );
  }

private:
  // <repo>/spikes/onnx — resolved via vendorRuntimeDir() (=<repo>/vendor/onnxruntime)
  // so the test passes regardless of the build dir it runs from.
  QString spikesDir() const
  {
    const QString vendor = PaleoOnnxService::vendorRuntimeDir();
    if ( !vendor.isEmpty() )
    {
      QDir d( vendor );
      if ( d.cdUp() && d.cdUp() &&
           QFileInfo::exists( d.absoluteFilePath( QStringLiteral( "spikes/onnx/toy.onnx" ) ) ) )
        return d.absoluteFilePath( QStringLiteral( "spikes/onnx" ) );
    }
    // Fallback: walk up from the binary looking for the spike model.
    QDir d( QCoreApplication::applicationDirPath() );
    for ( int i = 0; i < 10; ++i )
    {
      if ( QFileInfo::exists( d.absoluteFilePath( QStringLiteral( "spikes/onnx/toy.onnx" ) ) ) )
        return d.absoluteFilePath( QStringLiteral( "spikes/onnx" ) );
      if ( !d.cdUp() )
        break;
    }
    return QDir::current().absoluteFilePath( QStringLiteral( "spikes/onnx" ) );
  }
};

int main( int argc, char *argv[] )
{
  // A QCoreApplication instance is required for applicationDirPath(), which
  // vendorRuntimeDir() walks up from to locate vendor/onnxruntime.
  QCoreApplication app( argc, argv );
  TestOnnx tc;
  return QTest::qExec( &tc, argc, argv );
}

#include "tst_onnx.moc"
