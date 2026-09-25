#include "ui/edittools/vertexeditortools.h"

#include <QKeyEvent>

#include <qgsfeature.h>
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
} // namespace

// One in-flight vertex drag: the dragged vertex identified by (feature id,
// vertex number), the feature's pre-drag geometry (QgsGeometry copies are deep,
// so this is the sanctioned "clone current geometry → mutate → commit" clone),
// the highlighted marker (canvas-owned, tracked for color/center restore) and
// the rubber band previewing the whole translated geometry.
struct PaleoVertexTool::DragState
{
  qint64 fid = -1;
  int vertexNr = -1;
  QgsGeometry originalGeometry;
  QgsVertexMarker *marker = nullptr; // not owned (canvas-parented)
  QgsRubberBand *previewBand = nullptr; // not owned (canvas-parented)
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
  mDraggingVertex->marker = markerForVertex( mMarkers, fid, vertexNr );
  if ( mDraggingVertex->marker )
  {
    mDraggingVertex->marker->setColor( dragMarkerColor() );
    mDraggingVertex->marker->setIconSize( MARKER_ICON_SIZE + 4 );
  }
  mDraggingVertex->previewBand = createRubberBand( layer->geometryType() );

  // Prime the preview with the (so far zero-length) move, same math as move.
  QgsGeometry preview = mDraggingVertex->originalGeometry;
  const QgsPointXY lp = toLayerCoordinates( layer, e->mapPoint() );
  if ( preview.moveVertex( lp.x(), lp.y(), mDraggingVertex->vertexNr ) )
    mDraggingVertex->previewBand->setToGeometry( preview, layer );
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
    QgsGeometry preview = mDraggingVertex->originalGeometry;
    const QgsPointXY lp = toLayerCoordinates( layer, e->mapPoint() );
    if ( preview.moveVertex( lp.x(), lp.y(), mDraggingVertex->vertexNr ) )
      mDraggingVertex->previewBand->setToGeometry( preview, layer );
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
    QgsVertexId vid;
    if ( !geometry.vertexIdFromVertexNr( vertexNr, vid ) || !geometry.constGet() )
      return;
    const Qgis::GeometryType geometryType = QgsWkbTypes::geometryType( geometry.wkbType() );
    const int ringVertices = geometry.constGet()->vertexCount( vid.part, vid.ring );
    const int minimum = geometryType == Qgis::GeometryType::Line ? 3
                        : geometryType == Qgis::GeometryType::Polygon ? 5
                        : 2;
    if ( ringVertices < minimum )
    {
      emit messageEmitted( tr( "Cannot delete vertex: the feature would become invalid" ), Qgis::MessageLevel::Warning );
      return;
    }

    QgsGeometry mutated = geometry;
    layer->beginEditCommand( tr( "Deleted vertex" ) );
    if ( mutated.deleteVertex( vertexNr ) && layer->changeGeometry( fid, mutated ) )
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
  const QgsPointXY releasePoint = toLayerCoordinates( layer, e->mapPoint() );
  QgsGeometry geometry = mDraggingVertex->originalGeometry;

  layer->beginEditCommand( tr( "Moved vertex" ) );
  if ( geometry.moveVertex( releasePoint.x(), releasePoint.y(), mDraggingVertex->vertexNr )
       && layer->changeGeometry( mDraggingVertex->fid, geometry ) )
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

  QgsGeometry geometry = feature.geometry();
  layer->beginEditCommand( tr( "Added vertex" ) );
  if ( geometry.insertVertex( onSegment.x(), onSegment.y(), beforeVertex )
       && layer->changeGeometry( fid, geometry ) )
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

  if ( mDraggingVertex->previewBand )
  {
    delete mDraggingVertex->previewBand; // canvas-parented; remove explicitly
    mDraggingVertex->previewBand = nullptr;
  }
  if ( mDraggingVertex->marker )
  {
    mDraggingVertex->marker->setColor( idleMarkerColor() );
    mDraggingVertex->marker->setIconSize( MARKER_ICON_SIZE );
    mDraggingVertex->marker = nullptr;
  }
  delete mDraggingVertex;
  mDraggingVertex = nullptr;
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
