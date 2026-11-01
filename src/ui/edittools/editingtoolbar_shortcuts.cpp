// 层：视图
// PaleoEditingToolbar：画布撤销/重做快捷键（file-budget 拆分自 editingtoolbar.cpp）。
#include "editingtoolbar.h"
#include "../shortcuts/shortcutcatalog.h"
#include <QAction>
#include <QShortcut>
#include <QUndoGroup>
#include <QUndoStack>

// PW-118：Ctrl+Z / Ctrl+Y 在地图画布焦点内驱动编辑撤销栈（走动作以复用
// enabled 状态与提示）。
void PaleoEditingToolbar::installCanvasUndoShortcuts(QWidget *canvas)
{
  if ( canvas )
  {
    auto *scUndo = paleo::shortcuts::bindShortcut( QStringLiteral( "map.edit.undo" ), canvas );
    scUndo->setObjectName( QStringLiteral( "scMapEditUndo" ) );
    connect( scUndo, &QShortcut::activated, mActionUndo, [this] {
      if ( mActionUndo->isEnabled() )
        mActionUndo->trigger();
      else if ( mSecondaryUndo )
        mSecondaryUndo(); // #748：无会话内可撤销时落工作台撤销栈
    } );
    auto *scRedo = paleo::shortcuts::bindShortcut( QStringLiteral( "map.edit.redo" ), canvas );
    scRedo->setObjectName( QStringLiteral( "scMapEditRedo" ) );
    connect( scRedo, &QShortcut::activated, mActionRedo, [this] {
      if ( mActionRedo->isEnabled() )
        mActionRedo->trigger();
      else if ( mSecondaryRedo )
        mSecondaryRedo();
    } );
  }
}

// #748：主窗口撤销组接线。组的活动栈/栈状态变化都刷新按钮。
void PaleoEditingToolbar::setUndoGroup( QUndoGroup *group )
{
  if ( mUndoGroup )
    disconnect( mUndoGroup, nullptr, this, nullptr );
  mUndoGroup = group;
  if ( group )
  {
    connect( group, &QUndoGroup::activeStackChanged, this, &PaleoEditingToolbar::updateActionStates );
    connect( group, &QUndoGroup::indexChanged, this, &PaleoEditingToolbar::updateActionStates );
    connect( group, &QUndoGroup::canUndoChanged, this, &PaleoEditingToolbar::updateActionStates );
    connect( group, &QUndoGroup::canRedoChanged, this, &PaleoEditingToolbar::updateActionStates );
  }
  updateActionStates();
}

void PaleoEditingToolbar::routeUndo( bool undo )
{
  if ( QUndoStack *active = mUndoGroup ? mUndoGroup->activeStack() : nullptr )
  {
    if ( undo ? active->canUndo() : active->canRedo() )
      undo ? active->undo() : active->redo();
    return; // 焦点停靠面自有栈：不越界撤销画布会话
  }
  if ( mUndoStack )
    undo ? mUndoStack->undo() : mUndoStack->redo();
}

void PaleoEditingToolbar::applyUndoActionStates()
{
  QUndoStack *active = mUndoGroup ? mUndoGroup->activeStack() : nullptr;
  const bool canU = active ? active->canUndo() : ( mUndoStack && mUndoStack->canUndo() );
  const bool canR = active ? active->canRedo() : ( mUndoStack && mUndoStack->canRedo() );
  const QString undoText = active ? active->undoText() : ( mUndoStack ? mUndoStack->undoText() : QString() );
  const QString redoText = active ? active->redoText() : ( mUndoStack ? mUndoStack->redoText() : QString() );
  const auto stepTip = [this]( QAction *a, const QString &n, const QString &v ) { return n.isEmpty() ? a->property( "hint" ).toString() : tr( "%1：%2" ).arg( v, n ); }; // #869：提示带命令名
  mActionUndo->setEnabled( canU );
  mActionUndo->setToolTip( canU ? stepTip( mActionUndo, undoText, tr( "撤销" ) ) : tr( "没有可撤销的编辑" ) );
  mActionRedo->setEnabled( canR );
  mActionRedo->setToolTip( canR ? stepTip( mActionRedo, redoText, tr( "重做" ) ) : tr( "没有可重做的编辑" ) );
}
