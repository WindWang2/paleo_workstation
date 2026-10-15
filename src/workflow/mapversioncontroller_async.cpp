// 层：功能
#include "mapversioncontroller.h"
#include "mapversionvalidation_internal.h"
#include "../metadata/mapsnapshot.h"
#include "../metadata/paleoprojectstore.h"
#include "../catalog/datacatalog.h"
#include "../domain/predictionevidence.h"
#include "../qgis/qgislayerservice.h"
#include "../qgis/mappingfeatureidentity.h"
#include "../services/jobrunner.h"
#include <QDir>
#include <QElapsedTimer>
#include <QTimer>
#include <QUuid>
#include <QDateTime>
#include <QHash>
#include <QFileInfo>
#include <QJsonDocument>
#include <QTemporaryDir>
#include <atomic>
#include <qgsvectorlayer.h>
#include <qgsvectorlayerfeatureiterator.h>
#include <qgsvectorfilewriter.h>

namespace {
bool validatePublicationCatalog(DataCatalog *catalog, const QVariantMap &provenance, QString *error) {
  if (provenance.value("catalog_bound").toBool() && (!catalog || !catalog->isOpen())) {
    if (error) *error = QObject::tr("保存版本引用的资料目录不可用，请重新打开工程后发布"); return false;
  }
  const auto cited = provenance.value(QStringLiteral("cited_sources"));
  if (provenance.contains(QStringLiteral("cited_sources")) && cited.typeId() != QMetaType::QVariantList) {
    if (error) *error = QObject::tr("保存版本的人工来源清单格式损坏，发布被拒绝"); return false;
  }
  if (catalog)
    for (const auto &entry : cited.toList()) {
      const auto id = entry.toMap().value(QStringLiteral("version_id")).toString();
      if (id.isEmpty()) continue;
      if (catalog->versionById(id).id.isEmpty()) {
        if (error) *error = QObject::tr("保存证据的来源版本已缺失：%1").arg(id); return false;
      }
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
};
struct SnapshotJob {
  quint64 epoch = 0;
  MapSnapshotInput input;
  PreparedMapSnapshot prepared;
  QVector<VectorSnapshotSource> vectors;
  std::shared_ptr<QTemporaryDir> temporary;
  std::shared_ptr<std::atomic_bool> stale;
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
      // Owner-thread edits flip the shared flag. GPKG size/mtime is not a
      // content snapshot: WAL can change bytes without it, and a checkpoint
      // can change it without a business edit.
      const auto sourceChanged = [&] {
        return job.stale && job.stale->load(std::memory_order_acquire);
      };
      if (sourceChanged()) { job.error = QStringLiteral("矢量来源在准备后已变化，请重试"); return false; }
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
        if (sourceChanged()) { job.error = QStringLiteral("矢量来源在导出期间已变化，请重试"); return false; }
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
struct MapVersionController::SavePreparation {
  struct Layer {
    QString id;
    QPointer<QgsVectorLayer> vector;
  };
  QString horizon, projectDirectory;
  QVariantMap provenance;
  QVector<Layer> layers;
  QVector<QMetaObject::Connection> watches;
  QgsFeatureIterator features;
  MapVersionEvidenceScan scan;
  quint64 epoch = 0;
  qint64 total = 0;
  int layerIndex = 0;
  bool opened = false, cancelled = false, changed = false;
  ~SavePreparation() { close(); }
  void close() {
    features.close();
    for (const auto &watch : watches) QObject::disconnect(watch);
    watches.clear();
  }
};
struct MapVersionController::AsyncState {
  explicit AsyncState(QObject *owner) : runner(owner) {}
  paleo::jobs::JobRunner<SnapshotJob> runner;
  std::function<void()> releaseTokens;
  QVector<QMetaObject::Connection> sourceWatches;
  std::shared_ptr<SavePreparation> preparation;
  void disconnectSourceWatches() {
    for (const auto &watch : sourceWatches) QObject::disconnect(watch);
    sourceWatches.clear();
  }
};
MapVersionController::~MapVersionController() { cancelSnapshot(); }
void MapVersionController::setTaskService(PaleoTaskService *tasks) { if (m_tasks != tasks) cancelSnapshot(); m_tasks = tasks; }
bool MapVersionController::snapshotBusy() const { return m_async && (m_async->preparation || m_async->runner.busy()); }
void MapVersionController::cancelSnapshot() {
  if (!m_async) return;
  if (m_async->preparation) { m_async->preparation->cancelled = true; m_async->preparation->close(); }
  m_async->runner.requestCancel();
  // Worker still owns its source read. Release only when it has stopped.
}
void MapVersionController::abandonForProjectClose() { cancelSnapshot(); ++m_snapshotEpoch; }

bool MapVersionController::startSaveVersion(const QString &horizon, const QVariantMap &provenance, QString *error) {
  if (!m_tasks) {
    const auto result = saveVersion(horizon, provenance, error);
    emit snapshotFinished(horizon, "save", result.version > 0, false, error ? *error : QString());
    return result.version > 0;
  }
  if (!m_store || snapshotBusy()) { if (error) *error = tr("已有地图快照任务或版本存储不可用"); return false; }
  QVector<LayerDeclaration> declarations;
  if (m_layers && !m_layers->tryDeclared(&declarations, error)) return false;
  if (m_store->isReadOnly()) { if (error) *error = tr("工程不可写，保存被拒绝"); return false; }
  if (!m_async) m_async = std::make_shared<AsyncState>(this);
  auto preparation = std::make_shared<SavePreparation>();
  preparation->horizon = horizon; preparation->provenance = provenance;
  preparation->projectDirectory = m_store->projectDirectory(); preparation->epoch = m_snapshotEpoch;
  const std::weak_ptr<SavePreparation> weak = preparation;
  const auto changed = [weak] { if (auto p = weak.lock()) p->changed = true; };
  for (const auto &decl : declarations) {
    if (decl.horizon != horizon || decl.type.compare("vector", Qt::CaseInsensitive)) continue;
    auto *layer = qobject_cast<QgsVectorLayer *>(m_layers->instantiate(decl.layerId, error));
    if (!layer) continue; // Missing optional layers are recorded by the store.
    SavePreparation::Layer captured; captured.id = decl.layerId; captured.vector = layer;
    preparation->layers << captured;
    preparation->total += qMax<qint64>(0, layer->featureCount());
    preparation->watches << connect(layer, &QgsVectorLayer::geometryChanged, this, changed)
      << connect(layer, &QgsVectorLayer::attributeValueChanged, this, changed)
      << connect(layer, &QgsVectorLayer::featureAdded, this, changed)
      << connect(layer, &QgsVectorLayer::featureDeleted, this, changed)
      << connect(layer, &QgsVectorLayer::updatedFields, this, changed)
      << connect(layer, &QgsVectorLayer::subsetStringChanged, this, changed)
      << connect(layer, &QgsVectorLayer::editingStopped, this, changed)
      << connect(layer, &QgsMapLayer::dataSourceChanged, this, changed)
      << connect(layer, &QgsMapLayer::dataChanged, this, changed)
      << connect(layer, &QgsMapLayer::crsChanged, this, changed)
      << connect(layer, &QObject::destroyed, this, changed);
  }
  if (m_catalog) preparation->watches << connect(m_catalog, &DataCatalog::changed, this, changed);
  if (m_layers) preparation->watches << connect(m_layers, &QgisLayerService::layerDeclared, this, changed);
  m_async->preparation = preparation;
  emit snapshotBusyChanged(true);
  QTimer::singleShot(0, this, [this, preparation] { continueSavePreparation(preparation); });
  return true;
}

void MapVersionController::continueSavePreparation(const std::shared_ptr<SavePreparation> &p) {
  if (!m_async || m_async->preparation != p) return;
  QString error;
  if (p->cancelled || p->epoch != m_snapshotEpoch || !m_store || !m_tasks ||
      p->projectDirectory != m_store->projectDirectory()) {
    p->cancelled = true;
  } else if (p->changed) {
    error = tr("校验期间图层或来源已改变，请重新保存");
  }
  // One provider iterator on its owner thread, never processEvents/reentrant
  // nested loops. A bounded batch yields even when all features lack evidence.
  QElapsedTimer budget; budget.start(); int visited = 0;
  while (!p->cancelled && error.isEmpty() && p->layerIndex < p->layers.size() && visited < 64 && budget.elapsed() < 4) {
    auto &captured = p->layers[p->layerIndex]; auto *layer = captured.vector.data();
    if (!layer) { error = tr("校验期间图层已卸载，请重新保存"); break; }
    if (!p->opened) {
      p->scan.layerUids.clear();
      // Legacy identity migration still uses QGIS native edits on the owner.
      // Ordinary drafts already have these fields from copyForEditing.
      if (captured.id.startsWith("draft.") && layer->fields().indexOf("facies_code") >= 0 && !layer->readOnly() &&
          (layer->fields().indexOf(MappingFeatureIdentity::UidField) < 0 ||
           layer->fields().indexOf(MappingFeatureIdentity::ParentsField) < 0 ||
           layer->fields().indexOf(MappingFeatureIdentity::RootsField) < 0 ||
           layer->fields().indexOf(MappingFeatureIdentity::ReviewField) < 0)) {
        if (!MappingFeatureIdentity::ensure(layer, &error)) break;
        // The explicit migration is ours; subsequent changes invalidate the scan.
        p->changed = false;
      }
      if (layer->fields().indexOf("facies_evidence") < 0 && layer->fields().indexOf("paleo_interpretation") < 0) {
        p->scan.featureCount += layer->featureCount(); ++p->layerIndex; ++visited; continue;
      }
      QgsFeatureRequest request; QgsAttributeList attributes;
      for (const auto &name : {MappingFeatureIdentity::UidField, MappingFeatureIdentity::ParentsField,
                              MappingFeatureIdentity::RootsField, QString("facies_evidence"), QString("paleo_interpretation")}) {
        const auto index = layer->fields().indexOf(name); if (index >= 0) attributes << index;
      }
      request.setSubsetOfAttributes(attributes);
      p->features = layer->getFeatures(request); p->opened = true;
    }
    QgsFeature feature;
    if (!p->features.nextFeature(feature)) { p->features.close(); p->opened = false; ++p->layerIndex; continue; }
    ++visited;
    if (!p->scan.append(feature, p->horizon, m_catalog, &error, captured.id.startsWith("draft."))) break;
  }
  if (!p->cancelled && error.isEmpty() && p->epoch == m_snapshotEpoch)
    emit snapshotQueryProgress(p->horizon, p->scan.featureCount, p->total);
  // A progress consumer may cancel, switch projects or change a layer directly.
  if (p->epoch != m_snapshotEpoch) p->cancelled = true;
  if (p->changed && error.isEmpty()) error = tr("校验期间图层或来源已改变，请重新保存");
  if (!p->cancelled && error.isEmpty() && p->layerIndex < p->layers.size()) {
    QTimer::singleShot(0, this, [this, p] { continueSavePreparation(p); }); return;
  }
  p->close();
  if (!p->cancelled && error.isEmpty()) {
    auto provenance = p->provenance; provenance.insert("feature_summary", p->scan.summary());
    provenance.insert("cited_sources", p->scan.citedSources(m_catalog));
    // No event loop boundary between the validated revision and native commit.
    if (launchSaveSnapshot(p->horizon, provenance, &error)) {
      m_async->preparation.reset(); return;
    }
    if (error.isEmpty()) error = tr("无法启动地图快照任务");
  }
  m_async->preparation.reset();
  emit snapshotBusyChanged(false);
  if (p->epoch == m_snapshotEpoch)
    emit snapshotFinished(p->horizon, "save", false, p->cancelled, error);
}

bool MapVersionController::launchSaveSnapshot(const QString &horizon, const QVariantMap &provenance, QString *error) {
  // A new computation may acquire a source between query batches. Never run
  // the legacy native-commit cleanup over another operation's lock.
  if (m_projectStore && m_async && m_async->preparation)
    for (const auto &entry : m_projectStore->busyLayers())
      for (const auto &captured : m_async->preparation->layers)
        if (captured.vector && (entry.layerId == captured.id || entry.layerId == captured.vector->id()) &&
            entry.taskId != QLatin1String("edit")) {
          if (error) *error = entry.reason; return false;
        }
  auto job = std::make_shared<SnapshotJob>(); job->epoch = m_snapshotEpoch;
  QVector<LayerDeclaration> declarations; QVariantMap frozen;
  if (!captureVersion(horizon, provenance, &declarations, &frozen, error, true)) return false;
  job->input = m_store->snapshotInputForSave(horizon,
      QString::fromUtf8(QJsonDocument::fromVariant(frozen).toJson(QJsonDocument::Compact)), declarations, error);
  if (!job->input.valid) return false;
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
    job->vectors << source;
  }
  job->stale = std::make_shared<std::atomic_bool>(false);
  if (!m_async) m_async = std::make_shared<AsyncState>(this);
  m_async->runner.setTaskService(m_tasks);
  // This operation owns its source locks through worker completion. A later
  // replacement lock (or a reopened project) is never released by this task.
  QPointer<PaleoProjectStore> tokenStore = m_projectStore;
  const QString tokenProject = tokenStore ? tokenStore->metaDbPath() : QString();
  QStringList tokenIds;
  for (const auto &decl : declarations) {
    if (decl.horizon != horizon || decl.type.compare("vector", Qt::CaseInsensitive)) continue;
    tokenIds << decl.layerId;
    if (m_layers) if (auto *layer = m_layers->layer(decl.layerId)) tokenIds << layer->id();
  }
  tokenIds.removeDuplicates();
  const auto tokenOwner = QUuid::createUuid().toString(QUuid::WithoutBraces);
  if (tokenStore) {
    for (const auto &id : tokenIds) if (tokenStore->layerBusy(id, error)) return false;
    for (const auto &id : tokenIds) tokenStore->markLayerBusy(id, tokenOwner, tr("地图快照正在读取来源"));
  }
  m_async->disconnectSourceWatches();
  const auto stale = job->stale;
  const auto markStale = [stale] { stale->store(true, std::memory_order_release); };
  if (m_layers)
    for (const auto &vector : job->vectors) {
      auto *layer = qobject_cast<QgsVectorLayer *>(m_layers->layer(vector.layerId));
      if (!layer) continue;
      m_async->sourceWatches
        << connect(layer, &QgsVectorLayer::geometryChanged, this, markStale)
        << connect(layer, &QgsVectorLayer::attributeValueChanged, this, markStale)
        << connect(layer, &QgsVectorLayer::featureAdded, this, markStale)
        << connect(layer, &QgsVectorLayer::featureDeleted, this, markStale)
        << connect(layer, &QgsVectorLayer::updatedFields, this, markStale)
        << connect(layer, &QgsVectorLayer::subsetStringChanged, this, markStale)
        << connect(layer, &QgsVectorLayer::editingStopped, this, markStale)
        << connect(layer, &QgsMapLayer::dataSourceChanged, this, markStale)
        << connect(layer, &QgsMapLayer::dataChanged, this, markStale)
        << connect(layer, &QgsMapLayer::crsChanged, this, markStale)
        << connect(layer, &QObject::destroyed, this, markStale);
    }
  m_async->releaseTokens = [tokenStore, tokenProject, tokenIds, tokenOwner] {
    if (tokenStore && tokenStore->metaDbPath() == tokenProject)
      for (const auto &id : tokenIds) tokenStore->releaseLayerBusy(id, tokenOwner);
  };
  paleo::jobs::JobRunner<SnapshotJob>::Callbacks callbacks;
  callbacks.compute = computeSnapshot;
  callbacks.commit = [this](SnapshotJob &j, QString *err) {
    if (j.stale && j.stale->load(std::memory_order_acquire)) {
      if (err) *err = tr("矢量来源在导出期间已变化，请重试"); return false;
    }
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
    QString retainError;
    if (!retainMapCitations(m_catalog, j.input.horizon, result.version, j.prepared.provenance, &retainError) &&
        j.error.isEmpty())
      j.error = retainError;
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
    m_async->disconnectSourceWatches();
    job->vectors.clear(); job->temporary.reset();
    emit snapshotBusyChanged(snapshotBusy());
    if (m_async->releaseTokens) { m_async->releaseTokens(); m_async->releaseTokens = {}; }
    if (job->epoch == m_snapshotEpoch)
      emit snapshotFinished(job->input.horizon, "save", job->prepared.committed, job->cancelled, job->error);
  });
  auto *task = m_async->runner.start(tr("%1 保存地图与证据快照").arg(horizon), job, callbacks);
  if (!task) {
    disconnect(*connection);
    m_async->disconnectSourceWatches();
    if (m_async->releaseTokens) { m_async->releaseTokens(); m_async->releaseTokens = {}; }
    return false;
  }
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
