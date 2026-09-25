// tst_edittools — integration test suite for the ui/edittools wave:
// PaleoAddFeatureTool / PaleoReshapeTool / PaleoMoveTool /
// PaleoDeleteFeatureTool / PaleoVertexTool / PaleoUndoStack /
// PaleoEditingToolbar.
//
// Coverage map (acceptance checklist of the wave):
//   a) add feature lands in the edit buffer (point/line/polygon click flows,
//      featureCount + addedFeatures + DPI-safe geometry coords + one-command
//      undo reverts);
//   b) vertex drag mutates only the dragged vertex, undo restores;
//   c) reshape changes the selected feature only (vertex-count delta,
//      unselected untouched) + right-click vertex delete with the
//      below-minimum guard;
//   d) PaleoUndoStack attach/forward/refuse-switch + toolbar undo/redo
//      action state linkage;
//   e) toolbar editingStarted/editingStopped, combo filter, editing-layer
//      highlight, state label, canvas currentLayer sync;
//   f) save/cancel boundaries (persistence, stack cleared, refusal);
//   g) abort paths (Esc per tool, sub-threshold right-click cancel,
//      locked-layer warn-only via messageEmitted);
//   +) shared signal contract sweep across the five tools.
//
// QGIS 4.2 event-injection digest (empirically established in wave-1 and
// directly relied on below):
//   · the right-click commit does NOT append the right-click point to the
//     capture curve — every committed vertex must come from a left click;
//   · offscreen canvases magnify the requested extent by the screen-DPI
//     factor — every expected map coordinate goes through
//     getCoordinateTransform()->toMapCoordinates(), never raw pixel math;
//   · protected overrides shadow the public base hooks, so the white-box
//     shims below re-publicize them with using-declarations;
//   · seed data goes through dataProvider()->addFeatures() (outside any edit
//     session) so the per-layer native undo stack starts pristine;
//   · QgsProject::instance() is process-global — init()/cleanup() clear it.

#include <algorithm>
#include <memory>

#include <QtTest>
#include <QAction>
#include <QComboBox>
#include <QKeyEvent>
#include <QLabel>
#include <QSignalSpy>
#include <QUndoStack>

#include <qgsapplication.h>
#include <qgsfeature.h>
#include <qgsfeatureiterator.h>
#include <qgsgeometry.h>
#include <qgslinestring.h>
#include <qgsmapcanvas.h>
#include <qgsmapmouseevent.h>
#include <qgsmaptoolcapture.h>
#include <qgspolygon.h>
#include <qgspoint.h>
#include <qgsproject.h>
#include <qgsrectangle.h>
#include <qgsvectorlayer.h>
#include <qgsvectorlayereditbuffer.h>

#include "ui/edittools/editingtoolbar.h"
#include "ui/edittools/editingtools.h"
#include "ui/edittools/editingundostack.h"
#include "ui/edittools/vertexeditortools.h"

// White-box shims: promote the protected canvas/key hooks so events can be
// injected directly (same pattern as tests/tst_maptools.cpp). No Q_OBJECT —
// the vtable/moc of the real tool classes is reused as-is.
class TestAddTool : public PaleoAddFeatureTool
{
  public:
    using PaleoAddFeatureTool::PaleoAddFeatureTool;
    using PaleoAddFeatureTool::keyPressEvent; // protected → public for injection
    int vertexCount() { return size(); }      // base size() is non-const
};

class TestReshapeTool : public PaleoReshapeTool
{
  public:
    using PaleoReshapeTool::PaleoReshapeTool;
    using PaleoReshapeTool::keyPressEvent;
    int vertexCount() { return size(); }
};

class TestMoveTool : public PaleoMoveTool
{
  public:
    using PaleoMoveTool::PaleoMoveTool;
    // The contract header redeclares the event hooks in a protected section;
    // re-publicize them for direct event injection.
    using PaleoMoveTool::keyPressEvent;
    using PaleoMoveTool::canvasPressEvent;
    using PaleoMoveTool::canvasMoveEvent;
    using PaleoMoveTool::canvasReleaseEvent;
};

class TestDeleteTool : public PaleoDeleteFeatureTool
{
  public:
    using PaleoDeleteFeatureTool::PaleoDeleteFeatureTool;
    using PaleoDeleteFeatureTool::keyPressEvent;
    using PaleoDeleteFeatureTool::canvasReleaseEvent;
};

class TestVertexTool : public PaleoVertexTool
{
  public:
    using PaleoVertexTool::PaleoVertexTool;
    using PaleoVertexTool::canvasPressEvent;
    using PaleoVertexTool::canvasMoveEvent;
    using PaleoVertexTool::canvasReleaseEvent;
    using PaleoVertexTool::canvasDoubleClickEvent;
    using PaleoVertexTool::keyPressEvent;
};

class TestEditTools : public QObject
{
    Q_OBJECT
  private slots:
    // Test-fixture hygiene (QgsProject is process-global).
    void init();
    void cleanup();

    // a) add feature → edit buffer
    void addPointClickCommitsIntoEditBuffer();
    void addLineClicksCommitAndUndoRedo();
    void addPolygonClicksCommit();
    void targetLayerResolutionBoundAndCanvasFallback();

    // b) vertex drag
    void vertexMarkersFollowSelectionLifecycle();
    void vertexDragMovesSingleVertexAndUndoRestores();

    // c) reshape + vertex add/delete
    void reshapeChangesSelectedFeatureOnly();
    void vertexDoubleClickInsertsVertex();
    void vertexRightClickDeletesWithUndo();
    void vertexDeleteRefusedBelowMinimums();

    // d) undo/redo spine
    void undoStackAttachForwardUndoRedo();
    void undoStackSwitchRefusedAndCommitClears();
    void undoStackDetachReattachDestroyedSafe();

    // d/e/f) PaleoEditingToolbar (implementation by the parallel agent)
    void toolbarStartStopSignalsAndStateRendering();
    void toolbarComboFilterAndProjectRefresh();
    void toolbarEditToolActionAutoStartsSession();
    void toolbarUndoRedoActionStates();
    void toolbarSavePersistsAndClearsUndo();
    void toolbarCancelDiscardsEdits();
    void toolbarSaveRefusedOutsideSession();

    // g) abort paths
    void addAbortPaths();
    void reshapeAbortPaths();
    void moveEscCancelsDragAndIdleAborts();
    void deleteEscAndEmptySelectionWarns();
    void vertexEscCancelsDragAndIdleAborts();
    void warnOnlyRefusalsDontAbort();

    // +) shared signal contract across the five tools
    void signalContractAcrossTools();
};

// ---------------------------------------------------------------------------
// Helpers
// ---------------------------------------------------------------------------

// Fresh offscreen canvas. NB: QgsMapCanvas magnifies the requested extent by
// the screen-DPI factor, so pixel↔map math must go through
// getCoordinateTransform() in both directions.
static void configureCanvas( QgsMapCanvas &canvas )
{
  canvas.setDestinationCrs( QgsCoordinateReferenceSystem( QStringLiteral( "EPSG:4326" ) ) );
  canvas.resize( 200, 200 );
  canvas.setExtent( QgsRectangle( 0, 0, 100, 100 ) );
  canvas.refresh();
}

// Press+release pair fed straight into the tool hooks.
static void click( QgsMapTool &tool, QgsMapCanvas &canvas, const QPoint &px, const Qt::MouseButton button )
{
  QgsMapMouseEvent press( &canvas, QEvent::MouseButtonPress, px, button, button, Qt::NoModifier );
  QgsMapMouseEvent release( &canvas, QEvent::MouseButtonRelease, px, button, Qt::NoButton, Qt::NoModifier );
  tool.canvasPressEvent( &press );
  tool.canvasReleaseEvent( &release );
}

static void sendEsc( QgsMapTool &tool )
{
  QKeyEvent esc( QEvent::KeyPress, Qt::Key_Escape, Qt::NoModifier );
  tool.keyPressEvent( &esc );
}

// Pixel → map coordinate through the live canvas transform (DPI-safe).
static QgsPointXY mapPt( QgsMapCanvas &canvas, int x, int y )
{
  return canvas.getCoordinateTransform()->toMapCoordinates( x, y );
}

// Map coordinate → nearest pixel (inverse of mapPt).
static QPoint pxAt( QgsMapCanvas &canvas, double mx, double my )
{
  const QgsPointXY p = canvas.getCoordinateTransform()->transform( QgsPointXY( mx, my ) );
  return QPoint( static_cast<int>( std::lround( p.x() ) ), static_cast<int>( std::lround( p.y() ) ) );
}

// Axis-aligned square feature WKT.
static QString squareWkt( double x0, double y0, double size )
{
  const double x1 = x0 + size, y1 = y0 + size;
  return QStringLiteral( "Polygon ((%1 %2, %3 %2, %3 %4, %1 %4, %1 %2))" )
      .arg( x0 ).arg( y0 ).arg( x1 ).arg( y1 );
}

// Provider-level seeding: runs outside any edit session, so the per-layer
// native undo stack stays pristine (contrast layer.addFeature(), which needs
// a session and pushes its own undo command).
static QgsFeatureList seedFeatures( QgsVectorLayer &layer, const QStringList &wkts )
{
  QgsFeatureList list;
  for ( const QString &wkt : wkts )
  {
    QgsFeature f( layer.fields() );
    f.setGeometry( QgsGeometry::fromWkt( wkt ) );
    list << f;
  }
  layer.dataProvider()->addFeatures( list );
  return list;
}

static QgsFeatureId seedFeature( QgsVectorLayer &layer, const QgsGeometry &geometry )
{
  QgsFeatureList list;
  QgsFeature f( layer.fields() );
  f.setGeometry( geometry );
  list << f;
  layer.dataProvider()->addFeatures( list );
  return list.first().id();
}

// Total vertex count of a feature geometry (dense vertex numbering).
static int vertexTotal( QgsVectorLayer &layer, QgsFeatureId fid )
{
  const QgsGeometry g = layer.getFeature( fid ).geometry();
  int nr = 0;
  QgsVertexId vid;
  while ( g.vertexIdFromVertexNr( nr, vid ) )
    ++nr;
  return nr;
}

static const QgsPolygon *asPolygon( const QgsGeometry &geometry )
{
  return qgsgeometry_cast<const QgsPolygon *>( geometry.constGet() );
}

static const QgsLineString *asLineString( const QgsGeometry &geometry )
{
  return qgsgeometry_cast<const QgsLineString *>( geometry.constGet() );
}

// One edit command group exactly as the tools produce them
// (beginEditCommand + addFeature + endEditCommand) — used to stage edit
// buffer content in the undo-stack/toolbar tests without tool interplay.
static void addPointCommand( QgsVectorLayer *vl, int id )
{
  QgsFeature f( vl->fields() );
  f.setAttributes( QgsAttributes() << id );
  f.setGeometry( QgsGeometry::fromPointXY( QgsPointXY( id, id ) ) );
  vl->beginEditCommand( QStringLiteral( "add point %1" ).arg( id ) );
  QVERIFY2( vl->addFeature( f ), "addFeature failed" );
  vl->endEditCommand();
}

void TestEditTools::init()
{
  QgsProject::instance()->clear(); // hermetic project registry per test
}

void TestEditTools::cleanup()
{
  QgsProject::instance()->clear();
}

// ---------------------------------------------------------------------------
// a) add feature → edit buffer
// ---------------------------------------------------------------------------

void TestEditTools::addPointClickCommitsIntoEditBuffer()
{
  QgsMapCanvas canvas;
  configureCanvas( canvas );

  QgsVectorLayer layer( QStringLiteral( "Point?crs=EPSG:4326&field=id:integer" ), QStringLiteral( "pts" ), QStringLiteral( "memory" ) );
  QVERIFY2( layer.isValid(), "memory point layer failed to initialize" );
  layer.startEditing();
  canvas.setLayers( QList<QgsMapLayer *>{ &layer } );
  canvas.setCurrentLayer( &layer );
  canvas.refresh();

  TestAddTool tool( &canvas, nullptr, QgsMapToolCapture::CapturePoint, &layer );
  canvas.setMapTool( &tool );
  QSignalSpy editedSpy( &tool, &PaleoAddFeatureTool::featureEdited );
  QSignalSpy abortSpy( &tool, &PaleoAddFeatureTool::editAborted );

  const QgsPointXY expected = mapPt( canvas, 60, 80 );
  click( tool, canvas, QPoint( 60, 80 ), Qt::LeftButton );

  QCOMPARE( tool.committedCount(), 1 );
  QCOMPARE( layer.featureCount(), 1 );
  QVERIFY( layer.editBuffer() != nullptr );
  QCOMPARE( layer.editBuffer()->addedFeatures().size(), 1 );
  QCOMPARE( editedSpy.count(), 1 );
  QCOMPARE( editedSpy.at( 0 ).at( 0 ).toString(), layer.id() );
  QCOMPARE( abortSpy.count(), 0 );

  const QgsFeature added = layer.editBuffer()->addedFeatures().first();
  QCOMPARE( added.geometry().wkbType(), Qgis::WkbType::Point );
  const QgsPoint *pt = qgsgeometry_cast<const QgsPoint *>( added.geometry().constGet() );
  QVERIFY2( pt, "committed geometry is not a point" );
  QVERIFY( qgsDoubleNear( pt->x(), expected.x(), 1e-9 ) );
  QVERIFY( qgsDoubleNear( pt->y(), expected.y(), 1e-9 ) );

  // Continuous digitizing: a second click lands a second feature (one edit
  // command each → two native undo steps).
  click( tool, canvas, QPoint( 100, 120 ), Qt::LeftButton );
  QCOMPARE( tool.committedCount(), 2 );
  QCOMPARE( layer.featureCount(), 2 );
  QCOMPARE( layer.editBuffer()->addedFeatures().size(), 2 );
  QCOMPARE( layer.undoStack()->count(), 2 );

  // One undo step reverts exactly one committed feature.
  layer.undoStack()->undo();
  QCOMPARE( layer.featureCount(), 1 );

  layer.rollBack();
  canvas.unsetMapTool( &tool );
}

void TestEditTools::addLineClicksCommitAndUndoRedo()
{
  QgsMapCanvas canvas;
  configureCanvas( canvas );

  QgsVectorLayer layer( QStringLiteral( "LineString?crs=EPSG:4326&field=id:integer" ), QStringLiteral( "lines" ), QStringLiteral( "memory" ) );
  QVERIFY2( layer.isValid(), "memory line layer failed to initialize" );
  layer.startEditing();
  canvas.setLayers( QList<QgsMapLayer *>{ &layer } );
  canvas.setCurrentLayer( &layer );
  canvas.refresh();

  TestAddTool tool( &canvas, nullptr, QgsMapToolCapture::CaptureLine, &layer );
  canvas.setMapTool( &tool );
  QSignalSpy editedSpy( &tool, &PaleoAddFeatureTool::featureEdited );
  QSignalSpy abortSpy( &tool, &PaleoAddFeatureTool::editAborted );

  // Every committed vertex comes from a left click; the right-click only
  // commits (wave-1 digest: the release point is NOT appended).
  const QgsPointXY p1 = mapPt( canvas, 20, 160 );
  const QgsPointXY p2 = mapPt( canvas, 140, 60 );

  click( tool, canvas, QPoint( 20, 160 ), Qt::LeftButton );
  QCOMPARE( tool.vertexCount(), 1 );
  click( tool, canvas, QPoint( 140, 60 ), Qt::LeftButton );
  QCOMPARE( tool.vertexCount(), 2 );
  click( tool, canvas, QPoint( 140, 60 ), Qt::RightButton ); // commit

  QCOMPARE( tool.committedCount(), 1 );
  QCOMPARE( layer.featureCount(), 1 );
  QVERIFY( layer.editBuffer() != nullptr );
  QCOMPARE( layer.editBuffer()->addedFeatures().size(), 1 );
  QCOMPARE( editedSpy.count(), 1 );
  QCOMPARE( editedSpy.at( 0 ).at( 0 ).toString(), layer.id() );
  QCOMPARE( abortSpy.count(), 0 );

  const QgsFeature added = layer.editBuffer()->addedFeatures().first();
  QCOMPARE( added.geometry().wkbType(), Qgis::WkbType::LineString ); // flattened for straight-only layer
  const QgsLineString *ls = asLineString( added.geometry() );
  QVERIFY2( ls, "committed geometry is not a line string" );
  QCOMPARE( ls->numPoints(), 2 );
  QVERIFY( qgsDoubleNear( ls->xAt( 0 ), p1.x(), 1e-6 ) );
  QVERIFY( qgsDoubleNear( ls->yAt( 0 ), p1.y(), 1e-6 ) );
  QVERIFY( qgsDoubleNear( ls->xAt( 1 ), p2.x(), 1e-6 ) );
  QVERIFY( qgsDoubleNear( ls->yAt( 1 ), p2.y(), 1e-6 ) );

  // Native undo round trip: one gesture = one undo step.
  QUndoStack *us = layer.undoStack();
  QCOMPARE( us->count(), 1 );
  us->undo();
  QCOMPARE( layer.featureCount(), 0 );
  us->redo();
  QCOMPARE( layer.featureCount(), 1 );

  layer.rollBack();
  canvas.unsetMapTool( &tool );
}

void TestEditTools::addPolygonClicksCommit()
{
  QgsMapCanvas canvas;
  configureCanvas( canvas );

  QgsVectorLayer layer( QStringLiteral( "Polygon?crs=EPSG:4326&field=id:integer" ), QStringLiteral( "polys" ), QStringLiteral( "memory" ) );
  QVERIFY2( layer.isValid(), "memory polygon layer failed to initialize" );
  layer.startEditing();
  canvas.setLayers( QList<QgsMapLayer *>{ &layer } );
  canvas.setCurrentLayer( &layer );
  canvas.refresh();

  TestAddTool tool( &canvas, nullptr, QgsMapToolCapture::CapturePolygon, &layer );
  canvas.setMapTool( &tool );
  QSignalSpy editedSpy( &tool, &PaleoAddFeatureTool::featureEdited );
  QSignalSpy abortSpy( &tool, &PaleoAddFeatureTool::editAborted );

  const QgsPointXY p1 = mapPt( canvas, 20, 160 );
  const QgsPointXY p2 = mapPt( canvas, 140, 60 );
  const QgsPointXY p3 = mapPt( canvas, 60, 20 );

  click( tool, canvas, QPoint( 20, 160 ), Qt::LeftButton );
  click( tool, canvas, QPoint( 140, 60 ), Qt::LeftButton );
  click( tool, canvas, QPoint( 60, 20 ), Qt::LeftButton );
  QCOMPARE( tool.vertexCount(), 3 );
  click( tool, canvas, QPoint( 60, 20 ), Qt::RightButton ); // commit (≥3 vertices)

  QCOMPARE( tool.committedCount(), 1 );
  QCOMPARE( layer.featureCount(), 1 );
  QCOMPARE( layer.editBuffer()->addedFeatures().size(), 1 );
  QCOMPARE( editedSpy.count(), 1 );
  QCOMPARE( editedSpy.at( 0 ).at( 0 ).toString(), layer.id() );
  QCOMPARE( abortSpy.count(), 0 );

  const QgsFeature added = layer.editBuffer()->addedFeatures().first();
  QCOMPARE( added.geometry().wkbType(), Qgis::WkbType::Polygon ); // flattened for straight-only layer
  const QgsPolygon *poly = asPolygon( added.geometry() );
  QVERIFY2( poly, "committed geometry is not a polygon" );
  const QgsLineString *ring = qgsgeometry_cast<const QgsLineString *>( poly->exteriorRing() );
  QVERIFY2( ring, "committed polygon has no linear exterior ring" );
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

  layer.rollBack();
  canvas.unsetMapTool( &tool );
}

void TestEditTools::targetLayerResolutionBoundAndCanvasFallback()
{
  QgsMapCanvas canvas;
  configureCanvas( canvas );

  QgsVectorLayer bound( QStringLiteral( "Point?crs=EPSG:4326" ), QStringLiteral( "bound" ), QStringLiteral( "memory" ) );
  QgsVectorLayer current( QStringLiteral( "Point?crs=EPSG:4326" ), QStringLiteral( "current" ), QStringLiteral( "memory" ) );
  canvas.setLayers( QList<QgsMapLayer *>{ &bound, &current } );

  // Bound layer wins over the (null) canvas current layer.
  TestAddTool boundTool( &canvas, nullptr, QgsMapToolCapture::CapturePoint, &bound );
  QCOMPARE( boundTool.targetLayer(), &bound );

  // No bound layer → canvas current vector layer.
  canvas.setCurrentLayer( &current );
  TestAddTool fallbackTool( &canvas, nullptr, QgsMapToolCapture::CapturePoint, nullptr );
  QCOMPARE( fallbackTool.targetLayer(), &current );

  // Same resolution rule for the vertex tool, deferred to activate().
  TestVertexTool boundVertex( &canvas, &bound );
  QCOMPARE( boundVertex.targetLayer(), &bound );

  TestVertexTool fallbackVertex( &canvas ); // nullptr layer
  QCOMPARE( fallbackVertex.targetLayer(), nullptr );
  canvas.setMapTool( &fallbackVertex );
  QCOMPARE( fallbackVertex.targetLayer(), &current ); // resolved from canvas current layer
  canvas.unsetMapTool( &fallbackVertex );
}

// ---------------------------------------------------------------------------
// b) vertex drag
// ---------------------------------------------------------------------------

void TestEditTools::vertexMarkersFollowSelectionLifecycle()
{
  QgsMapCanvas canvas;
  configureCanvas( canvas );

  QgsVectorLayer layer( QStringLiteral( "LineString?crs=EPSG:4326&field=id:integer" ), QStringLiteral( "vtx" ), QStringLiteral( "memory" ) );
  QVERIFY2( layer.isValid(), "memory line layer failed to initialize" );
  const QgsFeatureId fid1 = seedFeature( layer, QgsGeometry::fromPolylineXY(
      { mapPt( canvas, 40, 140 ), mapPt( canvas, 100, 100 ), mapPt( canvas, 160, 60 ) } ) );
  const QgsFeatureId fid2 = seedFeature( layer, QgsGeometry::fromWkt( QStringLiteral( "LineString (1 1, 2 2)" ) ) );
  layer.startEditing();
  layer.selectByIds( { fid1 } );
  canvas.setLayers( QList<QgsMapLayer *>{ &layer } );
  canvas.setCurrentLayer( &layer );
  canvas.refresh();

  TestVertexTool tool( &canvas, &layer );
  QCOMPARE( tool.markerCount(), 0 ); // nothing before activation

  canvas.setMapTool( &tool );
  QCOMPARE( tool.markerCount(), 3 ); // one marker per selected-feature vertex

  // Selection churn while active rebuilds markers live.
  layer.selectByIds( { fid1, fid2 } );
  QCOMPARE( tool.markerCount(), 5 );
  layer.removeSelection();
  QCOMPARE( tool.markerCount(), 0 );

  // A press far beyond the search radius arms no drag.
  layer.selectByIds( { fid1 } );
  QCOMPARE( tool.markerCount(), 3 );
  click( tool, canvas, QPoint( 100, 20 ), Qt::LeftButton ); // ~80px from the nearest vertex
  QVERIFY( !tool.isDragging() );

  canvas.unsetMapTool( &tool );
  QCOMPARE( tool.markerCount(), 0 ); // deactivate clears markers

  layer.rollBack();
}

void TestEditTools::vertexDragMovesSingleVertexAndUndoRestores()
{
  QgsMapCanvas canvas;
  configureCanvas( canvas );

  QgsVectorLayer layer( QStringLiteral( "LineString?crs=EPSG:4326&field=id:integer" ), QStringLiteral( "drag" ), QStringLiteral( "memory" ) );
  QVERIFY2( layer.isValid(), "memory line layer failed to initialize" );
  const QgsFeatureId fid = seedFeature( layer, QgsGeometry::fromPolylineXY(
      { mapPt( canvas, 40, 140 ), mapPt( canvas, 100, 100 ), mapPt( canvas, 160, 60 ) } ) );
  layer.startEditing();
  layer.selectByIds( { fid } );
  canvas.setLayers( QList<QgsMapLayer *>{ &layer } );
  canvas.setCurrentLayer( &layer );
  canvas.refresh();

  TestVertexTool tool( &canvas, &layer );
  canvas.setMapTool( &tool );
  QSignalSpy editedSpy( &tool, &PaleoVertexTool::featureEdited );
  QSignalSpy msgSpy( &tool, &QgsMapTool::messageEmitted );

  const QgsPointXY p1 = mapPt( canvas, 40, 140 );
  const QgsPointXY p2 = mapPt( canvas, 100, 100 );
  const QgsPointXY p3 = mapPt( canvas, 160, 60 );
  const QgsPointXY pNew = mapPt( canvas, 70, 110 );

  // Real drag flow: press on vertex 0 → move → release elsewhere.
  QgsMapMouseEvent press( &canvas, QEvent::MouseButtonPress, QPoint( 40, 140 ), Qt::LeftButton, Qt::LeftButton, Qt::NoModifier );
  tool.canvasPressEvent( &press );
  QVERIFY2( tool.isDragging(), "press on a vertex must arm the drag" );

  QgsMapMouseEvent move( &canvas, QEvent::MouseMove, QPoint( 70, 110 ), Qt::NoButton, Qt::LeftButton, Qt::NoModifier );
  tool.canvasMoveEvent( &move );
  QVERIFY( tool.isDragging() );

  QgsMapMouseEvent release( &canvas, QEvent::MouseButtonRelease, QPoint( 70, 110 ), Qt::LeftButton, Qt::NoButton, Qt::NoModifier );
  tool.canvasReleaseEvent( &release );
  QVERIFY( !tool.isDragging() );

  QCOMPARE( editedSpy.count(), 1 );
  QCOMPARE( editedSpy.at( 0 ).at( 0 ).toString(), layer.id() );
  QCOMPARE( msgSpy.count(), 0 );
  QCOMPARE( tool.editedCount(), 1 );
  QCOMPARE( tool.markerCount(), 3 ); // markers rebuilt after the commit

  // Only the dragged vertex moved; the others are untouched.
  const QgsGeometry g = layer.getFeature( fid ).geometry();
  const QgsLineString *ls = asLineString( g );
  QVERIFY2( ls, "dragged geometry is not a line string" );
  QCOMPARE( ls->numPoints(), 3 );
  QVERIFY( qgsDoubleNear( ls->xAt( 0 ), pNew.x(), 1e-6 ) );
  QVERIFY( qgsDoubleNear( ls->yAt( 0 ), pNew.y(), 1e-6 ) );
  QVERIFY( qgsDoubleNear( ls->xAt( 1 ), p2.x(), 1e-6 ) );
  QVERIFY( qgsDoubleNear( ls->yAt( 1 ), p2.y(), 1e-6 ) );
  QVERIFY( qgsDoubleNear( ls->xAt( 2 ), p3.x(), 1e-6 ) );
  QVERIFY( qgsDoubleNear( ls->yAt( 2 ), p3.y(), 1e-6 ) );

  // One edit command → native undo restores the pre-drag position.
  QUndoStack *us = layer.undoStack();
  QVERIFY( us );
  QCOMPARE( us->count(), 1 );
  QVERIFY( us->canUndo() );
  us->undo();
  const QgsGeometry undone = layer.getFeature( fid ).geometry();
  const QgsLineString *lsu = asLineString( undone );
  QVERIFY( lsu );
  QVERIFY( qgsDoubleNear( lsu->xAt( 0 ), p1.x(), 1e-6 ) );
  QVERIFY( qgsDoubleNear( lsu->yAt( 0 ), p1.y(), 1e-6 ) );

  canvas.unsetMapTool( &tool );
  layer.rollBack();
}

// ---------------------------------------------------------------------------
// c) reshape + vertex add/delete
// ---------------------------------------------------------------------------

void TestEditTools::reshapeChangesSelectedFeatureOnly()
{
  QgsMapCanvas canvas;
  configureCanvas( canvas );

  QgsVectorLayer layer( QStringLiteral( "Polygon?crs=EPSG:4326&field=id:integer" ), QStringLiteral( "reshape" ), QStringLiteral( "memory" ) );
  QVERIFY2( layer.isValid(), "memory polygon layer failed to initialize" );
  const QgsFeatureList seeded = seedFeatures( layer, { squareWkt( 10, 10, 20 ), squareWkt( 60, 60, 20 ) } );
  const QgsFeatureId fidA = seeded.at( 0 ).id(); // square (10,10)-(30,30)
  const QgsFeatureId fidB = seeded.at( 1 ).id(); // far square (60,60)-(80,80)
  QCOMPARE( layer.featureCount(), 2 );

  layer.startEditing();
  layer.select( QgsFeatureIds{ fidA } );
  canvas.setLayers( QList<QgsMapLayer *>{ &layer } );
  canvas.setCurrentLayer( &layer );
  canvas.refresh();

  TestReshapeTool tool( &canvas, nullptr, &layer );
  canvas.setMapTool( &tool );
  QSignalSpy editedSpy( &tool, &PaleoReshapeTool::featureEdited );
  QSignalSpy abortSpy( &tool, &PaleoReshapeTool::editAborted );

  const QString originalB = layer.getFeature( fidB ).geometry().asWkt();

  // Reshape line with an interior bend: enters the selected square through
  // its left edge and exits through the right (QGIS 4.2 digest:
  // reshapeGeometry only reports Success when the line crosses out of the
  // polygon), bumping at (20,25). Every line vertex comes from a left click —
  // the right-click commit does not append its own position.
  click( tool, canvas, pxAt( canvas, 5, 20 ), Qt::LeftButton );
  click( tool, canvas, pxAt( canvas, 20, 25 ), Qt::LeftButton );
  click( tool, canvas, pxAt( canvas, 35, 20 ), Qt::LeftButton );
  QCOMPARE( tool.vertexCount(), 3 );
  click( tool, canvas, pxAt( canvas, 35, 20 ), Qt::RightButton ); // commit

  QCOMPARE( tool.reshapedCount(), 1 );
  QCOMPARE( editedSpy.count(), 1 );
  QCOMPARE( editedSpy.at( 0 ).at( 0 ).toString(), layer.id() );
  QCOMPARE( abortSpy.count(), 0 );

  // Selected feature: vertex count changed by the bent reshape line
  // (4 corners + closure → 6 ring points) and the area is no longer 20x20.
  const QgsGeometry geomA = layer.getFeature( fidA ).geometry();
  const QgsLineString *ringA = qgsgeometry_cast<const QgsLineString *>( asPolygon( geomA )->exteriorRing() );
  QVERIFY2( ringA, "reshaped geometry lost its linear exterior ring" );
  QCOMPARE( ringA->numPoints(), 6 );
  QVERIFY( !qgsDoubleNear( geomA.area(), 400.0, 1e-6 ) );

  // The unselected feature is untouched (bbox pre-filter skipped it).
  QCOMPARE( layer.getFeature( fidB ).geometry().asWkt(), originalB );

  // One edit command for the batch → one native undo step restores it.
  QUndoStack *us = layer.undoStack();
  QCOMPARE( us->count(), 1 );
  us->undo();
  const QgsGeometry restoredA = layer.getFeature( fidA ).geometry();
  const QgsLineString *restoredRing = qgsgeometry_cast<const QgsLineString *>( asPolygon( restoredA )->exteriorRing() );
  QVERIFY( restoredRing );
  QCOMPARE( restoredRing->numPoints(), 5 ); // 4 corners + closing point
  QVERIFY( qgsDoubleNear( restoredA.area(), 400.0, 1e-6 ) );

  layer.rollBack();
  canvas.unsetMapTool( &tool );
}

void TestEditTools::vertexDoubleClickInsertsVertex()
{
  QgsMapCanvas canvas;
  configureCanvas( canvas );

  QgsVectorLayer layer( QStringLiteral( "LineString?crs=EPSG:4326&field=id:integer" ), QStringLiteral( "ins" ), QStringLiteral( "memory" ) );
  QVERIFY2( layer.isValid(), "memory line layer failed to initialize" );
  const QgsFeatureId fid = seedFeature( layer, QgsGeometry::fromPolylineXY(
      { mapPt( canvas, 40, 140 ), mapPt( canvas, 100, 100 ), mapPt( canvas, 160, 60 ) } ) );
  layer.startEditing();
  layer.selectByIds( { fid } );
  canvas.setLayers( QList<QgsMapLayer *>{ &layer } );
  canvas.setCurrentLayer( &layer );
  canvas.refresh();

  TestVertexTool tool( &canvas, &layer );
  canvas.setMapTool( &tool );
  QSignalSpy editedSpy( &tool, &PaleoVertexTool::featureEdited );

  // (130,80) is exactly the midpoint of the segment (100,100)-(160,60).
  const QgsPointXY mid = mapPt( canvas, 130, 80 );
  QgsMapMouseEvent dbl( &canvas, QEvent::MouseButtonDblClick, QPoint( 130, 80 ), Qt::LeftButton, Qt::LeftButton, Qt::NoModifier );
  tool.canvasDoubleClickEvent( &dbl );

  QCOMPARE( editedSpy.count(), 1 );
  QCOMPARE( editedSpy.at( 0 ).at( 0 ).toString(), layer.id() );
  QCOMPARE( tool.editedCount(), 1 );
  QCOMPARE( vertexTotal( layer, fid ), 4 ); // 3 → 4

  const QgsLineString *ls = asLineString( layer.getFeature( fid ).geometry() );
  QVERIFY2( ls, "geometry is not a line string after insertion" );
  QCOMPARE( ls->numPoints(), 4 );
  // Inserted before the segment's next vertex: p1, p2, mid, p3.
  QVERIFY( qgsDoubleNear( ls->xAt( 2 ), mid.x(), 1e-6 ) );
  QVERIFY( qgsDoubleNear( ls->yAt( 2 ), mid.y(), 1e-6 ) );
  QVERIFY( qgsDoubleNear( ls->xAt( 3 ), mapPt( canvas, 160, 60 ).x(), 1e-6 ) );
  QVERIFY( qgsDoubleNear( ls->yAt( 3 ), mapPt( canvas, 160, 60 ).y(), 1e-6 ) );
  QCOMPARE( tool.markerCount(), 4 ); // markers rebuilt with the new vertex

  // Native undo removes the inserted vertex again.
  layer.undoStack()->undo();
  QCOMPARE( vertexTotal( layer, fid ), 3 );

  canvas.unsetMapTool( &tool );
  layer.rollBack();
}

void TestEditTools::vertexRightClickDeletesWithUndo()
{
  QgsMapCanvas canvas;
  configureCanvas( canvas );

  QgsVectorLayer layer( QStringLiteral( "LineString?crs=EPSG:4326&field=id:integer" ), QStringLiteral( "delv" ), QStringLiteral( "memory" ) );
  QVERIFY2( layer.isValid(), "memory line layer failed to initialize" );
  const QgsFeatureId fid = seedFeature( layer, QgsGeometry::fromPolylineXY(
      { mapPt( canvas, 40, 140 ), mapPt( canvas, 100, 100 ), mapPt( canvas, 160, 60 ) } ) );
  layer.startEditing();
  layer.selectByIds( { fid } );
  canvas.setLayers( QList<QgsMapLayer *>{ &layer } );
  canvas.setCurrentLayer( &layer );
  canvas.refresh();

  TestVertexTool tool( &canvas, &layer );
  canvas.setMapTool( &tool );
  QSignalSpy editedSpy( &tool, &PaleoVertexTool::featureEdited );
  QSignalSpy msgSpy( &tool, &QgsMapTool::messageEmitted );

  // 3 vertices: deleting down to 2 is legal (right-click on vertex 2).
  click( tool, canvas, QPoint( 160, 60 ), Qt::RightButton );

  QCOMPARE( editedSpy.count(), 1 );
  QCOMPARE( editedSpy.at( 0 ).at( 0 ).toString(), layer.id() );
  QCOMPARE( msgSpy.count(), 0 );
  QCOMPARE( tool.editedCount(), 1 );
  QCOMPARE( vertexTotal( layer, fid ), 2 );
  QCOMPARE( tool.markerCount(), 2 );

  const QgsLineString *ls = asLineString( layer.getFeature( fid ).geometry() );
  QVERIFY2( ls, "geometry is not a line string after vertex delete" );
  QVERIFY( qgsDoubleNear( ls->xAt( 0 ), mapPt( canvas, 40, 140 ).x(), 1e-6 ) );
  QVERIFY( qgsDoubleNear( ls->xAt( 1 ), mapPt( canvas, 100, 100 ).x(), 1e-6 ) );

  layer.undoStack()->undo();
  QCOMPARE( vertexTotal( layer, fid ), 3 );

  canvas.unsetMapTool( &tool );
  layer.rollBack();
}

void TestEditTools::vertexDeleteRefusedBelowMinimums()
{
  QgsMapCanvas canvas;
  configureCanvas( canvas );

  // Line guard: a 2-vertex line cannot drop to 1.
  {
    QgsVectorLayer layer( QStringLiteral( "LineString?crs=EPSG:4326&field=id:integer" ), QStringLiteral( "min1" ), QStringLiteral( "memory" ) );
    QVERIFY2( layer.isValid(), "memory line layer failed to initialize" );
    const QgsFeatureId fid = seedFeature( layer, QgsGeometry::fromPolylineXY(
        { mapPt( canvas, 40, 140 ), mapPt( canvas, 160, 60 ) } ) );
    layer.startEditing();
    layer.selectByIds( { fid } );
    canvas.setLayers( QList<QgsMapLayer *>{ &layer } );
    canvas.setCurrentLayer( &layer );
    canvas.refresh();

    TestVertexTool tool( &canvas, &layer );
    canvas.setMapTool( &tool );
    QSignalSpy editedSpy( &tool, &PaleoVertexTool::featureEdited );
    QSignalSpy msgSpy( &tool, &QgsMapTool::messageEmitted );

    click( tool, canvas, QPoint( 40, 140 ), Qt::RightButton ); // exact hit on vertex 0

    QCOMPARE( editedSpy.count(), 0 );            // refused…
    QCOMPARE( msgSpy.count(), 1 );               // …with a guard warning
    QCOMPARE( tool.editedCount(), 0 );
    QCOMPARE( vertexTotal( layer, fid ), 2 );    // geometry untouched
    QCOMPARE( layer.undoStack()->count(), 0 );   // no edit command pushed

    canvas.unsetMapTool( &tool );
    layer.rollBack();
  }

  // Ring guard: a quad ring (5 vertices incl. closure) may drop to 4
  // (triangle+closure) but not to 3.
  {
    QgsVectorLayer layer( QStringLiteral( "Polygon?crs=EPSG:4326&field=id:integer" ), QStringLiteral( "min2" ), QStringLiteral( "memory" ) );
    QVERIFY2( layer.isValid(), "memory polygon layer failed to initialize" );
    const QgsFeatureId quadFid = seedFeature( layer, QgsGeometry::fromPolygonXY( { QVector<QgsPointXY>{
        mapPt( canvas, 40, 140 ), mapPt( canvas, 160, 140 ), mapPt( canvas, 160, 60 ), mapPt( canvas, 40, 60 ) } } ) );
    layer.startEditing();
    layer.selectByIds( { quadFid } );
    canvas.setLayers( QList<QgsMapLayer *>{ &layer } );
    canvas.setCurrentLayer( &layer );
    canvas.refresh();

    TestVertexTool tool( &canvas, &layer );
    canvas.setMapTool( &tool );
    QCOMPARE( tool.markerCount(), 5 );

    QSignalSpy editedSpy( &tool, &PaleoVertexTool::featureEdited );
    QSignalSpy msgSpy( &tool, &QgsMapTool::messageEmitted );

    // 5 → 4 ring vertices is legal (click on vertex 2 at (160,60)).
    click( tool, canvas, QPoint( 160, 60 ), Qt::RightButton );
    QCOMPARE( editedSpy.count(), 1 );
    QCOMPARE( msgSpy.count(), 0 );
    QCOMPARE( vertexTotal( layer, quadFid ), 4 );
    QCOMPARE( tool.markerCount(), 4 );

    // 4 → 3 would be a degenerate ring — refused. After deleting vertex 2
    // the ring is (40,140) (160,140) (40,60) + closure; (160,140) is an
    // exact hit far from any other vertex.
    click( tool, canvas, QPoint( 160, 140 ), Qt::RightButton );
    QCOMPARE( editedSpy.count(), 1 );           // still only the first delete
    QCOMPARE( msgSpy.count(), 1 );              // guard warning
    QCOMPARE( vertexTotal( layer, quadFid ), 4 );
    QCOMPARE( tool.editedCount(), 1 );

    canvas.unsetMapTool( &tool );
    layer.rollBack();
  }
}

// ---------------------------------------------------------------------------
// d) PaleoUndoStack (non-toolbar part)
// ---------------------------------------------------------------------------

void TestEditTools::undoStackAttachForwardUndoRedo()
{
  auto layer = std::make_unique<QgsVectorLayer>( QStringLiteral( "Point?crs=EPSG:4326&field=id:integer" ),
                                                 QStringLiteral( "us1" ), QStringLiteral( "memory" ) );
  QVERIFY2( layer->isValid(), "memory layer failed to initialize" );

  // Detached state: everything is a silent no-op.
  PaleoUndoStack ps;
  QVERIFY( ps.layer() == nullptr );
  QVERIFY( !ps.canUndo() );
  QVERIFY( !ps.canRedo() );
  QCOMPARE( ps.count(), 0 );
  ps.undo();
  ps.redo();
  QVERIFY( ps.setLayer( nullptr ) ); // detach while detached still succeeds

  layer->startEditing();
  QSignalSpy layerSpy( &ps, &PaleoUndoStack::layerChanged );
  QSignalSpy canUndoSpy( &ps, &PaleoUndoStack::canUndoChanged );

  QVERIFY( ps.setLayer( layer.get() ) );
  QCOMPARE( ps.layer(), layer.get() );
  QCOMPARE( layerSpy.count(), 1 );
  QCOMPARE( layerSpy.takeFirst().at( 0 ).value<QgsVectorLayer *>(), layer.get() );
  QVERIFY( !ps.canUndo() );
  QCOMPARE( ps.count(), 0 );

  addPointCommand( layer.get(), 1 );
  addPointCommand( layer.get(), 2 );

  // Forwarding: two command groups on the native stack, canUndo re-emitted.
  QCOMPARE( ps.count(), 2 );
  QVERIFY( ps.canUndo() );
  QVERIFY( !ps.canRedo() );
  QCOMPARE( layer->featureCount(), 2 );
  QVERIFY( canUndoSpy.count() >= 1 );
  QCOMPARE( canUndoSpy.takeLast().at( 0 ).toBool(), true );

  // Wrapper undo/redo step the edit buffer exactly one group at a time.
  ps.undo();
  QCOMPARE( layer->featureCount(), 1 );
  QVERIFY( ps.canUndo() );
  QVERIFY( ps.canRedo() );
  QCOMPARE( ps.count(), 2 ); // count() is total groups, index-independent

  ps.redo();
  QCOMPARE( layer->featureCount(), 2 );
  QVERIFY( ps.canUndo() );
  QVERIFY( !ps.canRedo() );

  ps.undo();
  ps.undo();
  QCOMPARE( layer->featureCount(), 0 );
  QVERIFY( !ps.canUndo() ); // stepped to the bottom
  QVERIFY( ps.canRedo() );
  ps.undo();                // cannot step further: no-op
  QCOMPARE( layer->featureCount(), 0 );

  // Same-layer re-set is idempotent: no layerChanged churn.
  QSignalSpy idemSpy( &ps, &PaleoUndoStack::layerChanged );
  QVERIFY( ps.setLayer( layer.get() ) );
  QCOMPARE( idemSpy.count(), 0 );

  layer->rollBack();
}

void TestEditTools::undoStackSwitchRefusedAndCommitClears()
{
  auto a = std::make_unique<QgsVectorLayer>( QStringLiteral( "Point?crs=EPSG:4326&field=id:integer" ),
                                             QStringLiteral( "a" ), QStringLiteral( "memory" ) );
  auto b = std::make_unique<QgsVectorLayer>( QStringLiteral( "Point?crs=EPSG:4326&field=id:integer" ),
                                             QStringLiteral( "b" ), QStringLiteral( "memory" ) );
  a->startEditing();
  b->startEditing();

  PaleoUndoStack ps;
  QVERIFY( ps.setLayer( a.get() ) );
  addPointCommand( a.get(), 1 );

  // Dirty switch: refused, watch target unchanged, reason carried.
  QSignalSpy refusedSpy( &ps, &PaleoUndoStack::switchRefused );
  QSignalSpy layerSpy( &ps, &PaleoUndoStack::layerChanged );
  QVERIFY( !ps.setLayer( b.get() ) );
  QCOMPARE( refusedSpy.count(), 1 );
  const QList<QVariant> args = refusedSpy.takeFirst();
  QCOMPARE( args.at( 0 ).toString(), a->id() );    // the layer holding pending edits
  QVERIFY( !args.at( 1 ).toString().isEmpty() );   // human-readable reason
  QCOMPARE( ps.layer(), a.get() );
  QCOMPARE( layerSpy.count(), 0 );
  QVERIFY( ps.canUndo() );                         // state untouched

  // Commit clears the native stack (save/clear policy) → clean switch.
  QVERIFY( a->commitChanges() );
  QCOMPARE( ps.count(), 0 );
  QVERIFY( !ps.canUndo() );
  QVERIFY( !ps.canRedo() );

  QVERIFY( ps.setLayer( b.get() ) );
  QCOMPARE( ps.layer(), b.get() );
  QCOMPARE( layerSpy.count(), 1 );

  // Rollback clears the native stack the same way.
  addPointCommand( b.get(), 1 );
  QVERIFY( ps.canUndo() );
  b->rollBack();
  QCOMPARE( ps.count(), 0 );
  QVERIFY( !ps.canUndo() );
  QVERIFY( !ps.canRedo() );

  auto other = std::make_unique<QgsVectorLayer>( QStringLiteral( "Point?crs=EPSG:4326" ),
                                                 QStringLiteral( "other" ), QStringLiteral( "memory" ) );
  other->startEditing();
  QVERIFY( ps.setLayer( other.get() ) ); // clean switch without commit
  other->rollBack();
}

void TestEditTools::undoStackDetachReattachDestroyedSafe()
{
  PaleoUndoStack ps;
  auto layer = std::make_unique<QgsVectorLayer>( QStringLiteral( "Point?crs=EPSG:4326&field=id:integer" ),
                                                 QStringLiteral( "heap" ), QStringLiteral( "memory" ) );
  QVERIFY2( layer->isValid(), "memory layer failed to initialize" );
  layer->startEditing();
  QVERIFY( ps.setLayer( layer.get() ) );
  addPointCommand( layer.get(), 1 );
  QVERIFY( ps.canUndo() );

  // nullptr always detaches, even with pending commands (contract).
  QSignalSpy layerSpy( &ps, &PaleoUndoStack::layerChanged );
  QVERIFY( ps.setLayer( nullptr ) );
  QVERIFY( ps.layer() == nullptr );
  QVERIFY( !ps.canUndo() );
  QVERIFY( !ps.canRedo() );
  QCOMPARE( ps.count(), 0 );
  QCOMPARE( layerSpy.count(), 1 );
  ps.undo();                                  // no-op while detached
  QCOMPARE( layer->featureCount(), 1 );       // untouched through the wrapper

  // Re-attach: the pending command group is reachable again.
  QVERIFY( ps.setLayer( layer.get() ) );
  QVERIFY( ps.canUndo() );
  QCOMPARE( ps.count(), 1 );
  ps.undo();
  QCOMPARE( layer->featureCount(), 0 );

  // Foreign destruction (project teardown, layers on test stacks): the
  // QPointer + destroyed() path auto-detaches without use-after-free.
  QSignalSpy canUndoSpy( &ps, &PaleoUndoStack::canUndoChanged );
  addPointCommand( layer.get(), 5 );
  QVERIFY( ps.canUndo() );
  layer.reset();
  QVERIFY( ps.layer() == nullptr );      // QPointer reads null, not dangling
  QVERIFY( !ps.canUndo() );
  QVERIFY( !ps.canRedo() );
  QCOMPARE( ps.count(), 0 );
  ps.undo();                             // silent no-ops
  ps.redo();
  QVERIFY( ps.setLayer( nullptr ) );
  QCOMPARE( layerSpy.count(), 3 );       // detach + re-attach + auto-detach announced
  QCOMPARE( canUndoSpy.takeLast().at( 0 ).toBool(), false );

  // The wrapper stays usable afterwards. NB: the helper commits one
  // attribute, so the layer schema must carry the id field.
  auto fresh = std::make_unique<QgsVectorLayer>( QStringLiteral( "Point?crs=EPSG:4326&field=id:integer" ),
                                                 QStringLiteral( "fresh" ), QStringLiteral( "memory" ) );
  fresh->startEditing();
  QVERIFY( ps.setLayer( fresh.get() ) );
  addPointCommand( fresh.get(), 7 );
  QVERIFY( ps.canUndo() );
  ps.undo();
  QCOMPARE( fresh->featureCount(), 0 );
  fresh->rollBack();
}

// ---------------------------------------------------------------------------
// d/e/f) PaleoEditingToolbar — contract tests against editingtoolbar.h.
// (The .cpp is implemented by a parallel agent; failures here are reported,
// not fixed.)
// ---------------------------------------------------------------------------

void TestEditTools::toolbarStartStopSignalsAndStateRendering()
{
  QgsMapCanvas canvas;
  configureCanvas( canvas );

  // Refusal first: no current layer → startEditing refused, no signal noise.
  {
    PaleoEditingToolbar emptyBar( &canvas );
    QSignalSpy startedSpy( &emptyBar, &PaleoEditingToolbar::editingStarted );
    QSignalSpy refusedSpy( &emptyBar, &PaleoEditingToolbar::editRefused );
    QVERIFY2( !emptyBar.startEditing(), "startEditing must refuse without a current layer" );
    QVERIFY( refusedSpy.count() >= 1 );
    QCOMPARE( startedSpy.count(), 0 );
  }

  QgsVectorLayer layer( QStringLiteral( "Point?crs=EPSG:4326&field=id:integer" ), QStringLiteral( "editing-deck" ), QStringLiteral( "memory" ) );
  QVERIFY2( layer.isValid(), "memory layer failed to initialize" );

  PaleoEditingToolbar bar( &canvas );
  bar.setLayers( QList<QgsVectorLayer *>{ &layer } );
  bar.setCurrentLayer( &layer );
  QVERIFY( !bar.isEditing() );

  // setCurrentLayer contract: combo + canvas current layer follow.
  QCOMPARE( bar.currentLayer(), &layer );
  QCOMPARE( canvas.currentLayer(), static_cast<QgsMapLayer *>( &layer ) );

  QSignalSpy startedSpy( &bar, &PaleoEditingToolbar::editingStarted );
  QSignalSpy stoppedSpy( &bar, &PaleoEditingToolbar::editingStopped );

  QVERIFY( bar.startEditing() );
  QVERIFY( layer.isEditable() );
  QVERIFY( bar.isEditing() );
  QCOMPARE( startedSpy.count(), 1 );
  QCOMPARE( startedSpy.at( 0 ).at( 0 ).toString(), layer.id() );

  // State rendering: editing layer highlighted with the ● marker in the
  // combo; the label shows 编辑中 with the layer name.
  const QString comboText = bar.layerCombo()->currentText();
  QVERIFY2( comboText.contains( QChar( 0x25CF ) ), "editing layer must carry the ● highlight in the combo" );
  QVERIFY( comboText.contains( QStringLiteral( "editing-deck" ) ) );
  const QString labelText = bar.stateLabel()->text();
  QVERIFY2( labelText.contains( QStringLiteral( "编辑中" ) ), "state label must show 编辑中 while editing" );
  QVERIFY( labelText.contains( QStringLiteral( "editing-deck" ) ) );

  QVERIFY( bar.saveEditing() );
  QVERIFY( !layer.isEditable() );
  QVERIFY( !bar.isEditing() );
  QCOMPARE( stoppedSpy.count(), 1 );
  QCOMPARE( stoppedSpy.at( 0 ).at( 0 ).toString(), layer.id() );
  QCOMPARE( stoppedSpy.at( 0 ).at( 1 ).toBool(), true );
  QVERIFY( !bar.layerCombo()->currentText().contains( QChar( 0x25CF ) ) ); // highlight gone
  QVERIFY( !bar.stateLabel()->text().contains( QStringLiteral( "编辑中" ) ) );
}

void TestEditTools::toolbarComboFilterAndProjectRefresh()
{
  QgsMapCanvas canvas;
  configureCanvas( canvas );

  QgsVectorLayer lava( QStringLiteral( "Point?crs=EPSG:4326" ), QStringLiteral( "lava" ), QStringLiteral( "memory" ) );
  QgsVectorLayer ash( QStringLiteral( "Point?crs=EPSG:4326" ), QStringLiteral( "ash" ), QStringLiteral( "memory" ) );
  QgsVectorLayer silt( QStringLiteral( "Point?crs=EPSG:4326" ), QStringLiteral( "silt" ), QStringLiteral( "memory" ) );

  PaleoEditingToolbar bar( &canvas );

  // Default filter accepts every vector layer.
  bar.setLayers( QList<QgsVectorLayer *>{ &lava, &ash, &silt } );
  QCOMPARE( bar.layerCombo()->count(), 3 );

  // Host-injected filter: only passing layers are listed.
  QgsVectorLayer *rejected = &ash;
  bar.setLayerFilter( [rejected]( const QgsVectorLayer *l ) { return l != rejected; } );
  bar.setLayers( QList<QgsVectorLayer *>{ &lava, &ash, &silt } );
  QCOMPARE( bar.layerCombo()->count(), 2 );

  QStringList listed;
  for ( int i = 0; i < bar.layerCombo()->count(); ++i )
    listed << bar.layerCombo()->itemText( i );
  QVERIFY( listed.join( QLatin1Char( ' ' ) ).contains( QStringLiteral( "lava" ) ) );
  QVERIFY( listed.join( QLatin1Char( ' ' ) ).contains( QStringLiteral( "silt" ) ) );
  QVERIFY( !listed.join( QLatin1Char( ' ' ) ).contains( QStringLiteral( "ash" ) ) );

  // Keeping the current selection across a setLayers round trip.
  bar.setCurrentLayer( &lava );
  bar.setLayers( QList<QgsVectorLayer *>{ &lava, &ash, &silt } );
  QCOMPARE( bar.currentLayer(), &lava );

  // Project-driven candidates: refreshFromProject pulls QgsProject vector
  // layers (heap layers owned by the project; init()/cleanup() clear it).
  QgsVectorLayer *projA = new QgsVectorLayer( QStringLiteral( "Point?crs=EPSG:4326" ), QStringLiteral( "projA" ), QStringLiteral( "memory" ) );
  QgsVectorLayer *projB = new QgsVectorLayer( QStringLiteral( "Point?crs=EPSG:4326" ), QStringLiteral( "projB" ), QStringLiteral( "memory" ) );
  QVERIFY2( projA->isValid() && projB->isValid(), "project layers failed to initialize" );
  QgsProject::instance()->addMapLayer( projA );
  QgsProject::instance()->addMapLayer( projB );
  bar.refreshFromProject();
  QCOMPARE( bar.layerCombo()->count(), 2 );
  QStringList projectListed;
  for ( int i = 0; i < bar.layerCombo()->count(); ++i )
    projectListed << bar.layerCombo()->itemText( i );
  QVERIFY( projectListed.join( QLatin1Char( ' ' ) ).contains( QStringLiteral( "projA" ) ) );
  QVERIFY( projectListed.join( QLatin1Char( ' ' ) ).contains( QStringLiteral( "projB" ) ) );
}

void TestEditTools::toolbarEditToolActionAutoStartsSession()
{
  QgsMapCanvas canvas;
  configureCanvas( canvas );

  QgsVectorLayer layer( QStringLiteral( "Point?crs=EPSG:4326&field=id:integer" ), QStringLiteral( "autostart" ), QStringLiteral( "memory" ) );
  QVERIFY2( layer.isValid(), "memory layer failed to initialize" );
  QVERIFY( !layer.isEditable() );

  PaleoEditingToolbar bar( &canvas );
  bar.setLayers( QList<QgsVectorLayer *>{ &layer } );
  bar.setCurrentLayer( &layer );

  QSignalSpy startedSpy( &bar, &PaleoEditingToolbar::editingStarted );
  QSignalSpy stoppedSpy( &bar, &PaleoEditingToolbar::editingStopped );

  // Edit-tool actions auto-start the session and install the tool.
  bar.actionAddPoint()->trigger();
  QVERIFY2( layer.isEditable(), "triggering an edit-tool action must auto-start editing" );
  QCOMPARE( startedSpy.count(), 1 );
  QCOMPARE( startedSpy.at( 0 ).at( 0 ).toString(), layer.id() );
  QVERIFY2( canvas.mapTool() != nullptr, "triggering an edit-tool action must install a map tool" );
  auto *addTool = qobject_cast<PaleoAddFeatureTool *>( canvas.mapTool() );
  QVERIFY2( addTool, "actionAddPoint must install a PaleoAddFeatureTool" );
  QCOMPARE( addTool->mode(), QgsMapToolCapture::CapturePoint );

  // Save through the action surface ends the session cleanly.
  bar.actionSave()->trigger();
  QVERIFY( !layer.isEditable() );
  QCOMPARE( stoppedSpy.count(), 1 );
  QCOMPARE( stoppedSpy.at( 0 ).at( 1 ).toBool(), true );
}

void TestEditTools::toolbarUndoRedoActionStates()
{
  QgsMapCanvas canvas;
  configureCanvas( canvas );

  QgsVectorLayer layer( QStringLiteral( "Point?crs=EPSG:4326&field=id:integer" ), QStringLiteral( "undotb" ), QStringLiteral( "memory" ) );
  QVERIFY2( layer.isValid(), "memory layer failed to initialize" );

  PaleoEditingToolbar bar( &canvas );
  bar.setLayers( QList<QgsVectorLayer *>{ &layer } );
  bar.setCurrentLayer( &layer );
  QVERIFY( bar.undoStack() != nullptr );

  // Idle stack: both actions disabled.
  QVERIFY( !bar.actionUndo()->isEnabled() );
  QVERIFY( !bar.actionRedo()->isEnabled() );

  QVERIFY( bar.startEditing() );
  QVERIFY( !bar.actionUndo()->isEnabled() );
  QVERIFY( !bar.actionRedo()->isEnabled() );

  // The native stack drives the button states (canUndoChanged forwarding).
  addPointCommand( &layer, 1 );
  QVERIFY2( bar.actionUndo()->isEnabled(), "undo action must enable after an edit command" );
  QVERIFY( !bar.actionRedo()->isEnabled() );

  bar.undoStack()->undo();
  QVERIFY( !bar.actionUndo()->isEnabled() );
  QVERIFY2( bar.actionRedo()->isEnabled(), "redo action must enable after undo" );

  bar.undoStack()->redo();
  QVERIFY( bar.actionUndo()->isEnabled() );
  QVERIFY( !bar.actionRedo()->isEnabled() );

  layer.rollBack();
  QVERIFY( !bar.actionUndo()->isEnabled() );
  QVERIFY( !bar.actionRedo()->isEnabled() );
}

void TestEditTools::toolbarSavePersistsAndClearsUndo()
{
  QgsMapCanvas canvas;
  configureCanvas( canvas );

  QgsVectorLayer layer( QStringLiteral( "Point?crs=EPSG:4326&field=id:integer" ), QStringLiteral( "persist" ), QStringLiteral( "memory" ) );
  QVERIFY2( layer.isValid(), "memory layer failed to initialize" );
  QCOMPARE( layer.featureCount(), 0 );

  PaleoEditingToolbar bar( &canvas );
  bar.setLayers( QList<QgsVectorLayer *>{ &layer } );
  bar.setCurrentLayer( &layer );
  QVERIFY( bar.startEditing() );

  addPointCommand( &layer, 1 );
  addPointCommand( &layer, 2 );
  QCOMPARE( layer.featureCount(), 2 );
  QVERIFY( bar.actionUndo()->isEnabled() );

  QSignalSpy stoppedSpy( &bar, &PaleoEditingToolbar::editingStopped );
  QVERIFY2( bar.saveEditing(), "saving an active session must succeed" );
  QCOMPARE( stoppedSpy.count(), 1 );
  QCOMPARE( stoppedSpy.at( 0 ).at( 0 ).toString(), layer.id() );
  QCOMPARE( stoppedSpy.at( 0 ).at( 1 ).toBool(), true );

  // Memory-provider data persisted the commit; the native stack is cleared.
  QVERIFY( !layer.isEditable() );
  QVERIFY( !bar.isEditing() );
  QCOMPARE( layer.featureCount(), 2 );
  QCOMPARE( bar.undoStack()->count(), 0 );
  QVERIFY( !bar.undoStack()->canUndo() );
  QVERIFY( !bar.actionUndo()->isEnabled() );
  QVERIFY( !bar.actionRedo()->isEnabled() );

  // Reopening the session still sees the saved data.
  QVERIFY( layer.startEditing() );
  QCOMPARE( layer.featureCount(), 2 );
  QVERIFY( layer.editBuffer()->addedFeatures().isEmpty() );
  layer.rollBack();
}

void TestEditTools::toolbarCancelDiscardsEdits()
{
  QgsMapCanvas canvas;
  configureCanvas( canvas );

  QgsVectorLayer layer( QStringLiteral( "Point?crs=EPSG:4326&field=id:integer" ), QStringLiteral( "discard" ), QStringLiteral( "memory" ) );
  QVERIFY2( layer.isValid(), "memory layer failed to initialize" );
  const QgsFeatureId seeded = seedFeature( layer, QgsGeometry::fromWkt( QStringLiteral( "Point (3 3)" ) ) );
  QCOMPARE( layer.featureCount(), 1 );

  PaleoEditingToolbar bar( &canvas );
  bar.setLayers( QList<QgsVectorLayer *>{ &layer } );
  bar.setCurrentLayer( &layer );
  QVERIFY( bar.startEditing() );

  addPointCommand( &layer, 1 );
  addPointCommand( &layer, 2 );
  QCOMPARE( layer.featureCount(), 3 );

  QSignalSpy stoppedSpy( &bar, &PaleoEditingToolbar::editingStopped );
  QVERIFY2( bar.cancelEditing(), "cancelling an active session must succeed" );
  QCOMPARE( stoppedSpy.count(), 1 );
  QCOMPARE( stoppedSpy.at( 0 ).at( 0 ).toString(), layer.id() );
  QCOMPARE( stoppedSpy.at( 0 ).at( 1 ).toBool(), false );

  // Every unsaved edit is gone; the seeded feature survives.
  QVERIFY( !layer.isEditable() );
  QVERIFY( !bar.isEditing() );
  QCOMPARE( layer.featureCount(), 1 );
  QVERIFY( layer.getFeature( seeded ).isValid() );
  QCOMPARE( bar.undoStack()->count(), 0 );
  QVERIFY( !bar.undoStack()->canUndo() );
  QVERIFY( !bar.actionUndo()->isEnabled() );
}

void TestEditTools::toolbarSaveRefusedOutsideSession()
{
  QgsMapCanvas canvas;
  configureCanvas( canvas );

  QgsVectorLayer layer( QStringLiteral( "Point?crs=EPSG:4326&field=id:integer" ), QStringLiteral( "cold" ), QStringLiteral( "memory" ) );
  QVERIFY2( layer.isValid(), "memory layer failed to initialize" );
  QVERIFY( !layer.isEditable() );

  PaleoEditingToolbar bar( &canvas );
  bar.setLayers( QList<QgsVectorLayer *>{ &layer } );
  bar.setCurrentLayer( &layer );

  QSignalSpy stoppedSpy( &bar, &PaleoEditingToolbar::editingStopped );
  QSignalSpy refusedSpy( &bar, &PaleoEditingToolbar::editRefused );

  // Save on a layer without an edit session: refused, no stopped signal.
  QVERIFY2( !bar.saveEditing(), "saving outside an edit session must be refused" );
  QVERIFY( refusedSpy.count() >= 1 );
  QCOMPARE( stoppedSpy.count(), 0 );
  QVERIFY( !layer.isEditable() );
  QCOMPARE( layer.featureCount(), 0 );
}

// ---------------------------------------------------------------------------
// g) abort paths
// ---------------------------------------------------------------------------

void TestEditTools::addAbortPaths()
{
  QgsMapCanvas canvas;
  configureCanvas( canvas );

  QgsVectorLayer layer( QStringLiteral( "LineString?crs=EPSG:4326" ), QStringLiteral( "esc-add" ), QStringLiteral( "memory" ) );
  layer.startEditing();
  canvas.setLayers( QList<QgsMapLayer *>{ &layer } );
  canvas.setCurrentLayer( &layer );
  canvas.refresh();

  TestAddTool tool( &canvas, nullptr, QgsMapToolCapture::CaptureLine, &layer );
  canvas.setMapTool( &tool );
  QSignalSpy editedSpy( &tool, &PaleoAddFeatureTool::featureEdited );
  QSignalSpy abortSpy( &tool, &PaleoAddFeatureTool::editAborted );

  // Bare right-click with nothing captured → cancel gesture, not a commit.
  click( tool, canvas, QPoint( 50, 50 ), Qt::RightButton );
  QCOMPARE( abortSpy.count(), 1 );
  QCOMPARE( editedSpy.count(), 0 );
  QCOMPARE( layer.featureCount(), 0 );

  // One vertex then right-click → still below the 2-vertex commit threshold.
  tool.activate();
  click( tool, canvas, QPoint( 60, 60 ), Qt::LeftButton );
  QCOMPARE( tool.vertexCount(), 1 );
  click( tool, canvas, QPoint( 60, 60 ), Qt::RightButton );
  QCOMPARE( abortSpy.count(), 2 );
  QCOMPARE( editedSpy.count(), 0 );
  QCOMPARE( layer.featureCount(), 0 );

  // Esc mid-capture → §42.15 teardown signal, capture state cleaned.
  tool.activate();
  click( tool, canvas, QPoint( 40, 100 ), Qt::LeftButton );
  QCOMPARE( tool.vertexCount(), 1 );
  sendEsc( tool );
  QCOMPARE( abortSpy.count(), 3 );
  QCOMPARE( editedSpy.count(), 0 );
  QCOMPARE( tool.vertexCount(), 0 );
  QCOMPARE( layer.featureCount(), 0 );

  layer.rollBack();
  canvas.unsetMapTool( &tool );
}

void TestEditTools::reshapeAbortPaths()
{
  QgsMapCanvas canvas;
  configureCanvas( canvas );

  QgsVectorLayer layer( QStringLiteral( "Polygon?crs=EPSG:4326" ), QStringLiteral( "esc-reshape" ), QStringLiteral( "memory" ) );
  const QgsFeatureList seeded = seedFeatures( layer, { squareWkt( 10, 10, 20 ) } );
  layer.startEditing();
  layer.select( QgsFeatureIds{ seeded.at( 0 ).id() } );
  canvas.setLayers( QList<QgsMapLayer *>{ &layer } );
  canvas.setCurrentLayer( &layer );
  canvas.refresh();

  TestReshapeTool tool( &canvas, nullptr, &layer );
  canvas.setMapTool( &tool );
  QSignalSpy editedSpy( &tool, &PaleoReshapeTool::featureEdited );
  QSignalSpy abortSpy( &tool, &PaleoReshapeTool::editAborted );

  // Bare right-click: <2 vertices is a cancel gesture.
  click( tool, canvas, pxAt( canvas, 5, 20 ), Qt::RightButton );
  QCOMPARE( abortSpy.count(), 1 );
  QCOMPARE( editedSpy.count(), 0 );

  // One vertex then right-click: still below the threshold.
  tool.activate();
  click( tool, canvas, pxAt( canvas, 5, 20 ), Qt::LeftButton );
  QCOMPARE( tool.vertexCount(), 1 );
  click( tool, canvas, pxAt( canvas, 5, 20 ), Qt::RightButton );
  QCOMPARE( abortSpy.count(), 2 );
  QCOMPARE( editedSpy.count(), 0 );
  QVERIFY( qgsDoubleNear( layer.getFeature( seeded.at( 0 ).id() ).geometry().area(), 400.0, 1e-6 ) );

  // Esc mid-capture → teardown signal.
  tool.activate();
  click( tool, canvas, pxAt( canvas, 5, 20 ), Qt::LeftButton );
  sendEsc( tool );
  QCOMPARE( abortSpy.count(), 3 );
  QCOMPARE( editedSpy.count(), 0 );

  layer.rollBack();
  canvas.unsetMapTool( &tool );
}

void TestEditTools::moveEscCancelsDragAndIdleAborts()
{
  QgsMapCanvas canvas;
  configureCanvas( canvas );

  QgsVectorLayer layer( QStringLiteral( "Polygon?crs=EPSG:4326" ), QStringLiteral( "esc-move" ), QStringLiteral( "memory" ) );
  const QgsFeatureList seeded = seedFeatures( layer, { squareWkt( 10, 10, 20 ) } );
  const QgsFeatureId fid = seeded.at( 0 ).id();
  layer.startEditing();
  layer.select( QgsFeatureIds{ fid } );
  canvas.setLayers( QList<QgsMapLayer *>{ &layer } );
  canvas.setCurrentLayer( &layer );
  canvas.refresh();

  TestMoveTool tool( &canvas, &layer );
  canvas.setMapTool( &tool );
  QSignalSpy editedSpy( &tool, &PaleoMoveTool::featureEdited );
  QSignalSpy abortSpy( &tool, &PaleoMoveTool::editAborted );

  // Esc mid-drag: preview cancelled, edit buffer untouched, teardown signal.
  QgsMapMouseEvent press( &canvas, QEvent::MouseButtonPress, pxAt( canvas, 20, 20 ), Qt::LeftButton, Qt::LeftButton, Qt::NoModifier );
  tool.canvasPressEvent( &press );
  QVERIFY( tool.isDragging() );

  QgsMapMouseEvent move( &canvas, QEvent::MouseMove, pxAt( canvas, 60, 70 ), Qt::NoButton, Qt::LeftButton, Qt::NoModifier );
  tool.canvasMoveEvent( &move );

  sendEsc( tool );
  QCOMPARE( abortSpy.count(), 1 );
  QCOMPARE( editedSpy.count(), 0 );
  QCOMPARE( tool.movedCount(), 0 );
  QVERIFY( !tool.isDragging() );

  const QgsLineString *ring = qgsgeometry_cast<const QgsLineString *>( asPolygon( layer.getFeature( fid ).geometry() )->exteriorRing() );
  QVERIFY( ring );
  QVERIFY( qgsDoubleNear( ring->xAt( 0 ), 10.0, 1e-9 ) );
  QVERIFY( qgsDoubleNear( ring->yAt( 0 ), 10.0, 1e-9 ) );
  QVERIFY( qgsDoubleNear( layer.getFeature( fid ).geometry().area(), 400.0, 1e-9 ) );
  QCOMPARE( layer.undoStack()->count(), 0 ); // nothing landed in the edit buffer

  // Esc while idle: teardown signal again (owner tears the tool down).
  sendEsc( tool );
  QCOMPARE( abortSpy.count(), 2 );
  QCOMPARE( editedSpy.count(), 0 );

  layer.rollBack();
  canvas.unsetMapTool( &tool );
}

void TestEditTools::deleteEscAndEmptySelectionWarns()
{
  QgsMapCanvas canvas;
  configureCanvas( canvas );

  QgsVectorLayer layer( QStringLiteral( "Point?crs=EPSG:4326" ), QStringLiteral( "esc-del" ), QStringLiteral( "memory" ) );
  seedFeatures( layer, { QStringLiteral( "Point (25 25)" ) } );
  layer.startEditing();
  QVERIFY( layer.selectedFeatureCount() == 0 );
  canvas.setLayers( QList<QgsMapLayer *>{ &layer } );
  canvas.setCurrentLayer( &layer );
  canvas.refresh();

  TestDeleteTool tool( &canvas, &layer );
  canvas.setMapTool( &tool );
  QSignalSpy editedSpy( &tool, &PaleoDeleteFeatureTool::featureEdited );
  QSignalSpy abortSpy( &tool, &PaleoDeleteFeatureTool::editAborted );
  QSignalSpy msgSpy( &tool, &QgsMapTool::messageEmitted );

  // Click with an empty selection: warned, but nothing was gestured → no abort.
  click( tool, canvas, pxAt( canvas, 25, 25 ), Qt::LeftButton );
  QVERIFY( msgSpy.count() >= 1 );
  QCOMPARE( abortSpy.count(), 0 );
  QCOMPARE( editedSpy.count(), 0 );
  QCOMPARE( tool.deletedCount(), 0 );
  QCOMPARE( layer.featureCount(), 1 );

  // Esc → teardown signal (§42.15).
  sendEsc( tool );
  QCOMPARE( abortSpy.count(), 1 );
  QCOMPARE( editedSpy.count(), 0 );

  layer.rollBack();
  canvas.unsetMapTool( &tool );
}

void TestEditTools::vertexEscCancelsDragAndIdleAborts()
{
  QgsMapCanvas canvas;
  configureCanvas( canvas );

  QgsVectorLayer layer( QStringLiteral( "LineString?crs=EPSG:4326" ), QStringLiteral( "esc-vtx" ), QStringLiteral( "memory" ) );
  const QgsFeatureId fid = seedFeature( layer, QgsGeometry::fromPolylineXY(
      { mapPt( canvas, 40, 140 ), mapPt( canvas, 100, 100 ), mapPt( canvas, 160, 60 ) } ) );
  layer.startEditing();
  layer.selectByIds( { fid } );
  canvas.setLayers( QList<QgsMapLayer *>{ &layer } );
  canvas.setCurrentLayer( &layer );
  canvas.refresh();

  TestVertexTool tool( &canvas, &layer );
  canvas.setMapTool( &tool );
  QSignalSpy editedSpy( &tool, &PaleoVertexTool::featureEdited );
  QSignalSpy abortSpy( &tool, &PaleoVertexTool::editAborted );

  // Esc mid-drag: geometry unchanged, drag cleared, NO abort signal (the
  // gesture is cancelled, not the tool torn down).
  QgsMapMouseEvent press( &canvas, QEvent::MouseButtonPress, QPoint( 40, 140 ), Qt::LeftButton, Qt::LeftButton, Qt::NoModifier );
  tool.canvasPressEvent( &press );
  QVERIFY( tool.isDragging() );
  QgsMapMouseEvent move( &canvas, QEvent::MouseMove, QPoint( 70, 110 ), Qt::NoButton, Qt::LeftButton, Qt::NoModifier );
  tool.canvasMoveEvent( &move );

  sendEsc( tool );
  QVERIFY( !tool.isDragging() );
  QCOMPARE( editedSpy.count(), 0 );
  QCOMPARE( abortSpy.count(), 0 );
  QCOMPARE( tool.markerCount(), 3 ); // markers survive the cancelled drag
  QCOMPARE( vertexTotal( layer, fid ), 3 );
  const QgsLineString *ls = asLineString( layer.getFeature( fid ).geometry() );
  QVERIFY( ls );
  QVERIFY( qgsDoubleNear( ls->xAt( 0 ), mapPt( canvas, 40, 140 ).x(), 1e-6 ) );
  QVERIFY( qgsDoubleNear( ls->yAt( 0 ), mapPt( canvas, 40, 140 ).y(), 1e-6 ) );

  // Esc while idle → editAborted (owner tears the tool down).
  sendEsc( tool );
  QCOMPARE( abortSpy.count(), 1 );
  QCOMPARE( editedSpy.count(), 0 );

  canvas.unsetMapTool( &tool );
  layer.rollBack();
}

void TestEditTools::warnOnlyRefusalsDontAbort()
{
  QgsMapCanvas canvas;
  configureCanvas( canvas );

  // Add tool on a locked layer: the commit attempt is a warning, not an abort.
  {
    QgsVectorLayer layer( QStringLiteral( "LineString?crs=EPSG:4326" ), QStringLiteral( "locked-add" ), QStringLiteral( "memory" ) );
    QVERIFY( !layer.isEditable() );
    canvas.setLayers( QList<QgsMapLayer *>{ &layer } );
    canvas.setCurrentLayer( &layer );
    canvas.refresh();

    TestAddTool tool( &canvas, nullptr, QgsMapToolCapture::CaptureLine, &layer );
    canvas.setMapTool( &tool );
    QSignalSpy editedSpy( &tool, &PaleoAddFeatureTool::featureEdited );
    QSignalSpy abortSpy( &tool, &PaleoAddFeatureTool::editAborted );
    QSignalSpy msgSpy( &tool, &QgsMapTool::messageEmitted );

    click( tool, canvas, QPoint( 20, 160 ), Qt::LeftButton );
    click( tool, canvas, QPoint( 140, 60 ), Qt::LeftButton );
    click( tool, canvas, QPoint( 140, 60 ), Qt::RightButton ); // attempted commit

    QVERIFY( msgSpy.count() >= 1 ); // warned (tool or capture base)
    QCOMPARE( abortSpy.count(), 0 ); // a refusal is not an abort
    QCOMPARE( editedSpy.count(), 0 );
    QCOMPARE( tool.committedCount(), 0 );
    QVERIFY( layer.editBuffer() == nullptr );

    canvas.unsetMapTool( &tool );
  }

  // Reshape tool on a locked layer (selection present).
  {
    QgsVectorLayer layer( QStringLiteral( "Polygon?crs=EPSG:4326" ), QStringLiteral( "locked-reshape" ), QStringLiteral( "memory" ) );
    const QgsFeatureList seeded = seedFeatures( layer, { squareWkt( 10, 10, 20 ) } );
    layer.select( QgsFeatureIds{ seeded.at( 0 ).id() } );
    QVERIFY( !layer.isEditable() );
    canvas.setLayers( QList<QgsMapLayer *>{ &layer } );
    canvas.setCurrentLayer( &layer );
    canvas.refresh();

    TestReshapeTool tool( &canvas, nullptr, &layer );
    canvas.setMapTool( &tool );
    QSignalSpy editedSpy( &tool, &PaleoReshapeTool::featureEdited );
    QSignalSpy abortSpy( &tool, &PaleoReshapeTool::editAborted );
    QSignalSpy msgSpy( &tool, &QgsMapTool::messageEmitted );

    click( tool, canvas, pxAt( canvas, 5, 20 ), Qt::LeftButton );
    click( tool, canvas, pxAt( canvas, 35, 20 ), Qt::LeftButton );
    click( tool, canvas, pxAt( canvas, 35, 20 ), Qt::RightButton );

    QVERIFY( msgSpy.count() >= 1 );
    QCOMPARE( abortSpy.count(), 0 );
    QCOMPARE( editedSpy.count(), 0 );
    QCOMPARE( tool.reshapedCount(), 0 );
    QCOMPARE( layer.featureCount(), 1 );

    canvas.unsetMapTool( &tool );
  }

  // Move tool on a locked layer.
  {
    QgsVectorLayer layer( QStringLiteral( "Polygon?crs=EPSG:4326" ), QStringLiteral( "locked-move" ), QStringLiteral( "memory" ) );
    const QgsFeatureList seeded = seedFeatures( layer, { squareWkt( 10, 10, 20 ) } );
    layer.select( QgsFeatureIds{ seeded.at( 0 ).id() } );
    QVERIFY( !layer.isEditable() );
    canvas.setLayers( QList<QgsMapLayer *>{ &layer } );
    canvas.setCurrentLayer( &layer );
    canvas.refresh();

    TestMoveTool tool( &canvas, &layer );
    canvas.setMapTool( &tool );
    QSignalSpy editedSpy( &tool, &PaleoMoveTool::featureEdited );
    QSignalSpy abortSpy( &tool, &PaleoMoveTool::editAborted );
    QSignalSpy msgSpy( &tool, &QgsMapTool::messageEmitted );

    click( tool, canvas, pxAt( canvas, 20, 20 ), Qt::LeftButton );

    QVERIFY( msgSpy.count() >= 1 );
    QCOMPARE( abortSpy.count(), 0 );
    QCOMPARE( editedSpy.count(), 0 );
    QVERIFY( !tool.isDragging() );

    canvas.unsetMapTool( &tool );
  }

  // Move tool on an editable layer without a selection: warn only.
  {
    QgsVectorLayer layer( QStringLiteral( "Polygon?crs=EPSG:4326" ), QStringLiteral( "nosel-move" ), QStringLiteral( "memory" ) );
    seedFeatures( layer, { squareWkt( 10, 10, 20 ) } );
    layer.startEditing();
    canvas.setLayers( QList<QgsMapLayer *>{ &layer } );
    canvas.setCurrentLayer( &layer );
    canvas.refresh();

    TestMoveTool tool( &canvas, &layer );
    canvas.setMapTool( &tool );
    QSignalSpy abortSpy( &tool, &PaleoMoveTool::editAborted );
    QSignalSpy editedSpy( &tool, &PaleoMoveTool::featureEdited );
    QSignalSpy msgSpy( &tool, &QgsMapTool::messageEmitted );

    click( tool, canvas, pxAt( canvas, 20, 20 ), Qt::LeftButton );

    QVERIFY( msgSpy.count() >= 1 );
    QCOMPARE( abortSpy.count(), 0 );
    QCOMPARE( editedSpy.count(), 0 );
    QVERIFY( !tool.isDragging() );

    layer.rollBack();
    canvas.unsetMapTool( &tool );
  }

  // Delete tool on a locked layer.
  {
    QgsVectorLayer layer( QStringLiteral( "Point?crs=EPSG:4326" ), QStringLiteral( "locked-del" ), QStringLiteral( "memory" ) );
    const QgsFeatureList seeded = seedFeatures( layer, { QStringLiteral( "Point (25 25)" ) } );
    layer.select( QgsFeatureIds{ seeded.at( 0 ).id() } );
    QVERIFY( !layer.isEditable() );
    canvas.setLayers( QList<QgsMapLayer *>{ &layer } );
    canvas.setCurrentLayer( &layer );
    canvas.refresh();

    TestDeleteTool tool( &canvas, &layer );
    canvas.setMapTool( &tool );
    QSignalSpy editedSpy( &tool, &PaleoDeleteFeatureTool::featureEdited );
    QSignalSpy abortSpy( &tool, &PaleoDeleteFeatureTool::editAborted );
    QSignalSpy msgSpy( &tool, &QgsMapTool::messageEmitted );

    click( tool, canvas, pxAt( canvas, 25, 25 ), Qt::LeftButton );

    QVERIFY( msgSpy.count() >= 1 );
    QCOMPARE( abortSpy.count(), 0 );
    QCOMPARE( editedSpy.count(), 0 );
    QCOMPARE( layer.featureCount(), 1 );

    canvas.unsetMapTool( &tool );
  }

  // Vertex tool on a locked layer: markers are read-only plumbing and still
  // shown; every gesture warns and none aborts.
  {
    QgsVectorLayer layer( QStringLiteral( "LineString?crs=EPSG:4326" ), QStringLiteral( "locked-vtx" ), QStringLiteral( "memory" ) );
    const QgsFeatureId fid = seedFeature( layer, QgsGeometry::fromPolylineXY(
        { mapPt( canvas, 40, 140 ), mapPt( canvas, 100, 100 ), mapPt( canvas, 160, 60 ) } ) );
    layer.selectByIds( { fid } );
    QVERIFY( !layer.isEditable() );
    canvas.setLayers( QList<QgsMapLayer *>{ &layer } );
    canvas.setCurrentLayer( &layer );
    canvas.refresh();

    TestVertexTool tool( &canvas, &layer );
    canvas.setMapTool( &tool );
    QCOMPARE( tool.markerCount(), 3 );

    QSignalSpy editedSpy( &tool, &PaleoVertexTool::featureEdited );
    QSignalSpy abortSpy( &tool, &PaleoVertexTool::editAborted );
    QSignalSpy msgSpy( &tool, &QgsMapTool::messageEmitted );

    click( tool, canvas, QPoint( 40, 140 ), Qt::LeftButton ); // would be a drag hit
    QVERIFY( !tool.isDragging() );
    QCOMPARE( msgSpy.count(), 1 );

    QgsMapMouseEvent dbl( &canvas, QEvent::MouseButtonDblClick, QPoint( 130, 80 ), Qt::LeftButton, Qt::LeftButton, Qt::NoModifier );
    tool.canvasDoubleClickEvent( &dbl );
    QCOMPARE( msgSpy.count(), 2 );

    click( tool, canvas, QPoint( 160, 60 ), Qt::RightButton );
    QCOMPARE( msgSpy.count(), 3 );

    QCOMPARE( abortSpy.count(), 0 );
    QCOMPARE( editedSpy.count(), 0 );
    QCOMPARE( tool.editedCount(), 0 );
    QCOMPARE( vertexTotal( layer, fid ), 3 );

    canvas.unsetMapTool( &tool );
  }
}

// ---------------------------------------------------------------------------
// +) shared signal contract across the five tools: one committed gesture →
// exactly one featureEdited(target layer id); idle Esc → exactly one
// editAborted.
// ---------------------------------------------------------------------------

void TestEditTools::signalContractAcrossTools()
{
  QgsMapCanvas canvas;
  configureCanvas( canvas );

  // PaleoAddFeatureTool
  {
    QgsVectorLayer layer( QStringLiteral( "LineString?crs=EPSG:4326" ), QStringLiteral( "sw-add" ), QStringLiteral( "memory" ) );
    layer.startEditing();
    canvas.setLayers( QList<QgsMapLayer *>{ &layer } );
    canvas.setCurrentLayer( &layer );
    canvas.refresh();

    TestAddTool tool( &canvas, nullptr, QgsMapToolCapture::CaptureLine, &layer );
    canvas.setMapTool( &tool );
    QSignalSpy edited( &tool, &PaleoAddFeatureTool::featureEdited );
    QSignalSpy aborted( &tool, &PaleoAddFeatureTool::editAborted );

    click( tool, canvas, QPoint( 20, 160 ), Qt::LeftButton );
    click( tool, canvas, QPoint( 140, 60 ), Qt::LeftButton );
    click( tool, canvas, QPoint( 140, 60 ), Qt::RightButton );
    QVERIFY2( edited.count() == 1, "one add gesture must emit featureEdited exactly once" );
    QCOMPARE( edited.at( 0 ).at( 0 ).toString(), layer.id() );
    QCOMPARE( edited.at( 0 ).size(), 1 ); // single QString argument
    QCOMPARE( aborted.count(), 0 );

    sendEsc( tool ); // idle Esc → teardown signal
    QCOMPARE( aborted.count(), 1 );
    QCOMPARE( aborted.at( 0 ).size(), 0 ); // argument-less
    QCOMPARE( edited.count(), 1 );

    layer.rollBack();
    canvas.unsetMapTool( &tool );
  }

  // PaleoReshapeTool
  {
    QgsVectorLayer layer( QStringLiteral( "Polygon?crs=EPSG:4326" ), QStringLiteral( "sw-reshape" ), QStringLiteral( "memory" ) );
    const QgsFeatureList seeded = seedFeatures( layer, { squareWkt( 10, 10, 20 ) } );
    layer.startEditing();
    layer.select( QgsFeatureIds{ seeded.at( 0 ).id() } );
    canvas.setLayers( QList<QgsMapLayer *>{ &layer } );
    canvas.setCurrentLayer( &layer );
    canvas.refresh();

    TestReshapeTool tool( &canvas, nullptr, &layer );
    canvas.setMapTool( &tool );
    QSignalSpy edited( &tool, &PaleoReshapeTool::featureEdited );
    QSignalSpy aborted( &tool, &PaleoReshapeTool::editAborted );

    click( tool, canvas, pxAt( canvas, 5, 20 ), Qt::LeftButton );
    click( tool, canvas, pxAt( canvas, 20, 25 ), Qt::LeftButton );
    click( tool, canvas, pxAt( canvas, 35, 20 ), Qt::LeftButton );
    click( tool, canvas, pxAt( canvas, 35, 20 ), Qt::RightButton );
    QVERIFY2( edited.count() == 1, "one reshape gesture must emit featureEdited exactly once" );
    QCOMPARE( edited.at( 0 ).at( 0 ).toString(), layer.id() );
    QCOMPARE( edited.at( 0 ).size(), 1 );
    QCOMPARE( tool.reshapedCount(), 1 );
    QCOMPARE( aborted.count(), 0 );

    sendEsc( tool );
    QCOMPARE( aborted.count(), 1 );
    QCOMPARE( edited.count(), 1 );

    layer.rollBack();
    canvas.unsetMapTool( &tool );
  }

  // PaleoMoveTool
  {
    QgsVectorLayer layer( QStringLiteral( "Polygon?crs=EPSG:4326" ), QStringLiteral( "sw-move" ), QStringLiteral( "memory" ) );
    const QgsFeatureList seeded = seedFeatures( layer, { squareWkt( 10, 10, 20 ) } );
    layer.startEditing();
    layer.select( QgsFeatureIds{ seeded.at( 0 ).id() } );
    canvas.setLayers( QList<QgsMapLayer *>{ &layer } );
    canvas.setCurrentLayer( &layer );
    canvas.refresh();

    TestMoveTool tool( &canvas, &layer );
    canvas.setMapTool( &tool );
    QSignalSpy edited( &tool, &PaleoMoveTool::featureEdited );
    QSignalSpy aborted( &tool, &PaleoMoveTool::editAborted );

    QgsMapMouseEvent press( &canvas, QEvent::MouseButtonPress, pxAt( canvas, 20, 20 ), Qt::LeftButton, Qt::LeftButton, Qt::NoModifier );
    tool.canvasPressEvent( &press );
    QgsMapMouseEvent move( &canvas, QEvent::MouseMove, pxAt( canvas, 40, 60 ), Qt::NoButton, Qt::LeftButton, Qt::NoModifier );
    tool.canvasMoveEvent( &move );
    QgsMapMouseEvent release( &canvas, QEvent::MouseButtonRelease, pxAt( canvas, 40, 60 ), Qt::LeftButton, Qt::NoButton, Qt::NoModifier );
    tool.canvasReleaseEvent( &release );
    QVERIFY2( edited.count() == 1, "one move gesture must emit featureEdited exactly once" );
    QCOMPARE( edited.at( 0 ).at( 0 ).toString(), layer.id() );
    QCOMPARE( edited.at( 0 ).size(), 1 );
    QCOMPARE( tool.movedCount(), 1 );
    QCOMPARE( aborted.count(), 0 );

    sendEsc( tool );
    QCOMPARE( aborted.count(), 1 );
    QCOMPARE( edited.count(), 1 );

    layer.rollBack();
    canvas.unsetMapTool( &tool );
  }

  // PaleoDeleteFeatureTool
  {
    QgsVectorLayer layer( QStringLiteral( "Point?crs=EPSG:4326" ), QStringLiteral( "sw-del" ), QStringLiteral( "memory" ) );
    const QgsFeatureList seeded = seedFeatures( layer, { QStringLiteral( "Point (25 25)" ), QStringLiteral( "Point (75 75)" ) } );
    layer.startEditing();
    layer.select( QgsFeatureIds{ seeded.at( 0 ).id() } );
    canvas.setLayers( QList<QgsMapLayer *>{ &layer } );
    canvas.setCurrentLayer( &layer );
    canvas.refresh();

    TestDeleteTool tool( &canvas, &layer );
    canvas.setMapTool( &tool );
    QSignalSpy edited( &tool, &PaleoDeleteFeatureTool::featureEdited );
    QSignalSpy aborted( &tool, &PaleoDeleteFeatureTool::editAborted );

    click( tool, canvas, pxAt( canvas, 25, 25 ), Qt::LeftButton );
    QVERIFY2( edited.count() == 1, "one delete gesture must emit featureEdited exactly once" );
    QCOMPARE( edited.at( 0 ).at( 0 ).toString(), layer.id() );
    QCOMPARE( edited.at( 0 ).size(), 1 );
    QCOMPARE( tool.deletedCount(), 1 );
    QCOMPARE( aborted.count(), 0 );

    sendEsc( tool );
    QCOMPARE( aborted.count(), 1 );
    QCOMPARE( edited.count(), 1 );

    layer.rollBack();
    canvas.unsetMapTool( &tool );
  }

  // PaleoVertexTool
  {
    QgsVectorLayer layer( QStringLiteral( "LineString?crs=EPSG:4326" ), QStringLiteral( "sw-vtx" ), QStringLiteral( "memory" ) );
    const QgsFeatureId fid = seedFeature( layer, QgsGeometry::fromPolylineXY(
        { mapPt( canvas, 40, 140 ), mapPt( canvas, 100, 100 ), mapPt( canvas, 160, 60 ) } ) );
    layer.startEditing();
    layer.selectByIds( { fid } );
    canvas.setLayers( QList<QgsMapLayer *>{ &layer } );
    canvas.setCurrentLayer( &layer );
    canvas.refresh();

    TestVertexTool tool( &canvas, &layer );
    canvas.setMapTool( &tool );
    QSignalSpy edited( &tool, &PaleoVertexTool::featureEdited );
    QSignalSpy aborted( &tool, &PaleoVertexTool::editAborted );

    QgsMapMouseEvent dbl( &canvas, QEvent::MouseButtonDblClick, QPoint( 130, 80 ), Qt::LeftButton, Qt::LeftButton, Qt::NoModifier );
    tool.canvasDoubleClickEvent( &dbl );
    QVERIFY2( edited.count() == 1, "one vertex-insert gesture must emit featureEdited exactly once" );
    QCOMPARE( edited.at( 0 ).at( 0 ).toString(), layer.id() );
    QCOMPARE( edited.at( 0 ).size(), 1 );
    QCOMPARE( tool.editedCount(), 1 );
    QCOMPARE( aborted.count(), 0 );

    sendEsc( tool );
    QCOMPARE( aborted.count(), 1 );
    QCOMPARE( edited.count(), 1 );

    layer.rollBack();
    canvas.unsetMapTool( &tool );
  }
}

int main( int argc, char *argv[] )
{
  QgsApplication app( argc, argv, false );
  app.setPrefixPath( QStringLiteral( "/usr" ), true ); // distro install
  app.initQgis();
  TestEditTools tc;
  const int rc = QTest::qExec( &tc, argc, argv );
  QgsApplication::exitQgis();
  return rc;
}

#include "tst_edittools.moc"
