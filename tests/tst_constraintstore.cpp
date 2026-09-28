#include <QtTest>
#include <QDir>
#include <QFile>
#include <QSignalSpy>
#include <QTemporaryDir>

#include <qgsapplication.h>
#include <qgsmaplayer.h>
#include <qgsproject.h>
#include <qgsvectorlayer.h>

#include <gdal.h>

#include "../src/domain/types.h"
#include "../src/io/constraintstore.h"
#include "../src/metadata/layermanifest.h"
#include "../src/metadata/paleoprojectstore.h"
#include "../src/qgis/qgislayerservice.h"
#include "../src/qgis/qgisprocessingservice.h"
#include "../src/qgis/qgisprojectservice.h"
#include "../src/workflow/workflows.h"

class TestConstraintStore : public QObject
{
  Q_OBJECT

private slots:
  void initTestCase()
  {
    QVERIFY( QgsApplication::instance() != nullptr );
    GDALAllRegister();
  }

  void cleanup()
  {
    QgsProject::instance()->removeAllMapLayers();
  }

  // Direct ConstraintStore unit test:
  // - Append 2 constraints for horizon "T1" (line, polygon), 1 constraint for "T2" (point).
  // - Load("T1") returns 2 items with correct fields and WKT.
  // - Load("T2") returns 1 item.
  // - Load() (empty) returns 3 items.
  // - Remove("c-1") removes c-1 from gpkg; Load("T1") now returns 1 item ("c-2").
  // - Verify invalid WKT returns false with error.
  void testDirectStoreUnit()
  {
    QTemporaryDir dir;
    QVERIFY( dir.isValid() );
    const QString gpkgPath = dir.filePath( QStringLiteral( "test_constraints.gpkg" ) );

    PaleoProjectStore projectStore;
    ConstraintStore store( gpkgPath, &projectStore );

    QString err;
    // Append 2 constraints for horizon "T1" (line, polygon), 1 constraint for "T2" (point)
    QVERIFY2( store.append( QStringLiteral( "T1" ), QStringLiteral( "c-1" ),
                            QStringLiteral( "LINESTRING(0 0, 10 0)" ),
                            QStringLiteral( "line" ), 1, &err ), qPrintable( err ) );

    QVERIFY2( store.append( QStringLiteral( "T1" ), QStringLiteral( "c-2" ),
                            QStringLiteral( "POLYGON((0 0, 10 0, 10 10, 0 10, 0 0))" ),
                            QStringLiteral( "polygon" ), 2, &err ), qPrintable( err ) );

    QVERIFY2( store.append( QStringLiteral( "T2" ), QStringLiteral( "c-3" ),
                            QStringLiteral( "POINT(5 5)" ),
                            QStringLiteral( "point" ), 1, &err ), qPrintable( err ) );

    // Load("T1") returns 2 items with correct fields and WKT
    QVector<QVariantMap> t1Items = store.load( QStringLiteral( "T1" ) );
    QCOMPARE( t1Items.size(), 2 );

    QCOMPARE( t1Items[0].value( QStringLiteral( "id" ) ).toString(), QStringLiteral( "c-1" ) );
    QCOMPARE( t1Items[0].value( QStringLiteral( "horizon" ) ).toString(), QStringLiteral( "T1" ) );
    QCOMPARE( t1Items[0].value( QStringLiteral( "type" ) ).toString(), QStringLiteral( "line" ) );
    QCOMPARE( t1Items[0].value( QStringLiteral( "facies_code" ) ).toInt(), 1 );
    QCOMPARE( t1Items[0].value( QStringLiteral( "target_facies_code" ) ).toInt(), 1 );
    QVERIFY( t1Items[0].value( QStringLiteral( "wkt" ) ).toString().contains( QStringLiteral( "LINESTRING" ) ) );

    QCOMPARE( t1Items[1].value( QStringLiteral( "id" ) ).toString(), QStringLiteral( "c-2" ) );
    QCOMPARE( t1Items[1].value( QStringLiteral( "horizon" ) ).toString(), QStringLiteral( "T1" ) );
    QCOMPARE( t1Items[1].value( QStringLiteral( "type" ) ).toString(), QStringLiteral( "polygon" ) );
    QCOMPARE( t1Items[1].value( QStringLiteral( "facies_code" ) ).toInt(), 2 );
    QCOMPARE( t1Items[1].value( QStringLiteral( "target_facies_code" ) ).toInt(), 2 );
    QVERIFY( t1Items[1].value( QStringLiteral( "wkt" ) ).toString().contains( QStringLiteral( "POLYGON" ) ) );

    // Load("T2") returns 1 item
    QVector<QVariantMap> t2Items = store.load( QStringLiteral( "T2" ) );
    QCOMPARE( t2Items.size(), 1 );
    QCOMPARE( t2Items[0].value( QStringLiteral( "id" ) ).toString(), QStringLiteral( "c-3" ) );
    QCOMPARE( t2Items[0].value( QStringLiteral( "horizon" ) ).toString(), QStringLiteral( "T2" ) );
    QCOMPARE( t2Items[0].value( QStringLiteral( "type" ) ).toString(), QStringLiteral( "point" ) );
    QCOMPARE( t2Items[0].value( QStringLiteral( "facies_code" ) ).toInt(), 1 );
    QVERIFY( t2Items[0].value( QStringLiteral( "wkt" ) ).toString().contains( QStringLiteral( "POINT" ) ) );

    // Load() (empty) returns 3 items
    QVector<QVariantMap> allItems = store.load();
    QCOMPARE( allItems.size(), 3 );

    // Remove("c-1") removes c-1 from gpkg; Load("T1") now returns 1 item ("c-2")
    QVERIFY2( store.remove( QStringLiteral( "c-1" ), &err ), qPrintable( err ) );
    QVector<QVariantMap> t1AfterRemove = store.load( QStringLiteral( "T1" ) );
    QCOMPARE( t1AfterRemove.size(), 1 );
    QCOMPARE( t1AfterRemove[0].value( QStringLiteral( "id" ) ).toString(), QStringLiteral( "c-2" ) );

    // Verify invalid WKT returns false with error
    QString invalidErr;
    bool badOk = store.append( QStringLiteral( "T1" ), QStringLiteral( "c-invalid" ),
                               QStringLiteral( "INVALID_WKT(0 0)" ),
                               QStringLiteral( "line" ), 1, &invalidErr );
    QVERIFY( !badOk );
    QVERIFY( !invalidErr.isEmpty() );
  }

  // Real stack end-to-end test (projectSvc + manifest + layerSvc + store + workflow):
  // - Setup in QTemporaryDir (projectSvc.createProject, manifest.open, store.setProjectPaths, layers, proc, wf).
  // - Wire wf with store: wf.setStore(&store); (or wf.setConstraintStore(&cs);).
  // - Call wf.addConstraint("T1", "LINESTRING(0 0, 10 0)", "line", 1).
  // - Call wf.addConstraint("T1", "POLYGON((0 0, 10 0, 10 10, 0 10, 0 0))", "polygon", 2).
  // - Verify manifest declaration constraints.T1 has source <gpkg>|layername=constraints|subset=horizon='T1'.
  // - Check QTemporaryDir recreation / lifecycle:
  //   - Keep directory (dir.setAutoRemove(false)), destroy the C++ stack objects.
  //   - Construct new stack objects (projectSvc, manifest, store, layers, proc, newWf) on the same dir.
  //   - Call newWf.loadConstraints("T1"): verify it retrieves the 2 constraints.
  //   - Remove one constraint (via store or cs remove("c-1")).
  //   - Verify gpkg now only has 1 constraint.
  //   - Verify QGIS layer service can instantiate constraints.T1 as a valid vector layer.
  void testRealStackEndToEndAndLifecycle()
  {
    QString tempDir;
    QString expectedSource;

    // Phase 1: Real stack end-to-end test
    {
      QTemporaryDir dir;
      QVERIFY( dir.isValid() );
      dir.setAutoRemove( false );
      tempDir = dir.path();

      const QString qgz = dir.filePath( QStringLiteral( "proj.qgz" ) );
      const QString gpkg = dir.filePath( QStringLiteral( "project.gpkg" ) );
      const QString meta = dir.filePath( QStringLiteral( "project.sqlite" ) );

      QgisProjectService projectSvc;
      PaleoProjectStore store;
      LayerManifest manifest{ meta };
      QgisLayerService layers{ &projectSvc, &manifest };
      QgisProcessingService proc{ &store };

      QVERIFY( projectSvc.createProject( qgz ) );
      QVERIFY( manifest.open() );
      store.setProjectPaths( qgz, gpkg, meta );

      ConstraintWorkflow wf( &proc, &layers );
      wf.setStore( &store );

      QSignalSpy spy( &wf, &ConstraintWorkflow::constraintAdded );

      QString err;
      QString id1, id2;
      QVERIFY2( wf.addConstraint( QStringLiteral( "T1" ), QStringLiteral( "LINESTRING(0 0, 10 0)" ),
                                  QStringLiteral( "line" ), 1, &err, &id1 ), qPrintable( err ) );
      QCOMPARE( spy.count(), 1 );
      QCOMPARE( id1, QStringLiteral( "c-1" ) );

      QVERIFY2( wf.addConstraint( QStringLiteral( "T1" ), QStringLiteral( "POLYGON((0 0, 10 0, 10 10, 0 10, 0 0))" ),
                                  QStringLiteral( "polygon" ), 2, &err, &id2 ), qPrintable( err ) );
      QCOMPARE( spy.count(), 2 );
      QCOMPARE( id2, QStringLiteral( "c-2" ) );

      // Verify manifest declaration constraints.T1 has source <gpkg>|layername=constraints|subset=horizon='T1'
      expectedSource = QStringLiteral( "%1|layername=constraints|subset=horizon='T1'" ).arg( gpkg );
      const QVector<LayerDeclaration> decls = manifest.all();
      bool found = false;
      for ( const LayerDeclaration &d : decls )
      {
        if ( d.layerId == QStringLiteral( "constraints.T1" ) )
        {
          found = true;
          QCOMPARE( d.source, expectedSource );
          QCOMPARE( d.horizon, QStringLiteral( "T1" ) );
          QCOMPARE( d.type, QStringLiteral( "vector" ) );
          QCOMPARE( d.group, QStringLiteral( "02_Constraints" ) );
          break;
        }
      }
      QVERIFY2( found, "Declaration constraints.T1 not found in manifest" );
    }
    // C++ stack objects destroyed here

    // Phase 2: Lifecycle check with new stack objects on the same dir
    {
      const QString qgz = QDir( tempDir ).filePath( QStringLiteral( "proj.qgz" ) );
      const QString gpkg = QDir( tempDir ).filePath( QStringLiteral( "project.gpkg" ) );
      const QString meta = QDir( tempDir ).filePath( QStringLiteral( "project.sqlite" ) );

      QgisProjectService projectSvc;
      PaleoProjectStore store;
      LayerManifest manifest{ meta };
      QgisLayerService layers{ &projectSvc, &manifest };
      QgisProcessingService proc{ &store };

      QVERIFY( projectSvc.openProject( qgz ) );
      QVERIFY( manifest.open() );
      store.setProjectPaths( qgz, gpkg, meta );

      ConstraintWorkflow newWf( &proc, &layers );
      newWf.setStore( &store );

      // Call newWf.loadConstraints("T1"): verify it retrieves the 2 constraints
      QVector<QVariantMap> loaded = newWf.loadConstraints( QStringLiteral( "T1" ) );
      QCOMPARE( loaded.size(), 2 );
      QCOMPARE( loaded[0].value( QStringLiteral( "id" ) ).toString(), QStringLiteral( "c-1" ) );
      QCOMPARE( loaded[0].value( QStringLiteral( "horizon" ) ).toString(), QStringLiteral( "T1" ) );
      QCOMPARE( loaded[1].value( QStringLiteral( "id" ) ).toString(), QStringLiteral( "c-2" ) );
      QCOMPARE( loaded[1].value( QStringLiteral( "horizon" ) ).toString(), QStringLiteral( "T1" ) );

      // Remove one constraint (via store or cs remove("c-1"))
      ConstraintStore cs( gpkg, &store );
      QString err;
      QVERIFY2( cs.remove( QStringLiteral( "c-1" ), &err ), qPrintable( err ) );

      // Verify gpkg now only has 1 constraint
      QVector<QVariantMap> remaining = cs.load();
      QCOMPARE( remaining.size(), 1 );
      QCOMPARE( remaining[0].value( QStringLiteral( "id" ) ).toString(), QStringLiteral( "c-2" ) );

      // Verify QGIS layer service can instantiate constraints.T1 as a valid vector layer
      QgsMapLayer *layer = layers.instantiate( QStringLiteral( "constraints.T1" ), &err );
      QVERIFY2( layer != nullptr, qPrintable( err ) );
      QVERIFY( layer->isValid() );
      QgsVectorLayer *vlayer = qobject_cast<QgsVectorLayer *>( layer );
      QVERIFY( vlayer != nullptr );
      QCOMPARE( vlayer->featureCount(), 1 );
    }

    // Clean up temporary directory
    QDir( tempDir ).removeRecursively();
  }
};

int main( int argc, char *argv[] )
{
  QgsApplication app( argc, argv, false );
  app.setPrefixPath(qEnvironmentVariable("QGIS_PREFIX_PATH", QStringLiteral("/usr")), true);
  app.initQgis();
  QgsApplication::processingRegistry();
  GDALAllRegister();
  TestConstraintStore tc;
  const int rc = QTest::qExec( &tc, argc, argv );
  QgsApplication::exitQgis();
  return rc;
}

#include "tst_constraintstore.moc"
