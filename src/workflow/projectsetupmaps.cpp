// 层：功能
#include "projectsetup.h"
#include "folderimport.h"
#include "paleomaplibrary.h"
#include "../io/dataimportservice.h"
#include "../qgis/qgisprojectservice.h"
#include "../qgis/qgislayerservice.h"
#include "../services/paleotaskservice.h"

void ProjectSetupWorkflow::setMapImportServices(QgisLayerService *layers, PaleoTaskService *tasks)
{
  m_mapLayers = layers; m_mapTasks = tasks;
}
void ProjectSetupWorkflow::importMaps(const ProjectSetupSources &sources, const ProjectSetupPreview &preview,
                                    quint64 session, const QString &summary, bool success, int group)
{
  if (!m_project || m_project->sessionId() != session || !m_imports || !m_imports->importService()) return;
  QMap<QString, QStringList> groups;
  for (const auto &map : preview.maps) if (map.included) groups[map.horizon] << map.path;
  const auto horizons = groups.keys();
  if (group >= horizons.size()) {
    const bool saved = m_project->writeProject();
    emit finished(saved ? summary : summary + '\n' + m_project->lastErrors().join('\n'), success && saved);
    return;
  }
  if (!m_mapTasks) {
    // 惰化分支只服务「未接装配根」的调用方（单测/脚本化导入）：这个局部
    // PaleoTaskService 不在 app 共享任务服务的排空管辖内——退出时它的线程池
    // 不会被统一 drain（~PaleoTaskService 明示可孤立 pool）。生产路径必须经
    // setMapImportServices 注入共享服务（#811-3）。
    qWarning() << "project setup: map import task service lazily created"
               << "(not drained by the shared task service; bind via setMapImportServices)";
    m_mapTasks = new PaleoTaskService(nullptr, this);
    connect(m_project, &QgisProjectService::projectAboutToClose, m_mapTasks, &PaleoTaskService::beginNewSession);
  }
  PaleoMapLibrary::Request request;
  request.files = groups.value(horizons[group]); request.horizon = horizons[group];
  request.inferHorizons = false; request.normalizeFacies = sources.normalizeMaps;
  for (const auto &map : preview.maps) if (map.included && map.horizon == request.horizon)
    for (const auto &member : map.members) request.expectedHashes.insert(member, preview.mapHashes.value(member));
  QString error;
  auto *task = PaleoMapLibrary::importMaps(m_imports->importService()->catalog(), m_mapLayers, m_mapTasks, request, this,
      [this, sources, preview, session, summary, success, group](const PaleoMapLibrary::Result &result) {
    if (!m_project || m_project->sessionId() != session) return;
    const QString detail = tr("古地理图：导入 %1 套，未变化 %2 套。%3").arg(result.assets.size()).arg(result.unchanged)
        .arg(result.cancelled ? tr("已取消") : result.errors.join('\n'));
    if (result.cancelled) { emit finished(summary + '\n' + detail, false); return; }
    importMaps(sources, preview, session, summary + '\n' + detail, success && result.errors.isEmpty(), group + 1);
  }, &error);
  if (!task) emit finished(summary + '\n' + error, false);
}
