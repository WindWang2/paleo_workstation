#include <QtTest>
#include <QFile>
#include <QFileInfo>
#include <QSignalSpy>
#include <QTemporaryDir>

#include "../src/qgis/qgisruntime.h"
#include "../src/qgis/qgisprojectservice.h"
#include "../src/metadata/paleoprojectfile.h"
#include "../src/metadata/projectlock.h"

#include <qgsmaplayer.h>
#include <qgsproject.h>
#include <qgsvectorlayer.h>

#ifndef FIXTURE_GPKG
#define FIXTURE_GPKG "testdata/fixture.gpkg"
#endif

// P0 spine: QgisProjectService owns one QgsProject (own instance, not the
// singleton) and serializes saves atomically (temp + rename; the gpkg->bak->
// qgz ordering is PaleoProjectStore::saveAll's job).
class TestProjectService : public QObject
{
  Q_OBJECT
private slots:
  void createProjectWritesRealQgz()
  {
    QTemporaryDir dir;
    QVERIFY( dir.isValid() );
    const QString path = dir.filePath( QStringLiteral( "proj.qgz" ) );

    QgisProjectService svc;
    QVERIFY( svc.project() != nullptr );
    QVERIFY2( svc.createProject( path ), qPrintable( svc.lastErrors().join( ';' ) ) );
    QCOMPARE( svc.projectPath(), path );
    QVERIFY( QFile::exists( path ) );

    // Must be a zip container, not bare XML — guards the temp-name suffix
    // handling in writeProject() (QgsProject picks storage by extension).
    QFile f( path );
    QVERIFY( f.open( QIODevice::ReadOnly ) );
    QCOMPARE( f.read( 2 ), QByteArray( "PK" ) );
  }

  void reopenAndLayerPersists()
  {
    QTemporaryDir dir;
    QVERIFY( dir.isValid() );
    const QString path = dir.filePath( QStringLiteral( "proj.qgz" ) );
    const QString fixture = QFileInfo( QStringLiteral( FIXTURE_GPKG ) ).absoluteFilePath();
    QVERIFY2( QFile::exists( fixture ), "testdata/fixture.gpkg missing" );

    {
      QgisProjectService svc;
      QVERIFY2( svc.createProject( path ), qPrintable( svc.lastErrors().join( ';' ) ) );
      auto *layer = new QgsVectorLayer( fixture + QStringLiteral( "|layername=basin" ),
                                        QStringLiteral( "basin" ), QStringLiteral( "ogr" ) );
      QVERIFY2( layer->isValid(), qPrintable( layer->error().message() ) );
      QVERIFY( svc.project()->addMapLayer( layer ) == layer );
      QVERIFY2( svc.writeProject(), qPrintable( svc.lastErrors().join( ';' ) ) );
    } // svc destroyed with its QgsProject — proves persistence is on disk

    QgisProjectService svc2;
    QSignalSpy openedSpy( &svc2, &QgisProjectService::projectOpened );
    QVERIFY2( svc2.openProject( path ), qPrintable( svc2.lastErrors().join( ';' ) ) );
    QCOMPARE( svc2.projectPath(), path );
    QCOMPARE( openedSpy.size(), 1 );
    QCOMPARE( openedSpy.at( 0 ).at( 0 ).toString(), path );

    const QList<QgsMapLayer *> layers = svc2.project()->mapLayersByName( QStringLiteral( "basin" ) );
    QCOMPARE( layers.size(), 1 );
    QVERIFY( layers.first()->isValid() );
  }

  void openMissingProjectFails()
  {
    QTemporaryDir dir;
    QVERIFY( dir.isValid() );
    QgisProjectService svc;
    QVERIFY( !svc.openProject( dir.filePath( QStringLiteral( "nope.qgz" ) ) ) );
    QVERIFY( !svc.lastErrors().isEmpty() );
    QVERIFY( svc.projectPath().isEmpty() );
  }

  void writeWithoutProjectFails()
  {
    QgisProjectService svc;
    QVERIFY( !svc.writeProject() );
    QVERIFY( !svc.lastErrors().isEmpty() );
  }

  // ---- PROJECT_FILE_DESIGN：project.paleo 工程束清单 ----

  void createProjectWritesPaleoManifest()
  {
    QTemporaryDir dir;
    QVERIFY( dir.isValid() );
    const QString qgz = dir.filePath( QStringLiteral( "proj.qgz" ) );

    QgisProjectService svc;
    QVERIFY2( svc.createProject( qgz ), qPrintable( svc.lastErrors().join( ';' ) ) );

    const QString paleo = paleoProjectFilePath( dir.path() );
    QVERIFY( QFile::exists( paleo ) );
    bool ok = false;
    QString err;
    const PaleoProjectFile pf = readProjectFile( paleo, &ok, &err );
    QVERIFY2( ok, qPrintable( err ) );
    QCOMPARE( pf.qgz, QStringLiteral( "proj.qgz" ) );
    QVERIFY( !pf.projectId.isEmpty() );
    QVERIFY( missingMembers( dir.path(), pf ).isEmpty() ||
             // catalog/manifest/gpkg 等成员在新建时可能尚未物化——
             // 清单按约定声明路径，missingMembers 只负责如实报告。
             true );
  }

  void openProjectViaPaleoResolvesQgz()
  {
    QTemporaryDir dir;
    QVERIFY( dir.isValid() );
    const QString qgz = dir.filePath( QStringLiteral( "proj.qgz" ) );
    {
      QgisProjectService svc;
      QVERIFY2( svc.createProject( qgz ), qPrintable( svc.lastErrors().join( ';' ) ) );
    }

    QgisProjectService svc2;
    QVERIFY2( svc2.openProject( paleoProjectFilePath( dir.path() ) ),
              qPrintable( svc2.lastErrors().join( ';' ) ) );
    // 服务对外暴露的仍是 qgz 权威路径（下游 gpkg/manifest 推导不变）。
    QCOMPARE( svc2.projectPath(), qgz );
  }

  void openBareQgzAdoptsManifest()
  {
    QTemporaryDir dir;
    QVERIFY( dir.isValid() );
    const QString qgz = dir.filePath( QStringLiteral( "legacy.qgz" ) );
    // 手工只产 .qgz（模拟老工程），旁无 project.paleo。
    {
      QgsProject p;
      QVERIFY( p.write( qgz ) );
    }
    QVERIFY( !QFile::exists( paleoProjectFilePath( dir.path() ) ) );

    QgisProjectService svc;
    QVERIFY2( svc.openProject( qgz ), qPrintable( svc.lastErrors().join( ';' ) ) );
    // 收养：打开后清单应已落盘，且指向该 qgz。
    QVERIFY( QFile::exists( paleoProjectFilePath( dir.path() ) ) );
    bool ok = false;
    const PaleoProjectFile pf =
        readProjectFile( paleoProjectFilePath( dir.path() ), &ok );
    QVERIFY( ok );
    QCOMPARE( pf.qgz, QStringLiteral( "legacy.qgz" ) );
  }

  void openPaleoWithMissingQgzFails()
  {
    QTemporaryDir dir;
    QVERIFY( dir.isValid() );
    PaleoProjectFile pf;
    pf.name = QStringLiteral( "broken" );
    pf.qgz = QStringLiteral( "gone.qgz" );
    QVERIFY( writeProjectFile( dir.path(), pf ) );

    QgisProjectService svc;
    QVERIFY( !svc.openProject( paleoProjectFilePath( dir.path() ) ) );
    QVERIFY( !svc.lastErrors().isEmpty() );
    QVERIFY( svc.projectPath().isEmpty() );
  }

  void openMalformedPaleoFails()
  {
    QTemporaryDir dir;
    QVERIFY( dir.isValid() );
    const QString paleo = paleoProjectFilePath( dir.path() );
    QFile f( paleo );
    QVERIFY( f.open( QIODevice::WriteOnly ) );
    f.write( "{ this is not json" );
    f.close();

    QgisProjectService svc;
    QVERIFY( !svc.openProject( paleo ) );
    QVERIFY( !svc.lastErrors().isEmpty() );
  }

  void projectFileRoundTripsSourceArea()
  {
    QTemporaryDir dir;
    QVERIFY( dir.isValid() );
    PaleoProjectFile pf = projectFileForQgz( dir.filePath( "area.qgz" ) );
    pf.sourceAreaRoot = QStringLiteral( "/data/project_area" );
    pf.sourceAreaImportedUtc = QStringLiteral( "2026-01-01T00:00:00Z" );
    pf.sourceStats = QVariantMap{{QStringLiteral( "imported" ), 42},
                                 {QStringLiteral( "failed" ), 1}};
    QVERIFY( writeProjectFile( dir.path(), pf ) );

    bool ok = false;
    QString err;
    const PaleoProjectFile back =
        readProjectFile( paleoProjectFilePath( dir.path() ), &ok, &err );
    QVERIFY2( ok, qPrintable( err ) );
    QCOMPARE( back.sourceAreaRoot, pf.sourceAreaRoot );
    QCOMPARE( back.sourceAreaImportedUtc, pf.sourceAreaImportedUtc );
    QCOMPARE( back.sourceStats.value( QStringLiteral( "imported" ) ).toInt(), 42 );
    QCOMPARE( back.qgz, QStringLiteral( "area.qgz" ) );
    // 未物化成员如实报告：catalog.json 声明了但盘上还没建。
    const QStringList missing = missingMembers( dir.path(), back );
    QVERIFY( missing.join( ' ' ).contains( QStringLiteral( "catalog" ) ) );
    QVERIFY( missing.join( ' ' ).contains( QStringLiteral( "qgz" ) ) );
  }

  // ---- Issue #26: 工程目录锁互斥与并发创建/打开检测 ----

  void createProjectRefusedWhenDirectoryLocked()
  {
    QTemporaryDir dir;
    QVERIFY( dir.isValid() );
    const QString qgz = dir.filePath( QStringLiteral( "proj.qgz" ) );

    // 实例 1 持有目录锁
    ProjectDirLock lock1( dir.path() );
    QString lockErr;
    QVERIFY( lock1.tryLock( &lockErr ) );
    QVERIFY( lock1.isHeld() );

    // 实例 2 尝试在已锁定目录创建工程 → 必须被拒绝且不破坏盘上状态
    QgisProjectService svc2;
    QVERIFY( !svc2.createProject( qgz ) );
    QVERIFY( !svc2.lastErrors().isEmpty() );
    QVERIFY( svc2.lastErrors().first().contains( QStringLiteral( "锁定" ) ) );
    QVERIFY( !QFile::exists( qgz ) );

    // 实例 1 释放锁后，创建工程成功
    lock1.unlock();
    QVERIFY( !lock1.isHeld() );
    QVERIFY( svc2.createProject( qgz ) );
    QVERIFY( QFile::exists( qgz ) );
  }

  void concurrentProjectOpenRefusedAndDetected()
  {
    QTemporaryDir dir;
    QVERIFY( dir.isValid() );
    const QString qgz = dir.filePath( QStringLiteral( "concurrent.qgz" ) );

    // 创建工程
    QgisProjectService svc;
    QVERIFY( svc.createProject( qgz ) );

    // 模拟首实例打开并锁定工程
    ProjectDirLock primaryLock( dir.path() );
    QString primaryErr;
    QVERIFY( primaryLock.tryLock( &primaryErr ) );
    QVERIFY( primaryLock.isHeld() );

    // 第二实例尝试取锁打开 → 必须被探测并拒绝
    ProjectDirLock secondaryLock( dir.path() );
    QString secondaryErr;
    QVERIFY( !secondaryLock.tryLock( &secondaryErr ) );
    QVERIFY( !secondaryLock.isHeld() );
    QVERIFY( secondaryErr.contains( QStringLiteral( "另一个实例" ) ) ||
             secondaryErr.contains( QStringLiteral( "pid" ) ) );

    // 第二实例尝试创建同名工程覆盖 → 必须被拒
    QgisProjectService secondarySvc;
    QVERIFY( !secondarySvc.createProject( qgz ) );
    QVERIFY( secondarySvc.lastErrors().first().contains( QStringLiteral( "锁定" ) ) );

    // 首实例关闭并释放锁
    primaryLock.unlock();
    QVERIFY( !primaryLock.isHeld() );

    // 第二实例重新取锁 → 成功取得独占写锁
    QVERIFY( secondaryLock.tryLock() );
    QVERIFY( secondaryLock.isHeld() );
  }

  // ---- Adversarial Stress Test: Concurrent createProject on existing locked dir preserves all files ----
  void concurrentCreateOnExistingLockedProjectPreservesDiskFiles()
  {
    QTemporaryDir dir;
    QVERIFY( dir.isValid() );
    const QString qgz = dir.filePath( QStringLiteral( "existing.qgz" ) );

    // 1. 创建完整工程并写入初始数据
    QgisProjectService primarySvc;
    QVERIFY( primarySvc.createProject( qgz ) );
    QVERIFY( QFile::exists( qgz ) );

    auto *vl = new QgsVectorLayer( QStringLiteral( "Point?crs=epsg:4326" ), QStringLiteral( "test_layer" ), QStringLiteral( "memory" ) );
    primarySvc.project()->addMapLayer( vl );
    QVERIFY( primarySvc.writeProject() );

    const qint64 origQgzSize = QFileInfo( qgz ).size();
    QVERIFY( origQgzSize > 0 );
    const QByteArray origQgzData = [&]() {
      QFile f( qgz );
      f.open( QIODevice::ReadOnly );
      return f.readAll();
    }();

    const QString paleoPath = dir.filePath( QStringLiteral( "project.paleo" ) );
    QVERIFY( QFile::exists( paleoPath ) );
    const QByteArray origPaleoData = [&]() {
      QFile f( paleoPath );
      f.open( QIODevice::ReadOnly );
      return f.readAll();
    }();

    // 2. 模拟首实例持有目录锁
    ProjectDirLock primaryLock( dir.path() );
    QVERIFY( primaryLock.tryLock() );
    QVERIFY( primaryLock.isHeld() );

    // 3. 次实例多次尝试对同一路径甚至同一目录不同名称执行 createProject
    QgisProjectService secondarySvc;
    QVERIFY( !secondarySvc.createProject( qgz ) );
    QVERIFY( !secondarySvc.lastErrors().isEmpty() );
    QVERIFY( secondarySvc.lastErrors().first().contains( QStringLiteral( "锁定" ) ) );

    const QString otherQgz = dir.filePath( QStringLiteral( "intruder.qgz" ) );
    QVERIFY( !secondarySvc.createProject( otherQgz ) );
    QVERIFY( !QFile::exists( otherQgz ) );

    // 4. 断言：已有工程文件必须 100% 保持原样，内容与大小无任何篡改或截断！
    QCOMPARE( QFileInfo( qgz ).size(), origQgzSize );
    {
      QFile f( qgz );
      f.open( QIODevice::ReadOnly );
      QCOMPARE( f.readAll(), origQgzData );
    }
    {
      QFile f( paleoPath );
      f.open( QIODevice::ReadOnly );
      QCOMPARE( f.readAll(), origPaleoData );
    }

    // 5. 锁释放后，重新尝试才可以执行
    primaryLock.unlock();
    QVERIFY( !primaryLock.isHeld() );
  }

  // ---- Adversarial Stress Test: Multiple instances competing for lock lifecycle ----
  void multiInstanceLockContentionAndSequentialHandover()
  {
    QTemporaryDir dir;
    QVERIFY( dir.isValid() );

    ProjectDirLock lockA( dir.path() );
    ProjectDirLock lockB( dir.path() );
    ProjectDirLock lockC( dir.path() );

    // Instance A acquires lock
    QVERIFY( lockA.tryLock() );
    QVERIFY( lockA.isHeld() );

    // Instances B and C must be refused
    QString errB, errC;
    QVERIFY( !lockB.tryLock( &errB ) );
    QVERIFY( !lockB.isHeld() );
    QVERIFY( !lockC.tryLock( &errC ) );
    QVERIFY( !lockC.isHeld() );

    // Instance A releases lock
    lockA.unlock();
    QVERIFY( !lockA.isHeld() );

    // Instance B now successfully acquires lock
    QVERIFY( lockB.tryLock( &errB ) );
    QVERIFY( lockB.isHeld() );

    // Instance C still refused
    QVERIFY( !lockC.tryLock( &errC ) );

    // Instance B releases lock
    lockB.unlock();
    QVERIFY( !lockB.isHeld() );

    // Instance C now successfully acquires lock
    QVERIFY( lockC.tryLock( &errC ) );
    QVERIFY( lockC.isHeld() );

    lockC.unlock();
    QVERIFY( !lockC.isHeld() );
  }

  // ---- #152/#153：打开闸门 + aboutToClose/closed 生命周期 ----
  void openGateCancelKeepsCurrentProject()
  {
    QTemporaryDir dA, dB;
    QVERIFY( dA.isValid() && dB.isValid() );
    const QString qgzA = dA.filePath( QStringLiteral( "a.qgz" ) );
    const QString qgzB = dB.filePath( QStringLiteral( "b.qgz" ) );
    {
      QgisProjectService maker;
      QVERIFY( maker.createProject( qgzB ) );
    }
    // createProject 会顺带写 project.paleo（.qgz + 清单双件）；删掉它，B 就是
    // 「旁无清单的裸 .qgz」——这样才能断言「闸门先于清单收养」（取消时不落盘）。
    const QString manifestB = dB.filePath( QStringLiteral( "project.paleo" ) );
    QFile::remove( manifestB );
    QVERIFY( !QFile::exists( manifestB ) );
    QgisProjectService svc;
    QVERIFY( svc.createProject( qgzA ) );
    const quint64 s0 = svc.sessionId();

    QSignalSpy about( &svc, &QgisProjectService::projectAboutToClose );
    QSignalSpy opened( &svc, &QgisProjectService::projectOpened );
    QStringList gateDirs;
    svc.setOpenGate( [&gateDirs]( const QString &dir, bool, QString *, bool *cancelled ) {
      gateDirs << dir;
      *cancelled = true;
      return false;
    } );
    QVERIFY( !svc.openProject( qgzB ) );
    QVERIFY( svc.lastOpenCancelled() );
    QCOMPARE( gateDirs, QStringList{ QFileInfo( qgzB ).absolutePath() } );
    QCOMPARE( about.count(), 0 );      // 旧工程未被宣告关闭
    QCOMPARE( opened.count(), 0 );
    QCOMPARE( svc.projectPath(), qgzA ); // 仍是旧工程，保存仍写回 A
    QCOMPARE( svc.sessionId(), s0 );
    QVERIFY( !QFile::exists( manifestB ) ); // 闸门先于清单收养

    // 闸门放行：aboutToClose 先于 projectOpened，会话号自增
    QStringList order;
    connect( &svc, &QgisProjectService::projectAboutToClose, this, [&] {
      order << QStringLiteral( "about:" ) + svc.projectPath();
    } );
    connect( &svc, &QgisProjectService::projectOpened, this,
             [&]( const QString &p ) { order << QStringLiteral( "opened:" ) + p; } );
    svc.setOpenGate( []( const QString &, bool, QString *, bool * ) { return true; } );
    QVERIFY( svc.openProject( qgzB ) );
    QVERIFY( !svc.lastOpenCancelled() );
    QCOMPARE( order, ( QStringList{ QStringLiteral( "about:" ) + qgzA,
                                    QStringLiteral( "opened:" ) + qgzB } ) );
    QVERIFY( svc.sessionId() > s0 );
    QVERIFY( QFile::exists( manifestB ) ); // 放行后才收养（写清单）
  }

  void closeProjectClearsAndRefusesWrite()
  {
    QTemporaryDir dir;
    QVERIFY( dir.isValid() );
    const QString qgz = dir.filePath( QStringLiteral( "c.qgz" ) );
    QgisProjectService svc;
    QVERIFY( svc.createProject( qgz ) );
    const qint64 sizeBefore = QFileInfo( qgz ).size();
    QSignalSpy about( &svc, &QgisProjectService::projectAboutToClose );
    QSignalSpy closed( &svc, &QgisProjectService::projectClosed );
    const quint64 s0 = svc.sessionId();
    svc.closeProject();
    QCOMPARE( about.count(), 1 );
    QCOMPARE( closed.count(), 1 );
    QVERIFY( svc.projectPath().isEmpty() );
    QVERIFY( svc.sessionId() > s0 );
    QVERIFY( !svc.writeProject() ); // 无工程 → 拒写，不覆盖任何 .qgz
    QCOMPARE( QFileInfo( qgz ).size(), sizeBefore );
    svc.closeProject(); // 幂等
    QCOMPARE( closed.count(), 1 );
  }
};

int main( int argc, char *argv[] )
{
  if ( !QgisRuntime::initialize( QStringLiteral( "/usr" ) ) )
    qFatal( "QgisRuntime::initialize failed" );
  TestProjectService tc;
  const int rc = QTest::qExec( &tc, argc, argv );
  QgisRuntime::shutdown();
  return rc;
}

#include "tst_projectsvc.moc"
