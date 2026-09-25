#pragma once
#include <qgsmaptoolcapture.h>
#include <QString>

class QgsMapCanvas;
class QgsMapMouseEvent;
class QgsAdvancedDigitizingDockWidget;
class QgsCurvePolygon;
class QKeyEvent;
class SelectionContext;

// ui/maptools/ — PaleoDrawConstraintTool: QgsMapToolCapture-based line-drawing
// tool for the 约束 workflow (②). CaptureTechnique::Shape|StraightSegments.
// On finish: emits constraintDrawn(wkt) — ConstraintWorkflow.addConstraint
// consumes it. Esc/right-click aborts (§42.15 Esc behavior via cadCaptured).
// Vertex editor (QgsVertexEditor) shim ports separately when edit-mode UX lands.
class PaleoDrawConstraintTool : public QgsMapToolCapture
{
  Q_OBJECT
  public:
    // cadDock is injected by the embedder (shared dock) or, when nullptr, a
    // canvas-owned dock is fabricated — QgsMapToolAdvancedDigitizing asserts a
    // non-null dock and dereferences it in activate()/event dispatch.
    explicit PaleoDrawConstraintTool(QgsMapCanvas *canvas,
                                     QgsAdvancedDigitizingDockWidget *cadDock = nullptr);
    void activate() override;
    void deactivate() override;

    // QgsMapToolCapture impl — commit polyline on completion.
    void cadLineCaptureFinished(); // invoked via lineCaptured hook in .cpp

  signals:
    void constraintDrawn(const QString &wkt);      // LINESTRING(...) in canvas CRS
    void drawAborted();

  protected:
    // QGIS 4.2 hook: const QgsCurve* (was QgsLineString* pre-3.26). The base
    // invokes it via lineCaptured(curveToAdd.release()) — callee takes ownership.
    void lineCaptured(const QgsCurve *line) override;
    // Esc → drawAborted (same pattern as QgsMapToolDigitizeFeature::Esc →
    // digitizingCanceled); base then stops capturing and ignores the event.
    void keyPressEvent(QKeyEvent *e) override;
    // Right-click with <2 vertices is a cancel gesture (base would silently
    // stopCapturing()); with ≥2 it commits via lineCaptured().
    void cadCanvasReleaseEvent(QgsMapMouseEvent *e) override;

  private:
    QString mPendingWkt; // staged by lineCaptured(), consumed by cadLineCaptureFinished()
};

// ui/maptools/ — PaleoDrawPolygonTool: freehand-polygon variant of the
// constraint-drawing family (CapturePolygon, CaptureTechnique::StraightSegments).
// Left-click adds outline vertices; right-click commits once ≥3 vertices are on
// the capture curve (the base QgsMapToolCapture polygon threshold) — below that
// it is a cancel gesture surfaced as drawAborted(). Esc aborts identically.
// polygonCaptured() linearizes the compound capture ring so the signal contract
// stays plain "Polygon ((...))" WKT in canvas CRS — ConstraintWorkflow consumes
// it the same way as the line tool's output.
class PaleoDrawPolygonTool : public QgsMapToolCapture
{
  Q_OBJECT
  public:
    // Same cadDock contract as PaleoDrawConstraintTool (nullptr → canvas-owned).
    explicit PaleoDrawPolygonTool( QgsMapCanvas *canvas,
                                   QgsAdvancedDigitizingDockWidget *cadDock = nullptr );
    void activate() override;
    void deactivate() override;

    // QgsMapToolCapture impl — commit polygon on completion.
    void cadPolygonCaptureFinished(); // invoked via polygonCaptured hook in .cpp

  signals:
    void constraintDrawn( const QString &wkt );      // Polygon((...)) in canvas CRS
    void drawAborted();

  protected:
    // QGIS 4.2 hook: const QgsCurvePolygon* — borrowed pointer: the base calls
    // polygonCaptured( poly.get() ) and keeps ownership (unlike lineCaptured,
    // which releases the curve to the callee), so clone before transforming.
    void polygonCaptured( const QgsCurvePolygon *polygon ) override;
    // Esc → drawAborted (same pattern as PaleoDrawConstraintTool); base then
    // stops capturing and ignores the event.
    void keyPressEvent( QKeyEvent *e ) override;
    // Right-click with <3 vertices is a cancel gesture (base would silently
    // stopCapturing()); with ≥3 it commits via polygonCaptured().
    void cadCanvasReleaseEvent( QgsMapMouseEvent *e ) override;

  private:
    QString mPendingWkt; // staged by polygonCaptured(), consumed by cadPolygonCaptureFinished()
};

// ui/maptools/ — PaleoDrawRectTool: axis-aligned rectangle constraints.
// QGIS 4.2's CaptureTechnique::Shape only delegates to QgsMapToolShapeAbstract
// plug-ins registered in QgsGui::shapeMapToolRegistry() — the concrete
// rectangle tools ship in the app library (qgis_app), so Shape mode cannot
// drive a standalone tool and is not used here. Instead the tool captures two
// corner points directly:
//   · left-click plants corner 1; a second left-click commits the axis-aligned
//     bbox of the two captured corners;
//   · right-click with a corner planted finishes with the cursor position as
//     corner 2 (QgsMapToolShapeRectangleExtent convention);
//   · right-click with no corner planted, or Esc at any point, is a cancel
//     gesture surfaced as drawAborted().
// Emits "Polygon ((xmin ymin, xmax ymin, xmax ymax, xmin ymax, xmin ymin))" WKT
// in canvas CRS.
class PaleoDrawRectTool : public QgsMapToolCapture
{
  Q_OBJECT
  public:
    // Same cadDock contract as PaleoDrawConstraintTool (nullptr → canvas-owned).
    explicit PaleoDrawRectTool( QgsMapCanvas *canvas,
                                QgsAdvancedDigitizingDockWidget *cadDock = nullptr );
    void activate() override;
    void deactivate() override;

  signals:
    void constraintDrawn( const QString &wkt );      // Polygon((...)) in canvas CRS
    void drawAborted();

  protected:
    // Esc → drawAborted (same pattern as PaleoDrawConstraintTool); base then
    // stops capturing and ignores the event.
    void keyPressEvent( QKeyEvent *e ) override;
    // Two-corner commit / cancel dispatch — see class comment.
    void cadCanvasReleaseEvent( QgsMapMouseEvent *e ) override;

  private:
    // Emits the axis-aligned bbox WKT for corner 1 (first captured vertex,
    // reprojected to canvas CRS) and corner 2 — *eventCorner when given
    // (right-click finish, already canvas CRS) else the second captured
    // vertex — then stopCapturing(). No-ops if fewer than two corners resolve.
    void emitRectangle( const QgsPointXY *eventCorner = nullptr );
};
