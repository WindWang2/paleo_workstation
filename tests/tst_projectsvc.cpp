#include <QtTest>
#include <QFile>
#include <QFileInfo>
#include <QSignalSpy>
#include <QTemporaryDir>

#include "../src/qgis/qgisruntime.h"
#include "../src/qgis/qgisprojectservice.h"

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
