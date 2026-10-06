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

#include "../src/qgis/qgiseditingservice.h"
#include "../src/metadata/paleoprojectstore.h"
#include <algorithm>
#include <memory>

#include <QtTest>
#include <QAction>
#include <QComboBox>
#include <QKeyEvent>
#include <QLabel>
#include <QMouseEvent>
#include <QSignalSpy>
#include <QUndoStack>

#include <qgsapplication.h>
#include <qgsfeature.h>
#include <qgsfeatureiterator.h>
#include <qgsgeometry.h>
#include <qgslinestring.h>
#include <qgsmapcanvas.h>
#include <qgsmaptoolpan.h>
#include <qgsmapmouseevent.h>
#include <qgsmaptoolcapture.h>
#include <qgspolygon.h>
#include <qgspoint.h>
#include <qgsproject.h>
#include <qgsrectangle.h>
#include <qgssnappingconfig.h>
#include <qgssnappingutils.h>
#include <qgsvectorlayer.h>
#include <qgsvectorlayereditbuffer.h>
#include <qgsvertexmarker.h>

#include "qgis/qgiseditingservice.h"
#include "qgis/topologicalindex.h"
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
    void vertexDragSnapsToSnapTargetVertex();
    void vertexDragWithoutSnapConfigKeepsRawPoint();

    // c) reshape + vertex add/delete
    void reshapeChangesSelectedFeatureOnly();
    void vertexDoubleClickInsertsVertex();
    void vertexRightClickDeletesWithUndo();
    void vertexKeyboardDeleteMatchesRightClick();
    void vertexDeleteRefusedBelowMinimums();

    // c+) topological editing — shared-boundary coincident vertices
    void vertexTopoDragMovesCoincidentVertices();
    void vertexTopoOffLeavesNeighborUntouched();
    void vertexCoincidentEnvelopeBoundsNoise();
    void vertexTopoDoubleClickInsertsOnSharedEdge();
    void vertexTopoDeleteRemovesCoincidentVertices();
    void vertexTopoReleaseWeldsToNeighborVertex();
    void toolbarTopologicalActionMirrorsProjectFlag();
    void toolbarCrossLayerTopoMirrorsProjectEntry();
    void vertexMovePolygonClosureMaintainsClosedRing();
    void vertexDeletePolygonClosurePreservesRing();
    void vertexUndoRedoSyncsMarkers();
    void vertexTopoDragMovesAllMarkers();
    void vertexMoveClosedLineStringClosureInvariants();
    void vertexMovePolygonEndClosureVertexInvariants();
    void vertexDeleteEndClosureVertexInvariants();
    void vertexDeleteTriangleAllVerticesRejected();
    void vertexDeleteMultiFeaturePartialTriangleRefused();
    void vertexDeleteMultiCoincidentRingQuotaRefused();
    void vertexMoveAndDeletePolygonHoleInvariants();
    void vertexTopoSharedClosureMultiFeatureInvariants();
    void vertexTopoDeleteCoincidentPinchPointRefused();
    void vertexTopoDeleteCoincidentPinchPointAllowedOnHexagon();
    void vertexInFlightDragAbortedByExternalUndo();
    void vertexAdversarialTopoMarkersDynamicTracking();
    void vertexAdversarialRebuildOnRollbackSync();
    void vertexAdversarialInFlightDragAbortedByRollback();
    void vertexAdversarialLayerDestructionArmedToolSafety();
    void vertexAdversarialToolDestructionCanvasSafety();

    // d) undo/redo spine
    void undoStackAttachForwardUndoRedo();
    void undoStackSwitchRefusedAndCommitClears();
    void undoStackDetachReattachDestroyedSafe();

    // d/e/f) PaleoEditingToolbar (implementation by the parallel agent)
    void toolbarSelectionIsReadOnlyAndToolsFollowCanvas();
    void toolbarGeometryAndReadOnlyGates();
    void toolbarRemovedLayerClearsTarget();
    void toolbarStartStopSignalsAndStateRendering();
    void toolbarComboFilterAndProjectRefresh();
    void toolbarEditToolActionAutoStartsSession();
    void toolbarOutlivesCanvasDestruction();
    void toolbarUndoRedoActionStates();
    void toolbarSavePersistsAndClearsUndo();
    void toolbarServiceEndsEachSessionOnce();
    void toolbarCancelDiscardsEdits();
    void toolbarSaveRefusedOutsideSession();
    // ---- mapping 主线3：编辑会话生命周期加固 ----
    void toolbarProjectSwitchFinalizesSession();
    void toolbarProjectClearedDropsSessionCleanly();
    void toolbarUndoBoundaryAfterProjectFinalize();

    // g) abort paths
    void addAbortPaths();
    void reshapeAbortPaths();
    void moveEscCancelsDragAndIdleAborts();
    void deleteEscAndEmptySelectionWarns();
    void vertexEscCancelsDragAndIdleAborts();
    void geometryCommitGateValidatesNatively();
    void warnOnlyRefusalsDontAbort();

    // +) shared signal contract across the five tools
    void signalContractAcrossTools();

    // ---- mapping 主线2：R-tree 拓扑索引 + 跨层拓扑 ----
    void topoIndexCandidatesAndHealth();
    void topoIndexInvalidationOnEditSignals();
    void vertexTopoCrossLayerDragMovesBothLayers();
    void vertexTopoCrossLayerCrsMismatchExcluded();
    void vertexTopoCrossLayerDeleteAndUndo();
    void vertexTopoCrossLayerParticipantDestructionMidDrag();
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

void TestEditTools::vertexDragSnapsToSnapTargetVertex()
{
  QgsMapCanvas canvas;
  configureCanvas( canvas );

  QgsVectorLayer layer( QStringLiteral( "LineString?crs=EPSG:4326&field=id:integer" ), QStringLiteral( "edit" ), QStringLiteral( "memory" ) );
  QVERIFY( layer.isValid() );
  const QgsFeatureId fid = seedFeature( layer, QgsGeometry::fromPolylineXY(
      { mapPt( canvas, 40, 140 ), mapPt( canvas, 100, 100 ), mapPt( canvas, 160, 60 ) } ) );
  QgsVectorLayer snapSrc( QStringLiteral( "Point?crs=EPSG:4326&field=id:integer" ), QStringLiteral( "snap-src" ), QStringLiteral( "memory" ) );
  QVERIFY( snapSrc.isValid() );
  const QgsPointXY target = mapPt( canvas, 150, 80 );
  seedFeature( snapSrc, QgsGeometry::fromPointXY( target ) );

  layer.startEditing();
  layer.selectByIds( { fid } );
  canvas.setLayers( QList<QgsMapLayer *>{ &layer, &snapSrc } );
  canvas.setCurrentLayer( &layer );
  canvas.refresh();
  // AllLayers 模式的吸附索引走 QgsProject::instance() 的层注册表——与应用
  // 侧真实路径一致（生产层都在工程里）。
  QgsProject::instance()->addMapLayer( &layer, false, false );
  QgsProject::instance()->addMapLayer( &snapSrc, false, false );

  // Same shape as QgisCanvasController::nativeSnappingConfig() (AllLayers,
  // vertex+segment, pixel tolerance) — installed here directly because the
  // bare-canvas fixture never goes through the controller.
  QgsSnappingConfig cfg = canvas.snappingUtils()->config();
  cfg.setEnabled( true );
  cfg.setMode( Qgis::SnappingMode::AllLayers );
  cfg.setTypeFlag( Qgis::SnappingType::Vertex | Qgis::SnappingType::Segment );
  cfg.setTolerance( 10.0 );
  cfg.setUnits( Qgis::MapToolUnit::Pixels );
  canvas.snappingUtils()->setConfig( cfg );

  TestVertexTool tool( &canvas, &layer );
  canvas.setMapTool( &tool );

  // 吸附索引（QgsPointLocator）惰性/异步构建——先探到索引就绪再注入事件，
  // 否则首个 snapToMap 还在建索引时返回空匹配，断言会偶发失败。
  QgsPointLocator::Match warm;
  for ( int i = 0; i < 200 && !warm.isValid(); ++i )
  {
    warm = canvas.snappingUtils()->snapToMap( mapPt( canvas, 152, 82 ) );
    if ( !warm.isValid() )
      QTest::qWait( 10 );
  }
  QVERIFY2( warm.isValid(), "snap locator never prepared" );

  QgsMapMouseEvent press( &canvas, QEvent::MouseButtonPress, QPoint( 40, 140 ), Qt::LeftButton, Qt::LeftButton, Qt::NoModifier );
  tool.canvasPressEvent( &press );
  QVERIFY2( tool.isDragging(), "press on a vertex must arm the drag" );

  // Drop 2px off the snap source's vertex — inside the 10px tolerance, so the
  // commit must land on the exact vertex, not the raw cursor position.
  QgsMapMouseEvent move( &canvas, QEvent::MouseMove, QPoint( 152, 82 ), Qt::NoButton, Qt::LeftButton, Qt::NoModifier );
  tool.canvasMoveEvent( &move );
  QVERIFY2( move.isSnapped(), "move within tolerance must register a snap match" );

  QgsMapMouseEvent release( &canvas, QEvent::MouseButtonRelease, QPoint( 152, 82 ), Qt::LeftButton, Qt::NoButton, Qt::NoModifier );
  tool.canvasReleaseEvent( &release );
  QVERIFY( !tool.isDragging() );

  const QgsGeometry g = layer.getFeature( fid ).geometry();
  const QgsLineString *ls = asLineString( g );
  QVERIFY( ls );
  QVERIFY( qgsDoubleNear( ls->xAt( 0 ), target.x(), 1e-9 ) );
  QVERIFY( qgsDoubleNear( ls->yAt( 0 ), target.y(), 1e-9 ) );

  canvas.unsetMapTool( &tool );
  layer.rollBack();
}

void TestEditTools::vertexDragWithoutSnapConfigKeepsRawPoint()
{
  QgsMapCanvas canvas;
  configureCanvas( canvas );

  QgsVectorLayer layer( QStringLiteral( "LineString?crs=EPSG:4326&field=id:integer" ), QStringLiteral( "edit" ), QStringLiteral( "memory" ) );
  QVERIFY( layer.isValid() );
  const QgsFeatureId fid = seedFeature( layer, QgsGeometry::fromPolylineXY(
      { mapPt( canvas, 40, 140 ), mapPt( canvas, 100, 100 ), mapPt( canvas, 160, 60 ) } ) );
  QgsVectorLayer snapSrc( QStringLiteral( "Point?crs=EPSG:4326&field=id:integer" ), QStringLiteral( "snap-src" ), QStringLiteral( "memory" ) );
  QVERIFY( snapSrc.isValid() );
  const QgsPointXY target = mapPt( canvas, 150, 80 );
  seedFeature( snapSrc, QgsGeometry::fromPointXY( target ) );

  layer.startEditing();
  layer.selectByIds( { fid } );
  canvas.setLayers( QList<QgsMapLayer *>{ &layer, &snapSrc } );
  canvas.setCurrentLayer( &layer );
  canvas.refresh();

  // Explicitly disabled: default QgsSnappingConfig is off, pin it anyway so
  // the raw-drop contract cannot silently change with upstream defaults.
  QgsSnappingConfig cfg = canvas.snappingUtils()->config();
  cfg.setEnabled( false );
  canvas.snappingUtils()->setConfig( cfg );

  TestVertexTool tool( &canvas, &layer );
  canvas.setMapTool( &tool );

  QgsMapMouseEvent press( &canvas, QEvent::MouseButtonPress, QPoint( 40, 140 ), Qt::LeftButton, Qt::LeftButton, Qt::NoModifier );
  tool.canvasPressEvent( &press );
  QVERIFY( tool.isDragging() );

  QgsMapMouseEvent move( &canvas, QEvent::MouseMove, QPoint( 152, 82 ), Qt::NoButton, Qt::LeftButton, Qt::NoModifier );
  tool.canvasMoveEvent( &move );
  QVERIFY( !move.isSnapped() );

  QgsMapMouseEvent release( &canvas, QEvent::MouseButtonRelease, QPoint( 152, 82 ), Qt::LeftButton, Qt::NoButton, Qt::NoModifier );
  tool.canvasReleaseEvent( &release );

  // No snapping → the raw cursor position commits.
  const QgsPointXY raw = mapPt( canvas, 152, 82 );
  const QgsGeometry g = layer.getFeature( fid ).geometry();
  const QgsLineString *ls = asLineString( g );
  QVERIFY( ls );
  QVERIFY( qgsDoubleNear( ls->xAt( 0 ), raw.x(), 1e-6 ) );
  QVERIFY( qgsDoubleNear( ls->yAt( 0 ), raw.y(), 1e-6 ) );

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

void TestEditTools::vertexKeyboardDeleteMatchesRightClick()
{
  QgsMapCanvas canvas;
  configureCanvas( canvas );

  QgsVectorLayer layer( QStringLiteral( "LineString?crs=EPSG:4326&field=id:integer" ), QStringLiteral( "kbd-delv" ), QStringLiteral( "memory" ) );
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

  // W3：Delete 键删除光标下的节点（画布最后已知鼠标位置），与右键同路径。
  QMouseEvent hover( QEvent::MouseMove, QPointF( 160, 60 ), QPointF( 160, 60 ), QPointF( 160, 60 ),
                     Qt::NoButton, Qt::NoButton, Qt::NoModifier );
  QApplication::sendEvent( canvas.viewport(), &hover );
  QCOMPARE( canvas.mouseLastXY(), QPoint( 160, 60 ) );

  QKeyEvent del( QEvent::KeyPress, Qt::Key_Delete, Qt::NoModifier );
  tool.keyPressEvent( &del );

  QCOMPARE( editedSpy.count(), 1 );
  QCOMPARE( msgSpy.count(), 0 );
  QCOMPARE( tool.editedCount(), 1 );
  QCOMPARE( vertexTotal( layer, fid ), 2 );
  QCOMPARE( tool.markerCount(), 2 );

  // Backspace 同路径：删到 2 个节点后，在剩余节点上再删被拒（线最少 2 节点）。
  QMouseEvent hover2( QEvent::MouseMove, QPointF( 100, 100 ), QPointF( 100, 100 ), QPointF( 100, 100 ),
                      Qt::NoButton, Qt::NoButton, Qt::NoModifier );
  QApplication::sendEvent( canvas.viewport(), &hover2 );
  QCOMPARE( canvas.mouseLastXY(), QPoint( 100, 100 ) );
  QKeyEvent back( QEvent::KeyPress, Qt::Key_Backspace, Qt::NoModifier );
  tool.keyPressEvent( &back );
  QCOMPARE( editedSpy.count(), 1 ); // refused: minimum-vertex guard
  QCOMPARE( vertexTotal( layer, fid ), 2 );
  QVERIFY( msgSpy.count() >= 1 );

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
// c+) topological editing — shared-boundary coincident vertices
// ---------------------------------------------------------------------------

void TestEditTools::vertexTopoDragMovesCoincidentVertices()
{
  QgsMapCanvas canvas;
  configureCanvas( canvas );

  // Two lines sharing one endpoint — the fidB copy is NEVER selected.
  QgsVectorLayer layer( QStringLiteral( "LineString?crs=EPSG:4326&field=id:integer" ), QStringLiteral( "topo-drag" ), QStringLiteral( "memory" ) );
  QVERIFY2( layer.isValid(), "memory line layer failed to initialize" );
  const QgsPointXY shared = mapPt( canvas, 100, 100 );
  const QgsFeatureId fidA = seedFeature( layer, QgsGeometry::fromPolylineXY(
      { mapPt( canvas, 40, 140 ), shared, mapPt( canvas, 160, 60 ) } ) );
  const QgsFeatureId fidB = seedFeature( layer, QgsGeometry::fromPolylineXY(
      { shared, mapPt( canvas, 180, 140 ) } ) );
  layer.startEditing();
  layer.selectByIds( { fidA } );
  canvas.setLayers( QList<QgsMapLayer *>{ &layer } );
  canvas.setCurrentLayer( &layer );
  canvas.refresh();

  TestVertexTool tool( &canvas, &layer );
  tool.setTopologicalEditingEnabled( true );
  canvas.setMapTool( &tool );
  QSignalSpy editedSpy( &tool, &PaleoVertexTool::featureEdited );

  const QgsPointXY pNew = mapPt( canvas, 70, 110 );
  QgsMapMouseEvent press( &canvas, QEvent::MouseButtonPress, QPoint( 100, 100 ),
                          Qt::LeftButton, Qt::LeftButton, Qt::NoModifier );
  tool.canvasPressEvent( &press );
  QVERIFY2( tool.isDragging(), "press on the shared vertex must arm the drag" );
  QgsMapMouseEvent release( &canvas, QEvent::MouseButtonRelease, QPoint( 70, 110 ),
                            Qt::LeftButton, Qt::NoButton, Qt::NoModifier );
  tool.canvasReleaseEvent( &release );

  QCOMPARE( editedSpy.count(), 1 );
  QCOMPARE( layer.undoStack()->count(), 1 ); // one edit command covers both features

  const QgsLineString *lsA = asLineString( layer.getFeature( fidA ).geometry() );
  const QgsLineString *lsB = asLineString( layer.getFeature( fidB ).geometry() );
  QVERIFY2( lsA && lsB, "dragged geometries must stay line strings" );
  QVERIFY( qgsDoubleNear( lsA->xAt( 1 ), pNew.x(), 1e-6 ) );
  QVERIFY( qgsDoubleNear( lsA->yAt( 1 ), pNew.y(), 1e-6 ) );
  QVERIFY( qgsDoubleNear( lsB->xAt( 0 ), pNew.x(), 1e-6 ) ); // unselected neighbor followed
  QVERIFY( qgsDoubleNear( lsB->yAt( 0 ), pNew.y(), 1e-6 ) );
  QVERIFY( qgsDoubleNear( lsB->xAt( 1 ), mapPt( canvas, 180, 140 ).x(), 1e-6 ) ); // tail untouched

  // One undo step restores BOTH features to the shared position.
  layer.undoStack()->undo();
  const QgsLineString *lsAu = asLineString( layer.getFeature( fidA ).geometry() );
  const QgsLineString *lsBu = asLineString( layer.getFeature( fidB ).geometry() );
  QVERIFY( qgsDoubleNear( lsAu->xAt( 1 ), shared.x(), 1e-6 ) );
  QVERIFY( qgsDoubleNear( lsBu->xAt( 0 ), shared.x(), 1e-6 ) );

  canvas.unsetMapTool( &tool );
  layer.rollBack();
}

void TestEditTools::vertexTopoOffLeavesNeighborUntouched()
{
  QgsMapCanvas canvas;
  configureCanvas( canvas );

  QgsVectorLayer layer( QStringLiteral( "LineString?crs=EPSG:4326&field=id:integer" ), QStringLiteral( "topo-off" ), QStringLiteral( "memory" ) );
  QVERIFY2( layer.isValid(), "memory line layer failed to initialize" );
  const QgsPointXY shared = mapPt( canvas, 100, 100 );
  const QgsFeatureId fidA = seedFeature( layer, QgsGeometry::fromPolylineXY(
      { mapPt( canvas, 40, 140 ), shared, mapPt( canvas, 160, 60 ) } ) );
  const QgsFeatureId fidB = seedFeature( layer, QgsGeometry::fromPolylineXY(
      { shared, mapPt( canvas, 180, 140 ) } ) );
  layer.startEditing();
  layer.selectByIds( { fidA } );
  canvas.setLayers( QList<QgsMapLayer *>{ &layer } );
  canvas.setCurrentLayer( &layer );
  canvas.refresh();

  // Default OFF — coincident vertices are independent again.
  TestVertexTool tool( &canvas, &layer );
  QVERIFY( !tool.topologicalEditingEnabled() );
  canvas.setMapTool( &tool );

  const QgsPointXY pNew = mapPt( canvas, 70, 110 );
  QgsMapMouseEvent press( &canvas, QEvent::MouseButtonPress, QPoint( 100, 100 ),
                          Qt::LeftButton, Qt::LeftButton, Qt::NoModifier );
  tool.canvasPressEvent( &press );
  QgsMapMouseEvent release( &canvas, QEvent::MouseButtonRelease, QPoint( 70, 110 ),
                            Qt::LeftButton, Qt::NoButton, Qt::NoModifier );
  tool.canvasReleaseEvent( &release );

  const QgsLineString *lsA = asLineString( layer.getFeature( fidA ).geometry() );
  const QgsLineString *lsB = asLineString( layer.getFeature( fidB ).geometry() );
  QVERIFY( qgsDoubleNear( lsA->xAt( 1 ), pNew.x(), 1e-6 ) );
  QVERIFY( qgsDoubleNear( lsB->xAt( 0 ), shared.x(), 1e-6 ) ); // neighbor kept the split
  QVERIFY( qgsDoubleNear( lsB->yAt( 0 ), shared.y(), 1e-6 ) );

  canvas.unsetMapTool( &tool );
  layer.rollBack();
}

// BIZ-09 钉死：kTopoCandidateEnvelope（1e-9）只是 R-tree 预筛包络，命中
// 语义由 sameXy 的 qgsDoubleNear 把守（QGIS 4 = 绝对 4·DBL_EPSILON≈8.9e-16，
// 近精确）。邻居顶点偏移 1e-8（>> 包络）不得并入共点栈联动；逐位相等的
// 邻居必须联动（既有 vertexTopoDragMovesCoincidentVertices 用例钉住）。
// 包络若被放大到 ≥1e-8 本用例转红——常量回改需连本钉死一起评估。
void TestEditTools::vertexCoincidentEnvelopeBoundsNoise()
{
  QgsMapCanvas canvas;
  configureCanvas( canvas );

  QgsVectorLayer layer( QStringLiteral( "LineString?crs=EPSG:4326&field=id:integer" ), QStringLiteral( "topo-noise" ), QStringLiteral( "memory" ) );
  QVERIFY2( layer.isValid(), "memory line layer failed to initialize" );
  const QgsPointXY anchor = mapPt( canvas, 100, 100 );
  const QgsPointXY offset( anchor.x() + 1e-8, anchor.y() - 1e-8 );
  const QgsFeatureId fidA = seedFeature( layer, QgsGeometry::fromPolylineXY(
      { mapPt( canvas, 40, 140 ), anchor, mapPt( canvas, 160, 60 ) } ) );
  const QgsFeatureId fidB = seedFeature( layer, QgsGeometry::fromPolylineXY(
      { offset, mapPt( canvas, 180, 140 ) } ) );
  layer.startEditing();
  layer.selectByIds( { fidA } );
  canvas.setLayers( QList<QgsMapLayer *>{ &layer } );
  canvas.setCurrentLayer( &layer );
  canvas.refresh();

  TestVertexTool tool( &canvas, &layer );
  tool.setTopologicalEditingEnabled( true );
  canvas.setMapTool( &tool );
  QSignalSpy editedSpy( &tool, &PaleoVertexTool::featureEdited );

  const QgsPointXY pNew = mapPt( canvas, 70, 110 );
  QgsMapMouseEvent press( &canvas, QEvent::MouseButtonPress, QPoint( 100, 100 ),
                          Qt::LeftButton, Qt::LeftButton, Qt::NoModifier );
  tool.canvasPressEvent( &press );
  QVERIFY2( tool.isDragging(), "press on the anchor must arm the drag" );
  QgsMapMouseEvent release( &canvas, QEvent::MouseButtonRelease, QPoint( 70, 110 ),
                            Qt::LeftButton, Qt::NoButton, Qt::NoModifier );
  tool.canvasReleaseEvent( &release );

  QCOMPARE( editedSpy.count(), 1 );
  const QgsLineString *lsA = asLineString( layer.getFeature( fidA ).geometry() );
  const QgsLineString *lsB = asLineString( layer.getFeature( fidB ).geometry() );
  QVERIFY2( lsA && lsB, "dragged geometries must stay line strings" );
  QVERIFY( qgsDoubleNear( lsA->xAt( 1 ), pNew.x(), 1e-6 ) ); // 被拖顶点到位
  QVERIFY( qgsDoubleNear( lsB->xAt( 0 ), offset.x(), 0.0 ) ); // 包络外邻居不动
  QVERIFY( qgsDoubleNear( lsB->yAt( 0 ), offset.y(), 0.0 ) );

  canvas.unsetMapTool( &tool );
  layer.rollBack();
}

void TestEditTools::vertexTopoDoubleClickInsertsOnSharedEdge()
{
  QgsMapCanvas canvas;
  configureCanvas( canvas );

  // Adjacent squares sharing the x=30 edge: A = (10,10)-(30,30),
  // B = (30,10)-(50,30). Coincident vertices at (30,10) and (30,30).
  QgsVectorLayer layer( QStringLiteral( "Polygon?crs=EPSG:4326&field=id:integer" ), QStringLiteral( "topo-ins" ), QStringLiteral( "memory" ) );
  QVERIFY2( layer.isValid(), "memory polygon layer failed to initialize" );
  const QgsFeatureId fidA = seedFeature( layer, QgsGeometry::fromWkt( squareWkt( 10, 10, 20 ) ) );
  const QgsFeatureId fidB = seedFeature( layer, QgsGeometry::fromWkt( squareWkt( 30, 10, 20 ) ) );
  layer.startEditing();
  layer.selectByIds( { fidA } );
  canvas.setLayers( QList<QgsMapLayer *>{ &layer } );
  canvas.setCurrentLayer( &layer );
  canvas.refresh();

  TestVertexTool tool( &canvas, &layer );
  tool.setTopologicalEditingEnabled( true );
  canvas.setMapTool( &tool );
  QSignalSpy editedSpy( &tool, &PaleoVertexTool::featureEdited );

  QCOMPARE( vertexTotal( layer, fidA ), 5 );
  QCOMPARE( vertexTotal( layer, fidB ), 5 );

  // Double-click the midpoint of the shared edge → both rings gain a vertex.
  QgsMapMouseEvent dbl( &canvas, QEvent::MouseButtonDblClick, pxAt( canvas, 30, 20 ),
                        Qt::LeftButton, Qt::LeftButton, Qt::NoModifier );
  tool.canvasDoubleClickEvent( &dbl );

  QCOMPARE( editedSpy.count(), 1 );
  QCOMPARE( layer.undoStack()->count(), 1 );
  QCOMPARE( vertexTotal( layer, fidA ), 6 );
  QCOMPARE( vertexTotal( layer, fidB ), 6 ); // coincident edge got the vertex too

  // The inserted vertex sits on the shared edge: x lands exactly on 30 (the
  // projection onto the vertical segment), y at the clicked pixel's position.
  const QgsPointXY clickPt = mapPt( canvas, pxAt( canvas, 30, 20 ).x(), pxAt( canvas, 30, 20 ).y() );
  const QgsPolygon *polyA = asPolygon( layer.getFeature( fidA ).geometry() );
  const QgsPolygon *polyB = asPolygon( layer.getFeature( fidB ).geometry() );
  QVERIFY2( polyA && polyB, "mutated geometries must stay polygons" );
  const QgsLineString *ringA = qgsgeometry_cast<const QgsLineString *>( polyA->exteriorRing() );
  const QgsLineString *ringB = qgsgeometry_cast<const QgsLineString *>( polyB->exteriorRing() );
  QVERIFY2( ringA && ringB, "rings must survive" );
  // A's ring: (10,10)(30,10) (30,20) (30,30) (10,30) closure
  QVERIFY( qgsDoubleNear( ringA->xAt( 2 ), 30.0, 1e-6 ) );
  QVERIFY( qgsDoubleNear( ringA->yAt( 2 ), clickPt.y(), 1e-6 ) );
  // B's ring: (30,10)(50,10)(50,30)(30,30) (30,20) closure — reversed edge
  QVERIFY( qgsDoubleNear( ringB->xAt( 4 ), 30.0, 1e-6 ) );
  QVERIFY( qgsDoubleNear( ringB->yAt( 4 ), clickPt.y(), 1e-6 ) );

  canvas.unsetMapTool( &tool );
  layer.rollBack();
}

void TestEditTools::vertexTopoDeleteRemovesCoincidentVertices()
{
  QgsMapCanvas canvas;
  configureCanvas( canvas );

  QgsVectorLayer layer( QStringLiteral( "Polygon?crs=EPSG:4326&field=id:integer" ), QStringLiteral( "topo-del" ), QStringLiteral( "memory" ) );
  QVERIFY2( layer.isValid(), "memory polygon layer failed to initialize" );
  const QgsFeatureId fidA = seedFeature( layer, QgsGeometry::fromWkt( squareWkt( 10, 10, 20 ) ) );
  const QgsFeatureId fidB = seedFeature( layer, QgsGeometry::fromWkt( squareWkt( 30, 10, 20 ) ) );
  layer.startEditing();
  layer.selectByIds( { fidA } );
  canvas.setLayers( QList<QgsMapLayer *>{ &layer } );
  canvas.setCurrentLayer( &layer );
  canvas.refresh();

  TestVertexTool tool( &canvas, &layer );
  tool.setTopologicalEditingEnabled( true );
  canvas.setMapTool( &tool );
  QSignalSpy editedSpy( &tool, &PaleoVertexTool::featureEdited );
  QSignalSpy msgSpy( &tool, &QgsMapTool::messageEmitted );

  // Right-click the shared corner (30,30): both rings drop one vertex —
  // 5→4 stays above the ring minimum, so the batch commits.
  click( tool, canvas, pxAt( canvas, 30, 30 ), Qt::RightButton );

  QCOMPARE( editedSpy.count(), 1 );
  QCOMPARE( msgSpy.count(), 0 );
  QCOMPARE( vertexTotal( layer, fidA ), 4 );
  QCOMPARE( vertexTotal( layer, fidB ), 4 );
  QVERIFY( layer.getFeature( fidA ).geometry().isGeosValid() );
  QVERIFY( layer.getFeature( fidB ).geometry().isGeosValid() );

  canvas.unsetMapTool( &tool );
  layer.rollBack();
}

void TestEditTools::vertexTopoReleaseWeldsToNeighborVertex()
{
  QgsMapCanvas canvas;
  configureCanvas( canvas );

  QgsVectorLayer layer( QStringLiteral( "LineString?crs=EPSG:4326&field=id:integer" ), QStringLiteral( "topo-weld" ), QStringLiteral( "memory" ) );
  QVERIFY2( layer.isValid(), "memory line layer failed to initialize" );
  const QgsFeatureId fidA = seedFeature( layer, QgsGeometry::fromPolylineXY(
      { mapPt( canvas, 40, 140 ), mapPt( canvas, 100, 100 ) } ) );
  const QgsPointXY weldTarget = mapPt( canvas, 75, 95 );
  const QgsFeatureId fidB = seedFeature( layer, QgsGeometry::fromPolylineXY(
      { weldTarget, mapPt( canvas, 150, 120 ) } ) );
  layer.startEditing();
  layer.selectByIds( { fidA } );
  canvas.setLayers( QList<QgsMapLayer *>{ &layer } );
  canvas.setCurrentLayer( &layer );
  canvas.refresh();

  TestVertexTool tool( &canvas, &layer );
  tool.setTopologicalEditingEnabled( true );
  canvas.setMapTool( &tool );

  // Drag fidA's vertex 1 to ~3px from fidB's vertex 0 — inside the search
  // radius → welded onto the neighbor's exact position.
  QgsMapMouseEvent press( &canvas, QEvent::MouseButtonPress, QPoint( 100, 100 ),
                          Qt::LeftButton, Qt::LeftButton, Qt::NoModifier );
  tool.canvasPressEvent( &press );
  QVERIFY2( tool.isDragging(), "press on vertex must arm the drag" );
  QgsMapMouseEvent release( &canvas, QEvent::MouseButtonRelease, QPoint( 78, 97 ),
                            Qt::LeftButton, Qt::NoButton, Qt::NoModifier );
  tool.canvasReleaseEvent( &release );

  const QgsLineString *lsA = asLineString( layer.getFeature( fidA ).geometry() );
  QVERIFY( qgsDoubleNear( lsA->xAt( 1 ), weldTarget.x(), 1e-9 ) );
  QVERIFY( qgsDoubleNear( lsA->yAt( 1 ), weldTarget.y(), 1e-9 ) );

  // …and the welded vertex is itself coincident now — a second topo drag
  // grabs the welded stack (both vertices move together).
  QgsMapMouseEvent press2( &canvas, QEvent::MouseButtonPress, pxAt( canvas, weldTarget.x(), weldTarget.y() ),
                           Qt::LeftButton, Qt::LeftButton, Qt::NoModifier );
  tool.canvasPressEvent( &press2 );
  QVERIFY( tool.isDragging() );
  QgsMapMouseEvent release2( &canvas, QEvent::MouseButtonRelease, QPoint( 55, 125 ),
                             Qt::LeftButton, Qt::NoButton, Qt::NoModifier );
  tool.canvasReleaseEvent( &release2 );

  const QgsPointXY pNew = mapPt( canvas, 55, 125 );
  const QgsLineString *lsA2 = asLineString( layer.getFeature( fidA ).geometry() );
  const QgsLineString *lsB2 = asLineString( layer.getFeature( fidB ).geometry() );
  QVERIFY( qgsDoubleNear( lsA2->xAt( 1 ), pNew.x(), 1e-6 ) );
  QVERIFY( qgsDoubleNear( lsB2->xAt( 0 ), pNew.x(), 1e-6 ) ); // welded neighbor moved too

  canvas.unsetMapTool( &tool );
  layer.rollBack();
}

void TestEditTools::toolbarTopologicalActionMirrorsProjectFlag()
{
  QgsMapCanvas canvas;
  configureCanvas( canvas );

  QgsProject project;
  QVERIFY( !project.topologicalEditing() ); // QGIS default

  PaleoEditingToolbar bar( &canvas );
  QVERIFY( bar.actionTopological() );
  QVERIFY( bar.actionTopological()->isCheckable() );

  bar.setProject( &project );
  QVERIFY( !bar.actionTopological()->isChecked() );

  // Toggle on → the project flag follows (persists in .qgz).
  bar.actionTopological()->setChecked( true );
  QVERIFY( project.topologicalEditing() );

  // A project that already has the flag set lands checked — adoption without
  // a toggled write-back (would loop otherwise).
  QgsProject stored;
  stored.setTopologicalEditing( true );
  bar.setProject( &stored );
  QVERIFY( bar.actionTopological()->isChecked() );
  QVERIFY( stored.topologicalEditing() );
}

void TestEditTools::toolbarCrossLayerTopoMirrorsProjectEntry()
{
  QgsMapCanvas canvas;
  configureCanvas( canvas );

  QgsProject project;
  PaleoEditingToolbar bar( &canvas );
  QVERIFY( bar.actionCrossLayerTopo() );
  QVERIFY( bar.actionCrossLayerTopo()->isCheckable() );

  bar.setProject( &project );
  QVERIFY( !bar.actionCrossLayerTopo()->isChecked() );

  // 开关写入工程条目 paleo/crossLayerTopologicalEditing（随 .qgz 持久化）。
  bar.actionCrossLayerTopo()->setChecked( true );
  QCOMPARE( project.readNumEntry( QStringLiteral( "paleo" ),
                                  QStringLiteral( "crossLayerTopologicalEditing" ), 0 ), 1 );

  // 已置位的工程落进工具条 → 采纳而不回写；拓扑关闭时跨层仍禁用带 reason。
  QgsProject stored;
  stored.writeEntry( QStringLiteral( "paleo" ), QStringLiteral( "crossLayerTopologicalEditing" ), 1 );
  bar.setProject( &stored );
  QVERIFY( bar.actionCrossLayerTopo()->isChecked() );

  bar.actionTopological()->setChecked( false );
  QVERIFY( !bar.actionTopological()->isEnabled() || !bar.actionCrossLayerTopo()->isEnabled()
           || bar.actionCrossLayerTopo()->toolTip().contains( QStringLiteral( "拓扑" ) ) );
}

void TestEditTools::vertexMovePolygonClosureMaintainsClosedRing()
{
  QgsMapCanvas canvas;
  configureCanvas( canvas );

  QgsVectorLayer layer( QStringLiteral( "Polygon?crs=EPSG:4326&field=id:integer" ), QStringLiteral( "move-closure" ), QStringLiteral( "memory" ) );
  QVERIFY2( layer.isValid(), "memory polygon layer failed to initialize" );
  const QgsFeatureId fid = seedFeature( layer, QgsGeometry::fromWkt( squareWkt( 0, 0, 10 ) ) );
  layer.startEditing();
  layer.selectByIds( { fid } );
  canvas.setLayers( QList<QgsMapLayer *>{ &layer } );
  canvas.setCurrentLayer( &layer );
  canvas.refresh();

  TestVertexTool tool( &canvas, &layer );
  tool.setTopologicalEditingEnabled( false ); // explicitly verify non-topo mode
  canvas.setMapTool( &tool );
  QSignalSpy editedSpy( &tool, &PaleoVertexTool::featureEdited );

  // Drag vertex 0 at (0,0) to (-5,-5)
  const QPoint startPx = pxAt( canvas, 0, 0 );
  const QPoint targetPx = pxAt( canvas, -5, -5 );
  const QgsPointXY pNew = mapPt( canvas, targetPx.x(), targetPx.y() );

  QgsMapMouseEvent press( &canvas, QEvent::MouseButtonPress, startPx,
                          Qt::LeftButton, Qt::LeftButton, Qt::NoModifier );
  tool.canvasPressEvent( &press );
  QVERIFY2( tool.isDragging(), "press on vertex 0 must arm drag" );

  QgsMapMouseEvent move( &canvas, QEvent::MouseMove, targetPx,
                         Qt::NoButton, Qt::LeftButton, Qt::NoModifier );
  tool.canvasMoveEvent( &move );

  QgsMapMouseEvent release( &canvas, QEvent::MouseButtonRelease, targetPx,
                            Qt::LeftButton, Qt::NoButton, Qt::NoModifier );
  tool.canvasReleaseEvent( &release );

  QCOMPARE( editedSpy.count(), 1 );
  QCOMPARE( layer.undoStack()->count(), 1 );

  const QgsGeometry mutated = layer.getFeature( fid ).geometry();
  QVERIFY2( mutated.isGeosValid(), "mutated polygon must remain GEOS valid" );
  const QgsPolygon *poly = asPolygon( mutated );
  QVERIFY2( poly && poly->exteriorRing(), "must have valid polygon exterior ring" );
  const QgsLineString *ring = qgsgeometry_cast<const QgsLineString *>( poly->exteriorRing() );
  QVERIFY2( ring, "exterior ring must be line string" );
  QCOMPARE( ring->numPoints(), 5 );
  QVERIFY( ring->isClosed() );

  // Both vertex 0 and closure vertex (index 4) must match pNew
  QVERIFY( qgsDoubleNear( ring->xAt( 0 ), pNew.x(), 1e-6 ) );
  QVERIFY( qgsDoubleNear( ring->yAt( 0 ), pNew.y(), 1e-6 ) );
  QVERIFY( qgsDoubleNear( ring->xAt( 4 ), pNew.x(), 1e-6 ) );
  QVERIFY( qgsDoubleNear( ring->yAt( 4 ), pNew.y(), 1e-6 ) );

  // Undo restores both vertex 0 and closure vertex to (0,0)
  layer.undoStack()->undo();
  const QgsGeometry restored = layer.getFeature( fid ).geometry();
  QVERIFY( restored.isGeosValid() );
  const QgsPolygon *polyRestored = asPolygon( restored );
  const QgsLineString *ringRestored = qgsgeometry_cast<const QgsLineString *>( polyRestored->exteriorRing() );
  QVERIFY( ringRestored->isClosed() );
  QVERIFY( qgsDoubleNear( ringRestored->xAt( 0 ), 0.0, 1e-6 ) );
  QVERIFY( qgsDoubleNear( ringRestored->yAt( 0 ), 0.0, 1e-6 ) );
  QVERIFY( qgsDoubleNear( ringRestored->xAt( 4 ), 0.0, 1e-6 ) );
  QVERIFY( qgsDoubleNear( ringRestored->yAt( 4 ), 0.0, 1e-6 ) );

  canvas.unsetMapTool( &tool );
  layer.rollBack();
}

void TestEditTools::vertexDeletePolygonClosurePreservesRing()
{
  QgsMapCanvas canvas;
  configureCanvas( canvas );

  // Test 1: topoEditing = false
  {
    QgsVectorLayer layer( QStringLiteral( "Polygon?crs=EPSG:4326&field=id:integer" ), QStringLiteral( "del-closure-off" ), QStringLiteral( "memory" ) );
    QVERIFY2( layer.isValid(), "memory polygon layer failed to initialize" );
    const QgsFeatureId fid = seedFeature( layer, QgsGeometry::fromWkt( squareWkt( 0, 0, 10 ) ) );
    layer.startEditing();
    layer.selectByIds( { fid } );
    canvas.setLayers( QList<QgsMapLayer *>{ &layer } );
    canvas.setCurrentLayer( &layer );
    canvas.refresh();

    TestVertexTool tool( &canvas, &layer );
    tool.setTopologicalEditingEnabled( false );
    canvas.setMapTool( &tool );
    QSignalSpy editedSpy( &tool, &PaleoVertexTool::featureEdited );
    QSignalSpy msgSpy( &tool, &QgsMapTool::messageEmitted );

    // Right-click on vertex 0 (0,0)
    click( tool, canvas, pxAt( canvas, 0, 0 ), Qt::RightButton );

    QCOMPARE( editedSpy.count(), 1 );
    QCOMPARE( msgSpy.count(), 0 );
    QCOMPARE( vertexTotal( layer, fid ), 4 );
    QCOMPARE( tool.markerCount(), 4 );

    const QgsGeometry mutated = layer.getFeature( fid ).geometry();
    QVERIFY2( mutated.isGeosValid(), "mutated polygon must be GEOS valid" );
    const QgsPolygon *poly = asPolygon( mutated );
    QVERIFY( poly && poly->exteriorRing() );
    const QgsLineString *ring = qgsgeometry_cast<const QgsLineString *>( poly->exteriorRing() );
    QVERIFY( ring->isClosed() );
    QCOMPARE( ring->numPoints(), 4 );
    // Ensure start == end
    QVERIFY( qgsDoubleNear( ring->xAt( 0 ), ring->xAt( 3 ), 1e-6 ) );
    QVERIFY( qgsDoubleNear( ring->yAt( 0 ), ring->yAt( 3 ), 1e-6 ) );

    canvas.unsetMapTool( &tool );
    layer.rollBack();
  }

  // Test 2: topoEditing = true
  {
    QgsVectorLayer layer( QStringLiteral( "Polygon?crs=EPSG:4326&field=id:integer" ), QStringLiteral( "del-closure-on" ), QStringLiteral( "memory" ) );
    QVERIFY2( layer.isValid(), "memory polygon layer failed to initialize" );
    const QgsFeatureId fid = seedFeature( layer, QgsGeometry::fromWkt( squareWkt( 0, 0, 10 ) ) );
    layer.startEditing();
    layer.selectByIds( { fid } );
    canvas.setLayers( QList<QgsMapLayer *>{ &layer } );
    canvas.setCurrentLayer( &layer );
    canvas.refresh();

    TestVertexTool tool( &canvas, &layer );
    tool.setTopologicalEditingEnabled( true );
    canvas.setMapTool( &tool );
    QSignalSpy editedSpy( &tool, &PaleoVertexTool::featureEdited );
    QSignalSpy msgSpy( &tool, &QgsMapTool::messageEmitted );

    // Right-click on vertex 0 (0,0) with topo editing enabled
    click( tool, canvas, pxAt( canvas, 0, 0 ), Qt::RightButton );

    QCOMPARE( editedSpy.count(), 1 );
    QCOMPARE( msgSpy.count(), 0 );
    // Exactly 1 vertex count dropped (5 -> 4), NOT double deleted down to 3
    QCOMPARE( vertexTotal( layer, fid ), 4 );
    QCOMPARE( tool.markerCount(), 4 );

    const QgsGeometry mutated = layer.getFeature( fid ).geometry();
    QVERIFY2( mutated.isGeosValid(), "mutated polygon must be GEOS valid in topo mode" );
    const QgsPolygon *poly = asPolygon( mutated );
    QVERIFY( poly && poly->exteriorRing() );
    const QgsLineString *ring = qgsgeometry_cast<const QgsLineString *>( poly->exteriorRing() );
    QVERIFY( ring->isClosed() );
    QCOMPARE( ring->numPoints(), 4 );
    QVERIFY( qgsDoubleNear( ring->xAt( 0 ), ring->xAt( 3 ), 1e-6 ) );
    QVERIFY( qgsDoubleNear( ring->yAt( 0 ), ring->yAt( 3 ), 1e-6 ) );

    canvas.unsetMapTool( &tool );
    layer.rollBack();
  }
}

void TestEditTools::vertexUndoRedoSyncsMarkers()
{
  QgsMapCanvas canvas;
  configureCanvas( canvas );

  QgsVectorLayer layer( QStringLiteral( "LineString?crs=EPSG:4326&field=id:integer" ), QStringLiteral( "undo-sync" ), QStringLiteral( "memory" ) );
  QVERIFY2( layer.isValid(), "memory line layer failed to initialize" );
  const QgsPointXY pt0 = mapPt( canvas, 40, 140 );
  const QgsPointXY pt1 = mapPt( canvas, 100, 100 );
  const QgsPointXY pt2 = mapPt( canvas, 160, 60 );
  const QgsFeatureId fid = seedFeature( layer, QgsGeometry::fromPolylineXY( { pt0, pt1, pt2 } ) );

  layer.startEditing();
  layer.selectByIds( { fid } );
  canvas.setLayers( QList<QgsMapLayer *>{ &layer } );
  canvas.setCurrentLayer( &layer );
  canvas.refresh();

  TestVertexTool tool( &canvas, &layer );
  canvas.setMapTool( &tool );
  QCOMPARE( tool.markerCount(), 3 );
  QCOMPARE( tool.markers().size(), 3 );
  QVERIFY( qgsDoubleNear( tool.markers().at( 1 )->center().x(), pt1.x(), 1e-4 ) );
  QVERIFY( qgsDoubleNear( tool.markers().at( 1 )->center().y(), pt1.y(), 1e-4 ) );

  // Drag vertex 1 to (120, 120)
  const QPoint startPx = pxAt( canvas, pt1.x(), pt1.y() );
  const QPoint targetPx = pxAt( canvas, mapPt( canvas, 120, 120 ).x(), mapPt( canvas, 120, 120 ).y() );
  const QgsPointXY pt1New = mapPt( canvas, targetPx.x(), targetPx.y() );

  QgsMapMouseEvent press( &canvas, QEvent::MouseButtonPress, startPx,
                          Qt::LeftButton, Qt::LeftButton, Qt::NoModifier );
  tool.canvasPressEvent( &press );
  QVERIFY( tool.isDragging() );

  QgsMapMouseEvent release( &canvas, QEvent::MouseButtonRelease, targetPx,
                            Qt::LeftButton, Qt::NoButton, Qt::NoModifier );
  tool.canvasReleaseEvent( &release );

  // After move, marker at index 1 is at pt1New
  QCOMPARE( tool.markerCount(), 3 );
  QVERIFY( qgsDoubleNear( tool.markers().at( 1 )->center().x(), pt1New.x(), 1e-4 ) );
  QVERIFY( qgsDoubleNear( tool.markers().at( 1 )->center().y(), pt1New.y(), 1e-4 ) );

  // Trigger undo: markers must automatically sync to restored geometry
  layer.undoStack()->undo();
  QCOMPARE( tool.markerCount(), 3 );
  QVERIFY( qgsDoubleNear( tool.markers().at( 1 )->center().x(), pt1.x(), 1e-4 ) );
  QVERIFY( qgsDoubleNear( tool.markers().at( 1 )->center().y(), pt1.y(), 1e-4 ) );

  // Trigger redo: markers must automatically sync to pt1New again
  layer.undoStack()->redo();
  QCOMPARE( tool.markerCount(), 3 );
  QVERIFY( qgsDoubleNear( tool.markers().at( 1 )->center().x(), pt1New.x(), 1e-4 ) );
  QVERIFY( qgsDoubleNear( tool.markers().at( 1 )->center().y(), pt1New.y(), 1e-4 ) );

  canvas.unsetMapTool( &tool );
  layer.rollBack();
}

void TestEditTools::vertexTopoDragMovesAllMarkers()
{
  QgsMapCanvas canvas;
  configureCanvas( canvas );

  QgsVectorLayer layer( QStringLiteral( "LineString?crs=EPSG:4326&field=id:integer" ), QStringLiteral( "topo-markers" ), QStringLiteral( "memory" ) );
  QVERIFY2( layer.isValid(), "memory line layer failed to initialize" );
  const QgsPointXY shared = mapPt( canvas, 100, 100 );
  const QgsFeatureId fidA = seedFeature( layer, QgsGeometry::fromPolylineXY(
      { mapPt( canvas, 40, 140 ), shared, mapPt( canvas, 160, 60 ) } ) );
  const QgsFeatureId fidB = seedFeature( layer, QgsGeometry::fromPolylineXY(
      { shared, mapPt( canvas, 180, 140 ) } ) );

  layer.startEditing();
  layer.selectByIds( { fidA } ); // fidA only is selected; fidB is unselected neighbor
  canvas.setLayers( QList<QgsMapLayer *>{ &layer } );
  canvas.setCurrentLayer( &layer );
  canvas.refresh();

  TestVertexTool tool( &canvas, &layer );
  tool.setTopologicalEditingEnabled( true );
  canvas.setMapTool( &tool );

  // Press on the shared vertex
  const QPoint pressPx( 100, 100 );
  QgsMapMouseEvent press( &canvas, QEvent::MouseButtonPress, pressPx,
                          Qt::LeftButton, Qt::LeftButton, Qt::NoModifier );
  tool.canvasPressEvent( &press );
  QVERIFY2( tool.isDragging(), "press on shared vertex must arm drag" );

  // Topo markers must be created for the unselected feature's coincident vertex
  QVERIFY2( !tool.topoMarkers().isEmpty(), "topoMarkers must be populated for unselected coincident vertex" );

  // Move mouse to QPoint(70, 110)
  const QPoint movePx( 70, 110 );
  const QgsPointXY moveMapPt = mapPt( canvas, 70, 110 );
  QgsMapMouseEvent move( &canvas, QEvent::MouseMove, movePx,
                         Qt::NoButton, Qt::LeftButton, Qt::NoModifier );
  tool.canvasMoveEvent( &move );

  // All markers in topoMarkers must track the mouse position exactly
  for ( QgsVertexMarker *tm : tool.topoMarkers() )
  {
    QVERIFY( tm != nullptr );
    QVERIFY( qgsDoubleNear( tm->center().x(), moveMapPt.x(), 1e-4 ) );
    QVERIFY( qgsDoubleNear( tm->center().y(), moveMapPt.y(), 1e-4 ) );
  }

  // Cancel with Esc
  sendEsc( tool );
  QVERIFY( !tool.isDragging() );
  QVERIFY( tool.topoMarkers().isEmpty() );

  canvas.unsetMapTool( &tool );
  layer.rollBack();
}

void TestEditTools::vertexMoveClosedLineStringClosureInvariants()
{
  QgsMapCanvas canvas;
  configureCanvas( canvas );

  QgsVectorLayer layer( QStringLiteral( "LineString?crs=EPSG:4326&field=id:integer" ), QStringLiteral( "closed-ls-move" ), QStringLiteral( "memory" ) );
  QVERIFY2( layer.isValid(), "memory line layer failed to initialize" );

  const QgsPointXY p0( 20, 20 );
  const QgsPointXY p1( 40, 20 );
  const QgsPointXY p2( 40, 40 );
  const QgsPointXY p3( 20, 40 );
  // 5 vertices: closed ring line string
  const QgsFeatureId fid1 = seedFeature( layer, QgsGeometry::fromPolylineXY( { p0, p1, p2, p3, p0 } ) );

  layer.startEditing();
  layer.selectByIds( { fid1 } );
  canvas.setLayers( QList<QgsMapLayer *>{ &layer } );
  canvas.setCurrentLayer( &layer );
  canvas.refresh();

  TestVertexTool tool( &canvas, &layer );
  tool.setTopologicalEditingEnabled( false ); // Step 1: verify non-topological mode
  canvas.setMapTool( &tool );
  QSignalSpy editedSpy( &tool, &PaleoVertexTool::featureEdited );

  // Drag start/closure vertex at (20, 20) to (15, 15)
  const QPoint startPx = pxAt( canvas, 20, 20 );
  const QPoint targetPx = pxAt( canvas, 15, 15 );
  const QgsPointXY pNew = mapPt( canvas, targetPx.x(), targetPx.y() );

  QgsMapMouseEvent press( &canvas, QEvent::MouseButtonPress, startPx,
                          Qt::LeftButton, Qt::LeftButton, Qt::NoModifier );
  tool.canvasPressEvent( &press );
  QVERIFY2( tool.isDragging(), "press on closed line start vertex must arm drag" );

  QgsMapMouseEvent move( &canvas, QEvent::MouseMove, targetPx,
                         Qt::NoButton, Qt::LeftButton, Qt::NoModifier );
  tool.canvasMoveEvent( &move );

  QgsMapMouseEvent release( &canvas, QEvent::MouseButtonRelease, targetPx,
                            Qt::LeftButton, Qt::NoButton, Qt::NoModifier );
  tool.canvasReleaseEvent( &release );

  QCOMPARE( editedSpy.count(), 1 );
  QCOMPARE( layer.undoStack()->count(), 1 );

  const QgsGeometry mutated = layer.getFeature( fid1 ).geometry();
  QVERIFY2( mutated.isGeosValid(), "mutated closed line must remain GEOS valid" );
  const QgsLineString *ls = asLineString( mutated );
  QVERIFY2( ls, "must remain line string" );
  QCOMPARE( ls->numPoints(), 5 );
  QVERIFY2( ls->isClosed(), "closed line string must remain closed after moving closure vertex" );
  QVERIFY( qgsDoubleNear( ls->xAt( 0 ), pNew.x(), 1e-5 ) );
  QVERIFY( qgsDoubleNear( ls->yAt( 0 ), pNew.y(), 1e-5 ) );
  QVERIFY( qgsDoubleNear( ls->xAt( 4 ), pNew.x(), 1e-5 ) );
  QVERIFY( qgsDoubleNear( ls->yAt( 4 ), pNew.y(), 1e-5 ) );
  QVERIFY( qgsDoubleNear( ls->xAt( 0 ), ls->xAt( 4 ), 1e-6 ) );
  QVERIFY( qgsDoubleNear( ls->yAt( 0 ), ls->yAt( 4 ), 1e-6 ) );

  // Undo restores both endpoints
  layer.undoStack()->undo();
  const QgsGeometry undone = layer.getFeature( fid1 ).geometry();
  const QgsLineString *lsUndone = asLineString( undone );
  QVERIFY( lsUndone->isClosed() );
  QVERIFY( qgsDoubleNear( lsUndone->xAt( 0 ), 20.0, 1e-5 ) );
  QVERIFY( qgsDoubleNear( lsUndone->yAt( 0 ), 20.0, 1e-5 ) );
  QVERIFY( qgsDoubleNear( lsUndone->xAt( 4 ), 20.0, 1e-5 ) );
  QVERIFY( qgsDoubleNear( lsUndone->yAt( 4 ), 20.0, 1e-5 ) );

  // Redo re-applies move
  layer.undoStack()->redo();
  const QgsGeometry redone = layer.getFeature( fid1 ).geometry();
  const QgsLineString *lsRedone = asLineString( redone );
  QVERIFY( lsRedone->isClosed() );
  QVERIFY( qgsDoubleNear( lsRedone->xAt( 0 ), pNew.x(), 1e-5 ) );
  QVERIFY( qgsDoubleNear( lsRedone->xAt( 4 ), pNew.x(), 1e-5 ) );

  // Step 2: Now test with topologicalEditingEnabled = true and a neighbor closed line
  tool.setTopologicalEditingEnabled( true );
  const QgsFeatureId fid2 = seedFeature( layer, QgsGeometry::fromPolylineXY(
      { pNew, QgsPointXY( 10, 30 ), QgsPointXY( 10, 15 ), pNew } ) );
  layer.selectByIds( { fid1 } ); // fid1 selected, fid2 unselected neighbor sharing pNew

  const QPoint targetPx2 = pxAt( canvas, 25, 25 );
  const QgsPointXY pNew2 = mapPt( canvas, targetPx2.x(), targetPx2.y() );
  QgsMapMouseEvent press2( &canvas, QEvent::MouseButtonPress, targetPx,
                           Qt::LeftButton, Qt::LeftButton, Qt::NoModifier );
  tool.canvasPressEvent( &press2 );
  QVERIFY( tool.isDragging() );

  QgsMapMouseEvent release2( &canvas, QEvent::MouseButtonRelease, targetPx2,
                             Qt::LeftButton, Qt::NoButton, Qt::NoModifier );
  tool.canvasReleaseEvent( &release2 );

  const QgsLineString *ls1Topo = asLineString( layer.getFeature( fid1 ).geometry() );
  const QgsLineString *ls2Topo = asLineString( layer.getFeature( fid2 ).geometry() );
  QVERIFY2( ls1Topo && ls1Topo->isClosed(), "fid1 must remain closed in topo move" );
  QVERIFY2( ls2Topo && ls2Topo->isClosed(), "fid2 must remain closed in topo move" );
  QVERIFY( qgsDoubleNear( ls1Topo->xAt( 0 ), pNew2.x(), 1e-5 ) );
  QVERIFY( qgsDoubleNear( ls1Topo->xAt( 4 ), pNew2.x(), 1e-5 ) );
  QVERIFY( qgsDoubleNear( ls2Topo->xAt( 0 ), pNew2.x(), 1e-5 ) );
  QVERIFY( qgsDoubleNear( ls2Topo->xAt( 3 ), pNew2.x(), 1e-5 ) );

  canvas.unsetMapTool( &tool );
  layer.rollBack();
}

void TestEditTools::vertexMovePolygonEndClosureVertexInvariants()
{
  QgsMapCanvas canvas;
  configureCanvas( canvas );

  QgsVectorLayer layer( QStringLiteral( "Polygon?crs=EPSG:4326&field=id:integer" ), QStringLiteral( "move-end-closure" ), QStringLiteral( "memory" ) );
  QVERIFY2( layer.isValid(), "memory polygon layer failed to initialize" );
  const QgsFeatureId fid = seedFeature( layer, QgsGeometry::fromWkt( squareWkt( 20, 20, 20 ) ) );
  layer.startEditing();
  layer.selectByIds( { fid } );
  canvas.setLayers( QList<QgsMapLayer *>{ &layer } );
  canvas.setCurrentLayer( &layer );
  canvas.refresh();

  TestVertexTool tool( &canvas, &layer );
  tool.setTopologicalEditingEnabled( false );
  canvas.setMapTool( &tool );
  QSignalSpy editedSpy( &tool, &PaleoVertexTool::featureEdited );

  const QPoint startPx = pxAt( canvas, 20, 20 );
  const QPoint targetPx = pxAt( canvas, 15, 18 );
  const QgsPointXY pNew = mapPt( canvas, targetPx.x(), targetPx.y() );

  QgsMapMouseEvent press( &canvas, QEvent::MouseButtonPress, startPx,
                          Qt::LeftButton, Qt::LeftButton, Qt::NoModifier );
  tool.canvasPressEvent( &press );
  QVERIFY( tool.isDragging() );

  QgsMapMouseEvent release( &canvas, QEvent::MouseButtonRelease, targetPx,
                            Qt::LeftButton, Qt::NoButton, Qt::NoModifier );
  tool.canvasReleaseEvent( &release );

  QCOMPARE( editedSpy.count(), 1 );
  const QgsGeometry mutated = layer.getFeature( fid ).geometry();
  QVERIFY( mutated.isGeosValid() );
  const QgsPolygon *poly = asPolygon( mutated );
  QVERIFY( poly && poly->exteriorRing() );
  const QgsLineString *ring = qgsgeometry_cast<const QgsLineString *>( poly->exteriorRing() );
  QVERIFY( ring->isClosed() );
  QCOMPARE( ring->numPoints(), 5 );
  QVERIFY( qgsDoubleNear( ring->xAt( 0 ), pNew.x(), 1e-5 ) );
  QVERIFY( qgsDoubleNear( ring->yAt( 0 ), pNew.y(), 1e-5 ) );
  QVERIFY( qgsDoubleNear( ring->xAt( 4 ), pNew.x(), 1e-5 ) );
  QVERIFY( qgsDoubleNear( ring->yAt( 4 ), pNew.y(), 1e-5 ) );
  QVERIFY( qgsDoubleNear( ring->xAt( 0 ), ring->xAt( 4 ), 1e-9 ) );
  QVERIFY( qgsDoubleNear( ring->yAt( 0 ), ring->yAt( 4 ), 1e-9 ) );

  // Full undo/redo cycle
  layer.undoStack()->undo();
  const QgsGeometry undone = layer.getFeature( fid ).geometry();
  QVERIFY( undone.isGeosValid() );
  const QgsLineString *ringUndone = qgsgeometry_cast<const QgsLineString *>( asPolygon( undone )->exteriorRing() );
  QVERIFY( ringUndone->isClosed() );
  QVERIFY( qgsDoubleNear( ringUndone->xAt( 0 ), 20.0, 1e-5 ) );
  QVERIFY( qgsDoubleNear( ringUndone->xAt( 4 ), 20.0, 1e-5 ) );

  layer.undoStack()->redo();
  const QgsGeometry redone = layer.getFeature( fid ).geometry();
  QVERIFY( redone.isGeosValid() );
  const QgsLineString *ringRedone = qgsgeometry_cast<const QgsLineString *>( asPolygon( redone )->exteriorRing() );
  QVERIFY( ringRedone->isClosed() );
  QVERIFY( qgsDoubleNear( ringRedone->xAt( 0 ), pNew.x(), 1e-5 ) );
  QVERIFY( qgsDoubleNear( ringRedone->xAt( 4 ), pNew.x(), 1e-5 ) );

  canvas.unsetMapTool( &tool );
  layer.rollBack();
}

void TestEditTools::vertexDeleteEndClosureVertexInvariants()
{
  QgsMapCanvas canvas;
  configureCanvas( canvas );

  QgsVectorLayer layer( QStringLiteral( "Polygon?crs=EPSG:4326&field=id:integer" ), QStringLiteral( "del-quad-closure" ), QStringLiteral( "memory" ) );
  QVERIFY2( layer.isValid(), "memory polygon layer failed to initialize" );
  const QgsFeatureId fid = seedFeature( layer, QgsGeometry::fromWkt( squareWkt( 20, 20, 20 ) ) );
  layer.startEditing();
  layer.selectByIds( { fid } );
  canvas.setLayers( QList<QgsMapLayer *>{ &layer } );
  canvas.setCurrentLayer( &layer );
  canvas.refresh();

  TestVertexTool tool( &canvas, &layer );
  tool.setTopologicalEditingEnabled( false );
  canvas.setMapTool( &tool );
  QSignalSpy editedSpy( &tool, &PaleoVertexTool::featureEdited );
  QSignalSpy msgSpy( &tool, &QgsMapTool::messageEmitted );

  // Right-click on closure vertex at (20, 20)
  click( tool, canvas, pxAt( canvas, 20, 20 ), Qt::RightButton );

  QCOMPARE( editedSpy.count(), 1 );
  QCOMPARE( msgSpy.count(), 0 );
  QCOMPARE( vertexTotal( layer, fid ), 4 ); // 5 -> 4 vertices (quad -> triangle)
  QCOMPARE( tool.markerCount(), 4 );

  const QgsGeometry mutated = layer.getFeature( fid ).geometry();
  QVERIFY2( mutated.isGeosValid(), "mutated geometry must be a valid triangle" );
  const QgsPolygon *poly = asPolygon( mutated );
  QVERIFY( poly && poly->exteriorRing() );
  const QgsLineString *ring = qgsgeometry_cast<const QgsLineString *>( poly->exteriorRing() );
  QVERIFY2( ring->isClosed(), "ring must remain closed" );
  QCOMPARE( ring->numPoints(), 4 );
  QVERIFY( qgsDoubleNear( ring->xAt( 0 ), ring->xAt( 3 ), 1e-6 ) );
  QVERIFY( qgsDoubleNear( ring->yAt( 0 ), ring->yAt( 3 ), 1e-6 ) );

  // Verify undo restores 5 vertices
  layer.undoStack()->undo();
  QCOMPARE( vertexTotal( layer, fid ), 5 );
  QCOMPARE( tool.markerCount(), 5 );
  const QgsGeometry undone = layer.getFeature( fid ).geometry();
  QVERIFY( undone.isGeosValid() );
  const QgsLineString *ringUndone = qgsgeometry_cast<const QgsLineString *>( asPolygon( undone )->exteriorRing() );
  QCOMPARE( ringUndone->numPoints(), 5 );
  QVERIFY( ringUndone->isClosed() );

  // Verify redo restores 4 vertices
  layer.undoStack()->redo();
  QCOMPARE( vertexTotal( layer, fid ), 4 );
  QCOMPARE( tool.markerCount(), 4 );
  const QgsGeometry redone = layer.getFeature( fid ).geometry();
  QVERIFY( redone.isGeosValid() );

  canvas.unsetMapTool( &tool );
  layer.rollBack();
}

void TestEditTools::vertexDeleteTriangleAllVerticesRejected()
{
  QgsMapCanvas canvas;
  configureCanvas( canvas );

  QgsVectorLayer layer( QStringLiteral( "Polygon?crs=EPSG:4326&field=id:integer" ), QStringLiteral( "triangle-refuse" ), QStringLiteral( "memory" ) );
  QVERIFY2( layer.isValid(), "memory polygon layer failed to initialize" );

  const QVector<QgsPointXY> pts = { mapPt( canvas, 40, 140 ), mapPt( canvas, 140, 140 ), mapPt( canvas, 90, 60 ) };
  const QgsFeatureId fid = seedFeature( layer, QgsGeometry::fromPolygonXY( { pts } ) );

  layer.startEditing();
  layer.selectByIds( { fid } );
  canvas.setLayers( QList<QgsMapLayer *>{ &layer } );
  canvas.setCurrentLayer( &layer );
  canvas.refresh();

  TestVertexTool tool( &canvas, &layer );
  canvas.setMapTool( &tool );
  QSignalSpy editedSpy( &tool, &PaleoVertexTool::featureEdited );
  QSignalSpy msgSpy( &tool, &QgsMapTool::messageEmitted );

  QCOMPARE( vertexTotal( layer, fid ), 4 );
  QCOMPARE( tool.markerCount(), 4 );

  // Test rejecting delete on vertex 0 at (40, 140)
  click( tool, canvas, QPoint( 40, 140 ), Qt::RightButton );
  QCOMPARE( editedSpy.count(), 0 );
  QCOMPARE( msgSpy.count(), 1 );
  QCOMPARE( vertexTotal( layer, fid ), 4 );
  QCOMPARE( layer.undoStack()->count(), 0 );

  // Test rejecting delete on vertex 1 at (140, 140)
  click( tool, canvas, QPoint( 140, 140 ), Qt::RightButton );
  QCOMPARE( editedSpy.count(), 0 );
  QCOMPARE( msgSpy.count(), 2 );
  QCOMPARE( vertexTotal( layer, fid ), 4 );
  QCOMPARE( layer.undoStack()->count(), 0 );

  // Test rejecting delete on vertex 2 at (90, 60)
  click( tool, canvas, QPoint( 90, 60 ), Qt::RightButton );
  QCOMPARE( editedSpy.count(), 0 );
  QCOMPARE( msgSpy.count(), 3 );
  QCOMPARE( vertexTotal( layer, fid ), 4 );
  QCOMPARE( layer.undoStack()->count(), 0 );

  const QgsGeometry g = layer.getFeature( fid ).geometry();
  QVERIFY( g.isGeosValid() );
  const QgsPolygon *poly = asPolygon( g );
  QVERIFY( poly && poly->exteriorRing() );
  QCOMPARE( qgsgeometry_cast<const QgsLineString *>( poly->exteriorRing() )->numPoints(), 4 );

  canvas.unsetMapTool( &tool );
  layer.rollBack();
}

void TestEditTools::vertexDeleteMultiFeaturePartialTriangleRefused()
{
  QgsMapCanvas canvas;
  configureCanvas( canvas );

  QgsVectorLayer layer( QStringLiteral( "Polygon?crs=EPSG:4326&field=id:integer" ), QStringLiteral( "batch-refusal" ), QStringLiteral( "memory" ) );
  QVERIFY2( layer.isValid(), "memory polygon layer failed to initialize" );

  // Feature A: Quad (30,30)-(50,50) -> 5 vertices
  const QgsFeatureId fidA = seedFeature( layer, QgsGeometry::fromWkt( squareWkt( 30, 30, 20 ) ) );
  // Feature B: Triangle (30,30), (10,30), (20,50) -> 4 vertices
  const QVector<QgsPointXY> triPts = { QgsPointXY( 30, 30 ), QgsPointXY( 10, 30 ), QgsPointXY( 20, 50 ) };
  const QgsFeatureId fidB = seedFeature( layer, QgsGeometry::fromPolygonXY( { triPts } ) );

  layer.startEditing();
  layer.selectByIds( { fidA } ); // fidA is selected; shares (30,30) with fidB
  canvas.setLayers( QList<QgsMapLayer *>{ &layer } );
  canvas.setCurrentLayer( &layer );
  canvas.refresh();

  TestVertexTool tool( &canvas, &layer );
  tool.setTopologicalEditingEnabled( true );
  canvas.setMapTool( &tool );
  QSignalSpy editedSpy( &tool, &PaleoVertexTool::featureEdited );
  QSignalSpy msgSpy( &tool, &QgsMapTool::messageEmitted );

  QCOMPARE( vertexTotal( layer, fidA ), 5 );
  QCOMPARE( vertexTotal( layer, fidB ), 4 );

  // Right-click the shared corner (30, 30): Feature B would drop to 3, so entire batch must be refused!
  click( tool, canvas, pxAt( canvas, 30, 30 ), Qt::RightButton );

  QCOMPARE( editedSpy.count(), 0 ); // Refused!
  QCOMPARE( msgSpy.count(), 1 );   // Guard warning emitted
  QCOMPARE( layer.undoStack()->count(), 0 );

  // Invariant: Feature A must NOT have been modified
  QCOMPARE( vertexTotal( layer, fidA ), 5 );
  // Invariant: Feature B must NOT have been modified
  QCOMPARE( vertexTotal( layer, fidB ), 4 );

  QVERIFY( layer.getFeature( fidA ).geometry().isGeosValid() );
  QVERIFY( layer.getFeature( fidB ).geometry().isGeosValid() );

  canvas.unsetMapTool( &tool );
  layer.rollBack();
}

void TestEditTools::vertexDeleteMultiCoincidentRingQuotaRefused()
{
  QgsMapCanvas canvas;
  configureCanvas( canvas );

  QgsVectorLayer layer( QStringLiteral( "Polygon?crs=EPSG:4326&field=id:integer" ), QStringLiteral( "pinch-polygon" ), QStringLiteral( "memory" ) );
  QVERIFY2( layer.isValid(), "memory polygon layer failed to initialize" );

  // Construct a 5-point self-touching / hourglass pinch polygon:
  // (10,10) -> (50,50) -> (10,90) -> (50,50) [auto-closure appends (10,10)]
  // Vertex 0: (10,10)
  // Vertex 1: (50,50)
  // Vertex 2: (10,90)
  // Vertex 3: (50,50) [coincident with vertex 1]
  // Vertex 4: (10,10) [closure]
  // Total vertices: 5. Deleting (50,50) would delete BOTH vertex 1 and 3, reducing ring to 3 vertices (collapse!)
  const QVector<QgsPointXY> pinchPts = {
    mapPt( canvas, 10, 10 ),
    mapPt( canvas, 50, 50 ),
    mapPt( canvas, 10, 90 ),
    mapPt( canvas, 50, 50 )
  };
  const QgsFeatureId fid = seedFeature( layer, QgsGeometry::fromPolygonXY( { pinchPts } ) );

  layer.startEditing();
  layer.selectByIds( { fid } );
  canvas.setLayers( QList<QgsMapLayer *>{ &layer } );
  canvas.setCurrentLayer( &layer );
  canvas.refresh();

  TestVertexTool tool( &canvas, &layer );
  tool.setTopologicalEditingEnabled( true );
  canvas.setMapTool( &tool );
  QSignalSpy editedSpy( &tool, &PaleoVertexTool::featureEdited );
  QSignalSpy msgSpy( &tool, &QgsMapTool::messageEmitted );

  QCOMPARE( vertexTotal( layer, fid ), 5 );

  // Right-click the pinch point at (50, 50):
  // Both vertex 1 and 3 are coincident. Quota check must refuse because 5 < 4 + 2 = 6!
  click( tool, canvas, QPoint( 50, 50 ), Qt::RightButton );

  QCOMPARE( editedSpy.count(), 0 ); // Refused!
  QVERIFY( msgSpy.count() >= 1 );   // Guard warning emitted
  QCOMPARE( vertexTotal( layer, fid ), 5 ); // Invariant: not collapsed to 3!
  QCOMPARE( layer.undoStack()->count(), 0 );

  // Now verify that a 6-vertex polygon with coincident pinch point CAN be deleted down to 4 vertices (valid triangle):
  // (10,10) -> (50,50) -> (90,50) -> (10,90) -> (50,50) [auto-closure appends (10,10)]
  canvas.unsetMapTool( &tool );
  layer.rollBack();

  QgsVectorLayer layer6( QStringLiteral( "Polygon?crs=EPSG:4326&field=id:integer" ), QStringLiteral( "pinch6" ), QStringLiteral( "memory" ) );
  QVERIFY2( layer6.isValid(), "memory polygon layer failed to initialize" );
  const QVector<QgsPointXY> validPts = {
    mapPt( canvas, 10, 10 ),
    mapPt( canvas, 50, 50 ),
    mapPt( canvas, 90, 50 ),
    mapPt( canvas, 10, 90 ),
    mapPt( canvas, 50, 50 )
  };
  const QgsFeatureId fid6 = seedFeature( layer6, QgsGeometry::fromPolygonXY( { validPts } ) );
  layer6.startEditing();
  layer6.selectByIds( { fid6 } );
  canvas.setLayers( QList<QgsMapLayer *>{ &layer6 } );
  canvas.setCurrentLayer( &layer6 );
  canvas.refresh();

  TestVertexTool tool6( &canvas, &layer6 );
  tool6.setTopologicalEditingEnabled( true );
  canvas.setMapTool( &tool6 );
  QSignalSpy editedSpy6( &tool6, &PaleoVertexTool::featureEdited );

  QCOMPARE( vertexTotal( layer6, fid6 ), 6 );
  click( tool6, canvas, QPoint( 50, 50 ), Qt::RightButton );
  QCOMPARE( editedSpy6.count(), 1 ); // Accepted!
  QCOMPARE( vertexTotal( layer6, fid6 ), 4 ); // 6 - 2 = 4 (valid triangle)

  const QgsGeometry g6 = layer6.getFeature( fid6 ).geometry();
  QVERIFY( g6.isGeosValid() );

  canvas.unsetMapTool( &tool6 );
  layer6.rollBack();
}

void TestEditTools::vertexMoveAndDeletePolygonHoleInvariants()
{
  QgsMapCanvas canvas;
  configureCanvas( canvas );

  QgsVectorLayer layer( QStringLiteral( "Polygon?crs=EPSG:4326&field=id:integer" ), QStringLiteral( "hole-invariants" ), QStringLiteral( "memory" ) );
  QVERIFY2( layer.isValid(), "memory polygon layer failed to initialize" );

  const QString holeWkt = QStringLiteral( "Polygon ((10 10, 50 10, 50 50, 10 50, 10 10), (20 20, 35 20, 35 35, 20 35, 20 20))" );
  const QgsFeatureId fid = seedFeature( layer, QgsGeometry::fromWkt( holeWkt ) );

  layer.startEditing();
  layer.selectByIds( { fid } );
  canvas.setLayers( QList<QgsMapLayer *>{ &layer } );
  canvas.setCurrentLayer( &layer );
  canvas.refresh();

  TestVertexTool tool( &canvas, &layer );
  tool.setTopologicalEditingEnabled( false );
  canvas.setMapTool( &tool );
  QSignalSpy editedSpy( &tool, &PaleoVertexTool::featureEdited );
  QSignalSpy msgSpy( &tool, &QgsMapTool::messageEmitted );

  QCOMPARE( vertexTotal( layer, fid ), 10 );
  QCOMPARE( tool.markerCount(), 10 );

  // 1. Move closure vertex of hole at (20, 20) to (22, 22)
  const QPoint startPx = pxAt( canvas, 20, 20 );
  const QPoint targetPx = pxAt( canvas, 22, 22 );
  const QgsPointXY pNew = mapPt( canvas, targetPx.x(), targetPx.y() );

  QgsMapMouseEvent press( &canvas, QEvent::MouseButtonPress, startPx,
                          Qt::LeftButton, Qt::LeftButton, Qt::NoModifier );
  tool.canvasPressEvent( &press );
  QVERIFY( tool.isDragging() );

  QgsMapMouseEvent release( &canvas, QEvent::MouseButtonRelease, targetPx,
                            Qt::LeftButton, Qt::NoButton, Qt::NoModifier );
  tool.canvasReleaseEvent( &release );

  QCOMPARE( editedSpy.count(), 1 );
  const QgsGeometry mutated = layer.getFeature( fid ).geometry();
  QVERIFY2( mutated.isGeosValid(), "polygon with moved hole vertex must remain GEOS valid" );
  const QgsPolygon *poly = asPolygon( mutated );
  QVERIFY( poly && poly->numInteriorRings() == 1 );
  const QgsLineString *holeRing = qgsgeometry_cast<const QgsLineString *>( poly->interiorRing( 0 ) );
  QVERIFY2( holeRing && holeRing->isClosed(), "hole ring must remain closed" );
  QCOMPARE( holeRing->numPoints(), 5 );
  QVERIFY( qgsDoubleNear( holeRing->xAt( 0 ), pNew.x(), 1e-5 ) );
  QVERIFY( qgsDoubleNear( holeRing->yAt( 0 ), pNew.y(), 1e-5 ) );
  QVERIFY( qgsDoubleNear( holeRing->xAt( 4 ), pNew.x(), 1e-5 ) );
  QVERIFY( qgsDoubleNear( holeRing->yAt( 4 ), pNew.y(), 1e-5 ) );

  // 2. Delete closure vertex on the hole (5 -> 4 vertices on hole, 10 -> 9 total)
  click( tool, canvas, targetPx, Qt::RightButton );
  QCOMPARE( editedSpy.count(), 2 );
  QCOMPARE( vertexTotal( layer, fid ), 9 );
  const QgsGeometry delGeom = layer.getFeature( fid ).geometry();
  QVERIFY2( delGeom.isGeosValid(), "polygon with triangle hole must remain GEOS valid" );
  const QgsPolygon *delPoly = asPolygon( delGeom );
  const QgsLineString *delHole = qgsgeometry_cast<const QgsLineString *>( delPoly->interiorRing( 0 ) );
  QVERIFY( delHole->isClosed() );
  QCOMPARE( delHole->numPoints(), 4 ); // 3 vertices + closure point
  QVERIFY( qgsDoubleNear( delHole->xAt( 0 ), delHole->xAt( 3 ), 1e-6 ) );
  QVERIFY( qgsDoubleNear( delHole->yAt( 0 ), delHole->yAt( 3 ), 1e-6 ) );

  // 3. Attempting to delete a vertex from the 4-vertex hole must be rejected!
  const QPoint holePtPx = pxAt( canvas, delHole->xAt( 1 ), delHole->yAt( 1 ) );
  click( tool, canvas, holePtPx, Qt::RightButton );
  QCOMPARE( editedSpy.count(), 2 ); // Still 2
  QCOMPARE( msgSpy.count(), 1 );    // Warning
  QCOMPARE( vertexTotal( layer, fid ), 9 ); // Unchanged

  // 4. Undo restores back to 9 then 10 vertices
  layer.undoStack()->undo();
  QCOMPARE( vertexTotal( layer, fid ), 10 );
  layer.undoStack()->undo();
  QCOMPARE( vertexTotal( layer, fid ), 10 );
  const QgsGeometry restored = layer.getFeature( fid ).geometry();
  const QgsLineString *origHole = qgsgeometry_cast<const QgsLineString *>( asPolygon( restored )->interiorRing( 0 ) );
  QVERIFY( qgsDoubleNear( origHole->xAt( 0 ), 20.0, 1e-5 ) );

  canvas.unsetMapTool( &tool );
  layer.rollBack();
}

void TestEditTools::vertexTopoSharedClosureMultiFeatureInvariants()
{
  QgsMapCanvas canvas;
  configureCanvas( canvas );

  QgsVectorLayer layer( QStringLiteral( "Polygon?crs=EPSG:4326&field=id:integer" ), QStringLiteral( "topo-shared-closure" ), QStringLiteral( "memory" ) );
  QVERIFY2( layer.isValid(), "memory polygon layer failed to initialize" );

  // Feature A: Quad (20,20)-(40,40) -> closure at (20,20)
  const QgsFeatureId fidA = seedFeature( layer, QgsGeometry::fromWkt( squareWkt( 20, 20, 20 ) ) );
  // Feature B: Adjacent Quad (20,20)-(0,40) -> closure at (20,20)
  const QString wktB = QStringLiteral( "Polygon ((20 20, 20 40, 0 40, 0 20, 20 20))" );
  const QgsFeatureId fidB = seedFeature( layer, QgsGeometry::fromWkt( wktB ) );

  layer.startEditing();
  layer.selectByIds( { fidA } );
  canvas.setLayers( QList<QgsMapLayer *>{ &layer } );
  canvas.setCurrentLayer( &layer );
  canvas.refresh();

  TestVertexTool tool( &canvas, &layer );
  tool.setTopologicalEditingEnabled( true );
  canvas.setMapTool( &tool );
  QSignalSpy editedSpy( &tool, &PaleoVertexTool::featureEdited );

  QCOMPARE( vertexTotal( layer, fidA ), 5 );
  QCOMPARE( vertexTotal( layer, fidB ), 5 );

  // Move shared closure vertex (20, 20) to (18, 18)
  const QPoint startPx = pxAt( canvas, 20, 20 );
  const QPoint targetPx = pxAt( canvas, 18, 18 );
  const QgsPointXY pNew = mapPt( canvas, targetPx.x(), targetPx.y() );

  QgsMapMouseEvent press( &canvas, QEvent::MouseButtonPress, startPx,
                          Qt::LeftButton, Qt::LeftButton, Qt::NoModifier );
  tool.canvasPressEvent( &press );
  QVERIFY( tool.isDragging() );

  QgsMapMouseEvent release( &canvas, QEvent::MouseButtonRelease, targetPx,
                            Qt::LeftButton, Qt::NoButton, Qt::NoModifier );
  tool.canvasReleaseEvent( &release );

  QCOMPARE( editedSpy.count(), 1 );
  QCOMPARE( layer.undoStack()->count(), 1 );

  const QgsGeometry geomA = layer.getFeature( fidA ).geometry();
  const QgsGeometry geomB = layer.getFeature( fidB ).geometry();
  QVERIFY2( geomA.isGeosValid(), "Feature A must remain GEOS valid after topo move" );
  QVERIFY2( geomB.isGeosValid(), "Feature B must remain GEOS valid after topo move" );

  const QgsLineString *ringA = qgsgeometry_cast<const QgsLineString *>( asPolygon( geomA )->exteriorRing() );
  const QgsLineString *ringB = qgsgeometry_cast<const QgsLineString *>( asPolygon( geomB )->exteriorRing() );
  QVERIFY( ringA->isClosed() );
  QVERIFY( ringB->isClosed() );
  QVERIFY( qgsDoubleNear( ringA->xAt( 0 ), pNew.x(), 1e-5 ) );
  QVERIFY( qgsDoubleNear( ringA->xAt( 4 ), pNew.x(), 1e-5 ) );
  QVERIFY( qgsDoubleNear( ringB->xAt( 0 ), pNew.x(), 1e-5 ) );
  QVERIFY( qgsDoubleNear( ringB->xAt( 4 ), pNew.x(), 1e-5 ) );

  // Delete shared closure vertex at (18, 18) with topo editing enabled
  click( tool, canvas, targetPx, Qt::RightButton );

  QCOMPARE( editedSpy.count(), 2 );
  QCOMPARE( layer.undoStack()->count(), 2 );
  // Both features must have decremented by exactly 1 vertex (5 -> 4), NOT double deleted to 3
  QCOMPARE( vertexTotal( layer, fidA ), 4 );
  QCOMPARE( vertexTotal( layer, fidB ), 4 );

  const QgsGeometry delGeomA = layer.getFeature( fidA ).geometry();
  const QgsGeometry delGeomB = layer.getFeature( fidB ).geometry();
  QVERIFY( delGeomA.isGeosValid() );
  QVERIFY( delGeomB.isGeosValid() );
  const QgsLineString *delRingA = qgsgeometry_cast<const QgsLineString *>( asPolygon( delGeomA )->exteriorRing() );
  const QgsLineString *delRingB = qgsgeometry_cast<const QgsLineString *>( asPolygon( delGeomB )->exteriorRing() );
  QVERIFY( delRingA->isClosed() );
  QVERIFY( delRingB->isClosed() );

  // Undo delete: restores both to 5 vertices
  layer.undoStack()->undo();
  QCOMPARE( vertexTotal( layer, fidA ), 5 );
  QCOMPARE( vertexTotal( layer, fidB ), 5 );

  // Redo delete: restores both to 4 vertices
  layer.undoStack()->redo();
  QCOMPARE( vertexTotal( layer, fidA ), 4 );
  QCOMPARE( vertexTotal( layer, fidB ), 4 );

  canvas.unsetMapTool( &tool );
  layer.rollBack();
}

void TestEditTools::vertexTopoDeleteCoincidentPinchPointRefused()
{
  QgsMapCanvas canvas;
  configureCanvas( canvas );

  QgsVectorLayer layer( QStringLiteral( "Polygon?crs=EPSG:4326&field=id:integer" ),
                        QStringLiteral( "topo-pinch-refuse" ), QStringLiteral( "memory" ) );
  QVERIFY2( layer.isValid(), "memory polygon layer failed to initialize" );

  // Construct a polygon with 5 vertices (including closure) having a coincident pinch point:
  // V0: (20, 20), V1: (50, 50), V2: (50, 80), V3: (50, 50) [coincident with V1], V4: (20, 20) [closure]
  const QString pinchWkt = QStringLiteral( "Polygon ((20 20, 50 50, 50 80, 50 50, 20 20))" );
  const QgsFeatureId fid = seedFeature( layer, QgsGeometry::fromWkt( pinchWkt ) );

  layer.startEditing();
  layer.selectByIds( { fid } );
  canvas.setLayers( QList<QgsMapLayer *>{ &layer } );
  canvas.setCurrentLayer( &layer );
  canvas.refresh();

  TestVertexTool tool( &canvas, &layer );
  tool.setTopologicalEditingEnabled( true );
  canvas.setMapTool( &tool );

  QSignalSpy editedSpy( &tool, &PaleoVertexTool::featureEdited );
  QSignalSpy msgSpy( &tool, &QgsMapTool::messageEmitted );

  QCOMPARE( vertexTotal( layer, fid ), 5 );

  // Right-click at the coincident pinch-point coordinate (50, 50).
  // In topological mode, both vertex 1 and vertex 3 match the coordinate.
  // Deleting both would drop ring count to 5 - 2 = 3 vertices (2 distinct points),
  // which collapses below the polygon minimum (>= 4 vertices).
  // The operation MUST defend the ring and reject the deletion cleanly.
  click( tool, canvas, pxAt( canvas, 50, 50 ), Qt::RightButton );

  // Verify deletion is refused:
  QCOMPARE( editedSpy.count(), 0 );           // No commit
  QVERIFY( msgSpy.count() >= 1 );              // Warning emitted
  QCOMPARE( tool.editedCount(), 0 );
  QCOMPARE( layer.undoStack()->count(), 0 );   // Undo stack untouched
  QCOMPARE( vertexTotal( layer, fid ), 5 );    // Geometry untouched (5 vertices preserved)

  // Verify geometry remains untouched
  const QgsGeometry g = layer.getFeature( fid ).geometry();
  QVERIFY( g.constGet() != nullptr );
  QCOMPARE( g.constGet()->vertexCount( 0, 0 ), 5 );

  canvas.unsetMapTool( &tool );
  layer.rollBack();
}

void TestEditTools::vertexTopoDeleteCoincidentPinchPointAllowedOnHexagon()
{
  QgsMapCanvas canvas;
  configureCanvas( canvas );

  QgsVectorLayer layer( QStringLiteral( "Polygon?crs=EPSG:4326&field=id:integer" ),
                        QStringLiteral( "topo-pinch-allow" ), QStringLiteral( "memory" ) );
  QVERIFY2( layer.isValid(), "memory polygon layer failed to initialize" );

  // 6 vertices: V0: (20,20), V1: (50,50), V2: (50,80), V3: (50,50), V4: (20,80), V5: (20,20)
  // Deleting coincident V1 and V3 leaves 6 - 2 = 4 vertices (triangle: (20,20), (50,80), (20,80), (20,20)),
  // which is a valid polygon!
  const QString pinchWkt = QStringLiteral( "Polygon ((20 20, 50 50, 50 80, 50 50, 20 80, 20 20))" );
  const QgsFeatureId fid = seedFeature( layer, QgsGeometry::fromWkt( pinchWkt ) );

  layer.startEditing();
  layer.selectByIds( { fid } );
  canvas.setLayers( QList<QgsMapLayer *>{ &layer } );
  canvas.setCurrentLayer( &layer );
  canvas.refresh();

  TestVertexTool tool( &canvas, &layer );
  tool.setTopologicalEditingEnabled( true );
  canvas.setMapTool( &tool );

  QSignalSpy editedSpy( &tool, &PaleoVertexTool::featureEdited );
  QSignalSpy msgSpy( &tool, &QgsMapTool::messageEmitted );

  QCOMPARE( vertexTotal( layer, fid ), 6 );

  click( tool, canvas, pxAt( canvas, 50, 50 ), Qt::RightButton );

  // Verify deletion succeeds and yields a valid triangle:
  QCOMPARE( editedSpy.count(), 1 );
  QCOMPARE( msgSpy.count(), 0 );
  QCOMPARE( vertexTotal( layer, fid ), 4 );
  const QgsGeometry g = layer.getFeature( fid ).geometry();
  QVERIFY( g.isGeosValid() );

  canvas.unsetMapTool( &tool );
  layer.rollBack();
}

void TestEditTools::vertexInFlightDragAbortedByExternalUndo()
{
  QgsMapCanvas canvas;
  configureCanvas( canvas );

  QgsVectorLayer layer( QStringLiteral( "LineString?crs=EPSG:4326&field=id:integer" ), QStringLiteral( "inflight-undo" ), QStringLiteral( "memory" ) );
  QVERIFY2( layer.isValid(), "memory line layer failed to initialize" );

  const QgsPointXY p0 = mapPt( canvas, 40, 140 );
  const QgsPointXY p1 = mapPt( canvas, 100, 100 );
  const QgsPointXY p2 = mapPt( canvas, 160, 60 );
  const QgsFeatureId fid = seedFeature( layer, QgsGeometry::fromPolylineXY( { p0, p1, p2 } ) );

  layer.startEditing();
  layer.selectByIds( { fid } );
  canvas.setLayers( QList<QgsMapLayer *>{ &layer } );
  canvas.setCurrentLayer( &layer );
  canvas.refresh();

  TestVertexTool tool( &canvas, &layer );
  canvas.setMapTool( &tool );

  // 1. Move vertex 2 to push 1 command on the undo stack
  const QPoint v2Start = pxAt( canvas, p2.x(), p2.y() );
  const QPoint v2Target = pxAt( canvas, 180, 80 );
  QgsMapMouseEvent p( &canvas, QEvent::MouseButtonPress, v2Start, Qt::LeftButton, Qt::LeftButton, Qt::NoModifier );
  tool.canvasPressEvent( &p );
  QgsMapMouseEvent r( &canvas, QEvent::MouseButtonRelease, v2Target, Qt::LeftButton, Qt::NoButton, Qt::NoModifier );
  tool.canvasReleaseEvent( &r );
  QCOMPARE( layer.undoStack()->count(), 1 );

  // 2. Start an in-flight drag on vertex 0
  const QPoint v0Start = pxAt( canvas, p0.x(), p0.y() );
  const QPoint v0Move = pxAt( canvas, 60, 120 );
  QgsMapMouseEvent press( &canvas, QEvent::MouseButtonPress, v0Start, Qt::LeftButton, Qt::LeftButton, Qt::NoModifier );
  tool.canvasPressEvent( &press );
  QVERIFY2( tool.isDragging(), "tool must be in dragging state" );

  QgsMapMouseEvent move( &canvas, QEvent::MouseMove, v0Move, Qt::NoButton, Qt::LeftButton, Qt::NoModifier );
  tool.canvasMoveEvent( &move );
  QVERIFY( tool.isDragging() );

  // 3. Trigger external undo while drag is in flight
  layer.undoStack()->undo();

  // Invariant: drag state must be cleared immediately, no crash, markers refreshed
  QVERIFY2( !tool.isDragging(), "in-flight drag must be aborted upon external undo" );
  QCOMPARE( tool.markerCount(), 3 );

  canvas.unsetMapTool( &tool );
  layer.rollBack();
}

void TestEditTools::vertexAdversarialTopoMarkersDynamicTracking()
{
  QgsMapCanvas canvas;
  configureCanvas( canvas );

  QgsVectorLayer layer( QStringLiteral( "LineString?crs=EPSG:4326&field=id:integer" ), QStringLiteral( "topo-stress" ), QStringLiteral( "memory" ) );
  QVERIFY2( layer.isValid(), "memory line layer failed to initialize" );

  const QgsPointXY shared = mapPt( canvas, 100, 100 );
  // Feature A (selected)
  const QgsFeatureId fidA = seedFeature( layer, QgsGeometry::fromPolylineXY(
      { mapPt( canvas, 40, 140 ), shared, mapPt( canvas, 160, 60 ) } ) );
  // Feature B (unselected)
  const QgsFeatureId fidB = seedFeature( layer, QgsGeometry::fromPolylineXY(
      { shared, mapPt( canvas, 180, 140 ) } ) );
  // Feature C (unselected)
  const QgsFeatureId fidC = seedFeature( layer, QgsGeometry::fromPolylineXY(
      { mapPt( canvas, 20, 20 ), shared } ) );
  // Feature D (selected) also coincident at shared
  const QgsFeatureId fidD = seedFeature( layer, QgsGeometry::fromPolylineXY(
      { shared, mapPt( canvas, 100, 180 ) } ) );

  layer.startEditing();
  layer.selectByIds( { fidA, fidD } ); // A and D are selected; B and C are unselected
  canvas.setLayers( QList<QgsMapLayer *>{ &layer } );
  canvas.setCurrentLayer( &layer );
  canvas.refresh();

  TestVertexTool tool( &canvas, &layer );
  tool.setTopologicalEditingEnabled( true );
  canvas.setMapTool( &tool );

  // fidA has 3 vertices, fidD has 2 vertices -> selected features have 5 vertices total
  QCOMPARE( tool.markerCount(), 5 );

  // Press on shared vertex (100, 100)
  const QPoint pressPx( 100, 100 );
  QgsMapMouseEvent press( &canvas, QEvent::MouseButtonPress, pressPx,
                          Qt::LeftButton, Qt::LeftButton, Qt::NoModifier );
  tool.canvasPressEvent( &press );
  QVERIFY2( tool.isDragging(), "drag must be armed on shared junction" );

  // fidB and fidC are unselected, so topoMarkers must contain exactly 2 markers
  QCOMPARE( tool.topoMarkers().size(), 2 );

  // Step 1: Drag to (120, 110)
  const QPoint p1( 120, 110 );
  const QgsPointXY mp1 = mapPt( canvas, 120, 110 );
  QgsMapMouseEvent move1( &canvas, QEvent::MouseMove, p1,
                          Qt::NoButton, Qt::LeftButton, Qt::NoModifier );
  tool.canvasMoveEvent( &move1 );

  for ( QgsVertexMarker *tm : tool.topoMarkers() )
  {
    QVERIFY( tm != nullptr );
    QVERIFY( qgsDoubleNear( tm->center().x(), mp1.x(), 1e-4 ) );
    QVERIFY( qgsDoubleNear( tm->center().y(), mp1.y(), 1e-4 ) );
  }

  // Step 2: Drag to (80, 50)
  const QPoint p2( 80, 50 );
  const QgsPointXY mp2 = mapPt( canvas, 80, 50 );
  QgsMapMouseEvent move2( &canvas, QEvent::MouseMove, p2,
                          Qt::NoButton, Qt::LeftButton, Qt::NoModifier );
  tool.canvasMoveEvent( &move2 );

  for ( QgsVertexMarker *tm : tool.topoMarkers() )
  {
    QVERIFY( tm != nullptr );
    QVERIFY( qgsDoubleNear( tm->center().x(), mp2.x(), 1e-4 ) );
    QVERIFY( qgsDoubleNear( tm->center().y(), mp2.y(), 1e-4 ) );
  }

  // Step 3: Drag to (150, 150) and release to commit
  const QPoint p3( 150, 150 );
  const QgsPointXY mp3 = mapPt( canvas, 150, 150 );
  QgsMapMouseEvent release( &canvas, QEvent::MouseButtonRelease, p3,
                            Qt::LeftButton, Qt::NoButton, Qt::NoModifier );
  tool.canvasReleaseEvent( &release );

  QVERIFY( !tool.isDragging() );
  QVERIFY( tool.topoMarkers().isEmpty() );
  QCOMPARE( tool.editedCount(), 1 );

  // All 4 features must have moved their shared vertex to mp3
  const QgsLineString *lsA = asLineString( layer.getFeature( fidA ).geometry() );
  const QgsLineString *lsB = asLineString( layer.getFeature( fidB ).geometry() );
  const QgsLineString *lsC = asLineString( layer.getFeature( fidC ).geometry() );
  const QgsLineString *lsD = asLineString( layer.getFeature( fidD ).geometry() );
  QVERIFY( qgsDoubleNear( lsA->xAt( 1 ), mp3.x(), 1e-4 ) && qgsDoubleNear( lsA->yAt( 1 ), mp3.y(), 1e-4 ) );
  QVERIFY( qgsDoubleNear( lsB->xAt( 0 ), mp3.x(), 1e-4 ) && qgsDoubleNear( lsB->yAt( 0 ), mp3.y(), 1e-4 ) );
  QVERIFY( qgsDoubleNear( lsC->xAt( 1 ), mp3.x(), 1e-4 ) && qgsDoubleNear( lsC->yAt( 1 ), mp3.y(), 1e-4 ) );
  QVERIFY( qgsDoubleNear( lsD->xAt( 0 ), mp3.x(), 1e-4 ) && qgsDoubleNear( lsD->yAt( 0 ), mp3.y(), 1e-4 ) );

  canvas.unsetMapTool( &tool );
  layer.rollBack();
}

void TestEditTools::vertexAdversarialRebuildOnRollbackSync()
{
  QgsMapCanvas canvas;
  configureCanvas( canvas );

  QgsVectorLayer layer( QStringLiteral( "Polygon?crs=EPSG:4326&field=id:integer" ), QStringLiteral( "undo-redo-rollback" ), QStringLiteral( "memory" ) );
  QVERIFY2( layer.isValid(), "memory polygon layer failed to initialize" );

  const QgsFeatureId fid = seedFeature( layer, QgsGeometry::fromWkt( squareWkt( 10, 10, 20 ) ) );
  layer.startEditing();
  layer.selectByIds( { fid } );
  canvas.setLayers( QList<QgsMapLayer *>{ &layer } );
  canvas.setCurrentLayer( &layer );
  canvas.refresh();

  TestVertexTool tool( &canvas, &layer );
  canvas.setMapTool( &tool );

  // 5 vertices for a closed square: (10,10), (30,10), (30,30), (10,30), (10,10)
  QCOMPARE( tool.markerCount(), 5 );
  const QgsPointXY origPt2 = tool.markers().at( 2 )->center();

  // Move vertex 2 from (30, 30) to (35, 35)
  const QPoint startPx = pxAt( canvas, 30, 30 );
  const QPoint targetPx = pxAt( canvas, 35, 35 );
  const QgsPointXY movedPt2 = mapPt( canvas, targetPx.x(), targetPx.y() );

  QgsMapMouseEvent press( &canvas, QEvent::MouseButtonPress, startPx,
                          Qt::LeftButton, Qt::LeftButton, Qt::NoModifier );
  tool.canvasPressEvent( &press );
  QVERIFY( tool.isDragging() );

  QgsMapMouseEvent release( &canvas, QEvent::MouseButtonRelease, targetPx,
                            Qt::LeftButton, Qt::NoButton, Qt::NoModifier );
  tool.canvasReleaseEvent( &release );
  QCOMPARE( tool.editedCount(), 1 );
  QCOMPARE( tool.markerCount(), 5 );
  QVERIFY( qgsDoubleNear( tool.markers().at( 2 )->center().x(), movedPt2.x(), 1e-4 ) );
  QVERIFY( qgsDoubleNear( tool.markers().at( 2 )->center().y(), movedPt2.y(), 1e-4 ) );

  // 1. Undo: verify rebuildMarkers() restored marker 2 back to origPt2
  layer.undoStack()->undo();
  QCOMPARE( tool.markerCount(), 5 );
  QVERIFY( qgsDoubleNear( tool.markers().at( 2 )->center().x(), origPt2.x(), 1e-4 ) );
  QVERIFY( qgsDoubleNear( tool.markers().at( 2 )->center().y(), origPt2.y(), 1e-4 ) );

  // 2. Redo: verify rebuildMarkers() updated marker 2 back to movedPt2
  layer.undoStack()->redo();
  QCOMPARE( tool.markerCount(), 5 );
  QVERIFY( qgsDoubleNear( tool.markers().at( 2 )->center().x(), movedPt2.x(), 1e-4 ) );
  QVERIFY( qgsDoubleNear( tool.markers().at( 2 )->center().y(), movedPt2.y(), 1e-4 ) );

  // 3. Rollback: rollBack() rolls back all edits in the edit buffer
  layer.rollBack();
  // afterRollBack signal must trigger rebuildMarkers()
  QCOMPARE( tool.markerCount(), 5 );
  QVERIFY( qgsDoubleNear( tool.markers().at( 2 )->center().x(), origPt2.x(), 1e-4 ) );
  QVERIFY( qgsDoubleNear( tool.markers().at( 2 )->center().y(), origPt2.y(), 1e-4 ) );

  canvas.unsetMapTool( &tool );
}

void TestEditTools::vertexAdversarialInFlightDragAbortedByRollback()
{
  QgsMapCanvas canvas;
  configureCanvas( canvas );

  QgsVectorLayer layer( QStringLiteral( "LineString?crs=EPSG:4326&field=id:integer" ), QStringLiteral( "abort-drag-rollback" ), QStringLiteral( "memory" ) );
  QVERIFY2( layer.isValid(), "memory line layer failed to initialize" );

  const QgsPointXY pt0 = mapPt( canvas, 50, 50 );
  const QgsPointXY pt1 = mapPt( canvas, 150, 150 );
  const QgsFeatureId fid = seedFeature( layer, QgsGeometry::fromPolylineXY( { pt0, pt1 } ) );

  layer.startEditing();
  layer.selectByIds( { fid } );
  canvas.setLayers( QList<QgsMapLayer *>{ &layer } );
  canvas.setCurrentLayer( &layer );
  canvas.refresh();

  TestVertexTool tool( &canvas, &layer );
  canvas.setMapTool( &tool );

  // Press to arm drag
  const QPoint pressPx( 50, 50 );
  QgsMapMouseEvent press( &canvas, QEvent::MouseButtonPress, pressPx,
                          Qt::LeftButton, Qt::LeftButton, Qt::NoModifier );
  tool.canvasPressEvent( &press );
  QVERIFY2( tool.isDragging(), "drag must be active after press" );

  // Trigger external rollback while drag gesture is in-flight!
  layer.rollBack();

  // The tool must automatically detect rollback, clear drag state, and rebuild markers
  QVERIFY2( !tool.isDragging(), "in-flight drag must be aborted on layer rollback" );
  QVERIFY( tool.topoMarkers().isEmpty() );
  QCOMPARE( tool.markerCount(), 2 );

  // Subsequent move and release must not crash and must not commit anything
  const QPoint movePx( 80, 80 );
  QgsMapMouseEvent move( &canvas, QEvent::MouseMove, movePx,
                         Qt::NoButton, Qt::LeftButton, Qt::NoModifier );
  tool.canvasMoveEvent( &move );

  const QPoint releasePx( 90, 90 );
  QgsMapMouseEvent release( &canvas, QEvent::MouseButtonRelease, releasePx,
                            Qt::LeftButton, Qt::NoButton, Qt::NoModifier );
  tool.canvasReleaseEvent( &release );
  QCOMPARE( tool.editedCount(), 0 );

  // Idle Esc must emit editAborted
  QSignalSpy abortSpy( &tool, &PaleoVertexTool::editAborted );
  sendEsc( tool );
  QCOMPARE( abortSpy.count(), 1 );

  canvas.unsetMapTool( &tool );
}

void TestEditTools::vertexAdversarialLayerDestructionArmedToolSafety()
{
  QgsMapCanvas canvas;
  configureCanvas( canvas );

  auto *dynLayer = new QgsVectorLayer( QStringLiteral( "Point?crs=EPSG:4326&field=id:integer" ),
                                       QStringLiteral( "dyn-layer" ), QStringLiteral( "memory" ) );
  QVERIFY2( dynLayer->isValid(), "dynLayer failed to initialize" );
  const QgsFeatureId fid = seedFeature( *dynLayer, QgsGeometry::fromPointXY( mapPt( canvas, 100, 100 ) ) );

  dynLayer->startEditing();
  dynLayer->selectByIds( { fid } );
  canvas.setLayers( QList<QgsMapLayer *>{ dynLayer } );
  canvas.setCurrentLayer( dynLayer );
  canvas.refresh();

  TestVertexTool tool( &canvas, dynLayer );
  canvas.setMapTool( &tool );
  QCOMPARE( tool.markerCount(), 1 );

  // Arm drag on the point
  const QPoint pressPx( 100, 100 );
  QgsMapMouseEvent press( &canvas, QEvent::MouseButtonPress, pressPx,
                          Qt::LeftButton, Qt::LeftButton, Qt::NoModifier );
  tool.canvasPressEvent( &press );
  QVERIFY( tool.isDragging() );

  // Delete the layer while tool is armed!
  delete dynLayer;

  // mLayer->destroyed must trigger teardown in tool
  QVERIFY( tool.targetLayer() == nullptr );
  QVERIFY( !tool.isDragging() );
  QCOMPARE( tool.markerCount(), 0 );
  QVERIFY( tool.topoMarkers().isEmpty() );

  // Injected canvas events must be handled safely without null dereference
  const QPoint anyPx( 50, 50 );
  QgsMapMouseEvent move( &canvas, QEvent::MouseMove, anyPx,
                         Qt::NoButton, Qt::LeftButton, Qt::NoModifier );
  tool.canvasMoveEvent( &move );

  QgsMapMouseEvent release( &canvas, QEvent::MouseButtonRelease, anyPx,
                            Qt::LeftButton, Qt::NoButton, Qt::NoModifier );
  tool.canvasReleaseEvent( &release );

  QgsMapMouseEvent dclick( &canvas, QEvent::MouseButtonDblClick, anyPx,
                           Qt::LeftButton, Qt::LeftButton, Qt::NoModifier );
  tool.canvasDoubleClickEvent( &dclick );

  sendEsc( tool );

  canvas.unsetMapTool( &tool );
  canvas.setLayers( {} );
}

void TestEditTools::vertexAdversarialToolDestructionCanvasSafety()
{
  QgsMapCanvas canvas;
  configureCanvas( canvas );

  QgsVectorLayer layer( QStringLiteral( "Polygon?crs=EPSG:4326&field=id:integer" ),
                        QStringLiteral( "canvas-active-teardown" ), QStringLiteral( "memory" ) );
  QVERIFY2( layer.isValid(), "layer failed to initialize" );
  const QgsFeatureId fid = seedFeature( layer, QgsGeometry::fromWkt( squareWkt( 20, 20, 40 ) ) );

  layer.startEditing();
  layer.selectByIds( { fid } );
  canvas.setLayers( QList<QgsMapLayer *>{ &layer } );
  canvas.setCurrentLayer( &layer );
  canvas.refresh();

  const int initialItemCount = canvas.scene()->items().count();

  auto *dynTool = new TestVertexTool( &canvas, &layer );
  canvas.setMapTool( dynTool );

  // Markers must now be present on the canvas scene
  const int armedItemCount = canvas.scene()->items().count();
  QVERIFY2( armedItemCount > initialItemCount, "canvas scene must contain vertex markers" );

  // Press to start a drag (creates rubber band preview)
  const QPoint pressPx = pxAt( canvas, 20, 20 );
  QgsMapMouseEvent press( &canvas, QEvent::MouseButtonPress, pressPx,
                          Qt::LeftButton, Qt::LeftButton, Qt::NoModifier );
  dynTool->canvasPressEvent( &press );
  QVERIFY( dynTool->isDragging() );
  const int dragItemCount = canvas.scene()->items().count();
  QVERIFY2( dragItemCount > armedItemCount, "canvas scene must contain rubber band during drag" );

  // Destroy the tool while canvas is active and drag is in-flight!
  delete dynTool;

  // Verify all markers and rubber bands were removed from canvas scene
  const int finalItemCount = canvas.scene()->items().count();
  QCOMPARE( finalItemCount, initialItemCount );

  // Repaint canvas — must not crash or encounter dangling pointers
  canvas.refresh();
  canvas.update();

  layer.rollBack();
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

void TestEditTools::toolbarSelectionIsReadOnlyAndToolsFollowCanvas()
{
  QgsMapCanvas canvas;
  configureCanvas(canvas);
  QgsVectorLayer layer(QStringLiteral("Point?crs=EPSG:4326"), QStringLiteral("井位"), QStringLiteral("memory"));
  PaleoEditingToolbar bar(&canvas);
  bar.setLayers({&layer});
  QSignalSpy started(&bar, &PaleoEditingToolbar::editingStarted);
  bar.actionSelect()->trigger();
  QVERIFY(canvas.mapTool());
  QVERIFY(bar.actionSelect()->isChecked());
  QVERIFY(!layer.isEditable());
  QCOMPARE(started.count(), 0);
  QVERIFY(!bar.actionSave()->isEnabled());
  QgsMapToolPan pan(&canvas);
  canvas.setMapTool(&pan);
  QVERIFY(!bar.actionSelect()->isChecked());
  bar.actionSelect()->trigger(); // same command after navigation must re-arm
  QVERIFY(canvas.mapTool() != &pan);
  QVERIFY(bar.actionSelect()->isChecked());
  bar.actionAddPoint()->trigger();
  QVERIFY(layer.isEditable());
  QVERIFY(bar.actionAddFeature()->isChecked());
  canvas.setMapTool(&pan);
  QVERIFY(!bar.actionAddPoint()->isChecked());
  QVERIFY(!bar.actionAddFeature()->isChecked());
  bar.actionAddPoint()->trigger();
  QVERIFY(qobject_cast<PaleoAddFeatureTool *>(canvas.mapTool()));
  QVERIFY(bar.actionAddPoint()->isChecked());
  QCOMPARE(started.count(), 1); // navigation didn't end/restart the session
  QVERIFY(bar.cancelEditing());
}

void TestEditTools::toolbarGeometryAndReadOnlyGates()
{
  QgsMapCanvas canvas;
  QgsVectorLayer point(QStringLiteral("Point"), QStringLiteral("井"), QStringLiteral("memory"));
  QgsVectorLayer line(QStringLiteral("LineString"), QStringLiteral("约束线"), QStringLiteral("memory"));
  QgsVectorLayer polygon(QStringLiteral("Polygon"), QStringLiteral("相区"), QStringLiteral("memory"));
  PaleoEditingToolbar bar(&canvas);
  bar.setLayers({&point, &line, &polygon});
  bar.setCurrentLayer(&point);
  QVERIFY(bar.actionAddPoint()->isEnabled());
  QVERIFY(!bar.actionAddLine()->isEnabled());
  QVERIFY(!bar.actionAddPolygon()->isEnabled());
  QVERIFY(!bar.actionReshape()->isEnabled());
  QVERIFY(!bar.actionAddLine()->toolTip().isEmpty());
  bar.actionAddLine()->trigger();
  QVERIFY(!point.isEditable());
  bar.setCurrentLayer(&line);
  QVERIFY(bar.actionAddLine()->isEnabled());
  QVERIFY(bar.actionReshape()->isEnabled());
  QVERIFY(!bar.actionAddPoint()->isEnabled());
  bar.setCurrentLayer(&polygon);
  QVERIFY(bar.actionAddPolygon()->isEnabled());
  QVERIFY(polygon.setReadOnly());
  QVERIFY(bar.actionSelect()->isEnabled());
  QVERIFY(!bar.actionAddFeature()->isEnabled());
  QVERIFY(!bar.actionVertexEdit()->isEnabled());
  bar.actionSelect()->trigger();
  QVERIFY(!polygon.isEditable());
  bar.setLayerFilter([](const QgsVectorLayer *) { return false; });
  QVERIFY(!bar.actionSelect()->isEnabled());
  QVERIFY(!bar.actionMove()->isEnabled());
}

void TestEditTools::toolbarRemovedLayerClearsTarget()
{
  QgsMapCanvas canvas;
  auto *layer = new QgsVectorLayer(QStringLiteral("Point"), QStringLiteral("临时层"), QStringLiteral("memory"));
  PaleoEditingToolbar bar(&canvas);
  bar.setLayers({layer});
  bar.actionAddPoint()->trigger();
  QVERIFY(bar.isEditing());
  delete layer;
  QVERIFY(!bar.currentLayer());
  QVERIFY(!bar.isEditing());
  QVERIFY(!bar.actionSave()->isEnabled());
  QVERIFY(!bar.actionAddFeature()->isEnabled());
  QVERIFY(!canvas.mapTool());
}

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

// MEM-07 钉死：工具栏可后于画布析构。QgsMapTool 以画布为 QObject 父
// （级联删除会自动清 QPointer 的 mActiveEditTool），析构里对 mCanvas 的
// 访问必须全程守卫——画布先亡后删工具栏不得触碰已亡对象（QPointer 化
// 前是裸指针悬垂面；该路径兼验 destroyed 钩子的高亮释放）。
void TestEditTools::toolbarOutlivesCanvasDestruction()
{
  QgsVectorLayer layer( QStringLiteral( "Point?crs=EPSG:4326&field=id:integer" ), QStringLiteral( "canvas-first-death" ), QStringLiteral( "memory" ) );
  QVERIFY2( layer.isValid(), "memory layer failed to initialize" );

  auto *canvas = new QgsMapCanvas;
  configureCanvas( *canvas );
  auto *bar = new PaleoEditingToolbar( canvas );
  bar->setLayers( QList<QgsVectorLayer *>{ &layer } );
  bar->setCurrentLayer( &layer );

  bar->actionAddPoint()->trigger(); // 装上编辑工具并开启会话
  QVERIFY2( layer.isEditable(), "edit-tool action must auto-start the session" );
  QVERIFY2( canvas->mapTool() != nullptr, "edit-tool action must install a map tool" );

  delete canvas; // 画布先亡：工具级联析构 + destroyed 钩子触发
  delete bar;    // 工具栏后亡：析构走守卫路径（不得触碰已亡画布）

  layer.rollBack(); // 会话缓冲由宿主收尾——这里回滚防未提交编辑外溢
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

void TestEditTools::toolbarServiceEndsEachSessionOnce()
{
  QgsMapCanvas canvas;
  configureCanvas(canvas);
  QgsVectorLayer layer("Point?crs=EPSG:4326", "service-session", "memory");
  PaleoProjectStore store;
  QgisEditingService service(&store);
  PaleoEditingToolbar bar(&canvas);
  bar.setLayers({&layer});
  bar.setCurrentLayer(&layer);
  bar.setEditingService(&service);
  QSignalSpy stopped(&bar, &PaleoEditingToolbar::editingStopped);
  QVERIFY(bar.startEditing());
  QVERIFY(bar.saveEditing());
  QCOMPARE(stopped.count(), 1);
  QVERIFY(stopped.last()[1].toBool());
  QVERIFY(bar.startEditing());
  QVERIFY(bar.cancelEditing());
  QCOMPARE(stopped.count(), 2);
  QVERIFY(!stopped.last()[1].toBool());
  QVERIFY(bar.startEditing());
  QVERIFY(service.commitEdit(&layer)); // external service path still notifies
  QCOMPARE(stopped.count(), 3);
  QVERIFY(!bar.isEditing());
  QVERIFY(bar.startEditing());
  QVERIFY(service.rollbackEdit(&layer));
  QCOMPARE(stopped.count(), 4);
  QVERIFY(!bar.isEditing());
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

// ---------------------------------------------------------------------------
// mapping 主线3：编辑会话生命周期加固（工程边界收尾）
// ---------------------------------------------------------------------------

void TestEditTools::toolbarProjectSwitchFinalizesSession()
{
  QgsMapCanvas canvas;
  configureCanvas( canvas );

  QgsVectorLayer layer( QStringLiteral( "Point?crs=EPSG:4326&field=id:integer" ),
                        QStringLiteral( "switch-l" ), QStringLiteral( "memory" ) );
  QVERIFY( layer.isValid() );
  seedFeature( layer, QgsGeometry::fromPointXY( QgsPointXY( 7, 7 ) ) ); // 会话外种子（provider 面）

  QgsProject projectA;
  QgsProject projectB;

  PaleoEditingToolbar bar( &canvas );
  bar.setLayers( { &layer } );
  bar.setProject( &projectA );
  QSignalSpy stoppedSpy( &bar, &PaleoEditingToolbar::editingStopped );
  QSignalSpy refusedSpy( &bar, &PaleoEditingToolbar::editRefused );

  QVERIFY( bar.startEditing() );
  addPointCommand( &layer, 8 ); // 会话内编辑
  QCOMPARE( layer.undoStack()->count(), 1 );

  // 切工程：会话在旧工程侧收尾（提交优先），不留给图层析构。
  bar.setProject( &projectB );
  QCOMPARE( stoppedSpy.count(), 1 );
  QCOMPARE( stoppedSpy.at( 0 ).at( 1 ).toBool(), true ); // committed
  QVERIFY( refusedSpy.count() >= 1 );
  QVERIFY( refusedSpy.at( refusedSpy.count() - 1 ).at( 0 ).toString().contains( QStringLiteral( "切换工程" ) ) );
  QVERIFY( !bar.isEditing() );
  QCOMPARE( layer.featureCount(), 2 ); // 提交生效
  QCOMPARE( layer.undoStack()->count(), 0 ); // commit 清栈

  // 收尾后无会话：再次 setProject 幂等、不再发 editingStopped。
  const int stoppedBefore = stoppedSpy.count();
  bar.setProject( &projectA );
  QCOMPARE( stoppedSpy.count(), stoppedBefore );
}

void TestEditTools::toolbarProjectClearedDropsSessionCleanly()
{
  QgsMapCanvas canvas;
  configureCanvas( canvas );

  // 堆图层归工程所有：clear() 移除并析构——会话必须在移除波里收尾。
  auto *layer = new QgsVectorLayer( QStringLiteral( "Point?crs=EPSG:4326&field=id:integer" ),
                                    QStringLiteral( "clear-l" ), QStringLiteral( "memory" ) );
  QVERIFY( layer->isValid() );
  seedFeature( *layer, QgsGeometry::fromPointXY( QgsPointXY( 3, 3 ) ) ); // 提交面种子
  QgsProject project;
  project.addMapLayer( layer );

  PaleoEditingToolbar bar( &canvas );
  bar.setLayers( { layer } );
  bar.setProject( &project );
  QSignalSpy stoppedSpy( &bar, &PaleoEditingToolbar::editingStopped );

  QVERIFY( bar.startEditing() ); // 会话归工具条持有
  addPointCommand( layer, 5 );

  project.clear(); // layersWillBeRemoved → finalizeSession（提交）→ 图层析构

  QCOMPARE( stoppedSpy.count(), 1 );
  QCOMPARE( stoppedSpy.at( 0 ).at( 1 ).toBool(), true );
  QVERIFY( !bar.isEditing() );
  QVERIFY( bar.currentLayer() == nullptr || bar.currentLayer() != layer ); // 悬空引用不外泄
}

void TestEditTools::toolbarUndoBoundaryAfterProjectFinalize()
{
  QgsMapCanvas canvas;
  configureCanvas( canvas );

  QgsVectorLayer layer( QStringLiteral( "Point?crs=EPSG:4326&field=id:integer" ),
                        QStringLiteral( "undo-b" ), QStringLiteral( "memory" ) );
  QVERIFY( layer.isValid() );
  QgsProject projectA;
  QgsProject projectB;

  PaleoEditingToolbar bar( &canvas );
  bar.setLayers( { &layer } );
  bar.setProject( &projectA );

  QVERIFY( bar.startEditing() );
  addPointCommand( &layer, 1 );
  QVERIFY( bar.actionUndo()->isEnabled() ); // 会话内可撤销

  // 工程边界收尾 = 版本边界（§34：undo 不跨版本）——提交后 undo/redo 全关。
  bar.setProject( &projectB );
  QVERIFY( !bar.isEditing() );
  QVERIFY( !bar.actionUndo()->isEnabled() );
  QVERIFY( !bar.actionRedo()->isEnabled() );
  QVERIFY( !bar.undoStack()->canUndo() );
  QVERIFY( !bar.undoStack()->canRedo() );

  // 边界后的 undo 请求是无害 no-op（不崩、不改数据）。
  bar.actionUndo()->trigger();
  QCOMPARE( layer.featureCount(), 1 );
}

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

  // Click on empty space: warned, but nothing was gestured → no abort.
  click( tool, canvas, pxAt( canvas, 5, 5 ), Qt::LeftButton );
  QVERIFY( msgSpy.count() >= 1 );
  QCOMPARE( abortSpy.count(), 0 );
  QCOMPARE( editedSpy.count(), 0 );
  QCOMPARE( tool.deletedCount(), 0 );
  QCOMPARE( layer.featureCount(), 1 );

  // 命中式删除（W4）：单击命中要素即删——与是否选中无关。
  click( tool, canvas, pxAt( canvas, 25, 25 ), Qt::LeftButton );
  QCOMPARE( editedSpy.count(), 1 );
  QCOMPARE( tool.deletedCount(), 1 );
  QCOMPARE( layer.featureCount(), 0 );
  QVERIFY( layer.undoStack()->canUndo() );

  // Esc → teardown signal (§42.15).
  sendEsc( tool );
  QCOMPARE( abortSpy.count(), 1 );
  QCOMPARE( editedSpy.count(), 1 );

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

// QGIS_NATIVE_ADOPTION：拓扑提交门 = 原生 QgsGeometryValidator——合法几何
// 回空，蝴蝶结自相交环给出错误文本+坐标，null 几何如实报 empty。
void TestEditTools::geometryCommitGateValidatesNatively()
{
  // 合法几何 → 空错误串。
  QVERIFY( QgisEditingService::geometryCommitError(
               QgsGeometry::fromWkt( QStringLiteral( "POLYGON((0 0, 4 0, 4 4, 0 4, 0 0))" ) ),
               QStringLiteral( "geom" ) )
               .isEmpty() );
  QVERIFY( QgisEditingService::geometryCommitError(
               QgsGeometry::fromWkt( QStringLiteral( "POINT(1 2)" ) ),
               QStringLiteral( "geom" ) )
               .isEmpty() );

  // 蝴蝶结自相交 → 错误文本带位置信息；含 what 主语。
  const QString bowErr = QgisEditingService::geometryCommitError(
      QgsGeometry::fromWkt( QStringLiteral( "POLYGON((0 0, 2 2, 2 0, 0 2, 0 0))" ) ),
      QStringLiteral( "drawn feature" ) );
  QVERIFY( !bowErr.isEmpty() );
  QVERIFY( bowErr.contains( QLatin1String( "drawn feature" ) ) );
  QVERIFY( bowErr.contains( QLatin1String( "invalid" ) ) );

  // null 几何如实报 empty——与「拓扑违例」区分。
  QVERIFY( !QgisEditingService::geometryCommitError( QgsGeometry(),
                                                   QStringLiteral( "geom" ) )
               .isEmpty() );
  // 空但合法的退化几何（POLYGON EMPTY）放行——空 ≠ 拓扑违例。
  QVERIFY( QgisEditingService::geometryCommitError(
               QgsGeometry::fromWkt( QStringLiteral( "POLYGON EMPTY" ) ),
               QStringLiteral( "geom" ) )
               .isEmpty() );
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
    QSignalSpy msgSpy( &tool, &QgsMapTool::messageEmitted );

    // W4 爆炸半径：有选区时点击空处不得删选区——只警告。
    click( tool, canvas, pxAt( canvas, 40, 40 ), Qt::LeftButton );
    QCOMPARE( edited.count(), 0 );
    QCOMPARE( tool.deletedCount(), 0 );
    QCOMPARE( layer.featureCount(), 2 );
    QVERIFY( msgSpy.count() >= 1 );

    // 点击命中处只删命中的要素；未选中的另一要素不受影响。
    click( tool, canvas, pxAt( canvas, 25, 25 ), Qt::LeftButton );
    QVERIFY2( edited.count() == 1, "one delete gesture must emit featureEdited exactly once" );
    QCOMPARE( edited.at( 0 ).at( 0 ).toString(), layer.id() );
    QCOMPARE( edited.at( 0 ).size(), 1 );
    QCOMPARE( tool.deletedCount(), 1 );
    QCOMPARE( aborted.count(), 0 );
    QCOMPARE( layer.featureCount(), 1 );
    QVERIFY( layer.getFeature( seeded.at( 1 ).id() ).isValid() );

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


// ---------------------------------------------------------------------------
// mapping 主线2：QgsPointLocator 拓扑索引 + 跨层拓扑
// ---------------------------------------------------------------------------

void TestEditTools::topoIndexCandidatesAndHealth()
{
  QgsVectorLayer a( QStringLiteral( "Polygon?crs=EPSG:4326&field=id:integer" ),
                    QStringLiteral( "idx-a" ), QStringLiteral( "memory" ) );
  QgsVectorLayer b( QStringLiteral( "Polygon?crs=EPSG:4326&field=id:integer" ),
                    QStringLiteral( "idx-b" ), QStringLiteral( "memory" ) );
  QVERIFY( a.isValid() && b.isValid() );
  const QgsFeatureId fa = seedFeature( a, QgsGeometry::fromWkt( squareWkt( 10, 10, 20 ) ) );
  const QgsFeatureId fb = seedFeature( b, QgsGeometry::fromWkt( squareWkt( 30, 10, 20 ) ) );
  Q_UNUSED( fa );
  Q_UNUSED( fb );

  QgisTopologicalIndex idx;
  idx.setLayers( &a, { &b } );
  QVERIFY( !idx.isLayerIndexed( &a ) ); // 惰性：查询前未建
  QVERIFY( !idx.isLayerIndexed( &b ) );

  // 共点 (30,10)：a 的 v1、b 的 v0 与 b 的闭合顶点 v4（同位重复计数）。
  const QList<QgisTopologicalIndex::VertexHit> same =
      idx.verticesNear( QgsPointXY( 30, 10 ), 1e-9, /*includeOthers=*/true );
  QCOMPARE( idx.indexedLayerCount(), 2 );
  QCOMPARE( same.size(), 3 );
  QSet<QString> layersOf;
  for ( const QgisTopologicalIndex::VertexHit &h : same )
    layersOf.insert( h.layer->name() );
  QVERIFY( layersOf.contains( QStringLiteral( "idx-a" ) ) );
  QVERIFY( layersOf.contains( QStringLiteral( "idx-b" ) ) );

  // 单层查询：只出 scope 层的候选。
  const QList<QgisTopologicalIndex::VertexHit> scoped =
      idx.verticesNear( QgsPointXY( 30, 10 ), 1e-9, /*includeOthers=*/false );
  QCOMPARE( scoped.size(), 1 );
  QCOMPARE( scoped.first().layer, &a );

  // 候选 pos 与 dense 号对得上（v1 的位置 = (30,10)）。
  QVERIFY( qgsDoubleNear( scoped.first().pos.x(), 30.0 ) );
  QVERIFY( qgsDoubleNear( scoped.first().pos.y(), 10.0 ) );
  QCOMPARE( scoped.first().vertexNr, 1 );
}

void TestEditTools::topoIndexInvalidationOnEditSignals()
{
  QgsVectorLayer a( QStringLiteral( "Polygon?crs=EPSG:4326&field=id:integer" ),
                    QStringLiteral( "idx-dirty" ), QStringLiteral( "memory" ) );
  QVERIFY( a.isValid() );
  const QgsFeatureId fid = seedFeature( a, QgsGeometry::fromWkt( squareWkt( 10, 10, 20 ) ) );

  QgisTopologicalIndex idx;
  idx.setLayers( &a, {} );
  ( void )idx.verticesNear( QgsPointXY( 25, 25 ), 5.0, false ); // 建索引
  QVERIFY( idx.isLayerIndexed( &a ) );

  // 编辑信号 → 整层标脏（geometryChanged 走编辑会话）。
  a.startEditing();
  QgsGeometry moved = a.getFeature( fid ).geometry();
  QVERIFY( moved.moveVertex( 45, 45, 2 ) );
  QVERIFY( a.changeGeometry( fid, moved ) );
  QVERIFY( !idx.isLayerIndexed( &a ) );

  // 惰性重建：下一次查询看到新位置 (45,45) 而不是旧 (30,30)。
  const QList<QgisTopologicalIndex::VertexHit> hits =
      idx.verticesNear( QgsPointXY( 45, 45 ), 1e-9, false );
  QCOMPARE( hits.size(), 1 );
  QCOMPARE( hits.first().fid, fid );
  QCOMPARE( hits.first().vertexNr, 2 );
  QVERIFY( idx.isLayerIndexed( &a ) );

  // 回滚同样标脏并重建回旧位置。
  a.rollBack();
  QVERIFY( !idx.isLayerIndexed( &a ) );
  const QList<QgisTopologicalIndex::VertexHit> after =
      idx.verticesNear( QgsPointXY( 45, 45 ), 1e-9, false );
  QCOMPARE( after.size(), 0 );
  const QList<QgisTopologicalIndex::VertexHit> back =
      idx.verticesNear( QgsPointXY( 30, 30 ), 1e-9, false );
  QCOMPARE( back.size(), 1 );
}

void TestEditTools::vertexTopoCrossLayerDragMovesBothLayers()
{
  QgsMapCanvas canvas;
  configureCanvas( canvas );
  // 缩窄视野：searchRadiusMU ≈ 38px×mupp——200px 视野 10 单位时 ≈1.9 单位，
  // 释放点 (30,7) 距一切既有顶点 ≥3 单位 → 不触发落点焊接（焊接另有用例）。
  canvas.setExtent( QgsRectangle( 25, 5, 35, 15 ) );
  canvas.refresh();

  // 同 CRS 两层各一个方形，共享顶点 (30,10)；两层都在编辑会话。
  QgsVectorLayer a( QStringLiteral( "Polygon?crs=EPSG:4326&field=id:integer" ),
                    QStringLiteral( "x-a" ), QStringLiteral( "memory" ) );
  QgsVectorLayer b( QStringLiteral( "Polygon?crs=EPSG:4326&field=id:integer" ),
                    QStringLiteral( "x-b" ), QStringLiteral( "memory" ) );
  QVERIFY( a.isValid() && b.isValid() );
  const QgsFeatureId fa = seedFeature( a, QgsGeometry::fromWkt( squareWkt( 10, 10, 20 ) ) );
  const QgsFeatureId fb = seedFeature( b, QgsGeometry::fromWkt( squareWkt( 30, 10, 20 ) ) );
  QgsProject::instance()->addMapLayer( &a, /*addToLegend=*/false, /*takeOwnership=*/false ); // 栈层：不转移所有权
  QgsProject::instance()->addMapLayer( &b, /*addToLegend=*/false, /*takeOwnership=*/false );
  a.startEditing();
  b.startEditing();
  a.selectByIds( { fa } );
  canvas.setLayers( QList<QgsMapLayer *>{ &a, &b } );
  canvas.setCurrentLayer( &a );
  canvas.refresh();

  TestVertexTool tool( &canvas, &a );
  tool.setTopologicalEditingEnabled( true );
  tool.setCrossLayerTopologyEnabled( true );
  canvas.setMapTool( &tool );
  QSignalSpy editedSpy( &tool, &PaleoVertexTool::featureEdited );

  // 抓 (30,10)（a 的 v1 / b 的 v0+v4 闭合），拖到 (30,7) 释放。
  QgsMapMouseEvent press( &canvas, QEvent::MouseButtonPress, pxAt( canvas, 30, 10 ),
                          Qt::LeftButton, Qt::LeftButton, Qt::NoModifier );
  tool.canvasPressEvent( &press );
  QVERIFY2( tool.isDragging(), "press on the shared vertex must arm the drag" );
  QCOMPARE( tool.topoMarkers().size(), 2 ); // b 层 v0/v4 均在写集（无选区 → 红 marker）
  QgsMapMouseEvent rel( &canvas, QEvent::MouseButtonRelease, pxAt( canvas, 30, 7 ),
                        Qt::LeftButton, Qt::NoButton, Qt::NoModifier );
  tool.canvasReleaseEvent( &rel );

  // 两层各一组 edit command、各一次 featureEdited。
  QCOMPARE( editedSpy.count(), 2 );
  QCOMPARE( a.undoStack()->count(), 1 );
  QCOMPARE( b.undoStack()->count(), 1 );

  // a 的 v1 与 b 的 v0 现在都在 (30,25)（两环各自闭合被 moveVertex 保持）。
  auto vertexPos = []( QgsVectorLayer &l, QgsFeatureId fid, int nr ) {
    const QgsGeometry g = l.getFeature( fid ).geometry();
    QgsVertexId vid;
    if ( !g.vertexIdFromVertexNr( nr, vid ) )
      return QgsPointXY();
    const QgsPoint p = g.constGet()->vertexAt( vid );
    return QgsPointXY( p.x(), p.y() );
  };
  // 释放点是像素取整后的 map 点（DPI 放大），断言对它而不是名义 (30,7)。
  const QgsPointXY released = mapPt( canvas, pxAt( canvas, 30, 7 ).x(), pxAt( canvas, 30, 7 ).y() );
  const QgsPointXY pa1 = vertexPos( a, fa, 1 );
  const QgsPointXY pb0 = vertexPos( b, fb, 0 );
  QVERIFY( qgsDoubleNear( pa1.y(), released.y(), 1e-6 ) );
  QVERIFY( qgsDoubleNear( pb0.y(), released.y(), 1e-6 ) );
  QVERIFY( qgsDoubleNear( pb0.x(), released.x(), 1e-6 ) );

  // undo 按层独立：先 b 后 a，各自回到 (30,10)。
  b.undoStack()->undo();
  QVERIFY( qgsDoubleNear( vertexPos( b, fb, 0 ).y(), 10.0 ) );
  QVERIFY( qgsDoubleNear( vertexPos( a, fa, 1 ).y(), released.y(), 1e-6 ) ); // a 仍是移动后
  a.undoStack()->undo();
  QVERIFY( qgsDoubleNear( vertexPos( a, fa, 1 ).y(), 10.0 ) );

  canvas.unsetMapTool( &tool );
  a.rollBack();
  b.rollBack();
  QgsProject::instance()->removeMapLayer( &a );
  QgsProject::instance()->removeMapLayer( &b );
}

void TestEditTools::vertexTopoCrossLayerCrsMismatchExcluded()
{
  QgsMapCanvas canvas;
  configureCanvas( canvas );
  canvas.setExtent( QgsRectangle( 25, 5, 35, 15 ) ); // 同上：避开落点焊接
  canvas.refresh();

  // b 层与 a 层数值同坐标但 CRS 不同——跨层参与被同 CRS 门槛排除。
  QgsVectorLayer a( QStringLiteral( "Polygon?crs=EPSG:4326&field=id:integer" ),
                    QStringLiteral( "crs-a" ), QStringLiteral( "memory" ) );
  QgsVectorLayer b( QStringLiteral( "Polygon?crs=EPSG:3857&field=id:integer" ),
                    QStringLiteral( "crs-b" ), QStringLiteral( "memory" ) );
  QVERIFY( a.isValid() && b.isValid() );
  const QgsFeatureId fa = seedFeature( a, QgsGeometry::fromWkt( squareWkt( 10, 10, 20 ) ) );
  const QgsFeatureId fb = seedFeature( b, QgsGeometry::fromWkt( squareWkt( 30, 10, 20 ) ) );
  Q_UNUSED( fb );
  QgsProject::instance()->addMapLayer( &a, /*addToLegend=*/false, /*takeOwnership=*/false ); // 栈层：不转移所有权
  QgsProject::instance()->addMapLayer( &b, /*addToLegend=*/false, /*takeOwnership=*/false );
  a.startEditing();
  b.startEditing();
  a.selectByIds( { fa } );
  canvas.setLayers( QList<QgsMapLayer *>{ &a, &b } );
  canvas.setCurrentLayer( &a );
  canvas.refresh();

  TestVertexTool tool( &canvas, &a );
  tool.setTopologicalEditingEnabled( true );
  tool.setCrossLayerTopologyEnabled( true );
  canvas.setMapTool( &tool );

  QgsMapMouseEvent press( &canvas, QEvent::MouseButtonPress, pxAt( canvas, 30, 10 ),
                          Qt::LeftButton, Qt::LeftButton, Qt::NoModifier );
  tool.canvasPressEvent( &press );
  QVERIFY2( tool.isDragging(), "press on the shared vertex must arm the drag" );
  QCOMPARE( tool.topoMarkers().size(), 0 ); // b 不在写集（CRS 不同）
  QgsMapMouseEvent rel( &canvas, QEvent::MouseButtonRelease, pxAt( canvas, 30, 7 ),
                        Qt::LeftButton, Qt::NoButton, Qt::NoModifier );
  tool.canvasReleaseEvent( &rel );

  QCOMPARE( a.undoStack()->count(), 1 );
  QCOMPARE( b.undoStack()->count(), 0 ); // b 未被写
  const QgsGeometry gb = b.getFeature( fb ).geometry();
  QVERIFY( qgsDoubleNear( gb.constGet()->vertexAt( QgsVertexId( 0, 0, 0 ) ).y(), 10.0 ) );

  canvas.unsetMapTool( &tool );
  a.rollBack();
  b.rollBack();
  QgsProject::instance()->removeMapLayer( &a );
  QgsProject::instance()->removeMapLayer( &b );
}

void TestEditTools::vertexTopoCrossLayerDeleteAndUndo()
{
  QgsMapCanvas canvas;
  configureCanvas( canvas );

  // 两层共享顶点 (30,30)（a 的 v2 / b 的 v3）——右键批量删除跨层生效。
  QgsVectorLayer a( QStringLiteral( "Polygon?crs=EPSG:4326&field=id:integer" ),
                    QStringLiteral( "del-a" ), QStringLiteral( "memory" ) );
  QgsVectorLayer b( QStringLiteral( "Polygon?crs=EPSG:4326&field=id:integer" ),
                    QStringLiteral( "del-b" ), QStringLiteral( "memory" ) );
  QVERIFY( a.isValid() && b.isValid() );
  const QgsFeatureId fa = seedFeature( a, QgsGeometry::fromWkt( squareWkt( 10, 10, 20 ) ) );
  const QgsFeatureId fb = seedFeature( b, QgsGeometry::fromWkt( squareWkt( 30, 10, 20 ) ) );
  QgsProject::instance()->addMapLayer( &a, /*addToLegend=*/false, /*takeOwnership=*/false ); // 栈层：不转移所有权
  QgsProject::instance()->addMapLayer( &b, /*addToLegend=*/false, /*takeOwnership=*/false );
  a.startEditing();
  b.startEditing();
  a.selectByIds( { fa } );
  canvas.setLayers( QList<QgsMapLayer *>{ &a, &b } );
  canvas.setCurrentLayer( &a );
  canvas.refresh();

  TestVertexTool tool( &canvas, &a );
  tool.setTopologicalEditingEnabled( true );
  tool.setCrossLayerTopologyEnabled( true );
  canvas.setMapTool( &tool );
  QSignalSpy editedSpy( &tool, &PaleoVertexTool::featureEdited );

  QgsMapMouseEvent rel( &canvas, QEvent::MouseButtonRelease, pxAt( canvas, 30, 30 ),
                        Qt::RightButton, Qt::NoButton, Qt::NoModifier );
  tool.canvasReleaseEvent( &rel );

  QCOMPARE( editedSpy.count(), 2 ); // 两层各删一个共点顶点
  QCOMPARE( a.undoStack()->count(), 1 );
  QCOMPARE( b.undoStack()->count(), 1 );
  QCOMPARE( vertexTotal( a, fa ), 4 ); // 5 → 4（各删一个）
  QCOMPARE( vertexTotal( b, fb ), 4 );

  a.undoStack()->undo();
  b.undoStack()->undo();
  QCOMPARE( vertexTotal( a, fa ), 5 );
  QCOMPARE( vertexTotal( b, fb ), 5 );

  canvas.unsetMapTool( &tool );
  a.rollBack();
  b.rollBack();
  QgsProject::instance()->removeMapLayer( &a );
  QgsProject::instance()->removeMapLayer( &b );
}

// mapping 主线2 review 修复：拖拽途中跨层参与层析构——DragState 持有裸层
// 指针键，靠 destroyed→clearDragState 守卫；释放事件不得解引用悬空层。
void TestEditTools::vertexTopoCrossLayerParticipantDestructionMidDrag()
{
  QgsMapCanvas canvas;
  configureCanvas( canvas );
  canvas.setExtent( QgsRectangle( 25, 5, 35, 15 ) );
  canvas.refresh();

  QgsVectorLayer a( QStringLiteral( "Polygon?crs=EPSG:4326&field=id:integer" ),
                    QStringLiteral( "x-a" ), QStringLiteral( "memory" ) );
  QVERIFY( a.isValid() );
  const QgsFeatureId fa = seedFeature( a, QgsGeometry::fromWkt( squareWkt( 10, 10, 20 ) ) );
  // 参与层 b：堆对象、工程托管所有权——中途 delete 模拟析构。
  auto *b = new QgsVectorLayer( QStringLiteral( "Polygon?crs=EPSG:4326&field=id:integer" ),
                                QStringLiteral( "x-b" ), QStringLiteral( "memory" ) );
  QVERIFY( b->isValid() );
  const QgsFeatureId fb = seedFeature( *b, QgsGeometry::fromWkt( squareWkt( 30, 10, 20 ) ) );
  Q_UNUSED( fb );
  QgsProject::instance()->addMapLayer( &a, /*addToLegend=*/false, /*takeOwnership=*/false );
  QgsProject::instance()->addMapLayer( b ); // 工程所有
  a.startEditing();
  b->startEditing();
  a.selectByIds( { fa } );
  canvas.setLayers( QList<QgsMapLayer *>{ &a, b } );
  canvas.setCurrentLayer( &a );
  canvas.refresh();

  TestVertexTool tool( &canvas, &a );
  tool.setTopologicalEditingEnabled( true );
  tool.setCrossLayerTopologyEnabled( true );
  canvas.setMapTool( &tool );

  // 抓共享顶点 (30,10)——b 入写集（destroyed 守卫挂上）。
  QgsMapMouseEvent press( &canvas, QEvent::MouseButtonPress, pxAt( canvas, 30, 10 ),
                          Qt::LeftButton, Qt::LeftButton, Qt::NoModifier );
  tool.canvasPressEvent( &press );
  QVERIFY2( tool.isDragging(), "press on the shared vertex must arm the drag" );

  // 参与层中途析构（工程移除并 delete，destroyed 同步发出）→ 拖拽被丢弃，不崩。
  QgsProject::instance()->removeMapLayer( b ); // takeOwnership：析构随移除
  b = nullptr;
  QVERIFY( !tool.isDragging() ); // destroyed → clearDragState

  // 后续 release 是无拖拽 no-op；a 未被写入。
  QgsMapMouseEvent rel( &canvas, QEvent::MouseButtonRelease, pxAt( canvas, 30, 7 ),
                        Qt::LeftButton, Qt::NoButton, Qt::NoModifier );
  tool.canvasReleaseEvent( &rel );
  QCOMPARE( a.undoStack()->count(), 0 );

  canvas.unsetMapTool( &tool );
  a.rollBack();
  QgsProject::instance()->removeMapLayer( &a );
}

int main( int argc, char *argv[] )
{
  QgsApplication app( argc, argv, false );
  app.setPrefixPath(qEnvironmentVariable("QGIS_PREFIX_PATH", QStringLiteral("/usr")), true); // distro install
  app.initQgis();
  TestEditTools tc;
  const int rc = QTest::qExec( &tc, argc, argv );
  QgsApplication::exitQgis();
  return rc;
}

#include "tst_edittools.moc"
