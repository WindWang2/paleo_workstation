// 层：功能
#include "mapversioncontroller.h"
#include "workflowerrors_internal.h"
#include "mapversionvalidation_internal.h"
#include "mappingsamples.h"
#include "../domain/interpretationevidence.h"
#include "../qgis/mappingfeatureidentity.h"
#include "../qgis/mappingartifactwriter.h"
#include "../domain/predictionevidence.h"

#include "../io/timedeptool.h"
#include "../io/wellfileparsers.h"
#include "../catalog/datacatalog.h"
#include "../metadata/layermanifest.h"
#include "../metadata/paleoprojectstore.h"
#include "../qgis/qgislayerservice.h"
#include "../qgis/qgiseditingservice.h"
#include "../services/projectdata.h"

#include <QJsonArray>
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
    if (item.first.startsWith("draft.") && layer->fields().indexOf("facies_code") >= 0 && !layer->readOnly() &&
        !MappingFeatureIdentity::ensure(layer, error)) {
      return {};
    }
    if (layer->fields().indexOf("facies_evidence") < 0 &&
        layer->fields().indexOf("paleo_interpretation") < 0) {
      scan.featureCount += layer->featureCount(); continue;
    }
    scan.layerUids.clear();
    auto features = layer->getFeatures(); QgsFeature feature;
    while (features.nextFeature(feature)) {
      if (!scan.append(feature, horizon, m_catalog, error, item.first.startsWith("draft."))) return {};
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

  // 编辑会话 commit（§1223 版本提交边界）：该层位全部矢量图层逐一提交；
  // commitChanges 原生清 undo 栈，undoStack()->clear() 再兜底一次，保证
  // undo 不跨版本边界。栅格图层无编辑缓冲，跳过。
  bool commitFailed = false;
  QStringList committedWithEdits; // #480 P2-2：已成功落盘编辑的层（跨层原生
                                  // 提交非原子——失败时这些层无版本快照可回溯）
  QVector<QPointer<MapVersionEvidenceCommitProof>> proofs;
  // Bind every proof before the first native callback. Committing A must not
  // certify changes that its callback makes to the already-scanned layer B.
  if (featurePrevalidated)
    for (const auto &item : horizonVectorLayers)
      if (item.second && item.first.startsWith("draft."))
        proofs << new MapVersionEvidenceCommitProof(item.second, horizon, m_catalog);
  const auto expireProofs = qScopeGuard([&] {
    for (auto &proof : proofs) if (proof) { proof->releaseGuard(); delete proof.data(); }
  });
  for ( const auto &item : horizonVectorLayers )
  {
    auto *vl = item.second.data();
    if ( !vl )
      continue;
    bool layerCommitted = true;
    const bool hadEditBuffer = vl->isEditable(); // #480 P2-2：原生提交非原子，
                                                  // 记录已落盘层，失败时显式披露部分状态
    if ( vl->isEditable() )
    {
      if ( m_editSvc )
      {
        if ( !m_editSvc->commitEdit( vl, error ) )
          layerCommitted = false;
      }
      else if ( m_projectStore )
      {
        const auto res = m_projectStore->enqueueWrite( [vl]() -> PaleoProjectStore::WriteResult {
          if ( !vl->commitChanges() )
            return { false, QObject::tr( "commitChanges failed for layer '%1'" ).arg( vl->id() ) };
          return { true, QString() };
        } );
        if ( !res.ok )
        {
          if ( error && error->isEmpty() )
            *error = tr( "图层 %1 提交编辑失败：%2" ).arg( item.first, res.error );
          layerCommitted = false;
        }
      }
      else
      {
        if ( !vl->commitChanges() )
        {
          if ( error && error->isEmpty() )
            *error = tr( "图层 %1 提交编辑失败：%2" )
                         .arg( item.first, vl->commitErrors().join( QLatin1Char( ';' ) ) );
          layerCommitted = false;
        }
      }
    }
    if ( layerCommitted && hadEditBuffer )
      committedWithEdits << item.first;
    // #222：只有提交成功（或本无编辑缓冲）的层才清 undo 栈。提交失败时编辑
    // 缓冲保留供重试/回滚，撤销历史也必须一并保留；且约束会话仍挂着
    // QUndoStack::indexChanged，clear() 会触发 synchronize(0) 把失败态反向
    // 再 persist 进 store。
    if ( layerCommitted )
      vl->undoStack()->clear();
    else
      commitFailed = true;
  }

  if ( commitFailed )
  {
    // #480 P2-2：跨层原生提交非原子——先行层可能已落盘（GPKG 已写、undo
    // 已清）但本版本未生成。显式披露部分状态，不再只报失败层，避免用户
    // 误以为全部编辑仍在缓冲区可重试。
    if ( !committedWithEdits.isEmpty() && error )
      *error = tr( "部分层已提交编辑但本次未出版本快照：%1（提交失败：%2）" )
                   .arg( committedWithEdits.join( QLatin1Char( '、' ) ), *error );
    releaseCommittedEditLocks();
    return false;
  }
  releaseCommittedEditLocks();

  *declarations = declared; *frozen = frozenProvenance;
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

MapVersion MapVersionController::saveVersion( const QString &horizon, const QVariantMap &provenance,
                                              QString *error )
{
  QVector<LayerDeclaration> declared; QVariantMap frozenProvenance;
  if (!captureVersion(horizon, provenance, &declared, &frozenProvenance, error)) return {};
  const QString json = QString::fromUtf8(QJsonDocument::fromVariant(frozenProvenance).toJson(QJsonDocument::Compact));
  const auto clearAllTokens = [&] { releaseSnapshotTokens(horizon); };
  // Export committed vector views rather than copying a possibly open WAL-backed GPKG.
  // This also freezes provider subsets and memory layers, preserving business attributes.
  QTemporaryDir vectorStaging;
  if (!vectorStaging.isValid()) {
    setError(error, tr("无法创建地图版本矢量暂存目录")); clearAllTokens(); return {};
  }
  QVector<LayerDeclaration> snapshotDeclarations = declared;
  for (auto &decl : snapshotDeclarations) {
    if (decl.horizon != horizon || decl.type.compare("vector", Qt::CaseInsensitive) != 0) continue;
    auto *vector = qobject_cast<QgsVectorLayer *>(m_layers->layer(decl.layerId));
    if (!vector) continue;
    const QString path = vectorStaging.filePath(QString::number(&decl - snapshotDeclarations.data()) + ".gpkg");
    if (!MappingArtifactWriter::vectorSnapshot(vector, path, error)) { clearAllTokens(); return {}; }
    decl.source = path + "|layername=features";
  }

  // §41.2 sole write choke point：saveVersion 路由进 m_projectStore->enqueueWrite
  MapVersion v;
  if ( m_projectStore )
  {
    QString storeErr;
    const auto res = m_projectStore->enqueueWrite( [&]() -> PaleoProjectStore::WriteResult {
      if ( !m_store )
        return { false, QObject::tr( "MapVersionStore is null" ) };
      v = m_store->saveVersion( horizon, json, &storeErr, snapshotDeclarations );
      if ( v.version <= 0 )
        return { false, storeErr.isEmpty() ? QObject::tr( "saveVersion failed" ) : storeErr };
      return { true, QString() };
    } );
    if ( !res.ok )
    {
      if ( error && error->isEmpty() )
        *error = res.error;
      clearAllTokens();
      return MapVersion();
    }
  }
  else if ( m_store )
  {
    v = m_store->saveVersion( horizon, json, error, snapshotDeclarations );
  }

  clearAllTokens();

  if ( v.version > 0 ) {
    QString retainError;
    if (!retainMapCitations(m_catalog, horizon, v.version, frozenProvenance, &retainError) &&
        error && error->isEmpty())
      *error = retainError;
    emit versionSaved( horizon, v.version );
  }
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

    for ( const ProjectWell &well : projectData->wells() )
    {
      ++total;
      QJsonObject row;
      row.insert( QStringLiteral( "well_id" ), well.id );
      row.insert( QStringLiteral( "name" ), well.name );
      row.insert( QStringLiteral( "kind" ), QStringLiteral( "reason" ) );

      const QVector<WellTop> tops = projectData->topsFor( well.id );
      const WellTop *pick = nullptr;
      for ( const WellTop &top : tops )
        if ( top.horizon == horizon )
          pick = &top;

      const QVector<TdSample> td =
          pick ? projectData->tdTableFor( well.id ) : QVector<TdSample>();
      if ( !pick || ( qIsNaN( pick->tvd ) && qIsNaN( pick->md ) ) )
      {
        row.insert( QStringLiteral( "reason" ),
                    tr( "无 %1 分层" ).arg( horizon ) );
      }
      else if ( td.isEmpty() )
      {
        row.insert( QStringLiteral( "reason" ), tr( "无时深表" ) );
      }
      else
      {
        const TimeDepthTool::TdResult tdResult = MappingSamples::timeForTop( td, *pick );
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
