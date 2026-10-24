// 层：功能
#include "offlinebasemapimport.h"
#include "../qgis/offlinebasemapvalidation.h"
#include "../qgis/qgisprojectservice.h"
#include "../qgis/qgislayerservice.h"
#include "../io/paleomapfiles.h"
#include "../services/paleotaskservice.h"
#include "../catalog/datacatalog.h"
#include <QDir>
#include <QFileInfo>
#include <QPointer>
#include <QTemporaryDir>
#include <memory>
namespace PaleoOfflineBasemap {
namespace {
struct Prepared { std::unique_ptr<QTemporaryDir> stage; QVariantList members; QString relative; bool existing = false; };
}
PaleoTask *importFile(QgisProjectService *project, QgisLayerService *layers, PaleoTaskService *tasks,
                      const Request &request, QObject *receiver,
                      const std::function<void(bool, const QString &)> &done, QString *error) {
  const auto fail = [&](const QString &message) -> PaleoTask * { if (error) *error = message; return nullptr; };
  if (!project || project->projectPath().isEmpty() || project->isOpening() || project->isReadOnly() || !tasks)
    return fail(QObject::tr("请先打开可写工程并等待读取完成。"));
  if (!QFileInfo(request.path).isFile()) return fail(QObject::tr("请选择存在的离线底图文件。"));
  if (request.display && !project->mapConfiguration().georeference)
    return fail(QObject::tr("工程尚未设置坐标配准；可先取消“导入后显示”，或到“工程坐标与底图”完成设置。"));
  const auto session = project->sessionId(), taskSession = tasks->session();
  const auto configuration = project->mapConfiguration();
  const auto directory = QFileInfo(project->projectPath()).absolutePath();
  const auto prepared = std::make_shared<Prepared>();
  QPointer<QgisProjectService> owner(project); QPointer<QgisLayerService> layerService(layers);
  QPointer<PaleoTaskService> taskService(tasks); QPointer<QObject> target(receiver ? receiver : tasks);
  auto *task = tasks->start(QObject::tr("导入工程离线底图"), [prepared, request, directory](PaleoTask *task) {
    const auto cancelled = [task] { return task->cancelRequested(); };
    auto error = PaleoOfflineRaster::validate(request.path);
    if (!error.isEmpty() || cancelled()) return error;
    if (!QDir(directory).mkpath("basemap")) return QObject::tr("无法创建工程底图目录。");
    prepared->stage = std::make_unique<QTemporaryDir>(QDir(directory).filePath("basemap/.import-XXXXXX"));
    if (!prepared->stage->isValid()) return QObject::tr("无法创建底图临时目录。");
    task->reportStage("hash"); task->reportDetail(QObject::tr("复制并核验底图及配套文件，可在任务中心取消"));
    prepared->members = PaleoMapFiles::copyDataset(request.path, prepared->stage->path(), cancelled, &error);
    if (prepared->members.isEmpty()) return error.isEmpty() && !cancelled() ? QObject::tr("底图复制失败。") : error;
    prepared->relative = "basemap/" + PaleoMapFiles::fingerprint(prepared->members);
    error = PaleoOfflineRaster::validate(QDir(prepared->stage->path()).filePath(QFileInfo(request.path).fileName()));
    const auto destination = QDir(directory).filePath(prepared->relative);
    prepared->existing = QFileInfo::exists(destination);
    if (error.isEmpty() && prepared->existing) for (const auto &value : prepared->members) {
      const auto member = value.toMap();
      if (DataCatalog::sha256FileHex(QDir(destination).filePath(member.value("file").toString()), &error, cancelled) != member.value("sha256").toString())
        return QObject::tr("工程中同名底图副本已损坏，请检查文件后重试。");
    }
    return error;
  });
  if (!task) return fail(QObject::tr("后台任务服务尚未就绪。"));
  QObject::connect(project, &QgisProjectService::projectAboutToClose, task, &PaleoTask::requestCancel);
  QObject::connect(task, &PaleoTask::finished, tasks,
      [prepared, owner, layerService, taskService, target, request, session, taskSession, configuration, directory, task, done] {
    QString error = task->errorText();
    bool success = false;
    if (task->state() == PaleoTask::State::Succeeded) {
      if (!owner || !target || !taskService || owner->sessionId() != session || taskService->session() != taskSession ||
          owner->isOpening() || owner->isReadOnly()) {
        task->discardPublication(); error = QObject::tr("工程已变化，已取消底图导入。");
      } else if (owner->mapConfiguration().basemapTopo != configuration.basemapTopo ||
                 owner->mapConfiguration().basemapHillshade != configuration.basemapHillshade ||
                 owner->mapConfiguration().basemapEnabled != configuration.basemapEnabled) {
        error = QObject::tr("底图设置在导入期间已变化，请重新导入。"); task->failPublication(error);
      } else {
        const auto destination = QDir(directory).filePath(prepared->relative);
        const bool existing = QFileInfo::exists(destination);
        bool published = false;
        if (existing && !prepared->existing) error = QObject::tr("同一底图正在由其他任务保存，请重试。");
        else if (!existing) {
          published = QDir().rename(prepared->stage->path(), destination);
          if (!published) error = QObject::tr("无法将底图副本移入工程。");
        }
        if (error.isEmpty()) {
          auto settings = owner->mapConfiguration(); settings.basemapEnabled = request.display;
          const auto member = prepared->relative + '/' + QFileInfo(request.path).fileName();
          if (request.hillshade) settings.basemapHillshade = member; else settings.basemapTopo = member;
          success = owner->updateMapConfiguration(settings, &error);
          if (success) { if (layerService) layerService->refreshBasemaps(); success = owner->writeProject(); if (!success) error = owner->lastErrors().join('\n'); }
        }
        if (!success) {
          if (published && owner->mapConfiguration().basemapTopo != prepared->relative + '/' + QFileInfo(request.path).fileName() &&
              owner->mapConfiguration().basemapHillshade != prepared->relative + '/' + QFileInfo(request.path).fileName()) QDir(destination).removeRecursively();
          task->failPublication(error);
        }
      }
    } else if (task->state() == PaleoTask::State::Cancelled) error = QObject::tr("底图导入已取消。");
    prepared->stage.reset();
    if (target && done) done(success, error);
  });
  return task;
}
}
