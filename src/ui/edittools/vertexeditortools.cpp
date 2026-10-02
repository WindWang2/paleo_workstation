// 层：视图
#include "ui/edittools/vertexeditortools.h"

#include "qgis/topologicalindex.h"

#include <map>
#include <tuple>

#include <QKeyEvent>
#include <QHash>

#include <qgis.h>
#include <qgsfeature.h>
#include <qgsfeatureiterator.h>
#include <qgsgeometry.h>
#include <qgsmapcanvas.h>
#include <qgsmapmouseevent.h>
#include <qgspointlocator.h>
#include <qgsproject.h>
#include <qgsrectangle.h>
#include <qgsrubberband.h>
#include <qgssnapindicator.h>
#include <qgssnappingutils.h>
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

// Returns the matching closure vertex number if nr is the start or end of a closed ring; otherwise -1.
int matchingClosureVertex( const QgsGeometry &g, int nr )
{
  QgsVertexId vid;
  if ( !g.constGet() || !g.vertexIdFromVertexNr( nr, vid ) )
    return -1;
  const int ringVertices = g.constGet()->vertexCount( vid.part, vid.ring );
  if ( ringVertices < 2 )
    return -1;

  if ( vid.vertex == 0 || vid.vertex == ringVertices - 1 )
  {
    QgsVertexId firstVid( vid.part, vid.ring, 0 );
    QgsVertexId lastVid( vid.part, vid.ring, ringVertices - 1 );
    const QgsPoint ptFirst = g.constGet()->vertexAt( firstVid );
    const QgsPoint ptLast = g.constGet()->vertexAt( lastVid );
    if ( sameXy( ptFirst, ptLast ) )
    {
      const QgsVertexId otherVid = ( vid.vertex == 0 ) ? lastVid : firstVid;
      return g.vertexNrFromVertexId( otherVid );
    }
  }
  return -1;
}

// Determines if nr is a closure vertex (start 0 or end N) of a polygon ring.
bool isPolygonClosureVertex( const QgsGeometry &g, int nr, int &part, int &ring, int &startNr, int &endNr )
{
  if ( QgsWkbTypes::geometryType( g.wkbType() ) != Qgis::GeometryType::Polygon )
    return false;
  if ( !g.constGet() )
    return false;
  QgsVertexId vid;
  if ( !g.vertexIdFromVertexNr( nr, vid ) )
    return false;
  const int count = g.constGet()->vertexCount( vid.part, vid.ring );
  if ( count < 2 )
    return false;
  if ( vid.vertex == 0 || vid.vertex == count - 1 )
  {
    part = vid.part;
    ring = vid.ring;
    startNr = g.vertexNrFromVertexId( QgsVertexId( vid.part, vid.ring, 0 ) );
    endNr = g.vertexNrFromVertexId( QgsVertexId( vid.part, vid.ring, count - 1 ) );
    return true;
  }
  return false;
}

struct RingKey
{
  QgsVectorLayer *layer = nullptr;
  qint64 fid = -1;
  int part = 0;
  int ring = 0;
  bool operator==( const RingKey &o ) const
  {
    return layer == o.layer && fid == o.fid && part == o.part && ring == o.ring;
  }
};
inline size_t qHash( const RingKey &k, size_t seed = 0 )
{
  return qHashMulti( seed, k.layer, k.fid, k.part, k.ring );
}
} // namespace

// One in-flight vertex drag. The grabbed vertex is (fid, vertexNr) on the
// target layer; in topological mode `coincident` lists every vertex sharing
// its position (self included; cross-layer members point at their own layer).
// Per-feature state is keyed by (layer, fid) so a single feature carrying
// several coincident vertices moves them all on one clone — and cross-layer
// members keep their originals alongside the grabbed feature's.
struct PaleoVertexTool::DragState
{
  qint64 fid = -1;                          // grabbed feature (target layer)
  int vertexNr = -1;
  QgsGeometry originalGeometry;             // grabbed feature's pre-drag clone
  QgsVertexMarker *marker = nullptr;        // not owned (canvas-parented)
  QgsRubberBand *previewBand = nullptr;     // grabbed feature's preview band
  QgsPointXY grabPos;                       // grabbed vertex, layer CRS
  QList<CoincidentMember> coincident;       // write set ({self} when topo off)
  // layer → (fid → pre-drag clone)；目标层与跨层参与层共用一张表。
  QHash<QgsVectorLayer *, QHash<qint64, QgsGeometry>> originalByFid;
  QHash<QgsVectorLayer *, QHash<qint64, QgsRubberBand *>> previewBands; // canvas-parented
  QList<QgsVertexMarker *> topoMarkers;     // red markers for coincident
                                            // vertices outside the selection
  QList<QMetaObject::Connection> guardConnections; // 跨层参与层的 destroyed
                                                    // 守卫（clearDragState 断开）
};

PaleoVertexTool::PaleoVertexTool( QgsMapCanvas *canvas, QgsVectorLayer *layer )
  : QgsMapToolEdit( canvas )
  , mLayer( layer )
{
  setToolName( tr( "编辑节点" ) );
  setCursor( QCursor( Qt::CrossCursor ) );
  mSnapIndicator = std::make_unique<QgsSnapIndicator>( canvas );
}

PaleoVertexTool::~PaleoVertexTool()
{
  clearDragState();
  clearMarkers();
}

void PaleoVertexTool::activate()
{
  // A null ctor layer defers to the canvas' current layer until here.
  if ( !mLayer )
    mLayer = qobject_cast<QgsVectorLayer *>( mCanvas->currentLayer() );

  // Live marker rebuild on selection changes, undo/redo, rollback, and geometry mutations.
  if ( mLayer )
  {
    disconnect( mLayer.data(), &QgsVectorLayer::selectionChanged, this, nullptr );
    disconnect( mLayer.data(), &QgsVectorLayer::geometryChanged, this, nullptr );
    disconnect( mLayer.data(), &QgsMapLayer::layerModified, this, nullptr );
    disconnect( mLayer.data(), &QgsVectorLayer::afterRollBack, this, nullptr );
    disconnect( mLayer.data(), &QObject::destroyed, this, nullptr );

    connect( mLayer.data(), &QgsVectorLayer::selectionChanged, this, [this]
    {
      if ( mCanvas->mapTool() == this )
        rebuildMarkers();
    } );

    auto onExternalChange = [this]
    {
      if ( mCanvas->mapTool() == this && !mCommitting )
      {
        if ( mDraggingVertex )
          clearDragState();
        rebuildMarkers();
      }
    };

    connect( mLayer.data(), &QgsVectorLayer::geometryChanged, this, [onExternalChange]( QgsFeatureId, const QgsGeometry & ) {
      onExternalChange();
    } );
    connect( mLayer.data(), &QgsMapLayer::layerModified, this, onExternalChange );
    connect( mLayer.data(), &QgsVectorLayer::afterRollBack, this, onExternalChange );

    if ( QUndoStack *stack = mLayer->undoStack() )
    {
      disconnect( stack, &QUndoStack::indexChanged, this, nullptr );
      connect( stack, &QUndoStack::indexChanged, this, [onExternalChange]( int ) {
        onExternalChange();
      } );
    }

    connect( mLayer.data(), &QObject::destroyed, this, [this] {
      mLayer = nullptr;
      clearDragState();
      clearMarkers();
    } );
  }

  QgsMapToolEdit::activate();
  rebuildMarkers();
}

void PaleoVertexTool::deactivate()
{
  clearDragState();
  clearMarkers();
  if ( mSnapIndicator )
    mSnapIndicator->setMatch( QgsPointLocator::Match() ); // invalid → hidden
  QgsMapToolEdit::deactivate();
}

QgsVectorLayer *PaleoVertexTool::targetLayer() const
{
  return mLayer.data();
}

QList<QgsVertexMarker *> PaleoVertexTool::topoMarkers() const
{
  return mDraggingVertex ? mDraggingVertex->topoMarkers : QList<QgsVertexMarker *>();
}

// ---------------------------------------------------------------------------
// Canvas gestures
// ---------------------------------------------------------------------------

void PaleoVertexTool::canvasPressEvent( QgsMapMouseEvent *e )
{
  if ( e->button() != Qt::LeftButton )
    return; // right-button delete commits on release; other buttons ignored

  // Native snapping first — mapPoint() becomes the snapped position (see
  // header notes); the indicator shows what the grab/drop will bind to.
  e->snapPoint();
  mSnapIndicator->setMatch( e->mapPointMatch() );

  QgsVectorLayer *layer = targetLayer();
  if ( !layer || !layer->isEditable() )
  {
    emit messageEmitted( tr( "请先开始编辑，再移动节点" ), Qgis::MessageLevel::Warning );
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
  refreshTopoIndex(); // gesture start: re-scope the R-tree (lazy rebuild)

  mDraggingVertex = new DragState;
  mDraggingVertex->fid = fid;
  mDraggingVertex->vertexNr = vertexNr;
  mDraggingVertex->originalGeometry = feature.geometry(); // deep copy = pre-drag clone
  mDraggingVertex->grabPos = vertexXy( feature.geometry(), vertexNr );
  mDraggingVertex->coincident.append( { layer, fid, vertexNr } );
  const int closureNr = matchingClosureVertex( feature.geometry(), vertexNr );
  if ( closureNr >= 0 && closureNr != vertexNr )
    mDraggingVertex->coincident.append( { layer, fid, closureNr } );

  mDraggingVertex->originalByFid[layer].insert( fid, feature.geometry() );
  mDraggingVertex->marker = markerForVertex( mMarkers, fid, vertexNr );
  if ( mDraggingVertex->marker )
  {
    mDraggingVertex->marker->setColor( dragMarkerColor() );
    mDraggingVertex->marker->setIconSize( MARKER_ICON_SIZE + 4 );
  }
  mDraggingVertex->previewBand = createRubberBand( layer->geometryType() );
  mDraggingVertex->previewBands[layer].insert( fid, mDraggingVertex->previewBand );

  // Topological editing: every vertex sharing the grabbed position joins the
  // write set — coincident members on UNSELECTED features (and, cross-layer,
  // on neighbor layers) get a red marker so the shared-boundary move is
  // visible before commit.
  if ( mTopoEditing )
  {
    const QgsFeatureIds selected = layer->selectedFeatureIds();
    for ( const CoincidentMember &member : coincidentVertices( mDraggingVertex->grabPos ) )
    {
      bool known = false; // self + closure members are already in the set
      for ( const CoincidentMember &c : std::as_const( mDraggingVertex->coincident ) )
        if ( c == member )
        {
          known = true;
          break;
        }
      if ( known )
        continue;
      mDraggingVertex->coincident.append( member );
      if ( !mDraggingVertex->originalByFid.value( member.layer ).contains( member.fid ) )
      {
        const QgsFeature other = member.layer->getFeature( member.fid );
        if ( other.hasGeometry() )
          mDraggingVertex->originalByFid[member.layer].insert( member.fid, other.geometry() );
      }
      const bool inSelection =
          member.layer == layer && selected.contains( member.fid );
      if ( !inSelection )
      {
        const QgsPointXY mp = vertexXy( mDraggingVertex->originalByFid.value( member.layer ).value( member.fid ),
                                        member.vertexNr );
        QgsVertexMarker *marker = new QgsVertexMarker( mCanvas );
        marker->setIconType( QgsVertexMarker::ICON_CIRCLE );
        marker->setColor( dragMarkerColor() );
        marker->setIconSize( MARKER_ICON_SIZE );
        marker->setPenWidth( MARKER_PEN_WIDTH );
        marker->setCenter( toMapCoordinates( member.layer, mp ) );
        mDraggingVertex->topoMarkers.append( marker );
      }
      if ( !mDraggingVertex->previewBands.value( member.layer ).contains( member.fid ) )
        mDraggingVertex->previewBands[member.layer].insert(
            member.fid, createRubberBand( member.layer->geometryType() ) );
      // 跨层参与层：拖拽途中（按住鼠标时事件循环仍在跑）被析构 → 丢弃整个
      // 拖拽，绝不带着悬空层指针走到 release 提交（目标层由 activate() 接线）。
      if ( member.layer != layer )
        mDraggingVertex->guardConnections.append(
            connect( member.layer, &QObject::destroyed, this, [this] {
              clearDragState();
              rebuildMarkers();
            } ) ); // NB: 无 UniqueConnection——Qt6 对 functor+Unique 静默拒绝
    }
  }

  for ( const CoincidentMember &member : std::as_const( mDraggingVertex->coincident ) )
  {
    if ( member.layer != layer )
      continue; // idle markers exist for target-layer members only
    QgsVertexMarker *m = markerForVertex( mMarkers, member.fid, member.vertexNr );
    if ( m && m != mDraggingVertex->marker )
    {
      m->setColor( dragMarkerColor() );
      m->setIconSize( MARKER_ICON_SIZE + 4 );
    }
  }

  // Prime the previews with the (so far zero-length) move, same math as move.
  updateDragPreviews( e->mapPoint() );
}

void PaleoVertexTool::updateDragPreviews( const QgsPointXY &mapPoint )
{
  for ( auto lit = mDraggingVertex->originalByFid.begin();
        lit != mDraggingVertex->originalByFid.end(); ++lit )
  {
    QgsVectorLayer *writeLayer = lit.key();
    const QgsPointXY lp = toLayerCoordinates( writeLayer, mapPoint );
    for ( auto fit = lit.value().begin(); fit != lit.value().end(); ++fit )
    {
      QgsGeometry preview = fit.value();
      for ( const CoincidentMember &member : std::as_const( mDraggingVertex->coincident ) )
        if ( member.layer == writeLayer && member.fid == fit.key() )
          preview.moveVertex( lp.x(), lp.y(), member.vertexNr );
      mDraggingVertex->previewBands.value( writeLayer ).value( fit.key() )->setToGeometry( preview, writeLayer );
    }
  }
}

void PaleoVertexTool::canvasMoveEvent( QgsMapMouseEvent *e )
{
  // Hover and drag alike: refresh the snap match so mapPoint() is snapped
  // and the indicator follows the cursor (upstream vertex-tool behavior).
  e->snapPoint();
  mSnapIndicator->setMatch( e->mapPointMatch() );

  if ( !mDraggingVertex )
    return;

  // mapPoint() and marker centers are both map CRS — follow directly.
  if ( mDraggingVertex->marker )
    mDraggingVertex->marker->setCenter( e->mapPoint() );

  for ( QgsVertexMarker *tm : std::as_const( mDraggingVertex->topoMarkers ) )
  {
    if ( tm )
      tm->setCenter( e->mapPoint() );
  }

  for ( const CoincidentMember &member : std::as_const( mDraggingVertex->coincident ) )
  {
    if ( member.layer != targetLayer() )
      continue;
    if ( QgsVertexMarker *m = markerForVertex( mMarkers, member.fid, member.vertexNr ) )
      m->setCenter( e->mapPoint() );
  }

  updateDragPreviews( e->mapPoint() );
}

// Right-button release and Delete/Backspace share this batch delete (single
// entry point so the keyboard path can never drift from the mouse path).
void PaleoVertexTool::deleteVertexAtMapPoint( const QgsPointXY &mapPoint )
{
    QgsVectorLayer *layer = targetLayer();
    if ( !layer || !layer->isEditable() )
    {
      emit messageEmitted( tr( "请先开始编辑，再删除节点" ), Qgis::MessageLevel::Warning );
      return;
    }

    const QgsPointXY layerPoint = toLayerCoordinates( layer, mapPoint );
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
      emit messageEmitted( tr( "无法删除节点：要素将变为无效" ), Qgis::MessageLevel::Warning );
      return;
    }

    // Topological editing: collect every vertex coincident with the target —
    // the batch is refused whole when any member's own ring would break.
    refreshTopoIndex(); // gesture start: re-scope the R-tree (lazy rebuild)
    QList<CoincidentMember> writeSet;
    if ( mTopoEditing )
    {
      writeSet = coincidentVertices( vertexXy( geometry, vertexNr ) );
    }
    else
    {
      writeSet.append( { layer, fid, vertexNr } );
    }

    // De-duplicate polygon closure vertices in writeSet:
    // If both 0 and N of the same ring are present, drop N.
    // If only N is present, replace it with 0.
    // This ensures only 1 logical vertex count is deleted per ring.
    QList<CoincidentMember> dedupedWriteSet;
    QHash<QPair<QgsVectorLayer *, qint64>, QSet<QPair<int, int>>> closureRingsHandled;

    for ( const CoincidentMember &member : std::as_const( writeSet ) )
    {
      const QgsFeature feat = member.layer->getFeature( member.fid );
      if ( !feat.hasGeometry() || !feat.geometry().constGet() )
        continue;
      int part = -1, ring = -1, startNr = -1, endNr = -1;
      if ( isPolygonClosureVertex( feat.geometry(), member.vertexNr, part, ring, startNr, endNr ) )
      {
        const auto ringKey = qMakePair( member.layer, member.fid );
        const auto partRing = qMakePair( part, ring );
        if ( !closureRingsHandled[ringKey].contains( partRing ) )
        {
          closureRingsHandled[ringKey].insert( partRing );
          dedupedWriteSet.append( { member.layer, member.fid, startNr } );
        }
      }
      else
      {
        dedupedWriteSet.append( member );
      }
    }
    writeSet = dedupedWriteSet;

    // Group deletion quota by (layer, fid, part, ring) to prevent multiple
    // coincident deletions from collapsing a ring below its minimum vertex count.
    QHash<RingKey, int> deleteCountPerRing;
    for ( const CoincidentMember &member : std::as_const( writeSet ) )
    {
      const QgsFeature feat = member.layer->getFeature( member.fid );
      if ( !feat.hasGeometry() || !feat.geometry().constGet() )
        continue;
      QgsVertexId vid;
      if ( feat.geometry().vertexIdFromVertexNr( member.vertexNr, vid ) )
      {
        deleteCountPerRing[{ member.layer, member.fid, vid.part, vid.ring }]++;
      }
    }

    for ( const CoincidentMember &member : std::as_const( writeSet ) )
    {
      const QgsFeature other = member.layer->getFeature( member.fid );
      if ( !other.hasGeometry() || !other.geometry().constGet() )
      {
        emit messageEmitted( tr( "无法删除节点：共边要素将变为无效" ),
                             Qgis::MessageLevel::Warning );
        return;
      }
      QgsVertexId vid;
      if ( !other.geometry().vertexIdFromVertexNr( member.vertexNr, vid ) )
      {
        emit messageEmitted( tr( "无法删除节点：共边要素将变为无效" ),
                             Qgis::MessageLevel::Warning );
        return;
      }
      const int k = deleteCountPerRing.value( { member.layer, member.fid, vid.part, vid.ring }, 1 );
      const int ringVertices = other.geometry().constGet()->vertexCount( vid.part, vid.ring );
      const Qgis::GeometryType gt = QgsWkbTypes::geometryType( other.geometry().wkbType() );
      const int minReq = ( gt == Qgis::GeometryType::Polygon ) ? ( 4 + k )
                         : ( gt == Qgis::GeometryType::Line ) ? ( 2 + k )
                         : ( 1 + k );
      if ( ringVertices < minReq )
      {
        emit messageEmitted( tr( "无法删除节点：共边要素将变为无效" ),
                             Qgis::MessageLevel::Warning );
        return;
      }
    }

    mCommitting = true;
    // One edit command per touched layer (native undo stacks are per-layer;
    // cross-layer gestures undo layer by layer — see header notes).
    mCommitting = true;
    // One edit command per touched layer (native undo stacks are per-layer;
    // cross-layer gestures undo layer by layer — see header notes).
    QHash<QgsVectorLayer *, QList<CoincidentMember>> byLayer;
    for ( const CoincidentMember &member : std::as_const( writeSet ) )
      byLayer[member.layer].append( member );
    bool anyCommitted = false;
    bool anyFailed = false;
    for ( auto bl = byLayer.begin(); bl != byLayer.end(); ++bl )
    {
      QgsVectorLayer *writeLayer = bl.key();
      if ( !writeLayer->isEditable() )
      {
        // Mid-gesture session loss on a participant: drop its members, keep
        // the rest of the batch.
        anyFailed = true;
        continue;
      }
      writeLayer->beginEditCommand( tr( "删除节点" ) );
      // Group per fid: several coincident vertices on ONE feature must delete
      // in descending vertex order — dense numbering shifts as vertices drop.
      QHash<qint64, QList<int>> byFid;
      for ( const CoincidentMember &member : std::as_const( bl.value() ) )
        byFid[member.fid].append( member.vertexNr );
      bool layerOk = true;
      for ( auto it = byFid.begin(); it != byFid.end() && layerOk; ++it )
      {
        QgsGeometry mutated = writeLayer->getFeature( it.key() ).geometry();
        QList<int> nrs = it.value();
        std::sort( nrs.begin(), nrs.end(), std::greater<int>() );

        // Track which polygon rings had their closure vertex deleted so we restore closure afterwards.
        QList<QPair<int, int>> closedRingsTouched;
        if ( QgsWkbTypes::geometryType( mutated.wkbType() ) == Qgis::GeometryType::Polygon && mutated.constGet() )
        {
          for ( const int nr : std::as_const( nrs ) )
          {
            int part = -1, ring = -1, startNr = -1, endNr = -1;
            if ( isPolygonClosureVertex( mutated, nr, part, ring, startNr, endNr ) )
              closedRingsTouched.append( qMakePair( part, ring ) );
          }
        }

        for ( const int nr : std::as_const( nrs ) )
          if ( !mutated.deleteVertex( nr ) )
            layerOk = false;

        // Ensure each affected polygon ring maintains valid closure
        if ( layerOk && mutated.constGet() )
        {
          for ( const auto &pr : std::as_const( closedRingsTouched ) )
          {
            const int part = pr.first;
            const int ring = pr.second;
            const int remainingCount = mutated.constGet()->vertexCount( part, ring );
            if ( remainingCount >= 2 )
            {
              const QgsPoint newStart = mutated.constGet()->vertexAt( QgsVertexId( part, ring, 0 ) );
              const int closingNr = mutated.vertexNrFromVertexId( QgsVertexId( part, ring, remainingCount - 1 ) );
              if ( closingNr >= 0 )
                mutated.moveVertex( newStart.x(), newStart.y(), closingNr );
            }
          }
        }

        // Defense-in-depth: verify all rings in mutated geometry satisfy minimum counts
        if ( layerOk && mutated.constGet() )
        {
          const Qgis::GeometryType gt = QgsWkbTypes::geometryType( mutated.wkbType() );
          const int minAllowed = ( gt == Qgis::GeometryType::Polygon ) ? 4
                                 : ( gt == Qgis::GeometryType::Line ) ? 2
                                 : 1;
          for ( int p = 0; p < mutated.constGet()->partCount(); ++p )
          {
            for ( int r = 0; r < mutated.constGet()->ringCount( p ); ++r )
            {
              if ( mutated.constGet()->vertexCount( p, r ) < minAllowed )
              {
                layerOk = false;
                break;
              }
            }
            if ( !layerOk )
              break;
          }
        }

        if ( layerOk && !writeLayer->changeGeometry( it.key(), mutated ) )
          layerOk = false;
      }
      if ( layerOk )
      {
        writeLayer->endEditCommand();
        ++mEditedCount;
        emit featureEdited( writeLayer->id() );
        anyCommitted = true;
      }
      else
      {
        writeLayer->destroyEditCommand();
        anyFailed = true;
      }
    }
    if ( anyFailed )
    {
      emit messageEmitted( anyCommitted
                               ? tr( "部分图层拒绝了节点删除" )
                               : tr( "无法删除节点" ),
                           Qgis::MessageLevel::Warning );
    }
    mCommitting = false;

    rebuildMarkers();
    return;}

void PaleoVertexTool::canvasReleaseEvent( QgsMapMouseEvent *e )
{
  e->snapPoint();
  mSnapIndicator->setMatch( e->mapPointMatch() );

  // Right-button: delete the vertex under the cursor (QGIS gesture convention:
  // the delete fires on release, not press).
  if ( e->button() == Qt::RightButton )
  {
    deleteVertexAtMapPoint( e->mapPoint() );
    return;
  }

  if ( e->button() != Qt::LeftButton || !mDraggingVertex )
    return;

  QgsVectorLayer *layer = targetLayer();
  if ( !layer || !layer->isEditable() )
  {
    // The layer was editable when the drag armed but not anymore — drop the
    // drag rather than commit into a missing edit buffer.
    emit messageEmitted( tr( "编辑已停止——节点移动已丢弃" ), Qgis::MessageLevel::Warning );
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

  mCommitting = true;
  // Welded release position (target layer CRS) → map CRS once; per write
  // layer it converts through its own (same-CRS, participation-gated)
  // transform, so member layers land on the exact same position.
  const QgsPointXY mapRelease = toMapCoordinates( layer, releasePoint );
  // One clone per touched feature; moveVertex keeps vertex numbering stable
  // so every coincident member applies against its own pre-drag geometry.
  // One edit command per layer (see header notes: native undo is per-layer).
  bool anyCommitted = false;
  bool anyFailed = false;
  for ( auto lit = mDraggingVertex->originalByFid.begin();
        lit != mDraggingVertex->originalByFid.end(); ++lit )
  {
    QgsVectorLayer *writeLayer = lit.key();
    if ( !writeLayer->isEditable() )
    {
      // The layer was editable when the drag armed but not anymore — drop
      // its members rather than commit into a missing edit buffer.
      anyFailed = true;
      continue;
    }
    const QgsPointXY layerRelease = toLayerCoordinates( writeLayer, mapRelease );
    writeLayer->beginEditCommand( tr( "移动节点" ) );
    bool layerOk = true;
    for ( auto fit = lit.value().begin(); fit != lit.value().end() && layerOk; ++fit )
    {
      QgsGeometry geometry = fit.value();
      for ( const CoincidentMember &member : std::as_const( mDraggingVertex->coincident ) )
        if ( member.layer == writeLayer && member.fid == fit.key()
             && !geometry.moveVertex( layerRelease.x(), layerRelease.y(), member.vertexNr ) )
          layerOk = false;
      if ( layerOk && !writeLayer->changeGeometry( fit.key(), geometry ) )
        layerOk = false;
    }
    if ( layerOk )
    {
      writeLayer->endEditCommand();
      ++mEditedCount;
      emit featureEdited( writeLayer->id() );
      anyCommitted = true;
    }
    else
    {
      writeLayer->destroyEditCommand();
      anyFailed = true;
    }
  }
  if ( anyFailed )
  {
    emit messageEmitted( anyCommitted
                             ? tr( "部分图层的编辑已停止——这些移动已丢弃" )
                             : tr( "无法移动节点" ),
                         Qgis::MessageLevel::Warning );
  }
  mCommitting = false;

  clearDragState();
  rebuildMarkers(); // centers recomputed from the committed geometry
}

void PaleoVertexTool::canvasDoubleClickEvent( QgsMapMouseEvent *e )
{
  e->snapPoint();
  mSnapIndicator->setMatch( e->mapPointMatch() );

  QgsVectorLayer *layer = targetLayer();
  if ( !layer || !layer->isEditable() )
  {
    emit messageEmitted( tr( "请先开始编辑，再添加节点" ), Qgis::MessageLevel::Warning );
    return;
  }
  refreshTopoIndex(); // gesture start: re-scope the R-tree (lazy rebuild)

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
  // orientation (shared boundaries run opposite directions in the two rings);
  // cross-layer extends to same-CRS editable neighbors too. Candidates come
  // from the R-tree index (edgesNear), endpoint equality stays exact here.
  QList<CoincidentMember> inserts;
  inserts.append( { layer, fid, beforeVertex } );

  if ( mTopoEditing && beforeVertex >= 1 )
  {
    const QgsGeometry fg = feature.geometry();
    QgsVertexId va, vb;
    if ( fg.constGet()
         && fg.vertexIdFromVertexNr( beforeVertex - 1, va )
         && fg.vertexIdFromVertexNr( beforeVertex, vb )
         && va.part == vb.part && va.ring == vb.ring )
    {
      const QgsPointXY segA = vertexXy( fg, beforeVertex - 1 );
      const QgsPointXY segB = vertexXy( fg, beforeVertex );
      for ( const CoincidentMember &member : sharedEdgeMembers( segA, segB, layer, fid, beforeVertex ) )
        inserts.append( member );
    }
  }

  mCommitting = true;
  // One edit command per touched layer; per fid the insertion indices apply
  // in descending order (dense numbering shifts per insert).
  QHash<QgsVectorLayer *, QList<CoincidentMember>> byLayer;
  for ( const CoincidentMember &member : std::as_const( inserts ) )
    byLayer[member.layer].append( member );
  bool anyCommitted = false;
  bool anyFailed = false;
  for ( auto bl = byLayer.begin(); bl != byLayer.end(); ++bl )
  {
    QgsVectorLayer *writeLayer = bl.key();
    if ( !writeLayer->isEditable() )
    {
      anyFailed = true;
      continue;
    }
    // onSegment is target-layer CRS; participation is same-CRS — direct.
    const QgsPointXY layerPt = toLayerCoordinates( writeLayer, toMapCoordinates( layer, onSegment ) );
    writeLayer->beginEditCommand( tr( "添加节点" ) );
    QHash<qint64, QList<int>> byFid;
    for ( const CoincidentMember &member : std::as_const( bl.value() ) )
      byFid[member.fid].append( member.vertexNr );
    bool layerOk = true;
    for ( auto it = byFid.begin(); it != byFid.end() && layerOk; ++it )
    {
      QgsGeometry geometry = writeLayer->getFeature( it.key() ).geometry();
      QList<int> nrs = it.value();
      std::sort( nrs.begin(), nrs.end(), std::greater<int>() ); // dense numbering shifts per insert
      for ( const int nr : std::as_const( nrs ) )
        if ( !geometry.insertVertex( layerPt.x(), layerPt.y(), nr ) )
          layerOk = false;
      if ( layerOk && !writeLayer->changeGeometry( it.key(), geometry ) )
        layerOk = false;
    }
    if ( layerOk )
    {
      writeLayer->endEditCommand();
      ++mEditedCount;
      emit featureEdited( writeLayer->id() );
      anyCommitted = true;
    }
    else
    {
      writeLayer->destroyEditCommand();
      anyFailed = true;
    }
  }
  if ( anyFailed )
  {
    emit messageEmitted( anyCommitted
                             ? tr( "部分图层拒绝了节点插入" )
                             : tr( "无法添加节点" ),
                         Qgis::MessageLevel::Warning );
  }
  mCommitting = false;

  rebuildMarkers();
}

void PaleoVertexTool::keyPressEvent( QKeyEvent *e )
{
  if ( ( e->key() == Qt::Key_Delete || e->key() == Qt::Key_Backspace ) && !mDraggingVertex )
  {
    // QGIS 节点工具惯例：键盘删除作用于光标下的节点（画布最后已知鼠标位置）。
    // 与鼠标路径一致先过吸附——命中时删除吸附目标而非裸坐标下的顶点。
    QgsPointXY p = toMapCoordinates( mCanvas->mouseLastXY() );
    const QgsPointLocator::Match match = mCanvas->snappingUtils()->snapToMap( p );
    if ( match.isValid() )
      p = match.point();
    deleteVertexAtMapPoint( p );
    e->accept();
    return;
  }
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
             && mDraggingVertex->originalGeometry.constGet()
             && mDraggingVertex->originalGeometry.vertexIdFromVertexNr( mDraggingVertex->vertexNr, vid ) )
        {
          const QgsPoint pt = mDraggingVertex->originalGeometry.constGet()->vertexAt( vid );
          mDraggingVertex->marker->setCenter( toMapCoordinates( layer, QgsPointXY( pt.x(), pt.y() ) ) );
        }
      }
      clearDragState();
      rebuildMarkers();
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

  // 先断守卫连接：destroyed 波里到达的第二个连接不得重入 clearDragState
  for ( const QMetaObject::Connection &c : std::as_const( mDraggingVertex->guardConnections ) )
    disconnect( c );
  mDraggingVertex->guardConnections.clear();

  // previewBands contains previewBand itself — delete via the nested map to
  // avoid a double-free; topoMarkers are canvas-parented extras, delete directly.
  for ( auto lit = mDraggingVertex->previewBands.begin();
        lit != mDraggingVertex->previewBands.end(); ++lit )
    for ( QgsRubberBand *band : std::as_const( lit.value() ) )
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

void PaleoVertexTool::setCrossLayerTopologyEnabled( bool on )
{
  if ( mCrossLayerTopology == on )
    return;
  mCrossLayerTopology = on;
  // 同上：跨层开关中途翻转 → 写集参与面变了，丢弃在途拖拽防混合语义。
  clearDragState();
}

// ---------------------------------------------------------------------------
// Topological helpers (R-tree candidates via QgisTopologicalIndex — 主线2;
// exact XY equality stays qgsDoubleNear in layer coordinates, hit semantics
// unchanged from the former whole-layer linear scans)
// ---------------------------------------------------------------------------

void PaleoVertexTool::refreshTopoIndex()
{
  if ( !mTopoIndex )
    mTopoIndex = std::make_unique<QgisTopologicalIndex>( this );
  mTopoIndex->setLayers( targetLayer(),
                         mTopoEditing && mCrossLayerTopology
                             ? crossLayerParticipants()
                             : QList<QgsVectorLayer *>() );
}

QList<QgsVectorLayer *> PaleoVertexTool::crossLayerParticipants() const
{
  QList<QgsVectorLayer *> out;
  QgsVectorLayer *scope = targetLayer();
  // canvas 未显式 setProject 时 project() 为 null（无 instance 兜底）——
  // 回退进程级单例（测试常态；应用侧 canvas 已 setProject）。
  QgsProject *project = mCanvas ? mCanvas->project() : nullptr;
  if ( !project )
    project = QgsProject::instance();
  if ( !scope || !project )
    return out;
  const QgsCoordinateReferenceSystem crs = scope->crs();
  const QList<QgsVectorLayer *> layers = project->layers<QgsVectorLayer *>();
  for ( QgsVectorLayer *vl : layers )
  {
    if ( vl == scope || !vl->isEditable() || !vl->isSpatial() )
      continue;
    if ( !( vl->crs() == crs ) )
      continue; // 同 CRS 门槛：跨 CRS 共点没有无歧义的写路径
    out.append( vl );
  }
  return out;
}

double PaleoVertexTool::layerUnitRadius( QgsVectorLayer *layer, const QgsPointXY &mapPoint,
                                         double mapRadius )
{
  if ( layer->crs() == mCanvas->mapSettings().destinationCrs() )
    return mapRadius; // 常态：图层 CRS == 画布 CRS
  // 局部仿射探针：mapPoint 出发东/北各 1 map 单位折算层坐标长度取大者 ×2
  // 松弛——矩形只需包住候选，精确距离门仍在 map 空间（宁可多候选不丢候选）。
  const QgsPointXY o = toLayerCoordinates( layer, mapPoint );
  const QgsPointXY ex = toLayerCoordinates( layer, mapPoint + QgsVector( 1.0, 0.0 ) );
  const QgsPointXY ey = toLayerCoordinates( layer, mapPoint + QgsVector( 0.0, 1.0 ) );
  const double scale = std::max( o.distance( ex ), o.distance( ey ) );
  return mapRadius * 2.0 * std::max( scale, 1e-12 );
}

QList<PaleoVertexTool::CoincidentMember> PaleoVertexTool::coincidentVertices( const QgsPointXY &layerPoint )
{
  QList<CoincidentMember> out;
  QgsVectorLayer *layer = targetLayer();
  if ( !layer || !mTopoIndex )
    return out;

  // R-tree 邻域候选（1e-9 包络）→ 层坐标 qgsDoubleNear 精确过滤（同旧语义）。
  const QgsPoint target( layerPoint );
  const QList<QgisTopologicalIndex::VertexHit> hits =
      mTopoIndex->verticesNear( layerPoint, 1e-9, mTopoEditing && mCrossLayerTopology );
  for ( const QgisTopologicalIndex::VertexHit &hit : hits )
  {
    if ( !sameXy( QgsPoint( hit.pos ), target ) )
      continue;
    out.append( { hit.layer, hit.fid, hit.vertexNr } );
  }
  return out;
}

bool PaleoVertexTool::findNearestLayerVertex( const QgsPointXY &layerPoint,
                                              const QgsPointXY &excludedPos,
                                              QgsPointXY &nearestPos )
{
  QgsVectorLayer *layer = targetLayer();
  if ( !layer || !mTopoIndex )
    return false;

  const QgsPointXY mapPoint = toMapCoordinates( layer, layerPoint );
  const double radius = searchRadiusMU( mCanvas );
  const QgsPoint excluded( excludedPos );
  const double rectRadius = layerUnitRadius( layer, mapPoint, radius );

  bool found = false;
  double best = radius;
  const QList<QgisTopologicalIndex::VertexHit> hits =
      mTopoIndex->verticesNear( layerPoint, rectRadius, mTopoEditing && mCrossLayerTopology );
  for ( const QgisTopologicalIndex::VertexHit &hit : hits )
  {
    if ( sameXy( QgsPoint( hit.pos ), excluded ) )
      continue; // the coincident stack being dragged — welding to self is noise
    // 精确距离门在 map 空间（searchRadiusMU 随画布 DPI/比例尺，同旧语义）。
    const double dist = mapPoint.distance( toMapCoordinates( hit.layer, hit.pos ) );
    if ( dist <= best )
    {
      best = dist;
      nearestPos = hit.pos; // 命中层坐标（同 CRS 参与层，写集直接可用）
      found = true;
    }
  }
  return found;
}

QList<PaleoVertexTool::CoincidentMember> PaleoVertexTool::sharedEdgeMembers(
    const QgsPointXY &segA, const QgsPointXY &segB, QgsVectorLayer *hitLayer,
    qint64 hitFid, int beforeVertex )
{
  Q_UNUSED( beforeVertex ); // hit feature inserts via its own beforeVertex
  QList<CoincidentMember> out;
  if ( !mTopoIndex )
    return out;

  // 共享边矩形包络（端点 ±1e-9）：边的两端点必须精确等于 segA/segB（正/反
  // 两种走向——共享边界在两侧环里方向相反）。
  QgsRectangle rect( std::min( segA.x(), segB.x() ) - 1e-9, std::min( segA.y(), segB.y() ) - 1e-9,
                     std::max( segA.x(), segB.x() ) + 1e-9, std::max( segA.y(), segB.y() ) + 1e-9 );
  const QList<QgisTopologicalIndex::EdgeHit> hits =
      mTopoIndex->edgesNear( rect, mTopoEditing && mCrossLayerTopology );
  for ( const QgisTopologicalIndex::EdgeHit &hit : hits )
  {
    if ( hit.layer == hitLayer && hit.fid == hitFid )
      continue; // 双击命中的要素已按 beforeVertex 插入
    const bool forward = sameXy( QgsPoint( hit.p1 ), QgsPoint( segA ) )
                         && sameXy( QgsPoint( hit.p2 ), QgsPoint( segB ) );
    const bool reversed = sameXy( QgsPoint( hit.p1 ), QgsPoint( segB ) )
                          && sameXy( QgsPoint( hit.p2 ), QgsPoint( segA ) );
    if ( !forward && !reversed )
      continue;
    // 插入位置：在命中要素几何里精确定位 (p1→p2) 相邻顶点对，插在其后。
    //（Match::vertexIndex 的边语义实测为 firstVertex−1，跨几何类型不承诺，
    // 不依赖；回查只发生在索引筛出的单个命中要素上。）
    const QgsGeometry og = hit.layer->getFeature( hit.fid ).geometry();
    if ( !og.constGet() )
      continue;
    const int total = og.constGet()->vertexCount();
    for ( int nr = 0; nr + 1 < total; ++nr )
    {
      QgsVertexId va, vb;
      if ( !og.vertexIdFromVertexNr( nr, va ) || !og.vertexIdFromVertexNr( nr + 1, vb ) )
        break;
      if ( va.part != vb.part || va.ring != vb.ring )
        continue; // 不跨 part/ring 的相邻对才算边
      const QgsPoint pa = og.constGet()->vertexAt( va );
      const QgsPoint pb = og.constGet()->vertexAt( vb );
      if ( sameXy( pa, QgsPoint( hit.p1 ) ) && sameXy( pb, QgsPoint( hit.p2 ) ) )
      {
        out.append( { hit.layer, hit.fid, nr + 1 } ); // 插在边的第二顶点前
        break;
      }
    }
  }
  return out;
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
    if ( !geometry.constGet() )
      continue;
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
    if ( !geometry.constGet() )
      continue;
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
