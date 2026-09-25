#include <cmath>
#include <memory>

#include <QtTest>
#include <QSignalSpy>
#include <QKeyEvent>

#include <qgsapplication.h>
#include <qgscoordinatereferencesystem.h>
#include <qgscoordinatetransform.h>
#include <qgsgeometry.h>
#include <qgslinestring.h>
#include <qgsmapcanvas.h>
#include <qgsmapmouseevent.h>
#include <qgsmaptoolcapture.h>
#include <qgspoint.h>
#include <qgspolygon.h>
#include <qgsproject.h>
#include <qgsrectangle.h>
#include <qgsvectorlayer.h>

#include "../src/ui/maptools/paleoshapetools.h"

// Shims exposing protected methods for event injection and state inspection
class TestPointTool : public PaleoDrawPointTool
{
  public:
    using PaleoDrawPointTool::PaleoDrawPointTool;
    using PaleoDrawPointTool::keyPressEvent;
    bool capturing() const { return isCapturing(); }
    void deliverPoint( const QgsPoint &p ) { pointCaptured( p ); }
};

class TestCircleTool : public PaleoDrawCircleTool
{
  public:
    using PaleoDrawCircleTool::PaleoDrawCircleTool;
    using PaleoDrawCircleTool::keyPressEvent;
    bool capturing() const { return isCapturing(); }
    int vertexCount() { return size(); }
};

class TestEllipseTool : public PaleoDrawEllipseTool
{
  public:
    using PaleoDrawEllipseTool::PaleoDrawEllipseTool;
    using PaleoDrawEllipseTool::keyPressEvent;
    bool capturing() const { return isCapturing(); }
    int vertexCount() { return size(); }
};

class TestShapeTools : public QObject
{
  Q_OBJECT
private slots:
  void pointConstruction();
  void pointActivationRoundtrip();
  void pointCommitEmitsWkt();
  void pointDirectCapturedFallback();
  void pointAbortSemantics();

  void circleConstruction();
  void circleTwoClicksCommit24Vertices();
  void circleRightClickWithCenterCommits();
  void circleAbortSemantics();

  void ellipseConstruction();
  void ellipseThreeClicksCommit36Vertices();
  void ellipseRightClickWithTwoPointsCommits();
  void ellipseAbortSemantics();

  void reprojectionWithLayerCrs();
};

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

void TestShapeTools::pointConstruction()
{
  QgsMapCanvas canvas;
  configureCanvas( canvas );

  PaleoDrawPointTool tool( &canvas );
  QCOMPARE( tool.mode(), QgsMapToolCapture::CapturePoint );
  QVERIFY( tool.cadDockWidget() );
  QVERIFY( !tool.toolName().isEmpty() );
  QVERIFY( tool.flags() & QgsMapTool::EditTool );
}

void TestShapeTools::pointActivationRoundtrip()
{
  QgsMapCanvas canvas;
  configureCanvas( canvas );

  TestPointTool tool( &canvas );
  QCOMPARE( canvas.mapTool(), nullptr );

  canvas.setMapTool( &tool );
  QCOMPARE( canvas.mapTool(), static_cast<QgsMapTool *>( &tool ) );
  QVERIFY( tool.capturing() );

  canvas.unsetMapTool( &tool );
  QCOMPARE( canvas.mapTool(), nullptr );
  QVERIFY( !tool.capturing() );
}

void TestShapeTools::pointCommitEmitsWkt()
{
  QgsMapCanvas canvas;
  configureCanvas( canvas );

  TestPointTool tool( &canvas );
  canvas.setMapTool( &tool );
  QSignalSpy spy( &tool, &PaleoDrawPointTool::constraintDrawn );
  QSignalSpy abortSpy( &tool, &PaleoDrawPointTool::drawAborted );

  const QgsPointXY p = canvas.getCoordinateTransform()->toMapCoordinates( 60, 140 );

  click( tool, canvas, QPoint( 60, 140 ), Qt::LeftButton );

  QCOMPARE( spy.count(), 1 );
  QCOMPARE( abortSpy.count(), 0 );

  const QString wkt = spy.at( 0 ).at( 0 ).toString();
  QVERIFY( wkt.startsWith( QLatin1String( "Point (" ) ) );

  const QgsGeometry g = QgsGeometry::fromWkt( wkt );
  QVERIFY( !g.isNull() );
  const QgsPoint *pt = qgsgeometry_cast<const QgsPoint *>( g.constGet() );
  QVERIFY( pt );
  QVERIFY( qgsDoubleNear( pt->x(), p.x(), 1e-6 ) );
  QVERIFY( qgsDoubleNear( pt->y(), p.y(), 1e-6 ) );

  QVERIFY( !tool.capturing() );

  canvas.unsetMapTool( &tool );
}

void TestShapeTools::pointDirectCapturedFallback()
{
  QgsMapCanvas canvas;
  configureCanvas( canvas );

  TestPointTool tool( &canvas );
  QSignalSpy spy( &tool, &PaleoDrawPointTool::constraintDrawn );

  tool.deliverPoint( QgsPoint( 15.5, 25.5 ) );

  QCOMPARE( spy.count(), 1 );
  QCOMPARE( spy.at( 0 ).at( 0 ).toString(), QStringLiteral( "Point (15.5 25.5)" ) );
}

void TestShapeTools::pointAbortSemantics()
{
  QgsMapCanvas canvas;
  configureCanvas( canvas );

  TestPointTool tool( &canvas );
  canvas.setMapTool( &tool );
  QSignalSpy abortSpy( &tool, &PaleoDrawPointTool::drawAborted );
  QSignalSpy commitSpy( &tool, &PaleoDrawPointTool::constraintDrawn );

  // Right-click cancels
  click( tool, canvas, QPoint( 50, 50 ), Qt::RightButton );
  QCOMPARE( abortSpy.count(), 1 );
  QCOMPARE( commitSpy.count(), 0 );
  QVERIFY( !tool.capturing() );

  // Esc key cancels
  tool.activate();
  QVERIFY( tool.capturing() );
  QKeyEvent esc( QEvent::KeyPress, Qt::Key_Escape, Qt::NoModifier );
  tool.keyPressEvent( &esc );
  QCOMPARE( abortSpy.count(), 2 );
  QCOMPARE( commitSpy.count(), 0 );
  QVERIFY( !tool.capturing() );

  canvas.unsetMapTool( &tool );
}

void TestShapeTools::circleConstruction()
{
  QgsMapCanvas canvas;
  configureCanvas( canvas );

  PaleoDrawCircleTool tool( &canvas );
  QCOMPARE( tool.mode(), QgsMapToolCapture::CapturePolygon );
  QCOMPARE( tool.currentCaptureTechnique(), Qgis::CaptureTechnique::StraightSegments );
  QVERIFY( tool.cadDockWidget() );
  QVERIFY( !tool.toolName().isEmpty() );
  QVERIFY( tool.flags() & QgsMapTool::EditTool );
}

void TestShapeTools::circleTwoClicksCommit24Vertices()
{
  QgsMapCanvas canvas;
  configureCanvas( canvas );

  TestCircleTool tool( &canvas );
  canvas.setMapTool( &tool );
  QSignalSpy spy( &tool, &PaleoDrawCircleTool::constraintDrawn );
  QSignalSpy abortSpy( &tool, &PaleoDrawCircleTool::drawAborted );

  const QgsPointXY c = canvas.getCoordinateTransform()->toMapCoordinates( 100, 100 );
  const QgsPointXY rPt = canvas.getCoordinateTransform()->toMapCoordinates( 160, 100 );
  const double expectedRadius = std::hypot( rPt.x() - c.x(), rPt.y() - c.y() );

  click( tool, canvas, QPoint( 100, 100 ), Qt::LeftButton );
  QCOMPARE( tool.vertexCount(), 1 );
  QCOMPARE( spy.count(), 0 );

  click( tool, canvas, QPoint( 160, 100 ), Qt::LeftButton );

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

  // 24 segments approximation -> 24 + 1 vertices closed
  QCOMPARE( ring->numPoints(), 25 );
  QVERIFY( ring->isClosed() );

  const QgsRectangle bbox = g.boundingBox();
  const double bboxCenterX = ( bbox.xMinimum() + bbox.xMaximum() ) / 2.0;
  const double bboxCenterY = ( bbox.yMinimum() + bbox.yMaximum() ) / 2.0;
  QVERIFY( qgsDoubleNear( bboxCenterX, c.x(), 1e-6 ) );
  QVERIFY( qgsDoubleNear( bboxCenterY, c.y(), 1e-6 ) );

  const double measuredRadius = ( bbox.xMaximum() - bbox.xMinimum() ) / 2.0;
  QVERIFY( qgsDoubleNear( measuredRadius, expectedRadius, 1e-6 ) );

  QVERIFY( !tool.capturing() );
  QCOMPARE( tool.vertexCount(), 0 );

  canvas.unsetMapTool( &tool );
}

void TestShapeTools::circleRightClickWithCenterCommits()
{
  QgsMapCanvas canvas;
  configureCanvas( canvas );

  TestCircleTool tool( &canvas );
  canvas.setMapTool( &tool );
  QSignalSpy spy( &tool, &PaleoDrawCircleTool::constraintDrawn );
  QSignalSpy abortSpy( &tool, &PaleoDrawCircleTool::drawAborted );

  const QgsPointXY c = canvas.getCoordinateTransform()->toMapCoordinates( 80, 120 );
  const QgsPointXY rPt = canvas.getCoordinateTransform()->toMapCoordinates( 80, 60 );
  const double expectedRadius = std::hypot( rPt.x() - c.x(), rPt.y() - c.y() );

  click( tool, canvas, QPoint( 80, 120 ), Qt::LeftButton ); // 1 vertex (center)
  QCOMPARE( tool.vertexCount(), 1 );

  click( tool, canvas, QPoint( 80, 60 ), Qt::RightButton ); // commit at cursor

  QCOMPARE( spy.count(), 1 );
  QCOMPARE( abortSpy.count(), 0 );

  const QgsGeometry g = QgsGeometry::fromWkt( spy.at( 0 ).at( 0 ).toString() );
  QVERIFY( !g.isNull() );
  const QgsRectangle bbox = g.boundingBox();
  const double bboxCenterX = ( bbox.xMinimum() + bbox.xMaximum() ) / 2.0;
  const double bboxCenterY = ( bbox.yMinimum() + bbox.yMaximum() ) / 2.0;
  QVERIFY( qgsDoubleNear( bboxCenterX, c.x(), 1e-6 ) );
  QVERIFY( qgsDoubleNear( bboxCenterY, c.y(), 1e-6 ) );
  QVERIFY( qgsDoubleNear( ( bbox.yMaximum() - bbox.yMinimum() ) / 2.0, expectedRadius, 1e-6 ) );

  QVERIFY( !tool.capturing() );
  QCOMPARE( tool.vertexCount(), 0 );

  canvas.unsetMapTool( &tool );
}

void TestShapeTools::circleAbortSemantics()
{
  QgsMapCanvas canvas;
  configureCanvas( canvas );

  TestCircleTool tool( &canvas );
  canvas.setMapTool( &tool );
  QSignalSpy abortSpy( &tool, &PaleoDrawCircleTool::drawAborted );
  QSignalSpy commitSpy( &tool, &PaleoDrawCircleTool::constraintDrawn );

  // Bare right click aborts
  click( tool, canvas, QPoint( 50, 50 ), Qt::RightButton );
  QCOMPARE( abortSpy.count(), 1 );
  QCOMPARE( commitSpy.count(), 0 );
  QVERIFY( !tool.capturing() );

  // 1 vertex in flight, Esc aborts
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

void TestShapeTools::ellipseConstruction()
{
  QgsMapCanvas canvas;
  configureCanvas( canvas );

  PaleoDrawEllipseTool tool( &canvas );
  QCOMPARE( tool.mode(), QgsMapToolCapture::CapturePolygon );
  QCOMPARE( tool.currentCaptureTechnique(), Qgis::CaptureTechnique::StraightSegments );
  QVERIFY( tool.cadDockWidget() );
  QVERIFY( !tool.toolName().isEmpty() );
  QVERIFY( tool.flags() & QgsMapTool::EditTool );
}

void TestShapeTools::ellipseThreeClicksCommit36Vertices()
{
  QgsMapCanvas canvas;
  configureCanvas( canvas );

  TestEllipseTool tool( &canvas );
  canvas.setMapTool( &tool );
  QSignalSpy spy( &tool, &PaleoDrawEllipseTool::constraintDrawn );
  QSignalSpy abortSpy( &tool, &PaleoDrawEllipseTool::drawAborted );

  const QgsPointXY c = canvas.getCoordinateTransform()->toMapCoordinates( 100, 100 );
  const QgsPointXY a1 = canvas.getCoordinateTransform()->toMapCoordinates( 160, 100 );
  const QgsPointXY a2 = canvas.getCoordinateTransform()->toMapCoordinates( 100, 130 );

  const double d1 = std::hypot( a1.x() - c.x(), a1.y() - c.y() );
  const double d2 = std::hypot( a2.x() - c.x(), a2.y() - c.y() );
  const double expectedSemiMajor = std::max( d1, d2 );
  const double expectedSemiMinor = std::min( d1, d2 );

  click( tool, canvas, QPoint( 100, 100 ), Qt::LeftButton );
  QCOMPARE( tool.vertexCount(), 1 );
  QCOMPARE( spy.count(), 0 );

  click( tool, canvas, QPoint( 160, 100 ), Qt::LeftButton );
  QCOMPARE( tool.vertexCount(), 2 );
  QCOMPARE( spy.count(), 0 );

  click( tool, canvas, QPoint( 100, 130 ), Qt::LeftButton );

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

  // 36 segments approximation -> 37 points closed
  QCOMPARE( ring->numPoints(), 37 );
  QVERIFY( ring->isClosed() );

  const QgsRectangle bbox = g.boundingBox();
  const double bboxCenterX = ( bbox.xMinimum() + bbox.xMaximum() ) / 2.0;
  const double bboxCenterY = ( bbox.yMinimum() + bbox.yMaximum() ) / 2.0;
  QVERIFY( qgsDoubleNear( bboxCenterX, c.x(), 1e-5 ) );
  QVERIFY( qgsDoubleNear( bboxCenterY, c.y(), 1e-5 ) );

  // Axis lengths verification (aligned with x and y axes)
  const double semiX = ( bbox.xMaximum() - bbox.xMinimum() ) / 2.0;
  const double semiY = ( bbox.yMaximum() - bbox.yMinimum() ) / 2.0;
  QVERIFY( qgsDoubleNear( std::max( semiX, semiY ), expectedSemiMajor, 1e-4 ) );
  QVERIFY( qgsDoubleNear( std::min( semiX, semiY ), expectedSemiMinor, 1e-4 ) );

  QVERIFY( !tool.capturing() );
  QCOMPARE( tool.vertexCount(), 0 );

  canvas.unsetMapTool( &tool );
}

void TestShapeTools::ellipseRightClickWithTwoPointsCommits()
{
  QgsMapCanvas canvas;
  configureCanvas( canvas );

  TestEllipseTool tool( &canvas );
  canvas.setMapTool( &tool );
  QSignalSpy spy( &tool, &PaleoDrawEllipseTool::constraintDrawn );
  QSignalSpy abortSpy( &tool, &PaleoDrawEllipseTool::drawAborted );

  const QgsPointXY c = canvas.getCoordinateTransform()->toMapCoordinates( 100, 100 );
  const QgsPointXY a1 = canvas.getCoordinateTransform()->toMapCoordinates( 160, 100 );
  const QgsPointXY a2 = canvas.getCoordinateTransform()->toMapCoordinates( 100, 140 );

  click( tool, canvas, QPoint( 100, 100 ), Qt::LeftButton );
  click( tool, canvas, QPoint( 160, 100 ), Qt::LeftButton );
  QCOMPARE( tool.vertexCount(), 2 );

  click( tool, canvas, QPoint( 100, 140 ), Qt::RightButton ); // commit at cursor

  QCOMPARE( spy.count(), 1 );
  QCOMPARE( abortSpy.count(), 0 );

  const QgsGeometry g = QgsGeometry::fromWkt( spy.at( 0 ).at( 0 ).toString() );
  QVERIFY( !g.isNull() );
  const QgsPolygon *poly = qgsgeometry_cast<const QgsPolygon *>( g.constGet() );
  QVERIFY( poly );
  const QgsLineString *ring = qgsgeometry_cast<const QgsLineString *>( poly->exteriorRing() );
  QVERIFY( ring );
  QCOMPARE( ring->numPoints(), 37 );
  QVERIFY( ring->isClosed() );

  const QgsRectangle bbox = g.boundingBox();
  QVERIFY( qgsDoubleNear( ( bbox.xMinimum() + bbox.xMaximum() ) / 2.0, c.x(), 1e-5 ) );
  QVERIFY( qgsDoubleNear( ( bbox.yMinimum() + bbox.yMaximum() ) / 2.0, c.y(), 1e-5 ) );

  QVERIFY( !tool.capturing() );
  QCOMPARE( tool.vertexCount(), 0 );

  canvas.unsetMapTool( &tool );
}

void TestShapeTools::ellipseAbortSemantics()
{
  QgsMapCanvas canvas;
  configureCanvas( canvas );

  TestEllipseTool tool( &canvas );
  canvas.setMapTool( &tool );
  QSignalSpy abortSpy( &tool, &PaleoDrawEllipseTool::drawAborted );
  QSignalSpy commitSpy( &tool, &PaleoDrawEllipseTool::constraintDrawn );

  // Bare right click aborts
  click( tool, canvas, QPoint( 50, 50 ), Qt::RightButton );
  QCOMPARE( abortSpy.count(), 1 );
  QCOMPARE( commitSpy.count(), 0 );
  QVERIFY( !tool.capturing() );

  // Right-click with 1 point in flight aborts
  tool.activate();
  click( tool, canvas, QPoint( 50, 50 ), Qt::LeftButton );
  QCOMPARE( tool.vertexCount(), 1 );
  click( tool, canvas, QPoint( 60, 60 ), Qt::RightButton );
  QCOMPARE( abortSpy.count(), 2 );
  QCOMPARE( commitSpy.count(), 0 );
  QVERIFY( !tool.capturing() );

  // Esc with 1 point in flight aborts
  tool.activate();
  click( tool, canvas, QPoint( 50, 50 ), Qt::LeftButton );
  QCOMPARE( tool.vertexCount(), 1 );
  QKeyEvent esc( QEvent::KeyPress, Qt::Key_Escape, Qt::NoModifier );
  tool.keyPressEvent( &esc );
  QCOMPARE( abortSpy.count(), 3 );
  QCOMPARE( commitSpy.count(), 0 );
  QVERIFY( !tool.capturing() );

  // Esc with 2 points in flight aborts
  tool.activate();
  click( tool, canvas, QPoint( 50, 50 ), Qt::LeftButton );
  click( tool, canvas, QPoint( 70, 50 ), Qt::LeftButton );
  QCOMPARE( tool.vertexCount(), 2 );
  tool.keyPressEvent( &esc );
  QCOMPARE( abortSpy.count(), 4 );
  QCOMPARE( commitSpy.count(), 0 );
  QVERIFY( !tool.capturing() );

  canvas.unsetMapTool( &tool );
}

void TestShapeTools::reprojectionWithLayerCrs()
{
  QgsMapCanvas canvas;
  configureCanvas( canvas ); // destination CRS: EPSG:4326

  // Create a memory layer in EPSG:3857
  QgsVectorLayer vlayer( QStringLiteral( "Point?crs=EPSG:3857" ), QStringLiteral( "test_layer" ), QStringLiteral( "memory" ) );
  QVERIFY( vlayer.isValid() );
  canvas.setCurrentLayer( &vlayer );

  TestPointTool ptTool( &canvas );
  QSignalSpy ptSpy( &ptTool, &PaleoDrawPointTool::constraintDrawn );

  // EPSG:3857 coords (0, 0) transforms to EPSG:4326 (0, 0)
  ptTool.deliverPoint( QgsPoint( 0, 0 ) );
  QCOMPARE( ptSpy.count(), 1 );
  const QgsGeometry g = QgsGeometry::fromWkt( ptSpy.at( 0 ).at( 0 ).toString() );
  QVERIFY( !g.isNull() );
  const QgsPoint *pt = qgsgeometry_cast<const QgsPoint *>( g.constGet() );
  QVERIFY( pt );
  QVERIFY( qgsDoubleNear( pt->x(), 0.0, 1e-4 ) );
  QVERIFY( qgsDoubleNear( pt->y(), 0.0, 1e-4 ) );

  canvas.setCurrentLayer( nullptr );
}

int main( int argc, char *argv[] )
{
  QgsApplication app( argc, argv, false );
  app.setPrefixPath( QStringLiteral( "/usr" ), true );
  app.initQgis();
  TestShapeTools tc;
  const int rc = QTest::qExec( &tc, argc, argv );
  QgsApplication::exitQgis();
  return rc;
}

#include "tst_shapetools.moc"
