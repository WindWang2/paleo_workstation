// 层：视图
#include "paleomainwindow.h"
#include "dialogs/projectsetupdialog.h"
#include "../workflow/projectsetup.h"
#include "notifications/paleonotify.h"
#include "../qgis/qgisprojectservice.h"
#include <QMenu>

void PaleoMainWindow::runNewProject(const QString &sourceRoot, bool supplement)
{
  if (supplement && (!m_projectSvc || isProjectReadOnly() || m_projectSvc->isOpening())) return;
  auto *workflow = new ProjectSetupWorkflow(m_projectSvc, folderImportWorkflow(), this);
  connect(workflow, &ProjectSetupWorkflow::finished, this, [this, workflow](const QString &summary, bool success) {
    if (success) PaleoNotify::information(this, tr("工程已创建"), summary);
    else PaleoNotify::warning(this, tr("工程资料导入结果"), summary);
    workflow->deleteLater();
  });
  ProjectSetupDialog dialog(workflow, this);
  if (supplement) dialog.setExistingProject(m_projectSvc->projectPath(), workflow->currentSources());
  if (!sourceRoot.isEmpty()) dialog.setSourceRoot(sourceRoot);
  if (dialog.exec() != QDialog::Accepted || !maybeSaveProject()) { workflow->deleteLater(); return; }
  QString error;
  const bool ok = supplement ? workflow->applySources(dialog.sources(), dialog.preview(), &error)
                             : workflow->create(dialog.projectPath(), dialog.sources(), dialog.preview(), &error);
  if (!ok) {
    PaleoNotify::warning(this, tr("新建工程"), error); workflow->deleteLater();
  }
}
void PaleoMainWindow::attachProjectSetupAction(QMenu *menu)
{
  auto *action = menu->addAction(tr("工程资料设置…"));
  action->setObjectName(QStringLiteral("projectSourcesAction"));
  connect(menu, &QMenu::aboutToShow, this, [this, action] {
    action->setEnabled(m_projectSvc && !m_projectSvc->projectPath().isEmpty() && !m_projectSvc->isOpening() && !isProjectReadOnly());
  });
  connect(action, &QAction::triggered, this, [this] { runNewProject({}, true); });
}
