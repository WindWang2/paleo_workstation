#include <QtTest>
#include <QSignalSpy>
#include <QKeyEvent>

#include <qgsapplication.h>
#include <qgsgeometry.h>
#include <qgslinestring.h>
#include <qgsmapcanvas.h>
#include <qgsmapmouseevent.h>
#include <qgsmaptoolcapture.h>
#include <qgsrectangle.h>

#include "../src/ui/maptools/paleomaptools.h"

// Exposes the protected capture internals so the white-box assertions can
// inspect state and drive lineCaptured() directly (QGIS hands ownership of the
// released curve to the callee, hence the heap pointer in deliverLine()).
class TestConstraintTool : public PaleoDrawConstraintTool
{
  public:
    using PaleoDrawConstraintTool::PaleoDrawConstraintTool;
    using PaleoDrawConstraintTool::keyPressEvent; // protected → public for injection
    bool capturing() const { return isCapturing(); }
    int vertexCount() { return size(); } // base size() is non-const
    void deliverLine( const QgsCurve *line ) { lineCaptured( line ); }
};

class TestMapTools : public QObject
{
  Q_OBJECT
private slots:
  void construction();
  void activationRoundtrip();
  void lineCommitEmitsWkt();
  void escAbortsAndDeactivates();
  void rightClickCancelsBeforeTwoVertices();
  void directLineCapturedFallback();
};

// Fresh offscreen canvas. NB: QgsMapCanvas magnifies the requested extent by
// the screen-DPI factor, so pixel→map math must go through getCoordinateTransform().
static void configureCanvas( QgsMapCanvas &canvas )
{
  canvas.setDestinationCrs( QgsCoordinateReferenceSystem( QStringLiteral( "EPSG:4326" ) ) );
  canvas.resize( 200, 200 );
  canvas.setExtent( QgsRectangle( 0, 0, 100, 100 ) );
  canvas.refresh();
}

static void click( QgsMapTool &tool, QgsMapCanvas &canvas, const QPoint &px, const Qt::MouseButton button )
{
  QgsMapMouseEvent press( &canvas, QEvent::MouseButtonPress, px, button, button, Qt::NoModifier );
  QgsMapMouseEvent release( &canvas, QEvent::MouseButtonRelease, px, button, Qt::NoButton, Qt::NoModifier );
  tool.canvasPressEvent( &press );
  tool.canvasReleaseEvent( &release );
}

void TestMapTools::construction()
{
  QgsMapCanvas canvas;
  configureCanvas( canvas );

  PaleoDrawConstraintTool tool( &canvas );
  QCOMPARE( tool.mode(), QgsMapToolCapture::CaptureLine );
  QCOMPARE( tool.currentCaptureTechnique(), Qgis::CaptureTechnique::StraightSegments );
  QVERIFY( tool.cadDockWidget() ); // base asserts a non-null dock
  QVERIFY( !tool.toolName().isEmpty() );
  QVERIFY( tool.flags() & QgsMapTool::EditTool );
}

void TestMapTools::activationRoundtrip()
{
  QgsMapCanvas canvas;
  configureCanvas( canvas );

  TestConstraintTool tool( &canvas );
  QCOMPARE( canvas.mapTool(), nullptr );

  canvas.setMapTool( &tool );
  QCOMPARE( canvas.mapTool(), static_cast<QgsMapTool *>( &tool ) );
  QVERIFY( tool.capturing() ); // armed on activate()

  canvas.unsetMapTool( &tool );
  QCOMPARE( canvas.mapTool(), nullptr );
  QVERIFY( !tool.capturing() ); // capture state cleaned on deactivate()
}

void TestMapTools::lineCommitEmitsWkt()
{
  QgsMapCanvas canvas;
  configureCanvas( canvas );

  TestConstraintTool tool( &canvas );
  canvas.setMapTool( &tool );
  QSignalSpy spy( &tool, &PaleoDrawConstraintTool::constraintDrawn );
  QSignalSpy abortSpy( &tool, &PaleoDrawConstraintTool::drawAborted );

  // Expected map coords come from the canvas transform itself — the canvas
  // magnifies the requested extent by the (offscreen) screen DPI factor.
  const QgsPointXY p1 = canvas.getCoordinateTransform()->toMapCoordinates( 20, 160 );
  const QgsPointXY p2 = canvas.getCoordinateTransform()->toMapCoordinates( 140, 60 );

  click( tool, canvas, QPoint( 20, 160 ), Qt::LeftButton );
  QCOMPARE( tool.vertexCount(), 1 );
  click( tool, canvas, QPoint( 140, 60 ), Qt::LeftButton );
  QCOMPARE( tool.vertexCount(), 2 );
  click( tool, canvas, QPoint( 140, 60 ), Qt::RightButton ); // commit

  QCOMPARE( spy.count(), 1 );
  QCOMPARE( abortSpy.count(), 0 );

  const QString wkt = spy.at( 0 ).at( 0 ).toString();
  QVERIFY( wkt.startsWith( QLatin1String( "LineString" ) ) );

  const QgsGeometry g = QgsGeometry::fromWkt( wkt );
  QVERIFY( !g.isNull() );
  const QgsLineString *ls = qgsgeometry_cast<const QgsLineString *>( g.constGet() );
  QVERIFY( ls );
  QCOMPARE( ls->numPoints(), 2 );
  QVERIFY( qgsDoubleNear( ls->xAt( 0 ), p1.x(), 1e-6 ) );
  QVERIFY( qgsDoubleNear( ls->yAt( 0 ), p1.y(), 1e-6 ) );
  QVERIFY( qgsDoubleNear( ls->xAt( 1 ), p2.x(), 1e-6 ) );
  QVERIFY( qgsDoubleNear( ls->yAt( 1 ), p2.y(), 1e-6 ) );

  QVERIFY( !tool.capturing() );  // state cleaned after commit
  QCOMPARE( tool.vertexCount(), 0 );

  canvas.unsetMapTool( &tool );
}

void TestMapTools::escAbortsAndDeactivates()
{
  QgsMapCanvas canvas;
  configureCanvas( canvas );

  TestConstraintTool tool( &canvas );
  canvas.setMapTool( &tool );
  QSignalSpy abortSpy( &tool, &PaleoDrawConstraintTool::drawAborted );
  QSignalSpy commitSpy( &tool, &PaleoDrawConstraintTool::constraintDrawn );

  // §42.15: abort signal drives the deactivate path.
  QObject::connect( &tool, &PaleoDrawConstraintTool::drawAborted,
                    &canvas, [&canvas, &tool] { canvas.unsetMapTool( &tool ); } );

  click( tool, canvas, QPoint( 40, 100 ), Qt::LeftButton ); // 1 vertex in flight
  QVERIFY( tool.capturing() );

  QKeyEvent esc( QEvent::KeyPress, Qt::Key_Escape, Qt::NoModifier );
  tool.keyPressEvent( &esc );

  QCOMPARE( abortSpy.count(), 1 );
  QCOMPARE( commitSpy.count(), 0 );
  QVERIFY( !tool.capturing() );
  QCOMPARE( tool.vertexCount(), 0 );
  QCOMPARE( canvas.mapTool(), nullptr ); // deactivated via drawAborted
}

void TestMapTools::rightClickCancelsBeforeTwoVertices()
{
  QgsMapCanvas canvas;
  configureCanvas( canvas );

  TestConstraintTool tool( &canvas );
  canvas.setMapTool( &tool );
  QSignalSpy abortSpy( &tool, &PaleoDrawConstraintTool::drawAborted );
  QSignalSpy commitSpy( &tool, &PaleoDrawConstraintTool::constraintDrawn );

  // right-click with nothing drawn → cancel gesture, not a commit
  click( tool, canvas, QPoint( 50, 50 ), Qt::RightButton );
  QCOMPARE( abortSpy.count(), 1 );
  QCOMPARE( commitSpy.count(), 0 );
  QVERIFY( !tool.capturing() );

  // re-arm, one vertex, right-click → still cancel (degenerate line refused)
  tool.activate();
  click( tool, canvas, QPoint( 60, 60 ), Qt::LeftButton );
  QCOMPARE( tool.vertexCount(), 1 );
  click( tool, canvas, QPoint( 60, 60 ), Qt::RightButton );
  QCOMPARE( abortSpy.count(), 2 );
  QCOMPARE( commitSpy.count(), 0 );

  canvas.unsetMapTool( &tool );
}

void TestMapTools::directLineCapturedFallback()
{
  QgsMapCanvas canvas;
  configureCanvas( canvas );

  TestConstraintTool tool( &canvas );
  QSignalSpy spy( &tool, &PaleoDrawConstraintTool::constraintDrawn );

  // Deterministic path independent of event injection: callee owns the curve
  // (base invokes lineCaptured(curveToAdd.release())), so hand over a heap object.
  tool.deliverLine( new QgsLineString( QVector<QgsPoint> { QgsPoint( 1, 2 ), QgsPoint( 3, 4 ) } ) );

  QCOMPARE( spy.count(), 1 );
  QCOMPARE( spy.at( 0 ).at( 0 ).toString(), QStringLiteral( "LineString (1 2, 3 4)" ) );
}

int main( int argc, char *argv[] )
{
  QgsApplication app( argc, argv, false );
  app.setPrefixPath( QStringLiteral( "/usr" ), true ); // distro install
  app.initQgis();
  TestMapTools tc;
  const int rc = QTest::qExec( &tc, argc, argv );
  QgsApplication::exitQgis();
  return rc;
}

#include "tst_maptools.moc"
