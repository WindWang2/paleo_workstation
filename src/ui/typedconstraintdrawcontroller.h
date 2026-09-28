#pragma once
#include <QObject>
#include <QString>

class QgisCanvasController;
class ConstraintWorkflow;
class QgsMapTool;
class QgsAdvancedDigitizingDockWidget;

// ui/typedconstraintdrawcontroller.h — 类型化约束捕获（m2(B) 三入口：
// 物源线/展布线/控制点）。与 ConstraintDrawController（§42 通用约束，type
// 列落 shape 词）同构，但提交时 ConstraintStore 的 type 列落**地质类型词表**
// （provenance_line/distribution_line/control_point），shape 只决定画布工具。
// CAD dock 由首个控制器懒建后注入共享（避免双 dock）；无注入时自建。
// 层：视图
class TypedConstraintDrawController : public QObject
{
  Q_OBJECT
  public:
    TypedConstraintDrawController(QgisCanvasController *canvasCtl, ConstraintWorkflow *wf,
                                  QObject *parent = nullptr);

    // 复用壳里已有的 CAD dock（ConstraintDrawController 创建的那只）。
    void shareCadDock(QgsAdvancedDigitizingDockWidget *dock);

    // shape: "line" | "point"（工具选择）；constraintType 进 store 词表。
    // 未知 shape → captureFailed，不装工具。
    void startCapture(const QString &horizon, const QString &shape,
                      const QString &constraintType, int faciesCode);
    void cancel();
    bool active() const { return m_tool != nullptr; }
    QgsMapTool *currentTool() const { return m_tool; } // test seam

  public slots:
    void onDrawn(const QString &wkt); // public：测试可确定性驱动（同 drawctl 惯例）
    void onAborted();

  signals:
    void captureFinished(const QString &horizon, const QString &constraintType,
                         const QString &constraintId);
    void captureCancelled();
    void captureFailed(const QString &error);

  private:
    void teardown();

    QgisCanvasController *m_canvasCtl;
    ConstraintWorkflow *m_wf;
    QgsAdvancedDigitizingDockWidget *m_cadDock = nullptr; // 共享或自建
    bool m_ownCadDock = false;
    QgsMapTool *m_tool = nullptr;
    QString m_horizon;
    QString m_shape;
    QString m_constraintType;
    int m_faciesCode = -1;
};
