#include <QtTest>
#include <QJsonDocument>
#include <QJsonObject>
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
          QCOMPARE( d.group, QStringLiteral( "03_Constraints" ) );
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

  // ---- MEM-01: PaleoProjectStore 析构弱引用安全（QPointer 防 UAF）----
  void testProjectStoreDestructionWeakRefSafety()
  {
    QTemporaryDir dir;
    QVERIFY( dir.isValid() );
    const QString gpkgPath = dir.filePath( QStringLiteral( "test_weakref.gpkg" ) );

    auto projectStore = std::make_unique<PaleoProjectStore>();
    ConstraintStore store( gpkgPath, projectStore.get() );

    QString err;
    // When projectStore is alive, append works normally
    QVERIFY( store.append( QStringLiteral( "T1" ), QStringLiteral( "c-1" ),
                           QStringLiteral( "LINESTRING(0 0, 10 0)" ),
                           QStringLiteral( "line" ), 1, &err ) );

    // Now destroy the projectStore (simulates project close / unload)
    projectStore.reset();

    // Subsequent write operations must fail safely without crashing (UAF prevented by QPointer)
    err.clear();
    const bool appendResult = store.append( QStringLiteral( "T1" ), QStringLiteral( "c-2" ),
                                           QStringLiteral( "LINESTRING(10 0, 20 0)" ),
                                           QStringLiteral( "line" ), 1, &err );
    QVERIFY( !appendResult );
    QCOMPARE( err, QStringLiteral( "PaleoProjectStore destroyed or unavailable" ) );

    err.clear();
    const bool removeResult = store.remove( QStringLiteral( "c-1" ), &err );
    QVERIFY( !removeResult );
    QCOMPARE( err, QStringLiteral( "PaleoProjectStore destroyed or unavailable" ) );
  }

  // ---- MEM-01: ConstraintWorkflow 类型化生命周期管理与无动态属性走私 ----
  void testConstraintWorkflowTypedOwnershipAndLifecycle()
  {
    QTemporaryDir dir;
    QVERIFY( dir.isValid() );
    const QString qgz = dir.filePath( QStringLiteral( "project.qgz" ) );
    const QString gpkg = dir.filePath( QStringLiteral( "constraints.gpkg" ) );
    const QString meta = dir.filePath( QStringLiteral( "meta.json" ) );

    ConstraintWorkflow wf( nullptr, nullptr );
    QVERIFY( wf.constraintStore() == nullptr );

    // Scope block with PaleoProjectStore
    {
      PaleoProjectStore store;
      store.setProjectPaths( qgz, gpkg, meta );
      wf.setStore( &store );

      QVERIFY( wf.constraintStore() != nullptr );
      QCOMPARE( wf.constraintStore()->gpkgPath(), gpkg );

      // Assert no dynamic properties are used for pointer smuggling
      QVERIFY( !wf.property( "paleo.wf.constraintstore" ).isValid() );
      QVERIFY( !wf.property( "paleo.wf.owned_constraintstore" ).isValid() );
    }

    // store is now destroyed; constraintStore() must gracefully return nullptr
    QVERIFY( wf.constraintStore() == nullptr );

    // External store override
    ConstraintStore externalStore( gpkg, static_cast<PaleoProjectStore *>( nullptr ) );
    wf.setConstraintStore( &externalStore );
    QCOMPARE( wf.constraintStore(), &externalStore );

    wf.setConstraintStore( nullptr );
    QVERIFY( wf.constraintStore() == nullptr );
  }

  void testTypedLineParamsSurviveReopen()
  {
    QTemporaryDir dir;
    QVERIFY( dir.isValid() );
    const QString qgz = dir.filePath( QStringLiteral( "proj.qgz" ) );
    const QString gpkg = dir.filePath( QStringLiteral( "project.gpkg" ) );
    const QString meta = dir.filePath( QStringLiteral( "project.sqlite" ) );

    QString savedId;
    {
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
      QString err;
      QVariantMap soft;
      soft.insert( QStringLiteral( "semantic" ), QStringLiteral( "interpretive_boundary" ) );
      soft.insert( QStringLiteral( "softStrength" ), 0.5 );
      soft.insert( QStringLiteral( "ratio" ), 4.0 );
      QVERIFY2( wf.addConstraint( QStringLiteral( "T1" ), QStringLiteral( "LINESTRING(0 0, 10 0)" ),
                                  QStringLiteral( "interpretive_boundary" ), 3, &err, &savedId, soft ),
                qPrintable( err ) );
      QVariantMap plain;
      QString plainId;
      QVERIFY2( wf.addConstraint( QStringLiteral( "T1" ), QStringLiteral( "LINESTRING(0 0, 0 10)" ),
                                  QStringLiteral( "line" ), 1, &err, &plainId, plain ),
                qPrintable( err ) );
      const QVector<QVariantMap> fresh = wf.loadConstraints( QStringLiteral( "T1" ) );
      QCOMPARE( fresh.size(), 2 );
      const QVariantMap typed = fresh.at( 0 );
      QCOMPARE( typed.value( QStringLiteral( "type" ) ).toString(), QStringLiteral( "interpretive_boundary" ) );
      const QJsonObject json = QJsonDocument::fromJson( typed.value( QStringLiteral( "params_json" ) ).toString().toUtf8() ).object();
      QCOMPARE( json.value( QStringLiteral( "semantic" ) ).toString(), QStringLiteral( "interpretive_boundary" ) );
      QCOMPARE( json.value( QStringLiteral( "softStrength" ) ).toDouble(), 0.5 );
      QVERIFY( fresh.at( 1 ).value( QStringLiteral( "params_json" ) ).toString().isEmpty() );

      QVariantMap edited;
      edited.insert( QStringLiteral( "semantic" ), QStringLiteral( "direction_guide" ) );
      edited.insert( QStringLiteral( "ratio" ), 12.0 );
      QVERIFY2( wf.updateConstraintLine( savedId, edited, &err ), qPrintable( err ) );
    }

    QgisProjectService projectSvc;
    PaleoProjectStore store;
    LayerManifest manifest{ meta };
    QgisLayerService layers{ &projectSvc, &manifest };
    QgisProcessingService proc{ &store };
    QVERIFY( projectSvc.openProject( qgz ) );
    QVERIFY( manifest.open() );
    store.setProjectPaths( qgz, gpkg, meta );
    ConstraintWorkflow reopened( &proc, &layers );
    reopened.setStore( &store );
    const QVector<QVariantMap> loaded = reopened.loadConstraints( QStringLiteral( "T1" ) );
    QCOMPARE( loaded.size(), 2 );
    bool found = false;
    for ( const QVariantMap &row : loaded )
    {
      if ( row.value( QStringLiteral( "id" ) ).toString() != savedId )
        continue;
      found = true;
      QCOMPARE( row.value( QStringLiteral( "type" ) ).toString(), QStringLiteral( "direction_line" ) );
      const QJsonObject json = QJsonDocument::fromJson( row.value( QStringLiteral( "params_json" ) ).toString().toUtf8() ).object();
      QCOMPARE( json.value( QStringLiteral( "semantic" ) ).toString(), QStringLiteral( "direction_guide" ) );
      QCOMPARE( json.value( QStringLiteral( "ratio" ) ).toDouble(), 12.0 );
      QCOMPARE( json.value( QStringLiteral( "schemaVersion" ) ).toInt(), 1 );
    }
    QVERIFY( found );
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
