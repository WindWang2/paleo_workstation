#include <algorithm>

#include <QtTest>
#include <QSignalSpy>
#include <QKeyEvent>

#include <qgsapplication.h>
#include <qgscurvepolygon.h>
#include <qgsgeometry.h>
#include <qgslinestring.h>
#include <qgsmapcanvas.h>
#include <qgsmapmouseevent.h>
#include <qgsmaptoolcapture.h>
#include <qgspolygon.h>
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

// Same white-box shims for the polygon/rect tools. NB: unlike lineCaptured
// (ownership transfer), the base invokes polygonCaptured( poly.get() ) — a
// borrowed pointer — so deliverPolygon() hands over a stack object.
class TestPolygonTool : public PaleoDrawPolygonTool
{
  public:
    using PaleoDrawPolygonTool::PaleoDrawPolygonTool;
    using PaleoDrawPolygonTool::keyPressEvent; // protected → public for injection
    bool capturing() const { return isCapturing(); }
    int vertexCount() { return size(); } // base size() is non-const
    void deliverPolygon( const QgsCurvePolygon *polygon ) { polygonCaptured( polygon ); }
};

class TestRectTool : public PaleoDrawRectTool
{
  public:
    using PaleoDrawRectTool::PaleoDrawRectTool;
    using PaleoDrawRectTool::keyPressEvent; // protected → public for injection
    bool capturing() const { return isCapturing(); }
    int vertexCount() { return size(); } // base size() is non-const
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
  void polygonCommitEmitsWkt();
  void polygonAbortsBelowThreeVertices();
  void directPolygonCapturedFallback();
  void rectTwoCornersEmitAxisAlignedWkt();
  void rectAbortSemantics();
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

void TestMapTools::polygonCommitEmitsWkt()
{
  QgsMapCanvas canvas;
  configureCanvas( canvas );

  TestPolygonTool tool( &canvas );
  QCOMPARE( tool.mode(), QgsMapToolCapture::CapturePolygon );
  QCOMPARE( tool.currentCaptureTechnique(), Qgis::CaptureTechnique::StraightSegments );
  canvas.setMapTool( &tool );
  QSignalSpy spy( &tool, &PaleoDrawPolygonTool::constraintDrawn );
  QSignalSpy abortSpy( &tool, &PaleoDrawPolygonTool::drawAborted );

  const QgsPointXY p1 = canvas.getCoordinateTransform()->toMapCoordinates( 20, 160 );
  const QgsPointXY p2 = canvas.getCoordinateTransform()->toMapCoordinates( 140, 60 );
  const QgsPointXY p3 = canvas.getCoordinateTransform()->toMapCoordinates( 60, 20 );

  click( tool, canvas, QPoint( 20, 160 ), Qt::LeftButton );
  QCOMPARE( tool.vertexCount(), 1 );
  click( tool, canvas, QPoint( 140, 60 ), Qt::LeftButton );
  QCOMPARE( tool.vertexCount(), 2 );
  click( tool, canvas, QPoint( 60, 20 ), Qt::LeftButton );
  QCOMPARE( tool.vertexCount(), 3 );
  click( tool, canvas, QPoint( 60, 20 ), Qt::RightButton ); // commit (≥3 vertices)

  QCOMPARE( spy.count(), 1 );
  QCOMPARE( abortSpy.count(), 0 );

  const QString wkt = spy.at( 0 ).at( 0 ).toString();
  QVERIFY( wkt.startsWith( QLatin1String( "Polygon ((" ) ) );

  const QgsGeometry g = QgsGeometry::fromWkt( wkt );
  QVERIFY( !g.isNull() );
  const QgsPolygon *poly = qgsgeometry_cast<const QgsPolygon *>( g.constGet() );
  QVERIFY( poly );
  const QgsLineString *ring = qgsgeometry_cast<const QgsLineString *>( poly->exteriorRing() );
  QVERIFY( ring );
  QCOMPARE( ring->numPoints(), 4 ); // 3 captured vertices + closing point
  QVERIFY( ring->isClosed() );
  QVERIFY( qgsDoubleNear( ring->xAt( 0 ), p1.x(), 1e-6 ) );
  QVERIFY( qgsDoubleNear( ring->yAt( 0 ), p1.y(), 1e-6 ) );
  QVERIFY( qgsDoubleNear( ring->xAt( 1 ), p2.x(), 1e-6 ) );
  QVERIFY( qgsDoubleNear( ring->yAt( 1 ), p2.y(), 1e-6 ) );
  QVERIFY( qgsDoubleNear( ring->xAt( 2 ), p3.x(), 1e-6 ) );
  QVERIFY( qgsDoubleNear( ring->yAt( 2 ), p3.y(), 1e-6 ) );
  QVERIFY( qgsDoubleNear( ring->xAt( 3 ), p1.x(), 1e-6 ) ); // closed back to p1
  QVERIFY( qgsDoubleNear( ring->yAt( 3 ), p1.y(), 1e-6 ) );

  QVERIFY( !tool.capturing() );  // state cleaned after commit
  QCOMPARE( tool.vertexCount(), 0 );

  canvas.unsetMapTool( &tool );
}

void TestMapTools::polygonAbortsBelowThreeVertices()
{
  QgsMapCanvas canvas;
  configureCanvas( canvas );

  TestPolygonTool tool( &canvas );
  canvas.setMapTool( &tool );
  QSignalSpy abortSpy( &tool, &PaleoDrawPolygonTool::drawAborted );
  QSignalSpy commitSpy( &tool, &PaleoDrawPolygonTool::constraintDrawn );

  // right-click with nothing drawn → cancel gesture, not a commit
  click( tool, canvas, QPoint( 50, 50 ), Qt::RightButton );
  QCOMPARE( abortSpy.count(), 1 );
  QCOMPARE( commitSpy.count(), 0 );
  QVERIFY( !tool.capturing() );

  // re-arm, two vertices, right-click → still cancel (degenerate polygon refused)
  tool.activate();
  click( tool, canvas, QPoint( 60, 60 ), Qt::LeftButton );
  click( tool, canvas, QPoint( 80, 80 ), Qt::LeftButton );
  QCOMPARE( tool.vertexCount(), 2 );
  click( tool, canvas, QPoint( 80, 80 ), Qt::RightButton );
  QCOMPARE( abortSpy.count(), 2 );
  QCOMPARE( commitSpy.count(), 0 );
  QVERIFY( !tool.capturing() );

  // Esc mid-capture aborts too (§42.15 semantics, same as the line tool)
  tool.activate();
  click( tool, canvas, QPoint( 40, 40 ), Qt::LeftButton );
  QVERIFY( tool.capturing() );
  QKeyEvent esc( QEvent::KeyPress, Qt::Key_Escape, Qt::NoModifier );
  tool.keyPressEvent( &esc );
  QCOMPARE( abortSpy.count(), 3 );
  QCOMPARE( commitSpy.count(), 0 );
  QVERIFY( !tool.capturing() );

  canvas.unsetMapTool( &tool );
}

void TestMapTools::directPolygonCapturedFallback()
{
  QgsMapCanvas canvas;
  configureCanvas( canvas );

  TestPolygonTool tool( &canvas );
  QSignalSpy spy( &tool, &PaleoDrawPolygonTool::constraintDrawn );

  // Deterministic path independent of event injection: the callee receives a
  // borrowed pointer (base calls polygonCaptured(poly.get())), so a stack
  // object is fine — the tool clones it.
  QgsCurvePolygon poly;
  poly.setExteriorRing( new QgsLineString( QVector<QgsPoint> {
      QgsPoint( 0, 0 ), QgsPoint( 10, 0 ), QgsPoint( 10, 10 ), QgsPoint( 0, 0 ) } ) );
  tool.deliverPolygon( &poly );

  QCOMPARE( spy.count(), 1 );
  QCOMPARE( spy.at( 0 ).at( 0 ).toString(), QStringLiteral( "Polygon ((0 0, 10 0, 10 10, 0 0))" ) );
}

void TestMapTools::rectTwoCornersEmitAxisAlignedWkt()
{
  QgsMapCanvas canvas;
  configureCanvas( canvas );

  TestRectTool tool( &canvas );
  QCOMPARE( tool.mode(), QgsMapToolCapture::CapturePolygon );
  canvas.setMapTool( &tool );
  QSignalSpy spy( &tool, &PaleoDrawRectTool::constraintDrawn );
  QSignalSpy abortSpy( &tool, &PaleoDrawRectTool::drawAborted );

  const QgsPointXY c1 = canvas.getCoordinateTransform()->toMapCoordinates( 20, 160 );
  const QgsPointXY c2 = canvas.getCoordinateTransform()->toMapCoordinates( 140, 60 );

  // path A: two left-clicked corner points
  click( tool, canvas, QPoint( 20, 160 ), Qt::LeftButton );
  QCOMPARE( tool.vertexCount(), 1 );
  QCOMPARE( spy.count(), 0 ); // one corner is not a rectangle yet
  click( tool, canvas, QPoint( 140, 60 ), Qt::LeftButton ); // second corner commits

  QCOMPARE( spy.count(), 1 );
  QCOMPARE( abortSpy.count(), 0 );

  const QString wkt = spy.at( 0 ).at( 0 ).toString();
  QVERIFY( wkt.startsWith( QLatin1String( "Polygon ((" ) ) );

  const QgsGeometry g = QgsGeometry::fromWkt( wkt );
  QVERIFY( !g.isNull() );
  const QgsPolygon *poly = qgsgeometry_cast<const QgsPolygon *>( g.constGet() );
  QVERIFY( poly );
  const QgsLineString *ring = qgsgeometry_cast<const QgsLineString *>( poly->exteriorRing() );
  QVERIFY( ring );
  QCOMPARE( ring->numPoints(), 5 );
  QVERIFY( ring->isClosed() );

  const double xmin = std::min( c1.x(), c2.x() );
  const double xmax = std::max( c1.x(), c2.x() );
  const double ymin = std::min( c1.y(), c2.y() );
  const double ymax = std::max( c1.y(), c2.y() );
  // ring order: (xmin ymin) → (xmax ymin) → (xmax ymax) → (xmin ymax) → closed
  QVERIFY( qgsDoubleNear( ring->xAt( 0 ), xmin, 1e-6 ) );
  QVERIFY( qgsDoubleNear( ring->yAt( 0 ), ymin, 1e-6 ) );
  QVERIFY( qgsDoubleNear( ring->xAt( 1 ), xmax, 1e-6 ) );
  QVERIFY( qgsDoubleNear( ring->yAt( 1 ), ymin, 1e-6 ) );
  QVERIFY( qgsDoubleNear( ring->xAt( 2 ), xmax, 1e-6 ) );
  QVERIFY( qgsDoubleNear( ring->yAt( 2 ), ymax, 1e-6 ) );
  QVERIFY( qgsDoubleNear( ring->xAt( 3 ), xmin, 1e-6 ) );
  QVERIFY( qgsDoubleNear( ring->yAt( 3 ), ymax, 1e-6 ) );
  QVERIFY( qgsDoubleNear( ring->xAt( 4 ), xmin, 1e-6 ) );
  QVERIFY( qgsDoubleNear( ring->yAt( 4 ), ymin, 1e-6 ) );

  QVERIFY( !tool.capturing() );
  QCOMPARE( tool.vertexCount(), 0 );

  // path B: left-clicked corner 1 + right-click finish at cursor
  // (rectangle-from-extent convention)
  tool.activate();
  const QgsPointXY c3 = canvas.getCoordinateTransform()->toMapCoordinates( 30, 150 );
  const QgsPointXY c4 = canvas.getCoordinateTransform()->toMapCoordinates( 120, 40 );
  click( tool, canvas, QPoint( 30, 150 ), Qt::LeftButton );
  click( tool, canvas, QPoint( 120, 40 ), Qt::RightButton );

  QCOMPARE( spy.count(), 2 );
  QCOMPARE( abortSpy.count(), 0 );

  const QgsGeometry g2 = QgsGeometry::fromWkt( spy.at( 1 ).at( 0 ).toString() );
  const QgsPolygon *poly2 = qgsgeometry_cast<const QgsPolygon *>( g2.constGet() );
  QVERIFY( poly2 );
  const QgsLineString *ring2 = qgsgeometry_cast<const QgsLineString *>( poly2->exteriorRing() );
  QVERIFY( ring2 );
  QCOMPARE( ring2->numPoints(), 5 );
  QVERIFY( qgsDoubleNear( ring2->xAt( 0 ), std::min( c3.x(), c4.x() ), 1e-6 ) );
  QVERIFY( qgsDoubleNear( ring2->yAt( 0 ), std::min( c3.y(), c4.y() ), 1e-6 ) );
  QVERIFY( qgsDoubleNear( ring2->xAt( 2 ), std::max( c3.x(), c4.x() ), 1e-6 ) );
  QVERIFY( qgsDoubleNear( ring2->yAt( 2 ), std::max( c3.y(), c4.y() ), 1e-6 ) );

  canvas.unsetMapTool( &tool );
}

void TestMapTools::rectAbortSemantics()
{
  QgsMapCanvas canvas;
  configureCanvas( canvas );

  TestRectTool tool( &canvas );
  canvas.setMapTool( &tool );
  QSignalSpy abortSpy( &tool, &PaleoDrawRectTool::drawAborted );
  QSignalSpy commitSpy( &tool, &PaleoDrawRectTool::constraintDrawn );

  // right-click with no corner planted → cancel gesture
  click( tool, canvas, QPoint( 50, 50 ), Qt::RightButton );
  QCOMPARE( abortSpy.count(), 1 );
  QCOMPARE( commitSpy.count(), 0 );
  QVERIFY( !tool.capturing() );

  // one corner in flight, Esc → abort (§42.15 semantics)
  tool.activate();
  click( tool, canvas, QPoint( 60, 60 ), Qt::LeftButton );
  QCOMPARE( tool.vertexCount(), 1 );
  QKeyEvent esc( QEvent::KeyPress, Qt::Key_Escape, Qt::NoModifier );
  tool.keyPressEvent( &esc );
  QCOMPARE( abortSpy.count(), 2 );
  QCOMPARE( commitSpy.count(), 0 );
  QVERIFY( !tool.capturing() );
  QCOMPARE( tool.vertexCount(), 0 );

  canvas.unsetMapTool( &tool );
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
