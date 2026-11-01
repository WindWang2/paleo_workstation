// 层：视图
#pragma once
#include <QHash>
#include <QObject>
#include <QPointer>

class QUndoGroup;
class QUndoStack;
class QWidget;

// ui/edittools — PaleoUndoGroupRouter（#748）：主窗口 Undo/Redo 的焦点路由。
//
// 持有一个真正的 QUndoGroup；各停靠面（版面设计器、地震剖面/拾取、……）把
// 自己的原生 QUndoStack 登记为「焦点作用域」。QApplication::focusChanged 时
// 自新焦点控件向上找到最近的已登记作用域，setActiveStack 切到它的栈：
//   · 作用域登记 stack=nullptr（如地图画布）= 活动栈清空，主窗口动作回落
//     既有路由（会话内要素编辑栈 → 工作台属性栈）；
//   · 焦点落到未登记控件（功能区按钮、快速访问栏）时保持当前活动栈——
//     点击「撤销」按钮本身不会把目标切走；
//   · 作用域或栈销毁时自动解除，活动栈随之清空。
// 不另造撤销框架：命令仍只存在于各自 QUndoStack，本类只做选择。
class PaleoUndoGroupRouter : public QObject
{
    Q_OBJECT
  public:
    explicit PaleoUndoGroupRouter( QObject *parent = nullptr );
    ~PaleoUndoGroupRouter() override;

    QUndoGroup *group() const { return mGroup; }
    // 登记/改绑作用域（同一 scope 重复调用即改绑）；stack 可为 nullptr。
    void registerScope( QWidget *scope, QUndoStack *stack );
    void unregisterScope( QWidget *scope );
    QUndoStack *activeStack() const;
    // 焦点切换入口（构造时已接 qApp focusChanged；测试可直接调用）。
    void handleFocusChanged( QWidget *now );

  private:
    void detachStackIfUnused( QUndoStack *stack );

    QUndoGroup *mGroup = nullptr; // owned (QObject child)
    QHash<QWidget *, QPointer<QUndoStack>> mScopes;
    QPointer<QWidget> mActiveScope;
};
