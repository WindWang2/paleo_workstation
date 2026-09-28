// 层：视图
#pragma once
#include <QObject>
#include <QString>

class QgisCanvasController;
class ConstraintWorkflow;
class QgsMapTool;
class QgsAdvancedDigitizingDockWidget;

// ui/ — ConstraintDrawController: owns one active constraint-drawing session.
// §42 flow: ConstraintPage emits drawConstraintRequested(horizon, shape, code)
// → startCapture() installs the matching Paleo map tool on the canvas →
// constraintDrawn() delivers WKT to ConstraintWorkflow::addConstraint() →
// the tool is unset and destroyed. drawAborted()/cancel() take the same
// teardown path without invoking the workflow.
//
// Only one capture may be live at a time; starting a new one aborts the old.
// The CAD dock is shared across tools (created on first use, canvas-parented).
class ConstraintDrawController : public QObject
{
  Q_OBJECT
  public:
    ConstraintDrawController(QgisCanvasController *canvasCtl, ConstraintWorkflow *wf,
                             QObject *parent = nullptr);

    // shape: "line" | "polygon" | "rect" — unknown → captureFailed, no tool.
    void startCapture(const QString &horizon, const QString &shape, int faciesCode);
    void cancel();                    // owner-initiated abort → captureCancelled

    bool active() const { return m_tool != nullptr; }
    QString activeHorizon() const { return m_horizon; }
    QgsMapTool *currentTool() const { return m_tool; } // test seam

    // Tool-facing slots — connected from the map tool's signals. onDrawn is
    // public so tests can drive the wiring deterministically without event
    // injection.
  public slots:
    void onDrawn(const QString &wkt);
    void onAborted();

  signals:
    void captureFinished(const QString &horizon, const QString &constraintId);
    void captureCancelled();
    void captureFailed(const QString &error);

  private:
    void teardown();                  // unset + delete the live tool

    QgisCanvasController *m_canvasCtl;
    ConstraintWorkflow *m_wf;
    QgsAdvancedDigitizingDockWidget *m_cadDock = nullptr;
    QgsMapTool *m_tool = nullptr;
    QString m_horizon;
    QString m_shape;
    int m_faciesCode = -1;
};
