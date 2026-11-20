// 层：视图
// PaleoMainWindow：#748 主窗口撤销组——QUndoGroup 按焦点切换各停靠面原生栈。
#include "paleomainwindow.h"
#include "../qgis/qgiscanvascontroller.h"
#include "../qgis/qgisprojectservice.h"
#include "edittools/editingtoolbar.h"
#include "edittools/undogrouprouter.h"
#include "faults/faultmanagerpanel.h"
#include "layout/layoutundostack.h"
#include "layoutdesignershell.h"
#include "pages/datalist.h" // C-W3：数据列表局部撤销接入宿主写许可
#include "seismicsection/seismicsectiondockwidget.h"
#include <QApplication>

PaleoUndoGroupRouter *PaleoMainWindow::undoRouter()
{
  if (!m_undoRouter)
  {
    m_undoRouter = new PaleoUndoGroupRouter(this);
    m_undoRouter->setCommandChrome(ribbonBar());
    m_undoRouter->setWritable(!m_isProjectReadOnly);
    if (m_projectSvc)
      connect(m_projectSvc, &QgisProjectService::projectOpened, m_undoRouter, [this] {
        m_undoRouter->handleFocusChanged(QApplication::focusWidget());
      }, Qt::QueuedConnection);
  }
  return m_undoRouter;
}

void PaleoMainWindow::bindUndoGroup(PaleoEditingToolbar *edit)
{
  if (!edit)
    return;
  auto *router = undoRouter();
  edit->setUndoRouter(router);
  if (!m_canvasCtl || !m_canvasCtl->canvasWidget())
    return;
  const QPointer<paleo::fault::FaultManagerPanel> fault(m_faultPanel);
  const auto mapTarget = edit->canvasUndoTarget();
  const auto canvasTarget = [fault, mapTarget] {
    return fault && fault->cutDrawActive() ? fault->undoTarget() : mapTarget;
  };
  router->registerScope(m_canvasCtl->canvasWidget(), nullptr, {
      [canvasTarget](bool undo) { return canvasTarget().describe(undo); },
      [canvasTarget](bool undo) { return canvasTarget().step(undo); }
  });
  if (fault)
    connect(fault, &paleo::fault::FaultManagerPanel::cutDrawToggled,
            router, &PaleoUndoGroupRouter::targetChanged, Qt::UniqueConnection);
}

void PaleoMainWindow::updateUndoProjectGate(bool readOnly)
{
  if (m_undoRouter)
    m_undoRouter->setWritable(!readOnly);
  if (m_faultPanel)
    m_faultPanel->setUndoReadOnly(readOnly);
  if (m_seismicSectionDock)
    m_seismicSectionDock->setInterpretationReadOnly(readOnly);
  // C-W3（#995）：数据列表局部撤销（按钮 + data.list.undo/redo 快捷键 + 程序
  // 直调）接入同一宿主写许可——此前面板只看栈可操作性，只读工程下局部
  // Ctrl+Z 仍会改目录。不清栈：历史保留，恢复可写后按原契约继续可用。
  for (auto *panel : findChildren<DataListPanel *>())
    panel->setUndoReadOnly(readOnly);
  for (auto *shell : findChildren<PaleoLayoutDesignerShell *>())
    if (auto *undo = shell->undoStack())
      undo->setReadOnly(readOnly);
}

void PaleoMainWindow::resetUndoFocus()
{
  if (m_faultPanel)
    m_faultPanel->cancelCutDraw();
  if (m_undoRouter)
    m_undoRouter->resetFocus();
}
