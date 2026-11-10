// 层：视图
// PaleoEditingToolbar：画布撤销/重做快捷键（file-budget 拆分自 editingtoolbar.cpp）。
#include "editingtoolbar.h"
#include "../shortcuts/shortcutcatalog.h"
#include <QAction>
#include <QShortcut>
#include <QStatusBar>

void PaleoEditingToolbar::installCanvasUndoShortcuts(QWidget *canvas)
{
  if ( canvas )
  {
    auto *scUndo = paleo::shortcuts::bindShortcut( QStringLiteral( "map.edit.undo" ), canvas );
    scUndo->setObjectName( QStringLiteral( "scMapEditUndo" ) );
    connect( scUndo, &QShortcut::activated, mActionUndo, &QAction::trigger );
    auto *scRedo = paleo::shortcuts::bindShortcut( QStringLiteral( "map.edit.redo" ), canvas );
    scRedo->setObjectName( QStringLiteral( "scMapEditRedo" ) );
    connect( scRedo, &QShortcut::activated, mActionRedo, &QAction::trigger );
  }
}

void PaleoEditingToolbar::setSecondaryUndo( PaleoUndoTarget target )
{
  mSecondaryUndo = std::move( target );
  refreshUndoActions();
}

void PaleoEditingToolbar::setUndoRouter( PaleoUndoGroupRouter *router )
{
  if ( mUndoRouter )
    disconnect( mUndoRouter, nullptr, this, nullptr );
  mUndoRouter = router;
  if ( router )
    connect( router, &PaleoUndoGroupRouter::targetChanged, this, &PaleoEditingToolbar::refreshUndoActions );
  refreshUndoActions();
}

PaleoUndoTarget PaleoEditingToolbar::canvasUndoTarget()
{
  const QPointer<PaleoEditingToolbar> self(this);
  return { [self]( bool undo ) { return self ? self->describeCanvasUndo( undo ) : PaleoUndoStep{}; },
           [self]( bool undo ) { return self && self->stepCanvasUndo( undo ); } };
}

PaleoUndoStep PaleoEditingToolbar::describeCanvasUndo( bool undo ) const
{
  if ( mUndoStack && ( undo ? mUndoStack->canUndo() : mUndoStack->canRedo() ) )
    return { true, undo ? mUndoStack->undoText() : mUndoStack->redoText() };
  return mSecondaryUndo.describe ? mSecondaryUndo.describe( undo ) : PaleoUndoStep{};
}

bool PaleoEditingToolbar::stepCanvasUndo( bool undo )
{
  if ( mUndoStack && ( undo ? mUndoStack->canUndo() : mUndoStack->canRedo() ) )
  {
    undo ? mUndoStack->undo() : mUndoStack->redo();
    return true;
  }
  return mSecondaryUndo.step && mSecondaryUndo.describe &&
         mSecondaryUndo.describe( undo ).available && mSecondaryUndo.step( undo );
}

void PaleoEditingToolbar::routeUndo( bool undo )
{
  if ( mUndoStatus )
    mUndoStatus->clearMessage();
  // 撤销组按焦范围选择目标。测试和未把焦点放到画布时 mActiveScope 为空，
  // router->step 会直接返回；工具栏自己的撤销按钮仍应回退到当前编辑图层栈
  //（canvasUndoTarget / mUndoStack），否则覆盖绘制后撤销是空操作。
  if ( mUndoRouter && mUndoRouter->step( undo ) )
    return;
  stepCanvasUndo( undo );
}

void PaleoEditingToolbar::refreshUndoActions()
{
  const auto sync = [this]( QAction *action, bool undo, const QString &verb, const QString &unavailable ) {
    // 与 routeUndo 一致：撤销组无焦点范围（describe 不可用）时，回退到
    // 当前编辑图层栈，否则按钮会被禁用，QAction::trigger 在禁用时直接返回。
    auto step = mUndoRouter ? mUndoRouter->describe( undo ) : describeCanvasUndo( undo );
    if ( mUndoRouter && !step.available )
      step = describeCanvasUndo( undo );
    const auto hint = step.text.isEmpty() ? action->property( "hint" ).toString() : tr( "%1：%2" ).arg( verb, step.text );
    action->setToolTip( step.available ? hint : unavailable );
    action->setEnabled( step.available );
  };
  // Disabling a focused button can move focus synchronously. Query each action
  // immediately before updating it so redo reflects any new scope.
  sync( mActionUndo, true, tr( "撤销" ), tr( "没有可撤销的编辑" ) );
  sync( mActionRedo, false, tr( "重做" ), tr( "没有可重做的编辑" ) );
}
