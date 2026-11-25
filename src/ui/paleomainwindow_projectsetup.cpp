// 层：视图
#include "workflow/projectopen.h"
#include "paleomainwindow.h"
#include "dialogs/projectsetupdialog.h"
#include "dialogs/dataserviceimportwizard.h"
#include "../workflow/projectsetup.h"
#include "notifications/paleonotify.h"
#include "../qgis/qgisprojectservice.h"
#include <QMenu>
#include <QFileDialog>
#include <QFileInfo>

void PaleoMainWindow::runNewProject(const QString &sourceRoot, bool supplement)
{
  if (supplement && (!m_projectSvc || isProjectReadOnly() || m_projectSvc->isOpening())) return;
  auto *workflow = new ProjectSetupWorkflow(m_projectSvc, folderImportWorkflow(), this);
  workflow->setRequiredProjectRoot(ProjectSetupWorkflow::defaultProjectVolume());
  workflow->setMapImportServices(m_layerSvc, m_taskSvc);
  connect(workflow, &ProjectSetupWorkflow::finished, this, [this, workflow](const QString &summary, bool success) {
    if (success) PaleoNotify::information(this, tr("工程已创建"), summary);
    else PaleoNotify::warning(this, tr("工程资料导入结果"), summary);
    workflow->deleteLater();
  });
  // #798：保存确认提前到打开向导之前。原先在 dialog.exec() accept 之后才弹
  // maybeSaveProject，用户一取消就连带丢弃本轮已填的扫描/映射/图件勾选；提前
  // 后取消仅退出（向导尚未打开，无输入可丢），重试循环内也不再弹。
  if (!maybeSaveProject()) { workflow->deleteLater(); return; }
  if (!supplement) {
    DataServiceImportWizard dialog(workflow, this);
    if (!sourceRoot.isEmpty()) dialog.setSourceRoot(sourceRoot);
    while (dialog.exec() == QDialog::Accepted) {
      QString error;
      if (workflow->create(dialog.projectPath(), dialog.sources(), dialog.preview(), &error)) return;
      dialog.showCreationError(error);
    }
    workflow->deleteLater();
    return;
  }
  ProjectSetupDialog dialog(workflow, this);
  dialog.setExistingProject(m_projectSvc->projectPath(), workflow->currentSources());
  if (!sourceRoot.isEmpty()) dialog.setSourceRoot(sourceRoot);
  while (dialog.exec() == QDialog::Accepted) {
    QString error;
    const bool ok = supplement ? workflow->applySources(dialog.sources(), dialog.preview(), &error)
                               : workflow->create(dialog.projectPath(), dialog.sources(), dialog.preview(), &error);
    if (ok) return;
    // Keep all selected sources, mappings and preview results for a retry.
    dialog.showCreationError(error);
  }
  workflow->deleteLater();
}
void PaleoMainWindow::attachProjectSetupAction(QMenu *menu)
{
  auto *copy = menu->addAction(tr("复制工程到数据盘并打开…"));
  copy->setObjectName(QStringLiteral("copyProjectToDataVolume"));
  copy->setVisible(false);
  connect(menu, &QMenu::aboutToShow, this, [this, copy] {
    auto *workflow = projectOpenWorkflow();
    copy->setVisible(workflow && workflow->needsDataVolumeCopy());
  });
  connect(copy, &QAction::triggered, this, [this] {
    auto *workflow = projectOpenWorkflow();
    if (!workflow || !workflow->needsDataVolumeCopy()) return;
    const QString base = ProjectSetupWorkflow::defaultProjectDirectory();
    const QString selected = QFileDialog::getExistingDirectory(this, tr("选择数据盘上的目标父目录（将复制清单成员和受管资料）"),
        QFileInfo(base).isDir() ? base : m_projectSvc->writableProjectRoot());
    if (!selected.isEmpty()) workflow->copyCurrentTo(selected, m_taskSvc);
  });
  auto *action = menu->addAction(tr("工程资料设置…"));
  action->setObjectName(QStringLiteral("projectSourcesAction"));
  connect(menu, &QMenu::aboutToShow, this, [this, action] {
    action->setEnabled(m_projectSvc && !m_projectSvc->projectPath().isEmpty() && !m_projectSvc->isOpening() && !isProjectReadOnly());
  });
  routeCommand(action, QStringLiteral("project.sources")); // 处理器见 paleomainwindow_commands.cpp
}
