// 层：视图
#pragma once
#include <QObject>
#include <QPointer>
#include <QString>

class QgisCanvasController;
class ConstraintWorkflow;
class QgsMapTool;
class QgsAdvancedDigitizingDockWidget;

// ui/ — ConstraintDrawController: owns one active constraint-drawing session.
// §42 flow: ConstraintPage emits drawConstraintRequested(horizon, shape, code)
// → startCapture() installs the matching Paleo map tool on the canvas →
// constraintDrawn() delivers WKT to ConstraintWorkflow::drawConstraint()（完成即写工程矢量层）→
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

    // #413：canvasCtl/wf/cadDock 均外建——任一先析构时裸指针即悬垂；
    // QPointer 归零后 startCapture 走「尚未完成接线」诚实失败分支。
    QPointer<QgisCanvasController> m_canvasCtl;
    QPointer<ConstraintWorkflow> m_wf;
    QPointer<QgsAdvancedDigitizingDockWidget> m_cadDock;
    QgsMapTool *m_tool = nullptr;
    QString m_horizon;
    QString m_shape;
    int m_faciesCode = -1;
    // 画布换工具哨子连接（startCapture 重连）：本工具被外部换走时经
    // QgsMapCanvas::mapToolSet 的 oldTool 认出并走 onAborted 收尾，
    // 防 m_tool 陈旧（active() 谎报 + teardown 误卸外来工具）。
    QMetaObject::Connection m_toolWatch;
};
