// 层：视图
#pragma once
#include <QObject>
#include <QString>

class QAction;
class QMenu;
class QWidget;
class QUndoStack;
class QgsLayout;

// ui/layout — PaleoLayoutUndoStack: thin wrapper over a QgsLayout's own undo
// stack (QgsLayout::undoStack() -> QgsLayoutUndoStack -> QUndoStack).
//
// Contract (task D):
//  - Wraps the layout-owned stack. It never creates a second stack and never
//    clears it: constructing the wrapper over a layout with existing history
//    preserves that history.
//  - undoAction()/redoAction() are QUndoStack::createUndoAction/createRedoAction
//    actions (Ctrl+Z / Ctrl+Y), so their enabled/text state automatically
//    tracks the stack.
//  - The undo stack belongs to the layout, not to any QgsLayoutView: it stays
//    usable and correct while the designer view switches pages.
//  - A null layout is tolerated: the actions exist but stay disabled and
//    undo()/redo() are no-ops.
class PaleoLayoutUndoStack : public QObject
{
    Q_OBJECT

  public:
    explicit PaleoLayoutUndoStack( QgsLayout *layout, QObject *parent = nullptr );

    //! Ctrl+Z action; enabled state and text follow the wrapped stack.
    QAction *undoAction() const { return m_undoAction; }

    //! Ctrl+Y action; enabled state and text follow the wrapped stack.
    QAction *redoAction() const { return m_redoAction; }

    //! Appends the undo/redo group to a menu. A menu that already has actions
    //! gets a separator before the group. Idempotent per target.
    void attachMenu( QMenu *menu );

    //! Generic attachment for any widget. QMenu routes to attachMenu(); a
    //! QToolBar gets undo/redo followed by a trailing separator (designer
    //! toolbar habit); other widgets get the two actions appended.
    void attachWidget( QWidget *widget );

    bool canUndo() const;
    bool canRedo() const;

    //! The wrapped, layout-owned stack (null for a null layout). Escape hatch
    //! for macro commands etc. — never a Paleo-created stack.
    QUndoStack *stack() const { return m_stack; }

  public slots:
    //! Programmatic undo (same path as undoAction()); no-op when impossible.
    void undo();

    //! Programmatic redo (same path as redoAction()); no-op when impossible.
    void redo();

  signals:
    //! Forwarded from QUndoStack::canUndoChanged.
    void canUndoChanged( bool canUndo );

    //! Forwarded from QUndoStack::canRedoChanged.
    void canRedoChanged( bool canRedo );

  private:
    // Layout-owned QUndoStack (owned by QgsLayoutUndoStack, not by us).
    QUndoStack *m_stack = nullptr;
    QAction *m_undoAction = nullptr;
    QAction *m_redoAction = nullptr;
};
