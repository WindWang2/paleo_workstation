// 层：视图
#include "layoutundostack.h"
#include "../shortcuts/shortcutcatalog.h"

#include <QAction>
#include <QMenu>
#include <QToolBar>
#include <QUndoStack>

#include <qgslayout.h>
#include <qgslayoutundostack.h>

PaleoLayoutUndoStack::PaleoLayoutUndoStack( QgsLayout *layout, QObject *parent )
  : QObject( parent )
{
  // Wrap, never own: the stack instance belongs to the layout's
  // QgsLayoutUndoStack and outlives this wrapper as long as the layout does.
  if ( layout && layout->undoStack() )
    m_stack = layout->undoStack()->stack();

  if ( m_stack )
  {
    // QUndoStack-created actions keep enabled/text in sync with the stack and
    // trigger it directly — no Paleo-side state to keep up to date.
    m_undoAction = m_stack->createUndoAction( this, tr( "撤销(&U)" ) );
    m_redoAction = m_stack->createRedoAction( this, tr( "重做(&R)" ) );
    connect( m_stack, &QUndoStack::canUndoChanged, this, &PaleoLayoutUndoStack::canUndoChanged );
    connect( m_stack, &QUndoStack::canRedoChanged, this, &PaleoLayoutUndoStack::canRedoChanged );
  }
  else
  {
    // Null layout (or degenerate layout without a stack): uniform, permanently
    // disabled actions so UI wiring never sees a null action.
    m_undoAction = new QAction( tr( "撤销(&U)" ), this );
    m_redoAction = new QAction( tr( "重做(&R)" ), this );
    m_undoAction->setEnabled( false );
    m_redoAction->setEnabled( false );
    connect( m_undoAction, &QAction::triggered, this, &PaleoLayoutUndoStack::undo );
    connect( m_redoAction, &QAction::triggered, this, &PaleoLayoutUndoStack::redo );
  }

  m_undoAction->setObjectName( QStringLiteral( "mActionUndo" ) );
  m_redoAction->setObjectName( QStringLiteral( "mActionRedo" ) );
  // 方向63：键序登记在 shortcuts/shortcutcatalog（layout.undo/redo，StandardKey 求值）。
  paleo::shortcuts::bindAction( QStringLiteral( "layout.undo" ), m_undoAction );
  paleo::shortcuts::bindAction( QStringLiteral( "layout.redo" ), m_redoAction );
}

void PaleoLayoutUndoStack::attachMenu( QMenu *menu )
{
  if ( !menu || menu->actions().contains( m_undoAction ) )
    return;

  if ( !menu->actions().isEmpty() )
    menu->addSeparator(); // separate the edit group from preceding entries
  menu->addAction( m_undoAction );
  menu->addAction( m_redoAction );
}

void PaleoLayoutUndoStack::attachWidget( QWidget *widget )
{
  if ( !widget )
    return;

  if ( QMenu *menu = qobject_cast<QMenu *>( widget ) )
  {
    attachMenu( menu );
  }
  else if ( QToolBar *toolbar = qobject_cast<QToolBar *>( widget ) )
  {
    if ( toolbar->actions().contains( m_undoAction ) )
      return;
    toolbar->addAction( m_undoAction );
    toolbar->addAction( m_redoAction );
    toolbar->addSeparator(); // trailing: closes the undo/redo group (shell habit)
  }
  else if ( !widget->actions().contains( m_undoAction ) )
  {
    widget->addAction( m_undoAction );
    widget->addAction( m_redoAction );
  }
}

bool PaleoLayoutUndoStack::canUndo() const
{
  return m_stack && m_stack->canUndo();
}

bool PaleoLayoutUndoStack::canRedo() const
{
  return m_stack && m_stack->canRedo();
}

void PaleoLayoutUndoStack::undo()
{
  if ( m_stack && m_stack->canUndo() )
    m_stack->undo();
}

void PaleoLayoutUndoStack::redo()
{
  if ( m_stack && m_stack->canRedo() )
    m_stack->redo();
}
