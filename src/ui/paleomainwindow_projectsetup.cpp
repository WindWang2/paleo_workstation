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
  connect(workflow, &ProjectSetupWorkflow::projectHorizonsChanged, this, &PaleoMainWindow::refreshMappingHorizons);
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
  const auto copyUnavailable = [this]() -> QString {
    if (!m_projectSvc || m_projectSvc->projectPath().isEmpty()) return tr("请先打开工程");
    if (m_projectSvc->isOpening()) return tr("工程正在打开，请稍候");
    if (!projectOpenWorkflow()->needsDataVolumeCopy()) return tr("仅数据盘以外的只读工程需要复制");
    if (!m_taskSvc) return tr("任务服务尚未就绪");
    return {};
  };
  connect(menu, &QMenu::aboutToShow, this, [copy, copyUnavailable] { copy->setVisible(copyUnavailable().isEmpty()); });
  bindCommand(QStringLiteral("project.copyToDataVolume"), [this](const QVariantMap &) {
    auto *workflow = projectOpenWorkflow();
    const auto session = m_projectSvc->sessionId();
    const QString base = ProjectSetupWorkflow::defaultProjectDirectory();
    const QString selected = QFileDialog::getExistingDirectory(this, tr("选择数据盘上的目标父目录（将复制清单成员和受管资料）"),
        QFileInfo(base).isDir() ? base : m_projectSvc->writableProjectRoot());
    if (!selected.isEmpty() && session == m_projectSvc->sessionId()) workflow->copyCurrentTo(selected, m_taskSvc);
  }, copyUnavailable);
  routeCommand(copy, QStringLiteral("project.copyToDataVolume"));
  auto *action = menu->addAction(tr("工程资料设置…"));
  action->setObjectName(QStringLiteral("projectSourcesAction"));
  connect(menu, &QMenu::aboutToShow, this, [this, action] {
    action->setEnabled(m_projectSvc && !m_projectSvc->projectPath().isEmpty() && !m_projectSvc->isOpening() && !isProjectReadOnly());
  });
  routeCommand(action, QStringLiteral("project.sources")); // 处理器见 paleomainwindow_commands.cpp
}
