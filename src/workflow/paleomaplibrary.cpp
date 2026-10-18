// 层：功能
#include "paleomaplibrary.h"
#include "../catalog/datacatalog.h"
#include "../domain/mappinghorizons.h"
#include "../services/paleomapfiles.h"
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
      d.type = v.extra.value("source_type").toString(); d.source = catalog->versionFilePath(v);
      d.title = QObject::tr("%1 · %2 · v%3").arg(d.horizon, asset.displayName).arg(v.versionNumber); d.group = "05_PaleoMap";
      QString why; if (!layers->declare(d, &why)) errors << why;
    }
  }
  return errors;
}
PaleoTask *importMaps(DataCatalog *catalog, QgisLayerService *layers, PaleoTaskService *tasks,
                     const Request &request, QObject *receiver, std::function<void(const Result &)> done, QString *error) {
  if (!catalog || !catalog->isOpen() || catalog->refusesWrites() || !tasks || !isMappingHorizon(request.horizon)) {
    if (error) *error = QObject::tr("请先打开可写工程并选择图件所属层位"); return nullptr;
  }
  struct Job { std::unique_ptr<QTemporaryDir> stage; std::unique_ptr<DataCatalog> catalog; Result result; QStringList directories; };
  auto job = std::make_shared<Job>(); const auto directory = catalog->projectDir();
  job->stage = std::make_unique<QTemporaryDir>(QDir(directory).filePath(".paleomap-import-XXXXXX"));
  if (!job->stage->isValid()) { if (error) *error = QObject::tr("无法创建古地理图暂存目录"); return nullptr; }
  job->catalog = catalog->createStagingCopy(job->stage->path());
  if (!job->catalog) { if (error) *error = QObject::tr("无法准备图件目录快照"); return nullptr; }
  const auto sequence = catalog->mutationSeq(); QPointer<DataCatalog> owner(catalog); QPointer<QgisLayerService> layerService(layers);
  QPointer<QObject> target(receiver ? receiver : tasks); const auto horizons = mappingHorizons();
  auto *task = tasks->start(QObject::tr("导入现有古地理图"), [job, request, directory, horizons](PaleoTask *task) -> QString {
    const auto cancelled = [task] { return task->cancelRequested(); };
    const auto files = PaleoMapFiles::discover(request.files, request.folder, cancelled);
    if (files.isEmpty()) return cancelled() ? QString() : QObject::tr("没有找到可导入的古地理图");
    auto *cat = job->catalog.get();
    for (int i = 0; i < files.size(); ++i) {
      if (cancelled()) return {};
      const auto path = files[i];
      if (path.startsWith(QDir(directory).filePath("artifacts/"))) continue;
      const auto horizon = request.inferHorizons ? PaleoMapFiles::inferredHorizon(path, horizons, request.horizon) : request.horizon;
      QString error; const auto sourceName = QFileInfo(path).fileName();
      const auto old = [&]() { for (const auto &a : cat->assets()) if (a.type == "paleogeographic_map") {
          const auto v = cat->currentVersion(a.id); if (v.sourceUri == path && v.extra.value("horizon").toString() == horizon) return v;
        } return CatalogVersion{}; }();
      const auto assetId = old.id.isEmpty() ? cat->nextAssetId() : old.assetId;
      const auto versionId = cat->nextVersionId();
      const auto relative = "artifacts/" + DataCatalog::managedPath("raw", assetId, versionId, sourceName);
      const auto outputDir = QFileInfo(QDir(job->stage->path()).filePath(relative)).absolutePath();
      const auto parts = PaleoMapFiles::copyDataset(path, outputDir, cancelled, &error);
      if (cancelled()) return {};
      if (parts.isEmpty()) { job->result.errors << sourceName + ": " + error; QDir(outputDir).removeRecursively(); continue; }
      const auto identity = PaleoMapFiles::fingerprint(parts);
      if (!old.id.isEmpty() && old.extra.value("bundle_sha256").toString() == identity) {
        bool intact = true; const auto storedDirectory = QFileInfo(DataCatalog::resolvedVersionPath(directory, old)).absoluteDir();
        for (const auto &entry : old.extra.value("bundle_members").toList()) {
          const auto member = entry.toMap(); const auto name = member.value("file").toString(); QString why;
          intact &= DataCatalog::isSafePathSegment(name) && DataCatalog::sha256FileHex(storedDirectory.filePath(name), &why, cancelled) == member.value("sha256").toString();
        }
        if (intact) { ++job->result.unchanged; QDir(outputDir).removeRecursively(); continue; }
      }
      const auto stagedPath = QDir(job->stage->path()).filePath(relative);
      auto metadata = PaleoMapSource::inspect(stagedPath, &error);
      if (metadata.isEmpty()) { job->result.errors << sourceName + ": " + error; QDir(outputDir).removeRecursively(); continue; }
      metadata.insert("horizon", horizon); metadata.insert("paleomap", true); metadata.insert("kind", "existing_paleomap");
      metadata.insert("layer_id", (metadata.value("facies").toList().isEmpty() ? "paleomap." : "product.") + versionId); metadata.insert("bundle_members", parts); metadata.insert("bundle_sha256", identity);
      metadata.insert("mapping_product", !metadata.value("facies").toList().isEmpty());
      CatalogAsset asset{assetId, "paleogeographic_map", QFileInfo(path).suffix().toLower(), sourceName};
      CatalogVersion version; version.id = versionId; version.assetId = assetId; version.stage = "RAW";
      version.versionNumber = old.id.isEmpty() ? 1 : old.versionNumber + 1; version.path = relative; version.fileName = sourceName;
      version.sourceUri = path; version.managed = true; version.extra = metadata;
      for (const auto &p : parts) if (p.toMap().value("file").toString() == sourceName) version.sha256 = p.toMap().value("sha256").toString();
      if (!old.id.isEmpty()) version.parentVersionIds << old.id;
      if ((old.id.isEmpty() && !cat->addAsset(asset, &error)) || !cat->addVersion(version, &error)) return error;
      job->directories << QFileInfo(relative).path(); job->result.assets << assetId;
      task->reportBytes(i + 1, files.size());
    }
    if (job->result.assets.isEmpty() && job->result.unchanged == 0 && !job->result.errors.isEmpty()) return job->result.errors.join("\n");
    return {};
  });
  QObject::connect(task, &PaleoTask::finished, tasks, [job, owner, layerService, tasks, task, target, sequence, directory, done] {
    job->result.cancelled = task->cancelRequested() || !owner || owner->projectDir() != directory || task->session() != tasks->session();
    QStringList moved;
    if (!job->result.cancelled && task->state() == PaleoTask::State::Succeeded) {
      QString error;
      if (owner->mutationSeq() != sequence) error = QObject::tr("导入期间目录已变化，请重试本批次");
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
