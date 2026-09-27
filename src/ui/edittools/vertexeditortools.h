// 层：视图
#pragma once
#include <qgsmaptooledit.h>
#include <QString>

class QgsMapCanvas;
class QgsVectorLayer;
class QgsMapMouseEvent;
class QKeyEvent;

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
//   · right-click within search radius of a vertex → deleteVertex, refused
//     when it would drop a line below 2 vertices / a ring below 4 (closure
//     included) — QGIS geometry minimums, not Paleo math;
//   · Esc cancels an in-flight drag (or emits editAborted when idle — owner
//     tears the tool down, §42.15 pattern).
//
// Coordinate discipline: hit-testing runs in layer CRS (event →
// QgsMapTool::toLayerCoordinates(layer, mapPoint)); QgsVertexMarker centers
// take map CRS, so markers convert back through the canvas transform.
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
    int editedCount() const { return mEditedCount; } // committed edit commands

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

    void rebuildMarkers();    // selection → markers (map CRS centers)
    void clearMarkers();
    void clearDragState();
    // Nearest vertex of the selected features within the layer-unit search
    // radius of a layer-CRS point; fills fid/vertexNr/distance when found.
    bool findNearestVertex( const QgsPointXY &layerPoint, qint64 &fid, int &vertexNr );
    // Nearest on-segment point of the selected features (layer CRS) and the
    // insertion index (the "next vertex" of the closest segment).
    bool findSegmentInsertion( const QgsPointXY &layerPoint, qint64 &fid, int &beforeVertex, QgsPointXY &onSegment );

    QgsVectorLayer *mLayer = nullptr;   // not owned
    DragState *mDraggingVertex = nullptr; // owned, null when idle
    int mEditedCount = 0;
    // Canvas-item markers, owned via canvas parenting; tracked for teardown.
    QList<class QgsVertexMarker *> mMarkers;
};
