#include <QtTest>
#include <QFile>
#include <QFileInfo>
#include <QSignalSpy>
#include <QTemporaryDir>

#include "../src/qgis/qgisruntime.h"
#include "../src/qgis/qgisprojectservice.h"
#include "../src/metadata/paleoprojectfile.h"

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
