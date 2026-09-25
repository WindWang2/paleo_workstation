#include <QtTest>
#include <QFile>
#include <QFileInfo>
#include <QTemporaryDir>

#include <qgsmaplayer.h>
#include <qgsproject.h>

#include "../src/metadata/layermanifest.h"
#include "../src/qgis/manifestprojection.h"
#include "../src/qgis/qgislayerservice.h"
#include "../src/qgis/qgisprojectservice.h"
#include "../src/qgis/qgisruntime.h"

// §37 acceptance — manifest→.qgz projection loop. The manifest is the layer-SET
// authority; writeProject() must embed the full declared set into the .qgz so
// declared-but-uninstantiated layers survive a save/reopen cycle.
// Design: JSON array in QgsProject custom properties (scope "paleo").

static QString fixtureGpkg()
{
#ifdef FIXTURE_GPKG
  return QStringLiteral( FIXTURE_GPKG );
#else
  const QString testsDir = QFileInfo( QString::fromUtf8( __FILE__ ) ).absolutePath();
  return QDir( testsDir ).absoluteFilePath( QStringLiteral( "../testdata/fixture.gpkg" ) );
#endif
}

static LayerDeclaration decl( const QString &layerId, const QString &horizon,
                              const QString &type = QStringLiteral( "vector" ) )
{
  LayerDeclaration d;
  d.layerId = layerId;
  d.horizon = horizon;
  d.type = type;
  d.source = fixtureGpkg() + QStringLiteral( "|layername=basin" );
  d.styleRef = QStringLiteral( "styles/%1.qml" ).arg( layerId );
  d.group = QStringLiteral( "04_SingleFactor" );
  return d;
}

static QHash<QString, LayerDeclaration> byId( const QVector<LayerDeclaration> &v )
{
  QHash<QString, LayerDeclaration> m;
  for ( const LayerDeclaration &d : v )
    m.insert( d.layerId, d );
  return m;
}

// Order-insensitive, field-exact comparison of two declared sets.
static bool sameDeclSet( const QVector<LayerDeclaration> &a,
                         const QVector<LayerDeclaration> &b,
                         QString *why = nullptr )
{
  const QHash<QString, LayerDeclaration> ma = byId( a );
  const QHash<QString, LayerDeclaration> mb = byId( b );
  if ( ma.size() != a.size() || mb.size() != b.size() )
  {
    if ( why ) *why = QStringLiteral( "duplicate layerIds in a set" );
    return false;
  }
  if ( ma.size() != mb.size() )
  {
    if ( why ) *why = QStringLiteral( "size %1 != %2" ).arg( a.size() ).arg( b.size() );
    return false;
  }
  for ( auto it = ma.cbegin(); it != ma.cend(); ++it )
  {
    const auto hit = mb.constFind( it.key() );
    if ( hit == mb.constEnd() )
    {
      if ( why ) *why = QStringLiteral( "missing layerId '%1'" ).arg( it.key() );
      return false;
    }
    const LayerDeclaration &x = it.value();
    const LayerDeclaration &y = hit.value();
    if ( x.horizon != y.horizon || x.type != y.type || x.source != y.source ||
         x.styleRef != y.styleRef || x.group != y.group )
    {
      if ( why ) *why = QStringLiteral( "field mismatch on '%1'" ).arg( it.key() );
      return false;
    }
  }
  return true;
}

class TestManifestProjection : public QObject
{
  Q_OBJECT
private slots:
  void initTestCase()
  {
    QVERIFY( QgisRuntime::isInitialized() );
    QVERIFY2( QFile::exists( fixtureGpkg() ),
              qPrintable( QStringLiteral( "fixture missing: %1" ).arg( fixtureGpkg() ) ) );
  }

  // Declared set (3 layers / 2 horizons) survives write→reopen even though
  // only horizon T1 was ever instantiated.
  void declaredSetSurvivesProjectWrite()
  {
    QTemporaryDir dir;
    QVERIFY( dir.isValid() );
    const QString qgzPath = dir.filePath( QStringLiteral( "proj.qgz" ) );
    const QString dbPath = dir.filePath( QStringLiteral( "project.sqlite" ) );

    QVector<LayerDeclaration> declared;
    {
      QgisProjectService svc;
      QVERIFY2( svc.createProject( qgzPath ),
                qPrintable( svc.lastErrors().join( ';' ) ) );

      LayerManifest manifest( dbPath );
      QString err;
      QVERIFY2( manifest.open( &err ), qPrintable( err ) );

      QgisLayerService layerSvc( &svc, &manifest );
      QVERIFY2( layerSvc.declare( decl( QStringLiteral( "facies.T1" ), QStringLiteral( "T1" ) ), &err ), qPrintable( err ) );
      QVERIFY( layerSvc.declare( decl( QStringLiteral( "faults.T1" ), QStringLiteral( "T1" ) ) ) );
      QVERIFY( layerSvc.declare( decl( QStringLiteral( "facies.T2" ), QStringLiteral( "T2" ) ) ) );
      declared = layerSvc.declared();
      QCOMPARE( declared.size(), 3 );

      // The hook: writes embed whatever the provider returns at write time.
      svc.setDeclarationProvider( [&layerSvc]() { return layerSvc.declared(); } );

      // Only T1 is materialized; facies.T2 stays declaration-only.
      QCOMPARE( layerSvc.instantiateHorizon( QStringLiteral( "T1" ) ), 2 );
      QVERIFY( layerSvc.isInstantiated( QStringLiteral( "facies.T1" ) ) );
      QVERIFY( !layerSvc.isInstantiated( QStringLiteral( "facies.T2" ) ) );

      QVERIFY2( svc.writeProject(), qPrintable( svc.lastErrors().join( ';' ) ) );
    } // svc destroyed with its QgsProject — extraction must read from disk

    QgisProjectService svc2;
    QVERIFY2( svc2.openProject( qgzPath ),
              qPrintable( svc2.lastErrors().join( ';' ) ) );

    // The instantiated T1 layers still persist as normal QgsMapLayers.
    QCOMPARE( svc2.project()->mapLayers().size(), 2 );

    const QVector<LayerDeclaration> got =
      ManifestProjection::extractDeclarations( svc2.project() );

    QString why;
    QVERIFY2( sameDeclSet( got, declared, &why ), qPrintable( why ) );

    // Runtime flag must not be persisted: everything reloads uninstantiated.
    for ( const LayerDeclaration &d : got )
      QCOMPARE( d.instantiated, false );
  }

  // An empty manifest still writes an explicit empty array that round-trips.
  void emptyManifestRoundTrips()
  {
    QTemporaryDir dir;
    QVERIFY( dir.isValid() );
    const QString qgzPath = dir.filePath( QStringLiteral( "empty.qgz" ) );

    LayerManifest manifest( dir.filePath( QStringLiteral( "project.sqlite" ) ) );
    QVERIFY( manifest.open() );

    {
      QgisProjectService svc;
      // Provider set before createProject exercises the very first write too.
      svc.setDeclarationProvider( [&manifest]() { return manifest.all(); } );
      QVERIFY2( svc.createProject( qgzPath ),
                qPrintable( svc.lastErrors().join( ';' ) ) );
      QVERIFY2( svc.writeProject(), qPrintable( svc.lastErrors().join( ';' ) ) );
    }

    QgisProjectService svc2;
    QVERIFY2( svc2.openProject( qgzPath ),
              qPrintable( svc2.lastErrors().join( ';' ) ) );

    // Property exists and holds an explicit (valid) empty array.
    bool ok = false;
    const QString raw = svc2.project()->readEntry(
      ManifestProjection::scope(), ManifestProjection::key(), QString(), &ok );
    QVERIFY( ok );
    QCOMPARE( raw.trimmed(), QStringLiteral( "[]" ) );

    QVERIFY( ManifestProjection::extractDeclarations( svc2.project() ).isEmpty() );
  }

  // A project written without a provider has no projection — extract is empty,
  // and malformed/absent payloads degrade to empty rather than crashing.
  void absentAndMalformedPayloads()
  {
    QTemporaryDir dir;
    QVERIFY( dir.isValid() );
    const QString qgzPath = dir.filePath( QStringLiteral( "plain.qgz" ) );

    QgisProjectService svc;
    QVERIFY2( svc.createProject( qgzPath ),
              qPrintable( svc.lastErrors().join( ';' ) ) );
    QVERIFY( ManifestProjection::extractDeclarations( svc.project() ).isEmpty() );

    // Malformed JSON in the property → empty, not a crash.
    QVERIFY( svc.project()->writeEntry( ManifestProjection::scope(),
                                        ManifestProjection::key(),
                                        QStringLiteral( "{not-json" ) ) );
    QVERIFY( ManifestProjection::extractDeclarations( svc.project() ).isEmpty() );

    // Non-array JSON → empty.
    QVERIFY( svc.project()->writeEntry( ManifestProjection::scope(),
                                        ManifestProjection::key(),
                                        QStringLiteral( "{\"a\":1}" ) ) );
    QVERIFY( ManifestProjection::extractDeclarations( svc.project() ).isEmpty() );

    // Null-project safety.
    QString err;
    QVERIFY( !ManifestProjection::embedDeclarations( nullptr, {}, &err ) );
    QVERIFY( !err.isEmpty() );
    QVERIFY( ManifestProjection::extractDeclarations( nullptr ).isEmpty() );
  }

  // Direct embed/extract on a live project (no disk roundtrip) preserves all
  // fields and skips 'instantiated' even when a caller sets it.
  void embedExtractInPlace()
  {
    QTemporaryDir dir;
    QVERIFY( dir.isValid() );
    QgisProjectService svc;
    QVERIFY2( svc.createProject( dir.filePath( QStringLiteral( "live.qgz" ) ) ),
              qPrintable( svc.lastErrors().join( ';' ) ) );

    LayerDeclaration d = decl( QStringLiteral( "facies.T1" ), QStringLiteral( "T1" ) );
    d.instantiated = true; // must NOT persist

    QString err;
    QVERIFY2( ManifestProjection::embedDeclarations( svc.project(), {d}, &err ),
              qPrintable( err ) );

    const QVector<LayerDeclaration> got =
      ManifestProjection::extractDeclarations( svc.project() );
    QCOMPARE( got.size(), 1 );
    QCOMPARE( got.first().layerId, QStringLiteral( "facies.T1" ) );
    QCOMPARE( got.first().horizon, QStringLiteral( "T1" ) );
    QCOMPARE( got.first().type, QStringLiteral( "vector" ) );
    QCOMPARE( got.first().source, fixtureGpkg() + QStringLiteral( "|layername=basin" ) );
    QCOMPARE( got.first().styleRef, QStringLiteral( "styles/facies.T1.qml" ) );
    QCOMPARE( got.first().group, QStringLiteral( "04_SingleFactor" ) );
    QCOMPARE( got.first().instantiated, false );
  }
};

int main( int argc, char *argv[] )
{
  if ( !QgisRuntime::initialize( QStringLiteral( "/usr" ) ) )
    qFatal( "QgisRuntime::initialize failed" );
  TestManifestProjection tc;
  const int rc = QTest::qExec( &tc, argc, argv );
  QgisRuntime::shutdown();
  return rc;
}

#include "tst_manifestproj.moc"
