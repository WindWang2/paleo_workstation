// 层：视图
#include "ui/edittools/vertexeditortools.h"

#include <QKeyEvent>

#include <qgis.h>
#include <qgsfeature.h>
#include <qgsfeatureiterator.h>
#include <qgsgeometry.h>
#include <qgsmapcanvas.h>
#include <qgsmapmouseevent.h>
#include <qgsrubberband.h>
#include <qgsvectorlayer.h>
#include <qgsvertexmarker.h>
#include <qgswkbtypes.h>

// ---------------------------------------------------------------------------
// Evidence chain — QgsVertexTool is NOT usable in this build (QGIS 4.2.2,
// re-verified independently before this implementation was written):
//
//   $ ls /usr/include/qgis/qgsvertextool.h
//     ls: cannot access '/usr/include/qgis/qgsvertextool.h': 没有那个文件或目录
//   $ nm -D /usr/lib/libqgis_gui.so | grep QgsVertexTool
//     (no output, exit 1 — the class is not exported by gui at all)
//   $ nm -D /usr/lib/libqgis_app.so | grep QgsVertexTool | head
//     0000000000746720 T _ZN13QgsVertexTool10deactivateEv
//     000000000075b860 T _ZN13QgsVertexTool10moveVertexERK10QgsPointXYPKN15QgsPointLocator5MatchE
//     0000000000744c30 T _ZN13QgsVertexTool11addDragBandERK10QgsPointXYS2_
//     0000000000763680 T _ZN13QgsVertexTool11cleanEditorEx
//     ... (symbols present, APP_EXPORT, header not installed)
//
// Same audit verdict as src/ui/vertexeditorshim.h: the real vertex tool ships
// in libqgis_app.so without installed headers, so a standalone embed cannot
// link it. Sanctioned degrade implemented here instead:
//   · QgsMapToolEdit base (edit-tool flags/plumbing, no CAD dock dependency);
//   · QgsVertexMarker per selected-feature vertex (GUI_EXPORT canvas item);
//   · QgsMapToolEdit::createRubberBand for the in-flight drag preview;
//   · QgsGeometry native mutators (moveVertex/insertVertex/deleteVertex) and
//     closestSegmentWithContext for segment hit tests — no hand-rolled
//     point-to-segment math anywhere;
//   · every mutation wrapped in beginEditCommand/endEditCommand on the layer
//     edit buffer → native per-layer undo stack.
// ---------------------------------------------------------------------------

namespace
{
// QGIS node-tool look: white circles idle, red while dragged. Map-domain
// colors are QGIS style territory, not DESIGN.md UI tokens (DESIGN.md §93).
constexpr int MARKER_ICON_SIZE = 10;
constexpr int MARKER_PEN_WIDTH = 2;

QColor idleMarkerColor() { return QColor( 255, 255, 255 ); }
QColor dragMarkerColor() { return QColor( 255, 0, 0 ); }

// Markers carry their (feature id, vertex number) in the item itself so the
// highlight lookup does not depend on QSet iteration order of
// selectedFeatureIds() (the order can differ between rebuild and lookup).
// NB: QGIS 4's QgsMapCanvasItem is a plain QGraphicsItem (no QObject base),
// so dynamic properties are unavailable — hence the tagged subclass.
class TaggedVertexMarker : public QgsVertexMarker
{
  public:
    TaggedVertexMarker( QgsMapCanvas *canvas, qint64 featureId, int vertex )
      : QgsVertexMarker( canvas ), fid( featureId ), vertexNr( vertex ) {}

    qint64 fid = -1;
    int vertexNr = -1;
};

QgsVertexMarker *markerForVertex( const QList<QgsVertexMarker *> &markers, qint64 fid, int vertexNr )
{
  for ( QgsVertexMarker *marker : markers )
  {
    auto *tagged = dynamic_cast<TaggedVertexMarker *>( marker );
    if ( tagged && tagged->fid == fid && tagged->vertexNr == vertexNr )
      return tagged;
  }
  return nullptr;
}

// XY coincidence tolerant of last-ulp noise (GEOS round-trips); shared-
// boundary vertices produced by polygonize are bitwise-equal to start with.
bool sameXy( const QgsPoint &a, const QgsPoint &b )
{
  return qgsDoubleNear( a.x(), b.x() ) && qgsDoubleNear( a.y(), b.y() );
}

// Dense-numbered vertex position; invalid nr → empty (geometry-less).
QgsPointXY vertexXy( const QgsGeometry &geometry, int vertexNr )
{
  QgsVertexId vid;
  if ( !geometry.vertexIdFromVertexNr( vertexNr, vid ) || !geometry.constGet() )
    return QgsPointXY();
  const QgsPoint pt = geometry.constGet()->vertexAt( vid );
  return QgsPointXY( pt.x(), pt.y() );
}
} // namespace

// One in-flight vertex drag. The grabbed vertex is (fid, vertexNr); in
// topological mode `coincident` lists every layer vertex sharing its
// position (self included). Per-feature state is keyed by fid so a single
// feature carrying several coincident vertices moves them all on one clone.
struct PaleoVertexTool::DragState
{
  qint64 fid = -1;
  int vertexNr = -1;
  QgsGeometry originalGeometry;             // grabbed feature's pre-drag clone
  QgsVertexMarker *marker = nullptr;        // not owned (canvas-parented)
  QgsRubberBand *previewBand = nullptr;     // grabbed feature's preview band
  QgsPointXY grabPos;                       // grabbed vertex, layer CRS
  QList<QPair<qint64, int>> coincident;     // write set ({self} when topo off)
  QHash<qint64, QgsGeometry> originalByFid; // fid → pre-drag clone
  QHash<qint64, QgsRubberBand *> previewBands; // fid → band (canvas-parented)
  QList<QgsVertexMarker *> topoMarkers;     // red markers for coincident
                                            // vertices outside the selection
};

PaleoVertexTool::PaleoVertexTool( QgsMapCanvas *canvas, QgsVectorLayer *layer )
  : QgsMapToolEdit( canvas )
  , mLayer( layer )
{
  setToolName( tr( "Edit vertices" ) );
  setCursor( QCursor( Qt::CrossCursor ) );
}

PaleoVertexTool::~PaleoVertexTool()
{
  // Only drop the bookkeeping struct: the marker and preview band are
  // canvas-parented and may already be gone if the canvas died first, so the
  // destructor must not dereference them. deactivate() performs the full
  // cleanup while the canvas is guaranteed alive.
  delete mDraggingVertex;
  mDraggingVertex = nullptr;
}

void PaleoVertexTool::activate()
{
  // A null ctor layer defers to the canvas' current layer until here.
  if ( !mLayer )
    mLayer = qobject_cast<QgsVectorLayer *>( mCanvas->currentLayer() );

  // Live marker rebuild on selection changes while active. Context object
  // `this` auto-disconnects on destruction; the disconnect-then-connect pair
  // survives repeated activate() cycles without stacking duplicate handlers
  // (NB: Qt::UniqueConnection cannot be used — it rejects lambda slots).
  if ( mLayer )
  {
    disconnect( mLayer, &QgsVectorLayer::selectionChanged, this, nullptr );
    connect( mLayer, &QgsVectorLayer::selectionChanged, this, [this]
    {
      if ( mCanvas->mapTool() == this ) // ignore selection churn while inactive
        rebuildMarkers();
    } );
  }

  QgsMapToolEdit::activate();
  rebuildMarkers();
}

void PaleoVertexTool::deactivate()
{
  clearDragState();
  clearMarkers();
  QgsMapToolEdit::deactivate();
}

QgsVectorLayer *PaleoVertexTool::targetLayer() const
{
  return mLayer;
}

// ---------------------------------------------------------------------------
// Canvas gestures
// ---------------------------------------------------------------------------

void PaleoVertexTool::canvasPressEvent( QgsMapMouseEvent *e )
{
  if ( e->button() != Qt::LeftButton )
    return; // right-button delete commits on release; other buttons ignored

  QgsVectorLayer *layer = targetLayer();
  if ( !layer || !layer->isEditable() )
  {
    emit messageEmitted( tr( "Start editing before moving vertices" ), Qgis::MessageLevel::Warning );
    return;
  }

  const QgsPointXY layerPoint = toLayerCoordinates( layer, e->mapPoint() );
  qint64 fid = -1;
  int vertexNr = -1;
  if ( !findNearestVertex( layerPoint, fid, vertexNr ) )
    return; // empty-space press: no drag armed

  const QgsFeature feature = layer->getFeature( fid );
  if ( !feature.hasGeometry() )
    return;

  clearDragState(); // defensive: at most one in-flight drag

  mDraggingVertex = new DragState;
  mDraggingVertex->fid = fid;
  mDraggingVertex->vertexNr = vertexNr;
  mDraggingVertex->originalGeometry = feature.geometry(); // deep copy = pre-drag clone
  mDraggingVertex->grabPos = vertexXy( feature.geometry(), vertexNr );
  mDraggingVertex->coincident.append( qMakePair( fid, vertexNr ) );
  mDraggingVertex->originalByFid.insert( fid, feature.geometry() );
  mDraggingVertex->marker = markerForVertex( mMarkers, fid, vertexNr );
  if ( mDraggingVertex->marker )
  {
    mDraggingVertex->marker->setColor( dragMarkerColor() );
    mDraggingVertex->marker->setIconSize( MARKER_ICON_SIZE + 4 );
  }
  mDraggingVertex->previewBand = createRubberBand( layer->geometryType() );
  mDraggingVertex->previewBands.insert( fid, mDraggingVertex->previewBand );

  // Topological editing: every vertex sharing the grabbed position joins the
  // write set — coincident members on UNSELECTED features get a red marker so
  // the shared-boundary move is visible before commit.
  if ( mTopoEditing )
  {
    const QgsFeatureIds selected = layer->selectedFeatureIds();
    for ( const QPair<qint64, int> &member : coincidentVertices( mDraggingVertex->grabPos ) )
    {
      if ( member.first == fid && member.second == vertexNr )
        continue;
      mDraggingVertex->coincident.append( member );
      if ( !mDraggingVertex->originalByFid.contains( member.first ) )
      {
        const QgsFeature other = layer->getFeature( member.first );
        if ( other.hasGeometry() )
          mDraggingVertex->originalByFid.insert( member.first, other.geometry() );
      }
      if ( !selected.contains( member.first ) )
      {
        const QgsPointXY mp = vertexXy( mDraggingVertex->originalByFid.value( member.first ), member.second );
        QgsVertexMarker *marker = new QgsVertexMarker( mCanvas );
        marker->setIconType( QgsVertexMarker::ICON_CIRCLE );
        marker->setColor( dragMarkerColor() );
        marker->setIconSize( MARKER_ICON_SIZE );
        marker->setPenWidth( MARKER_PEN_WIDTH );
        marker->setCenter( toMapCoordinates( layer, mp ) );
        mDraggingVertex->topoMarkers.append( marker );
      }
      if ( !mDraggingVertex->previewBands.contains( member.first ) )
        mDraggingVertex->previewBands.insert( member.first,
                                              createRubberBand( layer->geometryType() ) );
    }
  }

  // Prime the previews with the (so far zero-length) move, same math as move.
  const QgsPointXY lp = toLayerCoordinates( layer, e->mapPoint() );
  for ( auto it = mDraggingVertex->originalByFid.begin(); it != mDraggingVertex->originalByFid.end(); ++it )
  {
    QgsGeometry preview = it.value();
    for ( const QPair<qint64, int> &member : std::as_const( mDraggingVertex->coincident ) )
      if ( member.first == it.key() )
        preview.moveVertex( lp.x(), lp.y(), member.second );
    mDraggingVertex->previewBands.value( it.key() )->setToGeometry( preview, layer );
  }
}

void PaleoVertexTool::canvasMoveEvent( QgsMapMouseEvent *e )
{
  if ( !mDraggingVertex )
    return;

  // mapPoint() and marker centers are both map CRS — follow directly.
  if ( mDraggingVertex->marker )
    mDraggingVertex->marker->setCenter( e->mapPoint() );

  if ( QgsVectorLayer *layer = targetLayer() )
  {
    const QgsPointXY lp = toLayerCoordinates( layer, e->mapPoint() );
    for ( auto it = mDraggingVertex->originalByFid.begin(); it != mDraggingVertex->originalByFid.end(); ++it )
    {
      QgsGeometry preview = it.value();
      for ( const QPair<qint64, int> &member : std::as_const( mDraggingVertex->coincident ) )
        if ( member.first == it.key() )
          preview.moveVertex( lp.x(), lp.y(), member.second );
      mDraggingVertex->previewBands.value( it.key() )->setToGeometry( preview, layer );
    }
  }
}

void PaleoVertexTool::canvasReleaseEvent( QgsMapMouseEvent *e )
{
  // Right-button: delete the vertex under the cursor (QGIS gesture convention:
  // the delete fires on release, not press).
  if ( e->button() == Qt::RightButton )
  {
    QgsVectorLayer *layer = targetLayer();
    if ( !layer || !layer->isEditable() )
    {
      emit messageEmitted( tr( "Start editing before deleting vertices" ), Qgis::MessageLevel::Warning );
      return;
    }

    const QgsPointXY layerPoint = toLayerCoordinates( layer, e->mapPoint() );
    qint64 fid = -1;
    int vertexNr = -1;
    if ( !findNearestVertex( layerPoint, fid, vertexNr ) )
      return; // no vertex under the cursor: silent no-op

    const QgsFeature feature = layer->getFeature( fid );
    if ( !feature.hasGeometry() )
      return;

    const QgsGeometry geometry = feature.geometry();

    // Minimum-vertex guard in the vertex's own ring (QGIS geometry minimums,
    // closure point counted for polygon rings): a line needs ≥3 vertices to
    // delete down to 2, a ring ≥5 to delete down to 4. A single point's last
    // vertex would delete the whole feature — refused here.
    auto ringFitsDelete = []( const QgsGeometry &g, int nr ) -> bool {
      QgsVertexId vid;
      if ( !g.vertexIdFromVertexNr( nr, vid ) || !g.constGet() )
        return false;
      const Qgis::GeometryType gt = QgsWkbTypes::geometryType( g.wkbType() );
      const int ringVertices = g.constGet()->vertexCount( vid.part, vid.ring );
      const int minimum = gt == Qgis::GeometryType::Line ? 3
                          : gt == Qgis::GeometryType::Polygon ? 5
                          : 2;
      return ringVertices >= minimum;
    };
    if ( !ringFitsDelete( geometry, vertexNr ) )
    {
      emit messageEmitted( tr( "Cannot delete vertex: the feature would become invalid" ), Qgis::MessageLevel::Warning );
      return;
    }

    // Topological editing: collect every vertex coincident with the target —
    // the batch is refused whole when any member's own ring would break.
    QList<QPair<qint64, int>> writeSet = { qMakePair( fid, vertexNr ) };
    if ( mTopoEditing )
    {
      writeSet = coincidentVertices( vertexXy( geometry, vertexNr ) );
      for ( const QPair<qint64, int> &member : std::as_const( writeSet ) )
      {
        const QgsFeature other = layer->getFeature( member.first );
        if ( !other.hasGeometry() || !ringFitsDelete( other.geometry(), member.second ) )
        {
          emit messageEmitted( tr( "Cannot delete vertex: a shared-boundary feature would become invalid" ),
                               Qgis::MessageLevel::Warning );
          return;
        }
      }
    }

    layer->beginEditCommand( tr( "Deleted vertex" ) );
    // Group per fid: several coincident vertices on ONE feature must delete
    // in descending vertex order — dense numbering shifts as vertices drop.
    QHash<qint64, QList<int>> byFid;
    for ( const QPair<qint64, int> &member : std::as_const( writeSet ) )
      byFid[member.first].append( member.second );
    bool committed = true;
    for ( auto it = byFid.begin(); it != byFid.end() && committed; ++it )
    {
      QgsGeometry mutated = layer->getFeature( it.key() ).geometry();
      QList<int> nrs = it.value();
      std::sort( nrs.begin(), nrs.end(), std::greater<int>() );
      for ( const int nr : std::as_const( nrs ) )
        if ( !mutated.deleteVertex( nr ) )
          committed = false;
      if ( committed && !layer->changeGeometry( it.key(), mutated ) )
        committed = false;
    }
    if ( committed )
    {
      layer->endEditCommand();
      ++mEditedCount;
      emit featureEdited( layer->id() );
    }
    else
    {
      layer->destroyEditCommand();
      emit messageEmitted( tr( "Could not delete vertex" ), Qgis::MessageLevel::Warning );
    }

    rebuildMarkers();
    return;
  }

  if ( e->button() != Qt::LeftButton || !mDraggingVertex )
    return;

  QgsVectorLayer *layer = targetLayer();
  if ( !layer || !layer->isEditable() )
  {
    // The layer was editable when the drag armed but not anymore — drop the
    // drag rather than commit into a missing edit buffer.
    emit messageEmitted( tr( "Editing was stopped — vertex move discarded" ), Qgis::MessageLevel::Warning );
    clearDragState();
    return;
  }

  // The release point commits even when it left the drag start's vicinity
  // (QGIS vertex-tool semantics: the whole press→release span is the move).
  QgsPointXY releasePoint = toLayerCoordinates( layer, e->mapPoint() );

  // Topological weld: landing on another vertex's doorstep snaps to its exact
  // position — this is how coincidence is CREATED (shared-boundary healing).
  if ( mTopoEditing )
  {
    QgsPointXY snapPos;
    if ( findNearestLayerVertex( releasePoint, mDraggingVertex->grabPos, snapPos ) )
      releasePoint = snapPos;
  }

  layer->beginEditCommand( tr( "Moved vertex" ) );
  bool committed = true;
  // One clone per touched feature; moveVertex keeps vertex numbering stable
  // so every coincident member applies against its own pre-drag geometry.
  for ( auto it = mDraggingVertex->originalByFid.begin();
        it != mDraggingVertex->originalByFid.end() && committed; ++it )
  {
    QgsGeometry geometry = it.value();
    for ( const QPair<qint64, int> &member : std::as_const( mDraggingVertex->coincident ) )
      if ( member.first == it.key()
           && !geometry.moveVertex( releasePoint.x(), releasePoint.y(), member.second ) )
        committed = false;
    if ( committed && !layer->changeGeometry( it.key(), geometry ) )
      committed = false;
  }
  if ( committed )
  {
    layer->endEditCommand();
    ++mEditedCount;
    emit featureEdited( layer->id() );
  }
  else
  {
    layer->destroyEditCommand();
    emit messageEmitted( tr( "Could not move vertex" ), Qgis::MessageLevel::Warning );
  }

  clearDragState();
  rebuildMarkers(); // centers recomputed from the committed geometry
}

void PaleoVertexTool::canvasDoubleClickEvent( QgsMapMouseEvent *e )
{
  QgsVectorLayer *layer = targetLayer();
  if ( !layer || !layer->isEditable() )
  {
    emit messageEmitted( tr( "Start editing before adding vertices" ), Qgis::MessageLevel::Warning );
    return;
  }

  const QgsPointXY layerPoint = toLayerCoordinates( layer, e->mapPoint() );
  qint64 fid = -1;
  int beforeVertex = -1;
  QgsPointXY onSegment;
  if ( !findSegmentInsertion( layerPoint, fid, beforeVertex, onSegment ) )
    return;

  const QgsFeature feature = layer->getFeature( fid );
  if ( !feature.hasGeometry() )
    return;

  // fid → insertion indices (before-vertex numbering). Topological editing
  // extends the hit feature's segment to every feature carrying a coincident
  // edge — consecutive vertices equal to the segment endpoints, either
  // orientation (shared boundaries run opposite directions in the two rings).
  QHash<qint64, QList<int>> inserts;
  inserts[fid].append( beforeVertex );

  if ( mTopoEditing && beforeVertex >= 1 )
  {
    const QgsPointXY segA = vertexXy( feature.geometry(), beforeVertex - 1 );
    const QgsPointXY segB = vertexXy( feature.geometry(), beforeVertex );

    QgsFeatureIterator fit = layer->getFeatures();
    QgsFeature other;
    while ( fit.nextFeature( other ) )
    {
      if ( other.id() == fid || !other.hasGeometry() || !other.geometry().constGet() )
        continue;

      const QgsGeometry og = other.geometry();
      const int total = og.constGet()->vertexCount();
      for ( int nr = 0; nr + 1 < total; ++nr )
      {
        QgsVertexId va, vb;
        if ( !og.vertexIdFromVertexNr( nr, va ) || !og.vertexIdFromVertexNr( nr + 1, vb ) )
          break;
        if ( va.part != vb.part || va.ring != vb.ring ) // not the same segment
          continue;
        const QgsPoint pa = og.constGet()->vertexAt( va );
        const QgsPoint pb = og.constGet()->vertexAt( vb );
        const bool forward = qgsDoubleNear( pa.x(), segA.x() ) && qgsDoubleNear( pa.y(), segA.y() )
                             && qgsDoubleNear( pb.x(), segB.x() ) && qgsDoubleNear( pb.y(), segB.y() );
        const bool reversed = qgsDoubleNear( pa.x(), segB.x() ) && qgsDoubleNear( pa.y(), segB.y() )
                              && qgsDoubleNear( pb.x(), segA.x() ) && qgsDoubleNear( pb.y(), segA.y() );
        if ( forward || reversed )
          inserts[other.id()].append( nr + 1 );
      }
    }
  }

  layer->beginEditCommand( tr( "Added vertex" ) );
  bool committed = true;
  for ( auto it = inserts.begin(); it != inserts.end() && committed; ++it )
  {
    QgsGeometry geometry = layer->getFeature( it.key() ).geometry();
    QList<int> nrs = it.value();
    std::sort( nrs.begin(), nrs.end(), std::greater<int>() ); // dense numbering shifts per insert
    for ( const int nr : std::as_const( nrs ) )
      if ( !geometry.insertVertex( onSegment.x(), onSegment.y(), nr ) )
        committed = false;
    if ( committed && !layer->changeGeometry( it.key(), geometry ) )
      committed = false;
  }
  if ( committed )
  {
    layer->endEditCommand();
    ++mEditedCount;
    emit featureEdited( layer->id() );
  }
  else
  {
    layer->destroyEditCommand();
    emit messageEmitted( tr( "Could not add vertex" ), Qgis::MessageLevel::Warning );
  }

  rebuildMarkers();
}

void PaleoVertexTool::keyPressEvent( QKeyEvent *e )
{
  if ( e->key() == Qt::Key_Escape )
  {
    if ( mDraggingVertex )
    {
      // Cancel: restore the dragged marker to its pre-drag position, drop the
      // drag state; the geometry was never touched (edits happen on release).
      if ( QgsVectorLayer *layer = targetLayer() )
      {
        QgsVertexId vid;
        if ( mDraggingVertex->marker
             && mDraggingVertex->originalGeometry.vertexIdFromVertexNr( mDraggingVertex->vertexNr, vid ) )
        {
          const QgsPoint pt = mDraggingVertex->originalGeometry.constGet()->vertexAt( vid );
          mDraggingVertex->marker->setCenter( toMapCoordinates( layer, QgsPointXY( pt.x(), pt.y() ) ) );
        }
      }
      clearDragState();
    }
    else
    {
      emit editAborted(); // §42.15: owner deactivates the tool via unsetMapTool()
    }
  }
  QgsMapToolEdit::keyPressEvent( e );
}

// ---------------------------------------------------------------------------
// Private implementation
// ---------------------------------------------------------------------------

void PaleoVertexTool::rebuildMarkers()
{
  // A selection change mid-drag invalidates the dragged marker — cancel first
  // so DragState never points at a deleted canvas item.
  if ( mDraggingVertex )
    clearDragState();

  clearMarkers();

  QgsVectorLayer *layer = targetLayer();
  if ( !layer )
    return;

  const QgsFeatureIds fids = layer->selectedFeatureIds();
  for ( QgsFeatureId fid : fids )
  {
    const QgsFeature feature = layer->getFeature( fid );
    if ( !feature.hasGeometry() )
      continue;

    const QgsGeometry geometry = feature.geometry();
    if ( !geometry.constGet() )
      continue;

    // Dense vertex numbering across all parts/rings via the native mapping.
    int vertexNr = 0;
    QgsVertexId vid;
    while ( geometry.vertexIdFromVertexNr( vertexNr, vid ) )
    {
      const QgsPoint pt = geometry.constGet()->vertexAt( vid );
      QgsVertexMarker *marker = new TaggedVertexMarker( mCanvas, fid, vertexNr );
      marker->setIconType( QgsVertexMarker::ICON_CIRCLE );
      marker->setColor( idleMarkerColor() );
      marker->setIconSize( MARKER_ICON_SIZE );
      marker->setPenWidth( MARKER_PEN_WIDTH );
      // Marker centers are map CRS — convert the layer-CRS vertex back.
      marker->setCenter( toMapCoordinates( layer, QgsPointXY( pt.x(), pt.y() ) ) );
      mMarkers.append( marker );
      ++vertexNr;
    }
  }
}

void PaleoVertexTool::clearMarkers()
{
  // Canvas-parented items; direct delete also unregisters them from the canvas.
  qDeleteAll( mMarkers );
  mMarkers.clear();
}

void PaleoVertexTool::clearDragState()
{
  if ( !mDraggingVertex )
    return;

  // previewBands contains previewBand itself — delete via the map to avoid
  // a double-free; topoMarkers are canvas-parented extras, delete directly.
  for ( QgsRubberBand *band : std::as_const( mDraggingVertex->previewBands ) )
    delete band;
  mDraggingVertex->previewBands.clear();
  mDraggingVertex->previewBand = nullptr;
  qDeleteAll( mDraggingVertex->topoMarkers );
  mDraggingVertex->topoMarkers.clear();
  if ( mDraggingVertex->marker )
  {
    mDraggingVertex->marker->setColor( idleMarkerColor() );
    mDraggingVertex->marker->setIconSize( MARKER_ICON_SIZE );
    mDraggingVertex->marker = nullptr;
  }
  delete mDraggingVertex;
  mDraggingVertex = nullptr;
}

void PaleoVertexTool::setTopologicalEditingEnabled( bool on )
{
  if ( mTopoEditing == on )
    return;
  mTopoEditing = on;
  // A mid-drag toggle leaves the captured coincident set stale — drop the
  // drag rather than commit a mixed-semantics gesture.
  clearDragState();
}

// ---------------------------------------------------------------------------
// Topological helpers
// ---------------------------------------------------------------------------

QList<QPair<qint64, int>> PaleoVertexTool::coincidentVertices( const QgsPointXY &layerPoint ) const
{
  QList<QPair<qint64, int>> out;
  QgsVectorLayer *layer = targetLayer();
  if ( !layer )
    return out;

  const QgsPoint target( layerPoint );
  QgsFeatureIterator fit = layer->getFeatures();
  QgsFeature feature;
  while ( fit.nextFeature( feature ) )
  {
    if ( !feature.hasGeometry() )
      continue;
    const QgsGeometry geometry = feature.geometry();
    int nr = 0;
    QgsVertexId vid;
    while ( geometry.vertexIdFromVertexNr( nr, vid ) )
    {
      if ( sameXy( geometry.constGet()->vertexAt( vid ), target ) )
        out.append( qMakePair( feature.id(), nr ) );
      ++nr;
    }
  }
  return out;
}

bool PaleoVertexTool::findNearestLayerVertex( const QgsPointXY &layerPoint,
                                              const QgsPointXY &excludedPos,
                                              QgsPointXY &nearestPos )
{
  QgsVectorLayer *layer = targetLayer();
  if ( !layer )
    return false;

  const QgsPointXY mapPoint = toMapCoordinates( layer, layerPoint );
  const double radius = searchRadiusMU( mCanvas );
  const QgsPoint excluded( excludedPos );

  bool found = false;
  double best = radius;
  QgsFeatureIterator fit = layer->getFeatures();
  QgsFeature feature;
  while ( fit.nextFeature( feature ) )
  {
    if ( !feature.hasGeometry() )
      continue;
    const QgsGeometry geometry = feature.geometry();
    int nr = 0;
    QgsVertexId vid;
    while ( geometry.vertexIdFromVertexNr( nr, vid ) )
    {
      const QgsPoint pt = geometry.constGet()->vertexAt( vid );
      ++nr;
      if ( sameXy( pt, excluded ) )
        continue; // the coincident stack being dragged — welding to self is noise
      const double dist = mapPoint.distance( toMapCoordinates( layer, QgsPointXY( pt.x(), pt.y() ) ) );
      if ( dist <= best )
      {
        best = dist;
        nearestPos = QgsPointXY( pt.x(), pt.y() );
        found = true;
      }
    }
  }
  return found;
}

bool PaleoVertexTool::findNearestVertex( const QgsPointXY &layerPoint, qint64 &fid, int &vertexNr )
{
  QgsVectorLayer *layer = targetLayer();
  if ( !layer )
    return false;

  // Tolerance chain (all native): QgsMapTool::searchRadiusMU(canvas) converts
  // searchRadiusMM() → pixels via the output DPI → map units via the canvas'
  // current map-units-per-pixel. Hit distances are measured in map space
  // (event and vertices both pushed through toMapCoordinates) so the radius is
  // exact even when layer and canvas CRS differ — no hand-rolled scale math.
  const QgsPointXY mapPoint = toMapCoordinates( layer, layerPoint );
  const double radius = searchRadiusMU( mCanvas );

  bool found = false;
  double best = radius;
  const QgsFeatureIds fids = layer->selectedFeatureIds();
  for ( QgsFeatureId featureId : fids )
  {
    const QgsFeature feature = layer->getFeature( featureId );
    if ( !feature.hasGeometry() )
      continue;

    const QgsGeometry geometry = feature.geometry();
    int nr = 0;
    QgsVertexId vid;
    while ( geometry.vertexIdFromVertexNr( nr, vid ) )
    {
      const QgsPoint pt = geometry.constGet()->vertexAt( vid );
      const double dist = mapPoint.distance( toMapCoordinates( layer, QgsPointXY( pt.x(), pt.y() ) ) );
      if ( dist <= best )
      {
        best = dist;
        fid = featureId;
        vertexNr = nr;
        found = true;
      }
      ++nr;
    }
  }
  return found;
}

bool PaleoVertexTool::findSegmentInsertion( const QgsPointXY &layerPoint, qint64 &fid, int &beforeVertex, QgsPointXY &onSegment )
{
  QgsVectorLayer *layer = targetLayer();
  if ( !layer )
    return false;

  // Native segment projection per selected feature: closestSegmentWithContext
  // returns the nearest on-segment point and the vertex index AFTER the
  // closest segment (insertion happens before that vertex). Its return value
  // is a squared distance — negative on error (e.g. point geometries).
  const QgsPointXY mapPoint = toMapCoordinates( layer, layerPoint );
  const double radius = searchRadiusMU( mCanvas );

  bool found = false;
  double best = radius;
  const QgsFeatureIds fids = layer->selectedFeatureIds();
  for ( QgsFeatureId featureId : fids )
  {
    const QgsFeature feature = layer->getFeature( featureId );
    if ( !feature.hasGeometry() )
      continue;

    QgsPointXY segmentPoint;
    int nextVertex = -1;
    const QgsGeometry geometry = feature.geometry();
    if ( geometry.closestSegmentWithContext( layerPoint, segmentPoint, nextVertex ) < 0 || nextVertex < 0 )
      continue;

    // Compare in map space (see findNearestVertex) — the projection itself
    // stays native, only the tolerance gate is CRS-safe.
    const double dist = mapPoint.distance( toMapCoordinates( layer, segmentPoint ) );
    if ( dist <= best )
    {
      best = dist;
      fid = featureId;
      beforeVertex = nextVertex;
      onSegment = segmentPoint;
      found = true;
    }
  }
  return found;
}
