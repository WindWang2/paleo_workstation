// 层：视图
// PaleoMainWindow：#748 主窗口撤销组——QUndoGroup 按焦点切换各停靠面原生栈。
#include "paleomainwindow.h"
#include "../qgis/qgiscanvascontroller.h"
#include "edittools/editingtoolbar.h"
#include "edittools/undogrouprouter.h"
#include "layout/layoutundostack.h"
#include "layoutdesignershell.h"
#include <qgsmapcanvas.h>

PaleoUndoGroupRouter *PaleoMainWindow::undoRouter()
{
  if (!m_undoRouter)
    m_undoRouter = new PaleoUndoGroupRouter(this);
  return m_undoRouter;
}

void PaleoMainWindow::bindUndoGroup(PaleoEditingToolbar *edit)
{
  if (!edit)
    return;
  edit->setUndoGroup(undoRouter()->group());
  // 画布获焦 = 活动栈清空：Ctrl+Z/按钮回落会话编辑栈 → 工作台属性栈。
  if (m_canvasCtl && m_canvasCtl->canvas())
    undoRouter()->registerScope(m_canvasCtl->canvas(), nullptr);
}

PaleoLayoutDesignerShell *PaleoMainWindow::newLayoutShell(QgsLayout *layout)
{
  auto *shell = new PaleoLayoutDesignerShell(layout, this);
  shell->setTaskService(m_taskSvc); // #85：导出走任务池 worker
  if (PaleoLayoutUndoStack *undo = shell->undoStack(); undo && undo->stack())
    undoRouter()->registerScope(shell, undo->stack());
  return shell;
}
