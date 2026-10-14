// 层：功能
#include "mapversioncontroller.h"
#include "../metadata/mapsnapshot.h"
#include "../metadata/paleoprojectstore.h"
#include "../catalog/datacatalog.h"
#include "../domain/predictionevidence.h"
#include "../qgis/qgislayerservice.h"
#include "../qgis/mappingfeatureidentity.h"
#include "../services/jobrunner.h"
#include <QDir>
#include <QDateTime>
#include <QHash>
#include <QFileInfo>
#include <QJsonDocument>
#include <QTemporaryDir>
#include <qgsvectorlayer.h>
#include <qgsvectorlayerfeatureiterator.h>
#include <qgsvectorfilewriter.h>

namespace {
bool validatePublicationCatalog(DataCatalog *catalog, const QVariantMap &provenance, QString *error) {
  if (provenance.value("catalog_bound").toBool() && (!catalog || !catalog->isOpen())) {
    if (error) *error = QObject::tr("保存版本引用的资料目录不可用，请重新打开工程后发布"); return false;
  }
  if (catalog)
    for (const auto &entry : provenance.value("evidence_snapshot").toList()) {
      const auto row = entry.toMap(); const auto evidence = row.value("evidence").toMap();
      if (catalog->versionById(row.value("version_id").toString()).id.isEmpty()) { if (error) *error = QObject::tr("保存成果的目录版本缺失"); return false; }
      for (const auto &input : evidence.value("inputs").toList())
        if (catalog->versionById(input.toMap().value("version_id").toString()).id.isEmpty()) { if (error) *error = QObject::tr("保存证据来源版本缺失"); return false; }
      for (const auto &finding : paleo::evidence::validate(evidence))
        if (finding.toMap().value("severity").toString() == "Blocking") { if (error) *error = finding.toMap().value("message").toString(); return false; }
    }
  return true;
}
struct VectorSnapshotSource {
  QString layerId, identity;
  std::shared_ptr<QgsVectorLayerFeatureSource> source;
  Qgis::WkbType geometryType;
  qint64 count = 0;
  QString originalFile;
  qint64 originalSize = -1, originalModified = -1;
};
struct SnapshotJob {
  quint64 epoch = 0;
  MapSnapshotInput input;
  PreparedMapSnapshot prepared;
  QVector<VectorSnapshotSource> vectors;
  std::shared_ptr<QTemporaryDir> temporary;
  QString error;
  bool cancelled = false;
  ~SnapshotJob() { discardPreparedMapSnapshot(prepared); }
};
bool computeSnapshot(SnapshotJob &job, const paleo::jobs::CancelFn &cancel,
                     const paleo::jobs::ProgressFn &progress) {
  if (job.input.operation == "save") {
    job.temporary = std::make_shared<QTemporaryDir>();
    if (!job.temporary->isValid()) { job.error = QStringLiteral("无法创建地图矢量暂存目录"); return false; }
    QHash<QString, QString> exported;
    for (int n = 0; n < job.vectors.size(); ++n) {
      if (cancel()) { job.cancelled = true; return false; }
      const auto &vector = job.vectors[n]; QString path = exported.value(vector.identity);
      const auto sourceUnchanged = [&] {
        if (vector.originalFile.isEmpty()) return true;
        const QFileInfo source(vector.originalFile);
        return source.isFile() && source.size() == vector.originalSize && source.lastModified().toMSecsSinceEpoch() == vector.originalModified;
      };
      if (!sourceUnchanged()) { job.error = QStringLiteral("矢量来源在准备后已变化，请重试"); return false; }
      if (path.isEmpty()) {
        path = job.temporary->filePath(QString::number(n) + ".gpkg");
        QgsVectorFileWriter::SaveVectorOptions options; options.driverName = "GPKG";
        options.layerName = "features"; options.fileEncoding = "UTF-8";
        std::unique_ptr<QgsVectorFileWriter> writer(QgsVectorFileWriter::create(path,
            vector.source->fields(), vector.geometryType, vector.source->crs(), QgsCoordinateTransformContext(), options));
        if (!writer || writer->hasError() != QgsVectorFileWriter::NoError) {
          job.error = writer ? writer->errorMessage() : QStringLiteral("无法创建矢量快照写入器"); return false;
        }
        auto features = vector.source->getFeatures(); QgsFeature feature; qint64 count = 0;
        while (features.nextFeature(feature)) {
          if (cancel()) { job.cancelled = true; return false; }
          if (!writer->addFeature(feature)) { job.error = writer->lastError(); return false; }
          if (++count % 256 == 0) progress(20.0 * (n + double(count) / qMax<qint64>(1, vector.count)) / qMax<qsizetype>(1, job.vectors.size()), "encode");
        }
        features.close(); writer.reset();
        QgsVectorLayer generated(path + "|layername=features", "snapshot", "ogr");
        if (!MappingFeatureIdentity::ensure(&generated, &job.error)) return false;
        if (!sourceUnchanged()) { job.error = QStringLiteral("矢量来源在导出期间已变化，请重试"); return false; }
        exported.insert(vector.identity, path);
      }
      for (auto &entry : job.input.files) {
        auto record = entry.toMap();
        if (record.value("layer_id").toString() != vector.layerId) continue;
        record.insert("source", path); record.insert("source_suffix", "|layername=features"); entry = record;
      }
    }
  }
  job.prepared = prepareMapSnapshot(job.input, cancel, [progress](double p) { progress(20 + .8 * p, "hash"); });
  job.cancelled = job.prepared.cancelled; job.error = job.prepared.error;
  return job.prepared.isValid();
}
}
struct MapVersionController::AsyncState {
  explicit AsyncState(QObject *owner) : runner(owner) {}
  paleo::jobs::JobRunner<SnapshotJob> runner;
  std::function<void()> releaseTokens;
};
MapVersionController::~MapVersionController() { cancelSnapshot(); }
void MapVersionController::setTaskService(PaleoTaskService *tasks) { if (m_tasks != tasks) cancelSnapshot(); m_tasks = tasks; }
bool MapVersionController::snapshotBusy() const { return m_async && m_async->runner.busy(); }
void MapVersionController::cancelSnapshot() {
  if (!m_async) return;
  m_async->runner.requestCancel();
  if (m_async->releaseTokens) { m_async->releaseTokens(); m_async->releaseTokens = {}; }
}
void MapVersionController::abandonForProjectClose() { cancelSnapshot(); ++m_snapshotEpoch; }

bool MapVersionController::startSaveVersion(const QString &horizon, const QVariantMap &provenance, QString *error) {
  if (!m_tasks) {
    const auto result = saveVersion(horizon, provenance, error);
    emit snapshotFinished(horizon, "save", result.version > 0, false, error ? *error : QString());
    return result.version > 0;
  }
  if (!m_store || snapshotBusy()) { if (error) *error = tr("已有地图快照任务或版本存储不可用"); return false; }
  auto job = std::make_shared<SnapshotJob>(); job->epoch = m_snapshotEpoch;
  QVector<LayerDeclaration> declarations; QVariantMap frozen;
  if (!captureVersion(horizon, provenance, &declarations, &frozen, error)) return false;
  job->input = m_store->snapshotInputForSave(horizon,
      QString::fromUtf8(QJsonDocument::fromVariant(frozen).toJson(QJsonDocument::Compact)), declarations, error);
  if (!job->input.valid) { releaseSnapshotTokens(horizon); return false; }
  for (const auto &decl : declarations) {
    if (decl.horizon != horizon || decl.type.compare("vector", Qt::CaseInsensitive) != 0) continue;
    auto *layer = qobject_cast<QgsVectorLayer *>(m_layers->layer(decl.layerId)); if (!layer) continue;
    QVariantList fieldDefinitions;
    const auto fields = layer->fields();
    for (int n = 0; n < fields.size(); ++n)
      fieldDefinitions << QVariantMap{{"name", fields[n].name()}, {"type", int(fields[n].type())},
          {"expression", layer->expressionField(n)}};
    const auto key = paleo::evidence::fingerprint(QVariantMap{{"uri", decl.source}, {"fields", fieldDefinitions},
        {"crs", layer->crs().toWkt()}, {"subset", layer->subsetString()}, {"native_layer", layer->id()}, {"version", layer->customProperty("paleoVersionId")}});
    VectorSnapshotSource source{decl.layerId, key, std::make_shared<QgsVectorLayerFeatureSource>(layer), layer->wkbType()};
    source.count = layer->featureCount();
    const QFileInfo file(decl.source.section('|', 0, 0));
    if (file.isFile()) { source.originalFile = file.absoluteFilePath(); source.originalSize = file.size(); source.originalModified = file.lastModified().toMSecsSinceEpoch(); }
    job->vectors << source;
  }
  if (!m_async) m_async = std::make_shared<AsyncState>(this);
  m_async->runner.setTaskService(m_tasks);
  // Capture the old project's token owner and IDs. Never free a same-name layer
  // in a newly opened project when an older task eventually finishes.
  QPointer<PaleoProjectStore> tokenStore = m_projectStore;
  const QString tokenProject = tokenStore ? tokenStore->metaDbPath() : QString();
  QStringList tokenIds;
  for (const auto &decl : declarations) {
    if (decl.horizon != horizon || decl.type.compare("vector", Qt::CaseInsensitive)) continue;
    tokenIds << decl.layerId;
    if (m_layers) if (auto *layer = m_layers->layer(decl.layerId)) tokenIds << layer->id();
  }
  m_async->releaseTokens = [tokenStore, tokenProject, tokenIds] {
    if (tokenStore && tokenStore->metaDbPath() == tokenProject)
      for (const auto &id : tokenIds) tokenStore->markLayerFree(id);
  };
  paleo::jobs::JobRunner<SnapshotJob>::Callbacks callbacks;
  callbacks.compute = computeSnapshot;
  callbacks.commit = [this](SnapshotJob &j, QString *err) {
    if (!j.error.isEmpty()) { if (err) *err = j.error; return false; }
    if (!m_store || j.epoch != m_snapshotEpoch || j.input.projectDirectory != m_store->projectDirectory()) {
      if (err) *err = tr("工程已切换，旧地图快照已丢弃"); return false;
    }
    MapVersion result;
    const auto commit = [&]() -> PaleoProjectStore::WriteResult {
      result = m_store->commitPreparedVersion(j.prepared, err); return {result.version > 0, err ? *err : QString()};
    };
    const auto status = m_projectStore ? m_projectStore->enqueueWrite(commit) : commit();
    if (!status.ok) { if (err && err->isEmpty()) *err = status.error; return false; }
    emit versionSaved(j.input.horizon, result.version); return true;
  };
  callbacks.cleanup = [](SnapshotJob &j) { discardPreparedMapSnapshot(j.prepared); };
  callbacks.onDropped = [](SnapshotJob &j, paleo::jobs::DropReason reason) {
    j.cancelled = reason == paleo::jobs::DropReason::Cancelled;
    j.error = paleo::jobs::dropReasonText(reason);
  };
  const auto connection = std::make_shared<QMetaObject::Connection>();
  *connection = connect(&m_async->runner, &paleo::jobs::JobRunnerBase::jobCompleted, this,
      [this, job, connection](quint64, bool) {
    disconnect(*connection);
    job->vectors.clear(); job->temporary.reset();
    emit snapshotBusyChanged(snapshotBusy());
    if (m_async->releaseTokens) { m_async->releaseTokens(); m_async->releaseTokens = {}; }
    if (job->epoch == m_snapshotEpoch)
      emit snapshotFinished(job->input.horizon, "save", job->prepared.committed, job->cancelled, job->error);
  });
  auto *task = m_async->runner.start(tr("%1 保存地图与证据快照").arg(horizon), job, callbacks);
  if (!task) { disconnect(*connection); if (m_async->releaseTokens) m_async->releaseTokens(); m_async->releaseTokens = {}; return false; }
  emit snapshotBusyChanged(snapshotBusy());
  task->setNavigationContext({{"horizon", horizon}, {"workspace", "compose"}, {"operation", "map_snapshot_save"}});
  return true;
}

bool MapVersionController::startPublish(const QString &horizon, const QString &residualSummary, QString *error) {
  if (!m_tasks) {
    const auto result = publish(horizon, residualSummary, error);
    emit snapshotFinished(horizon, "publish", !result.isEmpty(), false, error ? *error : QString()); return !result.isEmpty();
  }
  if (!m_store || snapshotBusy()) { if (error) *error = tr("已有地图快照任务或版本存储不可用"); return false; }
  QVector<LayerDeclaration> declarations;
  if (m_layers && !m_layers->tryDeclared(&declarations, error)) return false;
  const auto latest = m_store->latest(horizon);
  const auto provenance = QJsonDocument::fromJson(latest.provenance.toUtf8()).toVariant().toMap();
  if (!validatePublicationCatalog(m_catalog, provenance, error)) return false;
  auto job = std::make_shared<SnapshotJob>(); job->epoch = m_snapshotEpoch;
  job->input = m_store->snapshotInputForPublish(horizon, declarations, residualSummary, error);
  if (!job->input.valid) return false;
  if (!m_async) m_async = std::make_shared<AsyncState>(this);
  m_async->runner.setTaskService(m_tasks);
  paleo::jobs::JobRunner<SnapshotJob>::Callbacks callbacks; callbacks.compute = computeSnapshot;
  callbacks.commit = [this](SnapshotJob &j, QString *err) {
    if (!j.error.isEmpty()) { if (err) *err = j.error; return false; }
    if (!m_store || j.epoch != m_snapshotEpoch || j.input.projectDirectory != m_store->projectDirectory()) {
      if (err) *err = tr("工程已切换，旧发布快照已丢弃"); return false;
    }
    if (!validatePublicationCatalog(m_catalog, j.prepared.provenance, err)) return false;
    QString result;
    const auto commit = [&]() -> PaleoProjectStore::WriteResult {
      result = m_store->commitPreparedPublication(j.prepared, err); return {!result.isEmpty(), err ? *err : QString()};
    };
    const auto status = m_projectStore ? m_projectStore->enqueueWrite(commit) : commit();
    if (!status.ok) { if (err && err->isEmpty()) *err = status.error; return false; }
    emit published(j.input.horizon, j.input.versionNumber, result); return true;
  };
  callbacks.cleanup = [](SnapshotJob &j) { discardPreparedMapSnapshot(j.prepared); };
  callbacks.onDropped = [](SnapshotJob &j, paleo::jobs::DropReason reason) {
    j.cancelled = reason == paleo::jobs::DropReason::Cancelled; j.error = paleo::jobs::dropReasonText(reason);
  };
  const auto connection = std::make_shared<QMetaObject::Connection>();
  *connection = connect(&m_async->runner, &paleo::jobs::JobRunnerBase::jobCompleted, this,
      [this, job, connection](quint64, bool) {
    disconnect(*connection);
    job->vectors.clear(); job->temporary.reset();
    emit snapshotBusyChanged(snapshotBusy());
    if (job->epoch == m_snapshotEpoch)
      emit snapshotFinished(job->input.horizon, "publish", job->prepared.committed, job->cancelled, job->error);
  });
  auto *task = m_async->runner.start(tr("%1 发布地图与证据快照").arg(horizon), job, callbacks);
  if (!task) { disconnect(*connection); return false; }
  emit snapshotBusyChanged(snapshotBusy());
  task->setNavigationContext({{"horizon", horizon}, {"workspace", "compose"}, {"operation", "map_snapshot_publish"}});
  return true;
}
