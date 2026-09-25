#include <QtTest>
#include <QSignalSpy>
#include <qgsapplication.h>
#include <qgsmapcanvas.h>
#include <qgsmaptool.h>
#include <qgsmaptoolpan.h>
#include <qgsmapsettings.h>
#include <qgsrectangle.h>

#include "../src/qgis/qgiscanvascontroller.h"
#include "../src/services/toolavailability.h"
#include "../src/metadata/paleoprojectstore.h"

// PaleoProjectStore real impl comes from paleo_core.

class TestCanvasTools : public QObject
{
  Q_OBJECT
private slots:
  void canvasIsUsableOffscreen();
  void mapToolRoundtrip();
  void broadcastGuardCoalesces();
  void toolGating();
};

// (a) canvas() lazily creates a usable QgsMapCanvas offscreen
void TestCanvasTools::canvasIsUsableOffscreen()
{
  QgisCanvasController ctl;
  QgsMapCanvas *c = ctl.canvas();
  QVERIFY( c );
  QCOMPARE( ctl.canvas(), c ); // lazy-once: same instance on repeat access
  QVERIFY( c->mapSettings().flags().testFlag( Qgis::MapSettingsFlag::Antialiasing ) );
  QCOMPARE( c->canvasColor(), QColor( QStringLiteral( "#FFFFFF" ) ) );

  c->setExtent( QgsRectangle( 0, 0, 100, 100 ) );
  c->refresh();
  QCoreApplication::processEvents();
  QVERIFY( c->scale() > 0 );

  ctl.zoomToFullExtent(); // no layers → must not crash or wedge
}

// (b) setMapTool/deactivateTool roundtrip
void TestCanvasTools::mapToolRoundtrip()
{
  QgisCanvasController ctl;
  QgsMapCanvas *c = ctl.canvas();
  QCOMPARE( ctl.activeTool(), nullptr );

  QgsMapToolPan *pan = new QgsMapToolPan( c );
  ctl.setMapTool( pan );
  QCOMPARE( ctl.activeTool(), static_cast<QgsMapTool *>( pan ) );
  QCOMPARE( c->mapTool(), static_cast<QgsMapTool *>( pan ) );

  ctl.deactivateTool(); // §42.15 Esc path
  QCOMPARE( ctl.activeTool(), nullptr );
  QCOMPARE( c->mapTool(), nullptr );

  // nullptr-safe: must not crash, must not wedge tracked state
  ctl.setMapTool( nullptr );
  QCOMPARE( ctl.activeTool(), nullptr );

  delete pan;
}

// (c) §41.3 broadcast guard: nested begin/end balances, swallows echoes,
// fires exactly one coalesced re-broadcast at settle. No deadlock.
void TestCanvasTools::broadcastGuardCoalesces()
{
  QgisCanvasController ctl;
  QSignalSpy spy( &ctl, &QgisCanvasController::selectionBroadcast );
  QVERIFY( !ctl.broadcasting() );

  ctl.beginSelectionBroadcast();
  QVERIFY( ctl.broadcasting() );
  ctl.beginSelectionBroadcast(); // nested — echo swallowed, coalesced
  QVERIFY( ctl.broadcasting() );
  ctl.endSelectionBroadcast();   // inner end — outer window still in flight
  QVERIFY( ctl.broadcasting() );
  ctl.endSelectionBroadcast();   // settle → single coalesced re-broadcast
  QVERIFY( !ctl.broadcasting() );
  QCOMPARE( spy.count(), 1 );

  // Clean window: no nested echo → no re-broadcast owed
  spy.clear();
  ctl.beginSelectionBroadcast();
  QVERIFY( ctl.broadcasting() );
  ctl.endSelectionBroadcast();
  QVERIFY( !ctl.broadcasting() );
  QCOMPARE( spy.count(), 0 );
}

// (d) ToolAvailabilityService: global gate → per-layer busy → free (§35)
void TestCanvasTools::toolGating()
{
  PaleoProjectStore store;
  ToolAvailabilityService svc( &store );
  QSignalSpy spy( &svc, &ToolAvailabilityService::availabilityChanged );
  QString reason;

  // open gate + free layer → allowed
  QVERIFY2( svc.check( QStringLiteral( "layer_a" ), &reason ), qPrintable( reason ) );

  // global gate closed → denied with the gate's reason
  svc.setGlobalGate( false, QStringLiteral( "no project open" ) );
  QVERIFY( !svc.check( QStringLiteral( "layer_a" ), &reason ) );
  QVERIFY( reason.contains( QStringLiteral( "no project" ) ) );
  svc.setGlobalGate( true, QString() );

  // busy layer → denied, reason names the owning task
  svc.noteTaskOnLayer( QStringLiteral( "layer_a" ), QStringLiteral( "gridding-7" ),
                       QStringLiteral( "IDW interpolation" ) );
  QVERIFY( !svc.check( QStringLiteral( "layer_a" ), &reason ) );
  QVERIFY2( reason.contains( QStringLiteral( "gridding-7" ) ), qPrintable( reason ) );
  // sibling layer unaffected
  QVERIFY2( svc.check( QStringLiteral( "layer_b" ), &reason ), qPrintable( reason ) );

  // freed → allowed again
  svc.clearTaskOnLayer( QStringLiteral( "layer_a" ) );
  QVERIFY2( svc.check( QStringLiteral( "layer_a" ), &reason ), qPrintable( reason ) );

  // gate off, gate on, busy, free → one signal each
  QCOMPARE( spy.count(), 4 );
}

int main( int argc, char *argv[] )
{
  QgsApplication app( argc, argv, false );
  app.setPrefixPath( QStringLiteral( "/usr" ), true ); // distro install
  app.initQgis();
  TestCanvasTools tc;
  const int rc = QTest::qExec( &tc, argc, argv );
  QgsApplication::exitQgis();
  return rc;
}

#include "tst_canvas_tools.moc"
