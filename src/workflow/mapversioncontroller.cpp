// 层：功能
#include "mapversioncontroller.h"
#include "workflowerrors_internal.h"
#include "mapversionvalidation_internal.h"
#include "mappingsamples.h"
#include "../domain/interpretationevidence.h"
#include "../qgis/mappingfeatureidentity.h"
#include "../qgis/mappingartifactwriter.h"
#include "../metadata/managedpath.h"
#include "../domain/shaequals.h"
#include "../domain/predictionevidence.h"

#include "../io/timedeptool.h"
#include "../io/wellfileparsers.h"
#include "../catalog/datacatalog.h"
#include "../metadata/layermanifest.h"
#include "../metadata/paleoprojectstore.h"
#include "../qgis/qgislayerservice.h"
#include "../qgis/qgiseditingservice.h"
#include "../services/projectdata.h"

#include <QDebug>
#include <QElapsedTimer>
#include <QFileInfo>
#include <QJsonArray>
#include <QUuid>
#include <QJsonDocument>
#include <QSet>
#include <QJsonObject>
#include <QTemporaryDir>
#include <QDir>
#include <QScopeGuard>

#include <gdal.h>

#include <cmath>
#include <limits>

#include <qgsmaplayer.h>
#include <qgsvectorlayer.h>

namespace
{
using paleo::workflow_detail::setError;
} // namespace

MapVersionController::MapVersionController( MapVersionStore *store, QgisLayerService *layers,
                                            QObject *parent )
  : QObject( parent )
  , m_store( store )
  , m_layers( layers )
{
}

void MapVersionController::setCatalog(DataCatalog *catalog) { cancelSnapshot(); ++m_snapshotEpoch; m_catalog = catalog; }

bool MapVersionController::captureVersion(const QString &horizon, const QVariantMap &provenance,
    QVector<LayerDeclaration> *declarations, QVariantMap *frozen, QString *error, bool featurePrevalidated) {

  QVariantMap frozenProvenance = provenance;

  // 本函数只提交编辑并释放自己结束的 edit 锁。其他任务的锁按所有者保留。
  QVector<QPair<QString, QPointer<QgsVectorLayer>>> horizonVectorLayers;
  QVector<LayerDeclaration> declared;
  bool declaredOk = false;
  QStringList droppedLayers; // #480：实例化失败的幽灵层保持容忍语义（孤立 edit
                             // 标记仍经 horizonVectorLayers 释放），但不再静默——
                             // 记入 frozen provenance 披露缺层，并留痕日志。
  if ( m_layers )
  {
    declaredOk = m_layers->tryDeclared( &declared, error );
    if ( declaredOk )
    {
      for ( const LayerDeclaration &d : declared )
      {
        if ( d.horizon == horizon && d.type.compare( QStringLiteral( "vector" ), Qt::CaseInsensitive ) == 0 )
        {
          QString instErr;
          QgsMapLayer *layer = m_layers->instantiate( d.layerId, &instErr );
          auto *vl = qobject_cast<QgsVectorLayer *>( layer );
          if ( !vl )
          {
            droppedLayers << d.layerId;
            qWarning( "MapVersionController: declared layer '%s' failed to instantiate,"
                      " the version snapshot will not contain it: %s",
                      qPrintable( d.layerId ), qPrintable( instErr ) );
          }
          horizonVectorLayers.append( { d.layerId, vl } );
        }
      }
    }
  }

  auto releaseCommittedEditLocks = [&]() {
    if (!m_projectStore) return;
    for (const auto &item : horizonVectorLayers) {
      auto *layer = item.second.data();
      if (layer && layer->isEditable()) continue;
      m_projectStore->releaseLayerBusy(item.first, QStringLiteral("edit"));
      if (layer) m_projectStore->releaseLayerBusy(layer->id(), QStringLiteral("edit"));
    }
  };
  if (m_projectStore) {
    for (const auto &entry : m_projectStore->busyLayers()) {
      for (const auto &item : horizonVectorLayers) {
        auto *layer = item.second.data();
        const bool same = entry.layerId == item.first || (layer && entry.layerId == layer->id());
        if (!same) continue;
        // "edit" is the live session, or an orphan with no editable layer.
        // A foreign computation still blocks. Orphans are released only after
        // this attempt, and only when the layer is still not editable.
        if (entry.taskId == QLatin1String("edit")) continue;
        setError(error, entry.reason);
        return false;
      }
    }
  }

  if ( m_layers && !declaredOk )
    return false; // 清单读失败时不得在未提交编辑的情况下出版本

  // Validate persisted business JSON before committing the native edit buffer.
  MapVersionEvidenceScan scan;
  if (!featurePrevalidated) for (const auto &item : horizonVectorLayers) {
    auto *layer = item.second.data();
    if (!layer) continue;
    const bool requireIdentity = item.first.startsWith(QStringLiteral("draft.")) &&
        layer->fields().indexOf(MappingFeatureIdentity::UidField) >= 0;
    if (layer->fields().indexOf("facies_evidence") < 0 &&
        layer->fields().indexOf("paleo_interpretation") < 0) {
      scan.featureCount += layer->featureCount(); continue;
    }
    scan.layerUids.clear();
    auto features = layer->getFeatures(); QgsFeature feature;
    while (features.nextFeature(feature)) {
      if (!scan.append(feature, horizon, m_catalog, error, requireIdentity)) return {};
    }
  }
  if (!featurePrevalidated) frozenProvenance.insert("feature_summary", scan.summary());

  // #438：成果目录 Blocking / 损坏证据校验必须在 commitChanges / undo 清空之前。
  // 失败时编辑缓冲与撤销栈保持原样，map_versions 也不写新行。
  QVariantList versionSnapshots;
  if (m_catalog) {
    QHash<QString, CatalogVersion> byLayer;
    for (const auto &version : m_catalog->versions()) {
      const QString layer = version.extra.value("layer_id").toString();
      const QString draft = version.extra.value("draft_id").toString();
      if (!layer.isEmpty()) byLayer.insert(layer, version);
      if (!draft.isEmpty()) byLayer.insert(draft, version);
    }
    for (const auto &decl : declared) {
      if (decl.horizon != horizon) continue;
      CatalogVersion version = byLayer.value(decl.layerId);
      if (auto *layer = m_layers->layer(decl.layerId)) {
        const auto id = layer->customProperty("paleoVersionId").toString();
        if (!id.isEmpty()) version = m_catalog->versionById(id);
      }
      if (version.id.isEmpty()) continue;
      const QVariant evidence = version.extra.value("evidence");
      if (evidence.isValid() && evidence.typeId() != QMetaType::QVariantMap) {
        setError(error, tr("成果版本的证据字段损坏：%1").arg(version.id));
        return {};
      }
      const auto map = evidence.toMap();
      const auto findings = paleo::evidence::validate(map) + map.value("quality_issues").toList();
      for (const auto &entry : findings)
        if (entry.toMap().value("severity").toString() == "Blocking") {
          setError(error, tr("成果证据存在阻断性来源错误：%1").arg(version.id));
          return {};
        }
      versionSnapshots << QVariantMap{{"layer_id", decl.layerId}, {"version_id", version.id},
                                     {"asset_id", version.assetId}, {"sha256", version.sha256},
                                     {"evidence", map}, {"stale", version.extra.value("stale")}};
    }
  }
  frozenProvenance.insert("evidence_snapshot", versionSnapshots);
  frozenProvenance.insert("catalog_bound", !versionSnapshots.isEmpty());
  if (!featurePrevalidated)
    frozenProvenance.insert("cited_sources", scan.citedSources(m_catalog));
  if (!droppedLayers.isEmpty())
    frozenProvenance.insert("dropped_layers", droppedLayers); // #480：缺层披露
  if (m_catalog) {
    for (const auto &entry : frozenProvenance.value(QStringLiteral("cited_sources")).toList()) {
      const auto id = entry.toMap().value(QStringLiteral("version_id")).toString();
      if (id.isEmpty()) continue;
      if (m_catalog->versionById(id).id.isEmpty()) {
        setError(error, tr("图斑证据引用的来源版本缺失：%1").arg(id));
        return {};
      }
    }
  }

  if (frozenProvenance.value(QStringLiteral("save_attempt_id")).toString().isEmpty())
    frozenProvenance.insert(QStringLiteral("save_attempt_id"),
                            QUuid::createUuid().toString(QUuid::WithoutBraces));
  auto capture = std::make_shared<MapVersionSaveCapture>();
  capture->horizon = horizon;
  capture->projectDirectory = m_store ? m_store->projectDirectory() : QString();
  capture->epoch = m_snapshotEpoch;
  capture->attemptId = frozenProvenance.value(QStringLiteral("save_attempt_id")).toString();
  for (const auto &item : horizonVectorLayers) {
    MapVersionSaveCapture::Layer captured;
    captured.id = item.first;
    captured.layer = item.second;
    if (item.second) {
      captured.hadEditBuffer = item.second->isEditable();
      captured.undoCount = item.second->undoStack() ? item.second->undoStack()->count() : 0;
    }
    capture->layers << captured;
    if (captured.hadEditBuffer) capture->pendingLayers << captured.id;
  }
  m_saveCapture = capture;
  *declarations = declared; *frozen = frozenProvenance;
  return true;
}
void MapVersionController::noteOutcome(SaveOutcome::Kind kind, int version, const QString &horizon,
                                       const QString &message, const QString &stage) {
  m_lastOutcome = {};
  m_lastOutcome.kind = kind;
  m_lastOutcome.version = version;
  m_lastOutcome.horizon = horizon;
  m_lastOutcome.message = message;
  m_lastOutcome.stage = stage;
  if (m_saveCapture) {
    m_lastOutcome.committedLayers = m_saveCapture->committedLayers;
    m_lastOutcome.pendingLayers = m_saveCapture->pendingLayers;
  }
}
bool MapVersionController::resumePendingWorkCommit(const QString &horizon, MapVersion *out, QString *error) {
  if (!m_saveCapture || m_saveCapture->version <= 0 || m_saveCapture->horizon != horizon) return false;
  if (!m_store || m_saveCapture->epoch != m_snapshotEpoch ||
      m_saveCapture->projectDirectory != m_store->projectDirectory()) {
    m_saveCapture.reset();
    return false;
  }
  for (const auto &layer : m_saveCapture->layers) {
    if (!layer.hadEditBuffer) continue;
    auto *vector = layer.layer.data();
    if (!vector) { m_saveCapture.reset(); return false; }
    if (!vector->isEditable()) continue;
    const int undoCount = vector->undoStack() ? vector->undoStack()->count() : 0;
    if (undoCount != layer.undoCount) { m_saveCapture.reset(); return false; }
  }
  QString commitError;
  const bool committed = commitCapturedEdits(&commitError);
  if (!committed) {
    if (out) *out = m_store->latest(horizon);
    const auto message = tr("版本已保存（v%1），后续工作副本提交未完成：%2")
                             .arg(m_saveCapture->version).arg(commitError);
    setError(error, message);
    noteOutcome(SaveOutcome::Kind::SavedFollowUpPending, m_saveCapture->version, horizon, message,
                QStringLiteral("work_commit"));
    return true;
  }
  const auto saved = m_store->latest(horizon);
  if (out) *out = saved;
  QVector<LayerDeclaration> declaredLayers;
  if (m_layers) m_layers->tryDeclared(&declaredLayers, nullptr);
  rememberImmutableAssets(saved, declaredLayers);
  if (error) error->clear();
  noteOutcome(SaveOutcome::Kind::Saved, out ? out->version : m_saveCapture->version, horizon, {},
              QStringLiteral("complete"));
  m_saveCapture.reset();
  return true;
}
bool MapVersionController::commitCapturedEdits(QString *error) {
  if (!m_saveCapture) return false;
  if (m_saveFault == SaveFault::WorkCommit) {
    setError(error, tr("工作副本提交未完成"));
    return false;
  }
  bool commitFailed = false;
  QStringList committedWithEdits;
  QVector<QPointer<MapVersionEvidenceCommitProof>> proofs;
  for (const auto &item : m_saveCapture->layers)
    if (item.layer && item.id.startsWith(QStringLiteral("draft.")))
      proofs << new MapVersionEvidenceCommitProof(item.layer, m_saveCapture->horizon, m_catalog);
  const auto expireProofs = qScopeGuard([&] {
    for (auto &proof : proofs) if (proof) { proof->releaseGuard(); delete proof.data(); }
  });
  m_saveCapture->committedLayers.clear();
  m_saveCapture->pendingLayers.clear();
  for (const auto &item : m_saveCapture->layers) {
    auto *vl = item.layer.data();
    if (!vl) continue;
    if (!vl->isEditable()) {
      if (item.hadEditBuffer) m_saveCapture->committedLayers << item.id;
      continue;
    }
    bool layerCommitted = true;
    if (m_editSvc) {
      if (!m_editSvc->commitEdit(vl, error)) layerCommitted = false;
    } else if (m_projectStore) {
      const auto res = m_projectStore->enqueueWrite([vl]() -> PaleoProjectStore::WriteResult {
        if (!vl->commitChanges())
          return {false, QObject::tr("commitChanges failed for layer '%1'").arg(vl->id())};
        return {true, QString()};
      });
      if (!res.ok) {
        if (error && error->isEmpty())
          *error = tr("图层 %1 提交编辑失败：%2").arg(item.id, res.error);
        layerCommitted = false;
      }
    } else if (!vl->commitChanges()) {
      if (error && error->isEmpty())
        *error = tr("图层 %1 提交编辑失败：%2").arg(item.id, vl->commitErrors().join(QLatin1Char(';')));
      layerCommitted = false;
    }
    if (layerCommitted) {
      if (vl->undoStack()) vl->undoStack()->clear();
      m_saveCapture->committedLayers << item.id;
      if (item.hadEditBuffer) committedWithEdits << item.id;
    } else {
      commitFailed = true;
      m_saveCapture->pendingLayers << item.id;
    }
  }
  auto releaseCommittedEditLocks = [&]() {
    if (!m_projectStore) return;
    for (const auto &item : m_saveCapture->layers) {
      auto *layer = item.layer.data();
      if (layer && layer->isEditable()) continue;
      m_projectStore->releaseLayerBusy(item.id, QStringLiteral("edit"));
      if (layer) m_projectStore->releaseLayerBusy(layer->id(), QStringLiteral("edit"));
    }
  };
  if (commitFailed) {
    if (error)
      *error = tr("版本已保存，部分工作层已提交、其余编辑仍可撤销：已提交 %1；未提交 %2。%3")
                   .arg(committedWithEdits.join(QChar(0x3001)),
                        m_saveCapture->pendingLayers.join(QChar(0x3001)), *error);
    releaseCommittedEditLocks();
    return false;
  }
  releaseCommittedEditLocks();
  return true;
}
void MapVersionController::releaseSnapshotTokens(const QString &horizon) {
  if (!m_projectStore || !m_layers) return;
  for (const auto &decl : m_layers->declared()) {
    if (decl.horizon != horizon || decl.type != "vector") continue;
    auto *layer = qobject_cast<QgsVectorLayer *>(m_layers->layer(decl.layerId));
    if (layer && layer->isEditable()) continue;
    m_projectStore->releaseLayerBusy(decl.layerId, QStringLiteral("edit"));
    if (layer) m_projectStore->releaseLayerBusy(layer->id(), QStringLiteral("edit"));
  }
}

void MapVersionController::setSaveFaultForTest(SaveFault fault) {
  m_saveFault = fault;
  if (!m_store) return;
  if (fault == SaveFault::Verify || fault == SaveFault::Sql) {
    const auto stage = fault == SaveFault::Verify ? QStringLiteral("verify") : QStringLiteral("sql");
    m_store->setSaveFaultProbe([stage](const QString &name) { return name != stage; });
  } else {
    m_store->setSaveFaultProbe({});
  }
}
bool MapVersionController::tryReuseImmutable(const QString &layerId, LayerDeclaration *declaration) {
  if (!m_layers || !m_store || !declaration) return false;
  auto *layer = qobject_cast<QgsVectorLayer *>(m_layers->layer(layerId));
  if (!layer || layer->isEditable()) return false;
  const QString source = QFileInfo(layer->source().section(QLatin1Char('|'), 0, 0)).canonicalFilePath();
  const auto reuse = m_reuseBySource.constFind(source);
  if (reuse == m_reuseBySource.cend() || reuse->projectDirectory != m_store->projectDirectory()) return false;
  QString hashError;
  const QString sourceHash = paleo::managedpath::sha256FileHex(source, &hashError);
  ++m_lastMetrics.sourceHashes;
  if (!paleo::domain::shaEquals(sourceHash, reuse->sourceSha256)) return false;
  const QString snapshot = QFileInfo(reuse->snapshotPath).canonicalFilePath();
  if (snapshot.isEmpty() || snapshot == source) return false;
  const QString snapshotHash = paleo::managedpath::sha256FileHex(snapshot, &hashError);
  ++m_lastMetrics.destinationHashes;
  if (!paleo::domain::shaEquals(snapshotHash, reuse->snapshotSha256)) return false;
  declaration->source = snapshot + QStringLiteral("|layername=features");
  ++m_lastMetrics.reusedImmutableAssets;
  return true;
}
void MapVersionController::rememberImmutableAssets(const MapVersion &version,
                                                  const QVector<LayerDeclaration> &declared) {
  if (!m_store || !m_layers || version.version <= 0) return;
  const auto provenance = QJsonDocument::fromJson(version.provenance.toUtf8()).object();
  QHash<QString, QJsonObject> saved;
  for (const auto &entry : provenance.value(QStringLiteral("saved_layers")).toArray()) {
    const auto object = entry.toObject();
    saved.insert(object.value(QStringLiteral("layer_id")).toString(), object);
  }
  for (const auto &decl : declared) {
    if (decl.horizon != version.horizon || !saved.contains(decl.layerId)) continue;
    auto *layer = qobject_cast<QgsVectorLayer *>(m_layers->layer(decl.layerId));
    if (!layer || layer->isEditable()) continue;
    const QString source = QFileInfo(layer->source().section(QLatin1Char('|'), 0, 0)).canonicalFilePath();
    const auto snapshot = saved.value(decl.layerId);
    const QString snapshotPath = QFileInfo(m_store->projectDirectory(), snapshot.value(QStringLiteral("path")).toString())
                                     .absoluteFilePath();
    if (source.isEmpty() || QFileInfo(snapshotPath).canonicalFilePath() == source) continue;
    QString hashError;
    ImmutableAssetReuse reuse;
    reuse.projectDirectory = m_store->projectDirectory();
    reuse.sourceSha256 = paleo::managedpath::sha256FileHex(source, &hashError);
    reuse.snapshotPath = snapshotPath;
    reuse.snapshotSha256 = snapshot.value(QStringLiteral("sha256")).toString();
    if (reuse.sourceSha256.size() == 64 && reuse.snapshotSha256.size() == 64)
      m_reuseBySource.insert(source, reuse);
  }
}
MapVersion MapVersionController::saveVersion( const QString &horizon, const QVariantMap &provenance,
                                              QString *error )
{
  m_lastMetrics = {};
  MapVersion resumed;
  if (resumePendingWorkCommit(horizon, &resumed, error)) return resumed;
  QVector<LayerDeclaration> declared; QVariantMap frozenProvenance;
  if (!captureVersion(horizon, provenance, &declared, &frozenProvenance, error)) {
    noteOutcome(SaveOutcome::Kind::Rejected, 0, horizon, error ? *error : QString(), QStringLiteral("validate"));
    return {};
  }
  if (m_saveFault == SaveFault::Export) {
    setError(error, tr("地图矢量导出失败"));
    noteOutcome(SaveOutcome::Kind::Rejected, 0, horizon, error ? *error : QString(), QStringLiteral("export"));
    return {};
  }
  const QString json = QString::fromUtf8(QJsonDocument::fromVariant(frozenProvenance).toJson(QJsonDocument::Compact));
  const auto clearAllTokens = [&] { releaseSnapshotTokens(horizon); };
  QTemporaryDir vectorStaging;
  if (!vectorStaging.isValid()) {
    setError(error, tr("无法创建地图版本矢量暂存目录")); clearAllTokens();
    noteOutcome(SaveOutcome::Kind::Rejected, 0, horizon, error ? *error : QString(), QStringLiteral("export"));
    return {};
  }
  QElapsedTimer exportTimer; exportTimer.start();
  QVector<LayerDeclaration> snapshotDeclarations = declared;
  for (auto &decl : snapshotDeclarations) {
    if (decl.horizon != horizon || decl.type.compare("vector", Qt::CaseInsensitive) != 0) continue;
    if (!m_layers) continue;
    if (tryReuseImmutable(decl.layerId, &decl)) continue;
    auto *vector = qobject_cast<QgsVectorLayer *>(m_layers->layer(decl.layerId));
    if (!vector) continue;
    const QString path = vectorStaging.filePath(QString::number(&decl - snapshotDeclarations.data()) + ".gpkg");
    if (!MappingArtifactWriter::vectorSnapshot(vector, path, error)) {
      clearAllTokens();
      noteOutcome(SaveOutcome::Kind::Rejected, 0, horizon, error ? *error : QString(), QStringLiteral("export"));
      return {};
    }
    decl.source = path + "|layername=features";
    ++m_lastMetrics.providerExports;
    ++m_lastMetrics.identityNormalizations;
  }
  m_lastMetrics.providerExportMs = exportTimer.elapsed();
  MapSnapshotAccounting accounting;
  QElapsedTimer sqlTimer; sqlTimer.start();
  MapVersion v;
  if ( m_projectStore )
  {
    QString storeErr;
    const auto res = m_projectStore->enqueueWrite( [&]() -> PaleoProjectStore::WriteResult {
      if ( !m_store )
        return { false, QObject::tr( "MapVersionStore is null" ) };
      v = m_store->saveVersion( horizon, json, &storeErr, snapshotDeclarations, &accounting );
      if ( v.version <= 0 )
        return { false, storeErr.isEmpty() ? QObject::tr( "saveVersion failed" ) : storeErr };
      return { true, QString() };
    } );
    if ( !res.ok )
    {
      if ( error && error->isEmpty() )
        *error = res.error;
      clearAllTokens();
      noteOutcome(SaveOutcome::Kind::Rejected, 0, horizon, error ? *error : QString(), QStringLiteral("durable"));
      return MapVersion();
    }
  }
  else if ( m_store )
  {
    v = m_store->saveVersion( horizon, json, error, snapshotDeclarations, &accounting );
  }
  m_lastMetrics.sqlRegisterMs = sqlTimer.elapsed();
  m_lastMetrics.sourceHashes += accounting.sourceHashes;
  m_lastMetrics.destinationHashes += accounting.destinationHashes;
  m_lastMetrics.bytesCopied += accounting.bytesCopied;
  m_lastMetrics.files += accounting.files;
  if (v.version <= 0) {
    clearAllTokens();
    noteOutcome(SaveOutcome::Kind::Rejected, 0, horizon, error ? *error : QString(), QStringLiteral("durable"));
    return {};
  }
  if (m_saveCapture) m_saveCapture->version = v.version;
  QString commitError;
  if (!commitCapturedEdits(&commitError)) {
    const auto message = tr("版本已保存（v%1），后续工作副本提交未完成：%2").arg(v.version).arg(commitError);
    setError(error, message);
    noteOutcome(SaveOutcome::Kind::SavedFollowUpPending, v.version, horizon, message, QStringLiteral("work_commit"));
    emit versionSaved(horizon, v.version);
    return v;
  }
  clearAllTokens();
  rememberImmutableAssets(v, declared);
  QString retainError;
  if (!retainMapCitations(m_catalog, horizon, v.version, frozenProvenance, &retainError)) {
    qWarning("MapVersionController: retainMapCitations failed after saveVersion(%s v%d): %s",
             qPrintable(horizon), v.version, qPrintable(retainError));
    const auto message = tr("版本已保存（v%1），引用保留未完成：%2").arg(v.version).arg(retainError);
    noteOutcome(SaveOutcome::Kind::SavedFollowUpPending, v.version, horizon, message, QStringLiteral("retain"));
    setError(error, message);
    emit versionSaved(horizon, v.version);
    return v;
  }
  if (error) error->clear();
  noteOutcome(SaveOutcome::Kind::Saved, v.version, horizon, {}, QStringLiteral("complete"));
  if (m_saveCapture && m_saveCapture->pendingLayers.isEmpty()) m_saveCapture.reset();
  emit versionSaved( horizon, v.version );
  return v;
}

QString MapVersionController::publish( const QString &horizon, const QString &residualSummary,
                                       QString *error )
{
  if ( !m_store )
  {
    if ( error )
      *error = tr( "未绑定版本存储" );
    return QString();
  }
  // #394：只读双保险——store 层同位拒绝，这里先行拦截省一次清单读。
  if ( m_store->isReadOnly() )
  {
    if ( error )
      *error = tr( "工程目录被另一个实例锁定——本实例只读，发布被拒绝" );
    return QString();
  }
  // Revalidate the saved evidence manifest; current warnings never rewrite its facts.
  const auto saved = m_store->latest(horizon);
  const auto provenance = QJsonDocument::fromJson(saved.provenance.toUtf8()).object();
  if (provenance.value("catalog_bound").toBool() && (!m_catalog || !m_catalog->isOpen())) {
    setError(error, tr("保存版本引用的资料目录不可用，请重新打开工程后发布")); return {};
  }
  if (provenance.contains("cited_sources") && !provenance.value("cited_sources").isArray()) {
    setError(error, tr("保存版本的人工来源清单格式损坏，发布被拒绝")); return {};
  }
  for (const auto &entry : provenance.value("cited_sources").toArray()) {
    const auto id = entry.toObject().value("version_id").toString();
    if (id.isEmpty()) continue;
    if (m_catalog && m_catalog->versionById(id).id.isEmpty()) {
      setError(error, tr("保存证据的来源版本已缺失：%1").arg(id)); return {};
    }
  }
  for (const auto &entry : provenance.value("evidence_snapshot").toArray()) {
    const auto row = entry.toObject();
    const auto evidence = row.value("evidence").toObject().toVariantMap();
    for (const auto &finding : paleo::evidence::validate(evidence))
      if (finding.toMap().value("severity").toString() == "Blocking") {
        setError(error, finding.toMap().value("message").toString()); return {};
      }
    if (m_catalog) {
      if (m_catalog->versionById(row.value("version_id").toString()).id.isEmpty()) {
        setError(error, tr("保存成果的目录版本已缺失：%1").arg(row.value("version_id").toString())); return {};
      }
      for (const auto &input : evidence.value("inputs").toList()) {
        const auto id = input.toMap().value("version_id").toString();
        if (id.isEmpty() || m_catalog->versionById(id).id.isEmpty()) {
          setError(error, tr("保存证据的来源版本已缺失：%1").arg(id)); return {};
        }
      }
    }
  }
  QVector<LayerDeclaration> declared;
  if ( m_layers && !m_layers->tryDeclared( &declared, error ) )
    return QString(); // 清单读失败时不得发布空快照
  const QString dir = m_store->publish( horizon, declared, residualSummary, error );
  if ( !dir.isEmpty() )
    emit published( horizon, m_store->latest( horizon ).version, dir );
  return dir;
}

QString MapVersionController::residualSummaryJson( const ProjectDataFacade *projectData,
                                                   const QString &horizon )
{
  const ProjectDataFacade *pd = projectData;
  QJsonObject summary;
  summary.insert( QStringLiteral( "horizon" ), horizon );
  QJsonArray rows;
  QJsonArray missing;
  int total = 0, covered = 0;

  if ( projectData )
  {
    // 结构面栅格：走到采样步的井才需要它；无栅格的井记入 missing（缺的是
    // 残差本身，不是原因——发布门按「每口井都有残差或原因」卡住）。
    const HorizonRasterInfo raster = projectData->horizonRasterDecl( horizon );
    GDALAllRegister();
    GDALDatasetH ds = raster.valid
                          ? GDALOpen( raster.path.toUtf8().constData(), GA_ReadOnly )
                          : nullptr;
    const MappingSamples::ResidualInputs inputs(*projectData,horizon);

    for ( const ProjectWell &well : projectData->wells() )
    {
      ++total;
      QJsonObject row;
      row.insert( QStringLiteral( "well_id" ), well.id );
      row.insert( QStringLiteral( "name" ), well.name );
      row.insert( QStringLiteral( "kind" ), QStringLiteral( "reason" ) );

      QString topsError;
      const QVector<WellTop> tops=inputs.tops(well.id,topsError);
      const WellTop *pick = nullptr;
      for ( const WellTop &top : tops )
        if ( top.horizon == horizon )
          pick = &top;

      QString tdError;bool mdOnly=false;
      const QVector<TdSample> td=pick?inputs.timeDepth(well.id,mdOnly,tdError):QVector<TdSample>{};
      if(!topsError.isEmpty()) {
        row.insert(QStringLiteral("reason"),topsError);
        missing.append(well.name);rows.append(row);continue;
      }
      if ( !pick || ( qIsNaN( pick->tvd ) && qIsNaN( pick->md ) ) )
      {
        row.insert( QStringLiteral( "reason" ),
                    tr( "无 %1 分层" ).arg( horizon ) );
      }
      else if ( td.isEmpty() )
      {
        row.insert(QStringLiteral("reason"),tdError.isEmpty()?tr("无时深表"):tdError);
      }
      else
      {
        const TimeDepthTool::TdResult tdResult=MappingSamples::timeForTop(td,*pick,mdOnly);
        if ( !tdResult.ok() )
        {
          row.insert( QStringLiteral( "reason" ),
                      TimeDepthTool::reasonText( tdResult.status ) );
        }
        else if ( !ds )
        {
          // 层位还没有时间栅格 — 残差无从评起，这口井不算覆盖。
          missing.append( well.name );
          row.insert( QStringLiteral( "reason" ),
                      tr( "层位 %1 还没有时间栅格" ).arg( horizon ) );
          rows.append( row );
          continue;
        }
        else
        {
          // T25：与验证表同一点——分层 X/Y 优先，井口兜底（不是永远井口）。
          double sx = well.surfaceX, sy = well.surfaceY;
          MappingSamples::pickSamplePoint( well, pick, &sx, &sy );
          double rasterMs = qQNaN();
          const MappingSamples::SampleOutcome cell = MappingSamples::sampleRasterAt( ds, sx, sy, &rasterMs );
          switch ( cell )
          {
            case MappingSamples::SampleOutcome::Outside:
              row.insert( QStringLiteral( "reason" ), tr( "井位不在测网内" ) );
              break;
            case MappingSamples::SampleOutcome::Nodata:
              row.insert( QStringLiteral( "reason" ), tr( "井位落在空道" ) );
              break;
            case MappingSamples::SampleOutcome::Ok:
              row.insert( QStringLiteral( "kind" ), QStringLiteral( "residual" ) );
              row.insert( QStringLiteral( "residual_ms" ), tdResult.timeMs - rasterMs );
              break;
          }
        }
      }
      ++covered;
      rows.append( row );
    }
    if ( ds )
      GDALClose( ds );
  }

  summary.insert( QStringLiteral( "wells_total" ), total );
  summary.insert( QStringLiteral( "covered" ), covered );
  summary.insert( QStringLiteral( "missing" ), missing );
  summary.insert( QStringLiteral( "rows" ), rows );
  return QString::fromUtf8( QJsonDocument( summary ).toJson( QJsonDocument::Compact ) );
}

// B 包 staleness-lite：发布门 advisory 的数据口径——遍历资产的全部版本，
// 只数 stage==DERIVED 且 extra["stale"] 为真的（RAW/INTERMEDIATE 不计；
// OUTPUT 是发布产物本身，不在「下游待重算」语义里）。只读，绝不改 catalog。
int MapVersionController::staleDerivedCount( const DataCatalog *catalog )
{
  if ( !catalog || !catalog->isOpen() )
    return 0;
  int stale = 0;
  for ( const CatalogAsset &a : catalog->assets() )
    for ( const CatalogVersion &v : catalog->versionsForAsset( a.id ) )
      if ( v.stage == QLatin1String( "DERIVED" ) &&
           v.extra.value( QStringLiteral( "stale" ) ).toBool() )
        ++stale;
  return stale;
}

QString MapVersionController::stalePublishAdvisory( const DataCatalog *catalog )
{
  const int stale = staleDerivedCount( catalog );
  if ( stale <= 0 )
    return QString();
  // advisory 文案（规格定稿）：如实计数 + 明确不阻断——可见但不拦发布。
  return QObject::tr( "存在过时下游产物（%1 个）——不阻断本次发布，请确认后继续" )
      .arg( stale );
}
