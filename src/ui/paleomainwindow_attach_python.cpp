// 层：视图
#include "paleomainwindow.h"

#include "../workflow/pythonconsolecontroller.h"
#include "python/pythonconsolepanel.h"
#include "python/pythonreplpanel.h"

#include <QTabWidget>

// ---------------------------------------------------------------------------
// 方向68：Python 脚本面装配。幂等（页签已建则直接复用）。
// 底栏双页签：「Python 脚本」控制台 +「Python REPL」（实验性）。编排
// （解释器解析/协议/历史/REPL 会话）在 workflow/PythonConsoleController；
// 这里只装配面板。导入意图（importRequested）由组装根（app/main.cpp）
// 接 DataImportService——视图不碰 io。
// ---------------------------------------------------------------------------
PythonConsolePanel *PaleoMainWindow::attachPythonConsole(
    PythonConsoleController *controller)
{
  if (!controller)
    return nullptr;
  auto *bottomTabs = findChild<QTabWidget *>(QStringLiteral("bottomTabs"));
  if (!bottomTabs)
    return nullptr;
  auto *existing = bottomTabs->findChild<PythonConsolePanel *>(
      QStringLiteral("pythonConsolePanel"));
  if (existing)
    return existing;
  auto *panel = new PythonConsolePanel(controller, bottomTabs);
  panel->setObjectName(QStringLiteral("pythonConsolePanel"));
  bottomTabs->addTab(panel, tr("Python 脚本"));
  auto *repl = new PythonReplPanel(controller, bottomTabs);
  repl->setObjectName(QStringLiteral("pythonReplPanel"));
  bottomTabs->addTab(repl, tr("Python REPL（实验性）"));
  return panel;
}
