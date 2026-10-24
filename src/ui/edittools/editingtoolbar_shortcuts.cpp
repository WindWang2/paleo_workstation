// 层：视图
// PaleoEditingToolbar：画布撤销/重做快捷键（file-budget 拆分自 editingtoolbar.cpp）。
#include "editingtoolbar.h"
#include "../shortcuts/shortcutcatalog.h"
#include <QAction>
#include <QShortcut>

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
    } );
    auto *scRedo = paleo::shortcuts::bindShortcut( QStringLiteral( "map.edit.redo" ), canvas );
    scRedo->setObjectName( QStringLiteral( "scMapEditRedo" ) );
    connect( scRedo, &QShortcut::activated, mActionRedo, [this] {
      if ( mActionRedo->isEnabled() )
        mActionRedo->trigger();
    } );
  }
}
