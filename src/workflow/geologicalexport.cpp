// 层：功能
#include "geologicalexport.h"
#include "mapexport.h"
#include "../qgis/geologicalmaplayout.h"
#include "../qgis/dataexportwriter.h"
#include "../qgis/mapbooklayout.h"
#include "../catalog/datacatalog.h"
#include "../services/dataexport.h"
#include "../services/paleotaskservice.h"
#include <QDateTime>
#include <QDir>
#include <QDirIterator>
#include <QFileInfo>
#include <QJsonDocument>
#include <QPointer>
#include <QSaveFile>
#include <QTemporaryDir>
#include <qgis.h>
#include <qgslayoutitemmap.h>
#include <qgsprintlayout.h>
#include <qgsproject.h>
#include <qgsreadwritecontext.h>

namespace PaleoGeologicalExport {
namespace {
struct Job {
  PaleoLayoutExport::ProjectSnapshot project;
  QDomDocument layout;
  std::unique_ptr<QTemporaryDir> output, managed;
  std::unique_ptr<DataCatalog> catalog;
  QVariantMap description;
  QStringList requiredLayers;
  QVector<CatalogOp> journal;
  Result result;
};
}
PaleoTask *start(QgsPrintLayout *layout, DataCatalog *catalog, PaleoTaskService *tasks,
    const Request &request, QObject *receiver, std::function<void(const Result &)> done, QString *error) {
  const auto fail = [error](const QString &text) -> PaleoTask * { if (error) *error = text; return nullptr; };
  const QFileInfo destination(request.destination);
  if (!layout || !catalog || !catalog->isOpen() || !tasks || request.formats.isEmpty())
    return fail(QObject::tr("请先打开工程并准备图件版面"));
  if (!qIsFinite(request.dpi) || request.dpi < 72 || request.dpi > 1200)
    return fail(QObject::tr("导出 DPI 必须在 72～1200 范围内"));
  const auto managedParent = QDir(catalog->projectDir()).absoluteFilePath("exports/maps");
  if (request.registerInProject && destination.absolutePath() == managedParent && !QDir(managedParent).exists() && !QDir().mkpath(managedParent))
    return fail(QObject::tr("无法创建工区图件输出目录"));
  if (request.destination.isEmpty() || destination.exists() || !destination.absoluteDir().exists())
    return fail(QObject::tr("导出目标必须是已有父目录中的新批次目录"));
  if (request.includeSources && request.sourceVersionIds.isEmpty())
    return fail(QObject::tr("当前地图尚未绑定可追溯的数据版本，请先保存成果数据"));
  auto job = std::make_shared<Job>();
  if (!PaleoLayoutExport::captureProjectSnapshot(layout->project(), job->project, error)) return nullptr;
  QgsReadWriteContext writeContext;
  job->layout.appendChild(layout->writeXml(job->layout, writeContext));
  job->description = PaleoGeologicalMapLayout::describe(layout);
  for (auto *item : layout->items()) if (auto *map = dynamic_cast<QgsLayoutItemMap *>(item))
    for (auto *layer : map->layers()) if (layer && !job->requiredLayers.contains(layer->id())) job->requiredLayers << layer->id();
  const auto projectDirectory = catalog->projectDir();
  job->managed = std::make_unique<QTemporaryDir>(QDir(projectDirectory).filePath(".map-products-XXXXXX"));
  job->output = std::make_unique<QTemporaryDir>(destination.absoluteDir().filePath(".paleo-map-XXXXXX"));
  if (!job->managed->isValid() || !job->output->isValid()) return fail(QObject::tr("无法创建图件暂存目录"));
  job->catalog = catalog->createStagingCopy(job->managed->path());
  const auto sequence = catalog->mutationSeq();
  QPointer<DataCatalog> owner(catalog);
  QPointer<QObject> target(receiver ? receiver : tasks);
  auto *task = tasks->start(QObject::tr("导出地质图：%1").arg(destination.fileName()),
      [job, request, projectDirectory](PaleoTask *task) -> QString {
    const auto cancel = [task] { return task->cancelRequested(); };
    const auto fail = [job](const QString &message) { return job->result.error = message; };
    QVariantList inputMetadata;
    QHash<QString, QString> inputChecksums;
    const auto verifyInputs = [&](bool collect) -> QString {
      for (const auto &id : request.sourceVersionIds) {
        if (cancel()) return {};
        const auto version = job->catalog->versionById(id);
        if (version.id.isEmpty()) return QObject::tr("输入版本缺失：%1").arg(id);
        QString why;
        const auto actual = DataCatalog::sha256FileHex(job->catalog->versionFilePath(version), &why, cancel);
        if (cancel()) return {};
        if (actual.isEmpty()) return QObject::tr("输入版本 %1 无法读取：%2").arg(id, why);
        const auto expected = collect ? version.sha256 : inputChecksums.value(id);
        if (!expected.isEmpty() && actual.compare(expected, Qt::CaseInsensitive) != 0)
          return QObject::tr("输入版本 %1 的 SHA-256 不符，文件已变化，请重新保存或核对来源").arg(id);
        if (collect) {
          inputChecksums.insert(id, actual);
          if (version.sha256.isEmpty()) job->result.warnings << QObject::tr("输入 %1 没有历史摘要，只能核对本次导出期间的内容一致性").arg(id);
          inputMetadata.append(QVariantMap{{"version_id", id}, {"asset_id", version.assetId},
              {"sha256", actual}, {"stage", version.stage}, {"metadata", version.extra}});
        }
      }
      return {};
    };
    task->reportStage("hash", 0);
    const auto inputError = verifyInputs(true);
    if (!inputError.isEmpty()) return fail(inputError);
    task->reportStage("build", 5);
    if (cancel()) return {};
    QString error;
    auto project = PaleoLayoutExport::rebuildProject(job->project, &error, &job->result.warnings);
    if (!project) return fail(error);
    for (const auto &id : job->requiredLayers)
      if (!project->mapLayer(id) || !project->mapLayer(id)->isValid())
        return fail(QObject::tr("所选制图图层数据源不可用：%1").arg(id));
    QgsPrintLayout rebuilt(project.get());
    QgsReadWriteContext readContext;
    if (!rebuilt.readXml(job->layout.documentElement(), job->layout, readContext)) return fail(QObject::tr("无法重建图件版面"));
    QVariantList registered;
    QSet<int> formats;
    for (const auto format : request.formats) {
      if (cancel()) return {};
      if (formats.contains(int(format))) continue;
      formats.insert(int(format));
      const auto extension = PaleoLayoutExport::formatExtension(format);
      const auto output = PaleoLayoutExport::exportLayout(&rebuilt, job->output->filePath("map." + extension), format, request.dpi, request.pages, request.options);
      if (!output.ok) return fail(output.error);
      if (!PaleoGeologicalMapLayout::validateBounds(&rebuilt, &error)) return fail(error);
      for (const auto &path : output.files) {
        if (cancel()) return {};
        if (!PaleoMapBookLayout::validateExportedFile(path, format, -1, -1, &error)) return fail(error);
        if (request.registerInProject) {
          QString hash, managed;
          const auto asset = registerMapPdfAsset(job->catalog.get(), job->managed->path(), path, &hash, &managed, &error, extension, request.context.value("horizon").toString());
          if (asset.isEmpty()) return fail(error);
          job->result.assetIds << asset;
          registered.append(QVariantMap{{"asset_id", asset}, {"path", QFileInfo(path).fileName()}, {"sha256", hash}, {"format", extension}});
          if (format == PaleoLayoutExport::Format::Pdf && job->result.pdfAssetId.isEmpty()) {
            job->result.pdfAssetId = asset; job->result.pdfPath = managed; job->result.pdfSha256 = hash;
          }
        }
      }
    }
    if (cancel()) return {};
    const auto changedInput = verifyInputs(false);
    if (!changedInput.isEmpty()) return fail(changedInput);
    task->reportStage("encode", 65);
    QgsReadWriteContext templateContext;
    if (!rebuilt.saveAsTemplate(job->output->filePath("layout.qpt"), templateContext)) return fail(QObject::tr("无法写出版面模板"));
    const auto unbound = request.context.value("unbound_layers").toStringList();
    if (request.includeSources && !unbound.isEmpty())
      job->result.warnings << QObject::tr("制作源数据仅含已登记来源，以下图层无资产版本：%1").arg(unbound.join("、"));
    QVariantList layerData;
    if (request.includeData) {
      for (int i = 0; i < job->requiredLayers.size(); ++i) {
        if (cancel()) return {};
        auto *layer = project->mapLayer(job->requiredLayers[i]);
        QStringList files;
        QVariantMap detail;
        if (!PaleoDataExportWriter::writeLayer(layer, job->output->filePath(QString("data/layers/%1").arg(i + 1)), &files, &detail, &error)) return fail(error);
        detail.insert("qgis_layer_id", layer->id()); detail.insert("title", layer->name());
        QVariantList relativeFiles;
        for (const auto &file : files) relativeFiles << QDir(job->output->path()).relativeFilePath(file);
        detail.insert("files", relativeFiles); layerData.append(detail);
      }
    }
    if (request.includeSources) {
      task->reportStage("scan", 70);
      paleo::exporting::DataRequest data;
      data.versionIds = request.sourceVersionIds;
      data.destination = job->output->filePath("sources");
      data.scope = request.includeSources ? paleo::exporting::Scope::Sources : paleo::exporting::Scope::Current;
      const auto plan = paleo::exporting::plan(*job->catalog, projectDirectory, data, cancel);
      const auto result = paleo::exporting::execute(plan, cancel, [task](qint64 done, qint64 total, const QString &text) {
        task->reportBytes(done, total); task->reportDetail(text);
      });
      if (cancel()) return {};
      if (!result.complete()) return fail(result.error.isEmpty() ? result.failures.join("\n") : result.error);
      job->result.warnings << result.warnings;
    }
    QFile readme(job->output->filePath("README.txt"));
    const auto explanation = QObject::tr("Paleo 地质图导出\nmap.* 为带整饰的图件；metadata.json 记录格式、输入版本、图层和校验和。\n"
        "data/layers（如已选择）为当前图层的 GPKG 或数值 GeoTIFF，含 QML 样式。\n"
        "sources（如已选择）为已记录的制作输入版本。\n"
        "layout.qpt 是版面模板，使用时需先在 QGIS 工程加载对应数据；本批次没有生成独立 QGZ。\n"
        "远程底图仅记录来源，不包含离线瓦片。\n").toUtf8();
    if (!readme.open(QIODevice::WriteOnly) || readme.write(explanation) != explanation.size()) return fail(QObject::tr("无法写出图件使用说明"));
    readme.close();
    QVariantList files;
    QDirIterator iterator(job->output->path(), QDir::Files, QDirIterator::Subdirectories);
    while (iterator.hasNext()) {
      if (cancel()) return {};
      const auto path = iterator.next();
      const auto hash = DataCatalog::sha256FileHex(path, &error, cancel);
      if (cancel()) return {};
      if (hash.isEmpty()) return fail(error);
      const auto relative = QDir(job->output->path()).relativeFilePath(path);
      job->result.files << relative;
      files.append(QVariantMap{{"path", relative}, {"sha256", hash}, {"bytes", QFileInfo(path).size()}});
    }
    auto context = job->description;
    for (auto i = request.context.begin(); i != request.context.end(); ++i) context.insert(i.key(), i.value());
    const auto bytes = QJsonDocument::fromVariant(QVariantMap{{"schema", "paleo_map_export_v1"},
        {"registered_in_project", request.registerInProject}, {"exported_utc", QDateTime::currentDateTimeUtc().toString(Qt::ISODate)}, {"qgis_version", Qgis::version()},
        {"dpi", request.dpi}, {"world_file", request.options.worldFile}, {"geo_pdf", request.options.geoPdf},
        {"text_as_paths", request.options.textAsPaths}, {"transparent_background", request.options.transparentBackground}, {"context", context}, {"input_versions", request.sourceVersionIds},
        {"source_package_requested", request.includeSources},
        {"source_coverage_complete", request.includeSources && unbound.isEmpty()},
        {"input_metadata", inputMetadata}, {"files", files}, {"layer_data", layerData}, {"products", registered}, {"warnings", job->result.warnings}}).toJson(QJsonDocument::Indented);
    QSaveFile metadata(job->output->filePath("metadata.json"));
    if (!metadata.open(QIODevice::WriteOnly) || metadata.write(bytes) != bytes.size() || !metadata.commit()) return fail(QObject::tr("无法写出图件说明"));
    job->result.files << "metadata.json";
    // Keep the same immutable context on each registered image/PDF without
    // inventing computational parentage for a cartographic rendering.
    for (const auto &asset : job->result.assetIds) {
      const auto version = job->catalog->currentVersion(asset);
      auto history = version.extra.value("map_exports").toList();
      if (!job->catalog->updateVersionExtra(version.id, "horizon", context.value("horizon"), &error)) return fail(error);
      history.append(QVariantMap{{"context", context}, {"inputs", request.sourceVersionIds}, {"batch", request.destination}});
      if (!job->catalog->updateVersionExtra(version.id, "map_exports", history, &error)) return fail(error);
    }
    job->journal = job->catalog->journal();
    task->reportStage("publish", 95);
    return {};
  });
  QObject::connect(task, &PaleoTask::finished, tasks,
      [job, request, projectDirectory, owner, sequence, task, tasks, target, done] {
    const bool stale = !owner || owner->projectDir() != projectDirectory || (request.registerInProject && owner->mutationSeq() != sequence) || task->session() != tasks->session();
    job->result.cancelled = task->cancelRequested() || stale;
    if (stale) task->discardPublication();
    if (!job->result.cancelled && task->state() == PaleoTask::State::Succeeded) {
      if (!QDir().rename(job->output->path(), request.destination)) job->result.error = QObject::tr("无法提交图件目录，目标可能已存在");
      else if (request.registerInProject && !publishMapExportJournal(owner, projectDirectory, job->managed->path(), job->journal, &job->result.error))
        QDir(request.destination).removeRecursively();
      else {
        job->result.directory = QFileInfo(request.destination).absoluteFilePath();
        if (!job->result.pdfAssetId.isEmpty()) job->result.pdfPath = owner->versionFilePath(owner->currentVersion(job->result.pdfAssetId));
        for (auto &file : job->result.files) file = QDir(job->result.directory).filePath(file);
      }
      if (!job->result.error.isEmpty()) task->failPublication(job->result.error);
    }
    if (task->state() == PaleoTask::State::Failed && job->result.error.isEmpty()) job->result.error = task->errorText();
    if (job->result.cancelled || !job->result.error.isEmpty()) { job->result.pdfAssetId.clear(); job->result.pdfPath.clear(); job->result.pdfSha256.clear(); job->result.assetIds.clear(); job->result.files.clear(); }
    // Completed task history must not retain large snapshots or cancelled
    // staging trees. No live project files reside in these two private roots.
    job->catalog.reset();
    job->output.reset(); job->managed.reset();
    job->project = {}; job->layout.clear();
    if (target && done) done(job->result);
  });
  return task;
}
}
