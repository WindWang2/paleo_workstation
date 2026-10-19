// 层：功能
#include "paleomaplibrary.h"
#include "paleomapimport_internal.h"
#include "../catalog/datacatalog.h"
#include "../domain/mappinghorizons.h"
#include "../services/paleomapfiles.h"
#include <QJsonObject>
#include "../services/jsonsidecar.h"
#include <QJsonArray>
#include "../services/paleotaskservice.h"
#include "../qgis/paleomapsource.h"
#include "../qgis/qgislayerservice.h"
#include <QDir>
#include <QFile>
#include <QFileInfo>
#include <QPointer>
#include <QTemporaryDir>
#include <memory>
namespace PaleoMapLibrary {
QStringList declareMaps(DataCatalog *catalog, QgisLayerService *layers) {
  QStringList errors; if (!catalog || !layers) return errors;
  for (const auto &asset : catalog->assets()) {
    if (asset.type != "paleogeographic_map") continue;
    for (const auto &v : catalog->versionsForAsset(asset.id)) {
      if (!v.extra.value("spatial").toBool()) continue;
      LayerDeclaration d; d.layerId = v.extra.value("layer_id", "paleomap." + v.id).toString(); d.horizon = v.extra.value("horizon").toString();
      d.type = v.extra.value("source_type").toString(); d.source = catalog->versionFilePath(v) + v.extra.value("source_suffix").toString();
      d.title = QObject::tr("%1 · %2 · v%3").arg(d.horizon, v.extra.value("title", asset.displayName).toString()).arg(v.versionNumber); d.group = "05_PaleoMap";
      QString why; if (!layers->declare(d, &why)) errors << why;
    }
  }
  return errors;
}
PaleoTask *importMaps(DataCatalog *catalog, QgisLayerService *layers, PaleoTaskService *tasks,
                     const Request &request, QObject *receiver, std::function<void(const Result &)> done, QString *error) {
  if (!catalog || !catalog->isOpen() || catalog->refusesWrites() || !tasks || (request.assetIds.isEmpty() && !isMappingHorizon(request.horizon)) ||
      (!request.assetIds.isEmpty() && (!request.files.isEmpty() || !request.folder.isEmpty()))) {
    if (error) *error = QObject::tr("请先打开可写工程并选择图件所属层位"); return nullptr;
  }
  const auto recyclePath = paleo::dataopsSidecarPath(catalog, "recycle_bin.json");
  bool readable = false;
  const auto recycled = paleo::readJsonFile(recyclePath, "paleomap recycle", &readable).object();
  if (!readable) { if (error) *error = QObject::tr("无法读取可回收清单，请先修复后重试"); return nullptr; }
  for (const auto &value : recycled.value("removed").toArray())
    if (request.assetIds.contains(value.toObject().value("assetId").toString())) {
      if (error) *error = QObject::tr("选中的图件已删除，请先从可回收清单恢复"); return nullptr;
    }
  const auto recycleHash = DataCatalog::sha256FileHex(recyclePath);
  struct Job { std::unique_ptr<QTemporaryDir> stage; std::unique_ptr<DataCatalog> catalog; Result result; QStringList directories; };
  auto job = std::make_shared<Job>(); const auto directory = catalog->projectDir();
  job->stage = std::make_unique<QTemporaryDir>(QDir(directory).filePath(".paleomap-import-XXXXXX"));
  if (!job->stage->isValid()) { if (error) *error = QObject::tr("无法创建古地理图暂存目录"); return nullptr; }
  job->catalog = catalog->createStagingCopy(job->stage->path());
  if (!job->catalog) { if (error) *error = QObject::tr("无法准备图件目录快照"); return nullptr; }
  const auto sequence = catalog->mutationSeq(); QPointer<DataCatalog> owner(catalog); QPointer<QgisLayerService> layerService(layers);
  QPointer<QObject> target(receiver ? receiver : tasks); const auto horizons = mappingHorizons();
  auto *task = tasks->start(request.assetIds.isEmpty() ? QObject::tr("导入现有古地理图") : QObject::tr("整理三级古地理图"), [job, request, directory, horizons](PaleoTask *task) -> QString {
    const auto cancelled = [task] { return task->cancelRequested(); };
    auto files = PaleoMapFiles::discover(request.files, request.folder, cancelled);
    auto ids = request.assetIds; ids.removeDuplicates();
    if (!ids.isEmpty()) files = ids;
    if (files.isEmpty()) return cancelled() ? QString() : QObject::tr("没有找到可导入的古地理图");
    auto *cat = job->catalog.get();
    for (int i = 0; i < files.size(); ++i) {
      if (cancelled()) return {};
      const auto existing = ids.isEmpty() ? QString() : files[i];
      const auto current = cat->currentVersion(existing);
      const auto raw = current.extra.contains("source_raw_version_id")
          ? cat->versionById(current.extra.value("source_raw_version_id").toString()) : current;
      const auto path = existing.isEmpty() ? files[i] : DataCatalog::resolvedVersionPath(directory, raw);
      if (existing.isEmpty() && path.startsWith(QDir(directory).filePath("artifacts/"))) continue;
      const auto horizon = existing.isEmpty()
          ? (request.inferHorizons ? PaleoMapFiles::inferredHorizon(path, horizons, request.horizon) : request.horizon)
          : current.extra.value("horizon").toString();
      if (!existing.isEmpty() && (path.isEmpty() || !isMappingHorizon(horizon)))
      { job->result.errors << QObject::tr("图件 %1 缺少有效原件或层位，未整理").arg(existing); continue; }
      QString error; PaleoMapImport::Prepared result;
      if (!PaleoMapImport::prepare(cat, directory, job->stage->path(), path, horizon, request, cancelled, &result,
                                  &error, existing)) {
        if (cancelled()) return {};
        if (error.isEmpty()) return QObject::tr("图件准备失败：%1").arg(path);
        job->result.errors << QFileInfo(path).fileName() + ": " + error;
        // Catalog mutations are atomic across the batch; a bad source can be skipped.
        if (result.fatal) return error;
        continue;
      }
      job->result.errors += result.warnings;
      job->directories += result.directories;
      if (result.unchanged) ++job->result.unchanged;
      else if (!result.assetId.isEmpty()) job->result.assets << result.assetId;
      task->reportBytes(i + 1, files.size());
    }
    if (job->result.assets.isEmpty() && job->result.unchanged == 0 && !job->result.errors.isEmpty()) return job->result.errors.join("\n");
    return {};
  });
  QObject::connect(task, &PaleoTask::finished, tasks, [job, owner, layerService, tasks, task, target, sequence, directory, recyclePath, recycleHash, done] {
    job->result.cancelled = task->cancelRequested() || !owner || owner->projectDir() != directory || task->session() != tasks->session();
    QStringList moved;
    if (!job->result.cancelled && task->state() == PaleoTask::State::Succeeded) {
      QString error;
      if (owner->mutationSeq() != sequence || DataCatalog::sha256FileHex(recyclePath) != recycleHash)
        error = QObject::tr("图件处理期间目录或可回收清单已变化，请重试本批次");
      for (const auto &relative : job->directories) {
        if (!error.isEmpty()) break;
        const auto from = QDir(job->stage->path()).filePath(relative), to = QDir(directory).filePath(relative);
        if (!QDir().mkpath(QFileInfo(to).absolutePath()) || QFileInfo::exists(to) || !QDir().rename(from, to))
          error = QObject::tr("无法提交图件配套文件：%1").arg(to);
        else moved << to;
      }
      if (error.isEmpty()) owner->applyJournal(job->catalog->journal(), &error);
      if (!error.isEmpty()) { for (const auto &p : moved) QDir(p).removeRecursively(); job->result.errors << error; task->failPublication(error); job->result.assets.clear(); }
      else job->result.errors << declareMaps(owner, layerService);
    } else { job->result.assets.clear(); if (!task->errorText().isEmpty() && job->result.errors.join("\n") != task->errorText()) job->result.errors << task->errorText(); }
    if (job->result.cancelled) { task->discardPublication(); job->result.assets.clear(); }
    job->catalog.reset(); job->stage.reset(); if (target && done) done(job->result);
  });
  return task;
}
}
