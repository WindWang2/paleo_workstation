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

  m_undoAction = new QAction( this );
  m_redoAction = new QAction( this );
  connect( m_undoAction, &QAction::triggered, this, &PaleoLayoutUndoStack::undo );
  connect( m_redoAction, &QAction::triggered, this, &PaleoLayoutUndoStack::redo );
  if ( m_stack )
  {
    connect( m_stack, &QUndoStack::canUndoChanged, this, [this] {
      refreshActions();
      emit canUndoChanged( canUndo() );
    } );
    connect( m_stack, &QUndoStack::canRedoChanged, this, [this] {
      refreshActions();
      emit canRedoChanged( canRedo() );
    } );
    connect( m_stack, &QUndoStack::undoTextChanged, this, &PaleoLayoutUndoStack::refreshActions );
    connect( m_stack, &QUndoStack::redoTextChanged, this, &PaleoLayoutUndoStack::refreshActions );
    connect( m_stack, &QObject::destroyed, this, &PaleoLayoutUndoStack::refreshActions );
  }

  m_undoAction->setObjectName( QStringLiteral( "mActionUndo" ) );
  m_redoAction->setObjectName( QStringLiteral( "mActionRedo" ) );
  // 方向63：键序登记在 shortcuts/shortcutcatalog（layout.undo/redo，StandardKey 求值）。
  paleo::shortcuts::bindAction( QStringLiteral( "layout.undo" ), m_undoAction );
  paleo::shortcuts::bindAction( QStringLiteral( "layout.redo" ), m_redoAction );
  refreshActions();
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
  return !m_readOnly && m_stack && m_stack->canUndo();
}

bool PaleoLayoutUndoStack::canRedo() const
{
  return !m_readOnly && m_stack && m_stack->canRedo();
}

void PaleoLayoutUndoStack::undo()
{
  if ( canUndo() )
    m_stack->undo();
}

void PaleoLayoutUndoStack::redo()
{
  if ( canRedo() )
    m_stack->redo();
}

void PaleoLayoutUndoStack::setReadOnly( bool readOnly )
{
  if ( m_readOnly == readOnly )
    return;
  m_readOnly = readOnly;
  refreshActions();
  emit canUndoChanged( canUndo() );
  emit canRedoChanged( canRedo() );
}

void PaleoLayoutUndoStack::refreshActions()
{
  const auto sync = [](QAction *action, bool available, const QString &label, const QString &text) {
    action->setEnabled( available );
    action->setText( text.isEmpty() ? label : label + QLatin1Char( ' ' ) + text );
  };
  sync( m_undoAction, canUndo(), tr( "撤销(&U)" ), m_stack ? m_stack->undoText() : QString{} );
  sync( m_redoAction, canRedo(), tr( "重做(&R)" ), m_stack ? m_stack->redoText() : QString{} );
}
