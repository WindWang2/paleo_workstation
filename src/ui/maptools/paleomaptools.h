#pragma once
#include <qgsmaptoolcapture.h>
#include <QString>

class QgsMapCanvas;
class QgsMapMouseEvent;
class QgsAdvancedDigitizingDockWidget;
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
