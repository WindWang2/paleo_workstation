// 层：视图
#pragma once
#include <qgsmaptoolcapture.h>
#include <qgsmaptooledit.h>
#include <QPointer>
#include <QString>

class QgsMapCanvas;
class QgsAdvancedDigitizingDockWidget;
class QgsVectorLayer;
class QgsCurve;
class QgsCurvePolygon;
class QgsMapMouseEvent;
class QgsPoint;
class QKeyEvent;

// ui/edittools/ — vector editing toolset (digitizing). Edit behavior rides
// QGIS-native machinery only: QgsMapToolCapture for capture (rubber bands,
// CAD dock, snapping), QgsMapToolEdit helpers for edit-tool chrome, and every
// geometry mutation goes through QgsVectorLayer's edit buffer
// (beginEditCommand/endEditCommand → native undo). Paleo owns lifecycle
// management and business linkage only — no custom rubber-band painting
// outside QGIS classes, no hand-rolled geometry math (QgsGeometry mutators
// only).
//
// Shared signal contract across all tools in this family:
//   featureEdited(QString layerId) — one edit command group landed in the
//                                    target layer's edit buffer;
//   editAborted()                  — user cancelled the in-flight gesture
//                                    (Esc / degenerate commit click); owner
//                                    tears the tool down (§42.15 pattern).
//
// Target-layer rule: each tool may be bound to one explicit layer (ctor \a
// layer); with layer == nullptr the tool falls back to the canvas current
// layer (QgsMapTool::currentVectorLayer()). The embedder (PaleoEditingToolbar)
// enforces the horizon-constraint business rule before ever installing a
// tool — the tools themselves stay business-agnostic.

// Adds features (point/line/polygon) to the bound editable layer. Mirrors
// qgis_app's QgsMapToolDigitizeFeature, which is app-only in this QGIS 4.2.2
// build (nm -D libqgis_gui.so: zero QgsMapToolDigitizeFeature symbols) — the
// gui-exported QgsMapToolCapture base carries the whole capture state
// machine. CaptureMode parameterizes Point/Line/Polygon; CaptureNone derives
// the mode from the layer's geometry type (base follows currentLayerChanged).
// Completion hooks (pointCaptured/lineCaptured/polygonCaptured) receive
// geometry already stored in the target layer's CRS (the capture curve goes
// through nextPoint()'s map→layer transform), so commit is CRS-safe. Each
// commit is one edit command → one native undo step.
class PaleoAddFeatureTool : public QgsMapToolCapture
{
    Q_OBJECT
  public:
    // cadDock is injected by the embedder (shared dock) or, when nullptr, a
    // canvas-owned dock is fabricated — QgsMapToolAdvancedDigitizing asserts a
    // non-null dock and dereferences it in activate()/event dispatch (same
    // contract as PaleoDrawConstraintTool).
    explicit PaleoAddFeatureTool( QgsMapCanvas *canvas,
                                  QgsAdvancedDigitizingDockWidget *cadDock = nullptr,
                                  QgsMapToolCapture::CaptureMode mode = QgsMapToolCapture::CaptureNone,
                                  QgsVectorLayer *layer = nullptr );
    ~PaleoAddFeatureTool() override;

    void activate() override;
    void deactivate() override;

    // Resolved commit target: bound layer, else canvas current vector layer.
    QgsVectorLayer *targetLayer() const;
    // Features committed by this tool instance (telemetry/tests).
    int committedCount() const { return mCommittedCount; }

  signals:
    void featureEdited( const QString &layerId );
    void editAborted();

  protected:
    // Completion hooks — callee owns \a line (base releases it); \a point is
    // by value; \a polygon is borrowed (base keeps ownership — clone first).
    void pointCaptured( const QgsPoint &point ) override;
    void lineCaptured( const QgsCurve *line ) override;
    void polygonCaptured( const QgsCurvePolygon *polygon ) override;
    // Esc → editAborted (§42.15); base then stopCapturing()s and ignores.
    void keyPressEvent( QKeyEvent *e ) override;
    // Right-click below the base commit threshold (point: any click commits,
    // so this is line <2 / polygon <3) is a cancel gesture → editAborted().
    void cadCanvasReleaseEvent( QgsMapMouseEvent *e ) override;

  private:
    // beginEditCommand → addFeature → endEditCommand → featureEdited. Warns
    // (messageEmitted) and returns false when the layer is missing/not
    // editable; a refused commit is NOT an editAborted() (no user gesture).
    bool commitFeature( class QgsGeometry geometry );

    QPointer<QgsVectorLayer> mLayer; // not owned; nullptr → canvas current
    int mCommittedCount = 0;
};

// Reshape: draw a line across selected features; each intersecting selected
// feature's geometry is reshaped along it (QgsGeometry::reshapeGeometry —
// native). Mirrors qgis_app's QgsMapToolReshape (app-only in this build),
// including its base-class choice: app's reshape tool subclasses
// QgsMapToolCapture(CaptureLine), so the rubber band + CAD + snapping come
// from the native capture base, not from Paleo code. Features must be
// selected first; the captured line is in layer CRS on delivery.
class PaleoReshapeTool : public QgsMapToolCapture
{
    Q_OBJECT
  public:
    explicit PaleoReshapeTool( QgsMapCanvas *canvas,
                               QgsAdvancedDigitizingDockWidget *cadDock = nullptr,
                               QgsVectorLayer *layer = nullptr );
    ~PaleoReshapeTool() override;

    void activate() override;
    void deactivate() override;

    QgsVectorLayer *targetLayer() const;
    // Features whose geometry changed (telemetry/tests).
    int reshapedCount() const { return mReshapedCount; }

  signals:
    void featureEdited( const QString &layerId );
    void editAborted();

  protected:
    // Apply reshapeGeometry to every selected feature the line intersects;
    // one edit command wraps the whole batch → one native undo step.
    void lineCaptured( const QgsCurve *line ) override; // callee owns \a line
    void keyPressEvent( QKeyEvent *e ) override;        // Esc → editAborted
    void cadCanvasReleaseEvent( QgsMapMouseEvent *e ) override; // <2 vertices → editAborted

  private:
    QPointer<QgsVectorLayer> mLayer; // not owned
    int mReshapedCount = 0;
};

// Move: press on the canvas grabs the target layer's selected features,
// dragging previews the translation, release commits translated geometries
// (native QgsGeometry::translate on snapshot clones → changeGeometry in one
// edit command). Mirrors qgis_app's QgsMapToolMoveFeature (app-only in this
// build). Preview rides QgsMapToolEdit::createRubberBand — no custom
// painting. QgsMapToolEdit is not an advanced-digitizing tool: events arrive
// at the plain canvas*Event hooks, not the cad* variants.
class PaleoMoveTool : public QgsMapToolEdit
{
    Q_OBJECT
  public:
    explicit PaleoMoveTool( QgsMapCanvas *canvas, QgsVectorLayer *layer = nullptr );
    ~PaleoMoveTool() override;

    void activate() override;
    void deactivate() override;

    QgsVectorLayer *targetLayer() const;
    bool isDragging() const { return mDragging; }
    int movedCount() const { return mMovedCount; }

  signals:
    void featureEdited( const QString &layerId );
    void editAborted();

  protected:
    void canvasPressEvent( QgsMapMouseEvent *e ) override;
    void canvasMoveEvent( QgsMapMouseEvent *e ) override;
    void canvasReleaseEvent( QgsMapMouseEvent *e ) override;
    // Esc mid-drag cancels the preview and emits editAborted; Esc with
    // nothing in flight also emits editAborted (owner tears the tool down).
    void keyPressEvent( QKeyEvent *e ) override;

  private:
    void clearDragState();

    QPointer<QgsVectorLayer> mLayer;      // not owned
    bool mDragging = false;
    int mMovedCount = 0;
    class QgsPointXY *mStartPoint = nullptr;        // layer CRS, drag origin
    class QgsPointXY *mLastPoint = nullptr;         // layer CRS, current cursor
    class QgsRubberBand *mPreviewBand = nullptr;    // canvas-owned visual
    // fid → snapshot geometry in layer CRS (pre-drag originals).
    QList<QPair<qint64, class QgsGeometry *>> mSnapshots;
};

// Delete: click deletes the feature(s) UNDER the cursor on the target layer —
// hit-tested with the native vertex-search tolerance (QGIS map-tool convention:
// the gesture acts on what it points at, never on the whole selection).
// Nothing hit → messageEmitted warning, no editAborted (nothing was
// gestured). Esc emits editAborted (owner tears the tool down).
class PaleoDeleteFeatureTool : public QgsMapToolEdit
{
    Q_OBJECT
  public:
    explicit PaleoDeleteFeatureTool( QgsMapCanvas *canvas, QgsVectorLayer *layer = nullptr );
    ~PaleoDeleteFeatureTool() override;

    void activate() override;
    void deactivate() override;

    QgsVectorLayer *targetLayer() const;
    int deletedCount() const { return mDeletedCount; }

  signals:
    void featureEdited( const QString &layerId );
    void editAborted();

  protected:
    void canvasReleaseEvent( QgsMapMouseEvent *e ) override; // left click → delete hit features
    void keyPressEvent( QKeyEvent *e ) override;             // Esc → editAborted

  private:
    QPointer<QgsVectorLayer> mLayer; // not owned
    int mDeletedCount = 0;
};
