#include <QtTest>
#include <QFile>
#include <QFileInfo>
#include <QTemporaryDir>

#include "../src/app/appcontext.h"
#include "../src/metadata/layermanifest.h"
#include "../src/qgis/manifestprojection.h"
#include "../src/qgis/qgislayerservice.h"
#include "../src/qgis/qgisprojectservice.h"
#include "../src/qgis/qgisruntime.h"
#include "../src/metadata/projectlock.h"
#include "../src/metadata/paleoprojectstore.h"

// §37 recovery — the SQLite manifest store is authoritative, but a .qgz moved
// or shared WITHOUT its ".project.sqlite" sidecar must still recover its full
// declared layer set from the embedded ManifestProjection. The inverse also
// holds: when the store file EXISTS it wins — rehydration must never
// resurrect layers a user deliberately deleted from the manifest.

static QString fixtureGpkg()
{
#ifdef FIXTURE_GPKG
  return QStringLiteral( FIXTURE_GPKG );
#else
  return QStringLiteral( "testdata/fixture.gpkg" );
#endif
}

static LayerDeclaration decl( const QString &layerId, const QString &horizon )
{
  LayerDeclaration d;
  d.layerId = layerId;
  d.horizon = horizon;
  d.type = QStringLiteral( "vector" );
  d.source = fixtureGpkg() + QStringLiteral( "|layername=basin" );
  d.styleRef = QStringLiteral( "styles/%1.qml" ).arg( layerId );
  d.group = QStringLiteral( "04_SingleFactor" );
  return d;
}

class TestRehydrate : public QObject
{
  Q_OBJECT
private slots:
  void initTestCase()
  {
    QVERIFY( QgisRuntime::isInitialized() );
  }

  // A .qgz copied to a new directory without its sidecar sqlite recovers the
  // declared set on open — and a subsequent write keeps the projection intact.
  void missingStoreRehydratesFromQgz()
  {
    QTemporaryDir dir1, dir2;
    QVERIFY( dir1.isValid() && dir2.isValid() );
    const QString qgz1 = dir1.filePath( QStringLiteral( "proj.qgz" ) );

    {
      AppContext ctx( QStringLiteral( "/usr" ) );
      QVERIFY( ctx.ready() );
      QVERIFY2( ctx.projectSvc()->createProject( qgz1 ),
                qPrintable( ctx.projectSvc()->lastErrors().join( ';' ) ) );

      QString err;
      QVERIFY2( ctx.layerSvc()->declare( decl( QStringLiteral( "facies.T1" ),
                                             QStringLiteral( "T1" ) ), &err ),
                qPrintable( err ) );
      QVERIFY( ctx.layerSvc()->declare( decl( QStringLiteral( "facies.T2" ),
                                              QStringLiteral( "T2" ) ) ) );
      QVERIFY2( ctx.projectSvc()->writeProject(),
                qPrintable( ctx.projectSvc()->lastErrors().join( ';' ) ) );
    } // ctx1 destroyed — manifest sqlite stays behind in dir1

    // Simulate a shared/moved project: only the .qgz travels.
    const QString qgz2 = dir2.filePath( QStringLiteral( "proj.qgz" ) );
    QVERIFY( QFile::copy( qgz1, qgz2 ) );
    QVERIFY( !QFile::exists( qgz2 + QStringLiteral( ".project.sqlite" ) ) );

    {
      AppContext ctx( QStringLiteral( "/usr" ) );
      QVERIFY( ctx.ready() );
      QVERIFY2( ctx.projectSvc()->openProject( qgz2 ),
                qPrintable( ctx.projectSvc()->lastErrors().join( ';' ) ) );

      const QVector<LayerDeclaration> got = ctx.manifest()->all();
      QCOMPARE( got.size(), 2 );
      for ( const LayerDeclaration &d : got )
      {
        QVERIFY( d.layerId.startsWith( QStringLiteral( "facies." ) ) );
        QCOMPARE( d.instantiated, false ); // runtime flag — lazy instantiation decides
        QCOMPARE( d.source, fixtureGpkg() + QStringLiteral( "|layername=basin" ) );
      }
      // The rebound manifest feeds the layer service as usual.
      QCOMPARE( ctx.layerSvc()->declared().size(), 2 );

      // Writing again must not lose the restored set.
      QVERIFY2( ctx.projectSvc()->writeProject(),
                qPrintable( ctx.projectSvc()->lastErrors().join( ';' ) ) );
      QCOMPARE( ManifestProjection::extractDeclarations( ctx.projectSvc()->project() ).size(), 2 );
    }
  }

  // An existing (even deliberately empty) store is authoritative: a .qgz that
  // still embeds stale declarations must NOT resurrect them on open.
  void existingStoreWinsOverEmbeddedProjection()
  {
    QTemporaryDir dir1, dir3;
    QVERIFY( dir1.isValid() && dir3.isValid() );
    const QString qgz1 = dir1.filePath( QStringLiteral( "proj.qgz" ) );

    {
      AppContext ctx( QStringLiteral( "/usr" ) );
      QVERIFY( ctx.ready() );
      QVERIFY2( ctx.projectSvc()->createProject( qgz1 ),
                qPrintable( ctx.projectSvc()->lastErrors().join( ';' ) ) );
      QVERIFY( ctx.layerSvc()->declare( decl( QStringLiteral( "facies.T1" ),
                                            QStringLiteral( "T1" ) ) ) );
      QVERIFY2( ctx.projectSvc()->writeProject(),
                qPrintable( ctx.projectSvc()->lastErrors().join( ';' ) ) );
    }

    const QString qgz3 = dir3.filePath( QStringLiteral( "proj.qgz" ) );
    QVERIFY( QFile::copy( qgz1, qgz3 ) );

    // Pre-create the sidecar store: it exists but is empty — the user removed
    // every declaration and that decision must stick.
    const QString metaPath = qgz3 + QStringLiteral( ".project.sqlite" );
    {
      LayerManifest emptyStore( metaPath );
      QVERIFY( emptyStore.open() );
      QVERIFY( emptyStore.all().isEmpty() );
    }
    QVERIFY( QFile::exists( metaPath ) );

    {
      AppContext ctx( QStringLiteral( "/usr" ) );
      QVERIFY( ctx.ready() );
      QVERIFY2( ctx.projectSvc()->openProject( qgz3 ),
                qPrintable( ctx.projectSvc()->lastErrors().join( ';' ) ) );

      // Embedded projection still describes facies.T1 — but the store wins.
      QVERIFY( !ManifestProjection::extractDeclarations( ctx.projectSvc()->project() ).isEmpty() );
      QVERIFY( ctx.manifest()->all().isEmpty() );
      QVERIFY( ctx.layerSvc()->declared().isEmpty() );
    }
  }

  // A project with no embedded projection + no store: manifest just opens empty.
  void plainProjectOpensWithEmptyManifest()
  {
    QTemporaryDir dir;
    QVERIFY( dir.isValid() );
    const QString qgz = dir.filePath( QStringLiteral( "plain.qgz" ) );

    // Write a bare .qgz with no provider — no paleo custom property at all.
    {
      QgisProjectService svc;
      QVERIFY2( svc.createProject( qgz ),
                qPrintable( svc.lastErrors().join( ';' ) ) );
    }

    AppContext ctx( QStringLiteral( "/usr" ) );
    QVERIFY( ctx.ready() );
    QVERIFY2( ctx.projectSvc()->openProject( qgz ),
              qPrintable( ctx.projectSvc()->lastErrors().join( ';' ) ) );
    QVERIFY( ctx.manifest()->all().isEmpty() );
  }

  // ---- Issue #26: AppContext 目录锁生命周期与并发只读降级 ----
  void appContextLockLifecycleAndReadOnlyDowngrade()
  {
    QTemporaryDir dir;
    QVERIFY( dir.isValid() );
    const QString qgz = dir.filePath( QStringLiteral( "lock_test.qgz" ) );

    // 1. 创建工程
    {
      QgisProjectService svc;
      QVERIFY( svc.createProject( qgz ) );
    }

    // 2. 实例 1 打开工程（首实例取得独占锁）
    AppContext ctx1( QStringLiteral( "/usr" ) );
    QVERIFY( ctx1.ready() );
    QVERIFY( ctx1.projectSvc()->openProject( qgz ) );
    QVERIFY( !ctx1.isProjectReadOnly() );
    QVERIFY( ctx1.store() && !ctx1.store()->isReadOnly() );

    // 3. 实例 2 打开同一工程（无头环境自动降级为只读）
    AppContext ctx2( QStringLiteral( "/usr" ) );
    QVERIFY( ctx2.ready() );
    QVERIFY( ctx2.projectSvc()->openProject( qgz ) );
    QVERIFY( ctx2.isProjectReadOnly() );
    QVERIFY( ctx2.store() && ctx2.store()->isReadOnly() );

    // 4. 实例 1 执行 closeProject()，验证锁被干净释放
    ctx1.closeProject();
    QVERIFY( !ctx1.isProjectReadOnly() );

    // 5. 实例 3 尝试取锁，能够直接成功
    ProjectDirLock lock3( dir.path() );
    QVERIFY( lock3.tryLock() );
    QVERIFY( lock3.isHeld() );
    lock3.unlock();
  }
};

int main( int argc, char *argv[] )
{
  if ( !QgisRuntime::initialize( QStringLiteral( "/usr" ) ) )
    qFatal( "QgisRuntime::initialize failed" );
  TestRehydrate tc;
  const int rc = QTest::qExec( &tc, argc, argv );
  QgisRuntime::shutdown();
  return rc;
}

#include "tst_rehydrate.moc"
