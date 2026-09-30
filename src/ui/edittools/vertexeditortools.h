// 层：视图
#pragma once
#include <qgsmaptooledit.h>
#include <QPointer>
#include <QString>
#include <QList>
#include <memory>

class QgsMapCanvas;
class QgsVectorLayer;
class QgsMapMouseEvent;
class QgsSnapIndicator;
class QKeyEvent;
class QgisTopologicalIndex;

// ui/edittools/ — vertex editing, real implementation next to the
// PaleoVertexEditorShim placeholder (src/ui/vertexeditorshim.*).
//
// QgsVertexTool evidence (QGIS 4.2.2, this build):
//   · /usr/include/qgis/ has NO qgsvertextool.h (only qgsvertexmarker.h);
//   · nm -D /usr/lib/libqgis_gui.so | grep -c QgsVertexTool → 0 — the class
//     is not exported by gui at all;
//   · the symbols live in libqgis_app.so (APP_EXPORT, header not installed),
//     same audit verdict as vertexeditorshim.h documents for QgsVertexEditor.
// Sanctioned degrade: PaleoVertexTool = QgsMapToolEdit + native visual
// primitives (QgsVertexMarker per vertex, QgsMapToolEdit::createRubberBand
// for the drag preview) + native geometry mutators through the edit buffer.
// All edits are beginEditCommand/endEditCommand groups → native undo.
//
// Gestures (on the target layer's SELECTED features):
//   · press within search radius of a vertex → drag it; release commits
//     moveVertex in one edit command;
//   · double-click near a feature's segment → insertVertex at the nearest
//     on-segment point (closestSegmentWithContext — native);
//   · right-click within search radius of a vertex (or Delete/Backspace with
//     the cursor over one) → deleteVertex, refused
//     when it would drop a line below 2 vertices / a ring below 4 (closure
//     included) — QGIS geometry minimums, not Paleo math;
//   · Esc cancels an in-flight drag (or emits editAborted when idle — owner
//     tears the tool down, §42.15 pattern).
//
// Snapping: every canvas gesture first calls QgsMapMouseEvent::snapPoint(),
// so mapPoint() is the canvas snappingUtils-snapped position (vertex|segment,
// all layers — config installed by QgisCanvasController::nativeSnappingConfig)
// for grab hit-test, drag preview, commit and insert alike. A QgsSnapIndicator
// mirrors the event's match so the snapped target is visible while hovering
// and dragging — same affordance as upstream QgsVertexTool.
//
// Topological editing (QGIS QgsVertexTool semantics, same-layer scope):
// when enabled, vertex gestures apply to every vertex in the layer whose XY
// coincides with the grabbed one — the shared-boundary case facies polygons
// produce. Drag moves all coincident copies, double-click on a shared edge
// inserts a vertex in every feature carrying that edge, right-click delete
// removes every coincident copy; release welds: landing within the search
// radius of another layer vertex snaps to its exact position. Grab domain
// stays the selected features; the write set extends to all coincident
// vertices whether selected or not. OFF by default — the host
// (PaleoEditingToolbar) drives it from the project's topologicalEditing flag.
//
// Coincidence/edge candidates come from QgisTopologicalIndex (mapping 主线2)
// — a per-layer QgsPointLocator R-tree with gesture-lazy rebuilds — replacing
// the former whole-layer linear scans; exact XY equality (qgsDoubleNear) and
// map-space search-radius gating stay in this tool, so hit semantics are
// unchanged.
//
// Cross-layer topological editing (主线2): when both the topo flag and the
// cross-layer flag are on, the coincidence/edge/weld write set extends to
// NEIGHBOR vector layers that (a) share the target layer's CRS and (b) are
// themselves in an edit session (QGIS can only write editable layers — same
// rule as native cross-layer topo). Participants are collected from the
// canvas project at each gesture. Undo is per-layer native stacks: a
// cross-layer gesture is one edit command per touched layer — undo steps
// layer by layer (QGIS has no cross-layer undo stack; same as upstream).
// OFF by default — host-driven (PaleoEditingToolbar mirror +
// "paleo/crossLayerTopologicalEditing" project custom property).
//
// Coordinate discipline: hit-testing runs in layer CRS (event →
// QgsMapTool::toLayerCoordinates(layer, mapPoint)); QgsVertexMarker centers
// take map CRS, so markers convert back through the canvas transform.
// Cross-layer members carry positions in their own layer's CRS —
// participation is gated to same-CRS layers, so no cross-CRS math here.
class PaleoVertexTool : public QgsMapToolEdit
{
    Q_OBJECT
  public:
    explicit PaleoVertexTool( QgsMapCanvas *canvas, QgsVectorLayer *layer = nullptr );
    ~PaleoVertexTool() override;

    void activate() override;   // (re)build vertex markers for the selection
    void deactivate() override; // markers + drag state cleared

    QgsVectorLayer *targetLayer() const;
    bool isDragging() const { return mDraggingVertex != nullptr; }
    // Vertices currently displayed (selected features' total vertex count).
    int markerCount() const { return mMarkers.size(); }
    QList<class QgsVertexMarker *> markers() const { return mMarkers; }
    QList<class QgsVertexMarker *> topoMarkers() const;
    int editedCount() const { return mEditedCount; } // committed edit commands

    // Same-layer topological editing (see header notes). Host-driven.
    void setTopologicalEditingEnabled( bool on );
    bool topologicalEditingEnabled() const { return mTopoEditing; }

    // Cross-layer topological editing (see header notes). Host-driven;
    // participates only when mTopoEditing is on too.
    void setCrossLayerTopologyEnabled( bool on );
    bool crossLayerTopologyEnabled() const { return mCrossLayerTopology; }

    // Owned topo index (built lazily; null before the first topo gesture).
    QgisTopologicalIndex *topologicalIndex() const { return mTopoIndex.get(); }

  signals:
    void featureEdited( const QString &layerId );
    void editAborted();

  protected:
    void canvasPressEvent( QgsMapMouseEvent *e ) override;
    void canvasMoveEvent( QgsMapMouseEvent *e ) override;
    void canvasReleaseEvent( QgsMapMouseEvent *e ) override;
    void canvasDoubleClickEvent( QgsMapMouseEvent *e ) override; // add vertex
    void keyPressEvent( QKeyEvent *e ) override;                 // Esc

  private:
    // Internal drag bookkeeping (defined in the .cpp to keep this header
    // free of geometry includes): one in-flight vertex identified by
    // (feature id, vertex number) plus its feature's pre-drag geometry.
    struct DragState;
    friend struct DragState;

    // One member of a topological write set: the owning layer (== target
    // layer for same-layer topo, a same-CRS neighbor for cross-layer),
    // the feature id and the dense vertex number.
    struct CoincidentMember
    {
        QgsVectorLayer *layer = nullptr;
        qint64 fid = -1;
        int vertexNr = -1;
        bool operator==( const CoincidentMember &other ) const
        {
            return layer == other.layer && fid == other.fid && vertexNr == other.vertexNr;
        }
    };

    void rebuildMarkers();    // selection → markers (map CRS centers)
    void clearMarkers();
    void clearDragState();
    // Batch vertex delete under a map-CRS point — the right-button-release
    // gesture and the Delete/Backspace key share this single path.
    void deleteVertexAtMapPoint( const QgsPointXY &mapPoint );
    // Nearest vertex of the selected features within the layer-unit search
    // radius of a layer-CRS point; fills fid/vertexNr/distance when found.
    bool findNearestVertex( const QgsPointXY &layerPoint, qint64 &fid, int &vertexNr );
    // Nearest on-segment point of the selected features (layer CRS) and the
    // insertion index (the "next vertex" of the closest segment).
    bool findSegmentInsertion( const QgsPointXY &layerPoint, qint64 &fid, int &beforeVertex, QgsPointXY &onSegment );

    // Topological index plumbing (主线2). refreshTopoIndex re-scopes the
    // index to the target layer (+ cross-layer participants when enabled);
    // called at each gesture start — rebuilds are gesture-lazy, never per
    // mouse-move.
    void refreshTopoIndex();
    QList<QgsVectorLayer *> crossLayerParticipants() const;
    // Map-space search radius → scope-layer rect radius (local affine probe,
    // 2× slack; exact gating still happens in map space afterwards, so an
    // over-inclusive rect only costs candidates, never results).
    double layerUnitRadius( QgsVectorLayer *layer, const QgsPointXY &mapPoint, double mapRadius );

    // Every (layer, fid, vertexNr) at the given target-layer-CRS position
    // (cross-layer participants included when the flag is on).
    QList<CoincidentMember> coincidentVertices( const QgsPointXY &layerPoint );
    // Nearest vertex across ALL features (cross-layer when on) within the
    // search radius, excluding vertices at \a excludedPos (the drag origin's
    // coincident stack — welding to your own start is a no-op). Returns the
    // vertex position in the hitting layer's CRS (same CRS as target).
    bool findNearestLayerVertex( const QgsPointXY &layerPoint, const QgsPointXY &excludedPos,
                                 QgsPointXY &nearestPos );
    // Members carrying a segment equal to A→B (either orientation): the
    // double-click topo insert applies to every one of them;
    // vertexNr = insertion index (before that vertex), same convention as
    // findSegmentInsertion's beforeVertex.
    QList<CoincidentMember> sharedEdgeMembers( const QgsPointXY &segA, const QgsPointXY &segB,
                                               QgsVectorLayer *hitLayer, qint64 hitFid, int beforeVertex );

    QPointer<QgsVectorLayer> mLayer;      // not owned
    DragState *mDraggingVertex = nullptr; // owned, null when idle
    int mEditedCount = 0;
    bool mTopoEditing = false;
    bool mCrossLayerTopology = false;
    bool mCommitting = false;
    std::unique_ptr<QgisTopologicalIndex> mTopoIndex; // lazily created
    std::unique_ptr<QgsSnapIndicator> mSnapIndicator; // canvas snap feedback
    // Canvas-item markers, owned via canvas parenting; tracked for teardown.
    QList<class QgsVertexMarker *> mMarkers;
};
