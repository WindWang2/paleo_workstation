// 层：功能
// 编辑副本与资产生命周期：copyForEditing/saveEditingVersion/removalProducts/removeProduct/snapshotConstraints/importConstraints/importBoundaryLayer。
// 从 mappingworkbench.cpp 析出——行为守恒平移（goal/core-deepening A-R2）：
// 函数体逐字未动，仅按子域归位到独立 TU。
#include "mappingworkbench.h"
#include "workflows_internal.h"
#include "../algorithms/faciesmapping/formationdominance.h"
#include "../catalog/purgelease.h"
#include "../domain/faciescatalog.h"
#include "../domain/mappinghorizons.h"
#include "../domain/singlefactorrequest.h"
#include "../io/constraintstore.h"
#include "../io/lasparser.h"
#include "../qgis/facieshierarchyrenderer.h"
#include "../qgis/factorstylewriter.h"
#include "../qgis/layervocabulary.h"
#include "../qgis/mappingartifactwriter.h"
#include "../qgis/qgislayerservice.h"
#include "../qgis/qgisprocessingservice.h"
#include "../qgis/qgisprojectservice.h"
#include "../qgis/qgisstyleservice.h"
#include "../qgis/wellattributestore.h"
#include "../services/futuretask.h"
#include "../services/jobrunner.h"
#include "../services/paleotaskservice.h"
#include "assetops.h"
#include "constraintimport.h"
#include "derivedassets.h"
#include "evidencerecord.h"
#include "../domain/interpretationevidence.h"
#include "../qgis/mappingfeatureidentity.h"
#include "services/projectdata.h"
#include "services/seismichorizoncluster.h"
#include "wellpredictionservice.h"
#include "workflows.h"
#include <QDateTime>
#include <QDir>
#include <QFile>
#include <QFileInfo>
#include <QFutureWatcher>
#include <QJsonArray>
#include <QJsonDocument>
#include <QRegularExpression>
#include <QSaveFile>
#include <QSet>
#include <QSignalBlocker>
#include <qgsvectorfilewriter.h>
#include <QTemporaryDir>
#include <QUuid>
#include <QtConcurrent>
#include <algorithm>
#include <cmath>
#include <gdal.h>
#include <limits>
#include <qgscoordinatereferencesystem.h>
#include <qgseditorwidgetsetup.h>
#include <qgsgeometry.h>
#include <qgsmaplayer.h>
#include <qgsproject.h>
#include <qgsrasterlayer.h>
#include <qgsunittypes.h>
#include <qgsvectorlayer.h>

namespace {
void fail(QString *error, const QString &text) {
  if (error)
    *error = text;
}
QString uid() { return QUuid::createUuid().toString(QUuid::WithoutBraces); }
QString filePath(const QString &source) { return source.section('|', 0, 0); }
// 无大地基准的工程/局部直角米制 CRS（LOCAL_CS / ENGCRS，均映射到
// Qgis::CrsType::Engineering）与本工程局部网格等价；带基准的投影或
// 地理 CRS 仍拒绝。约束线与测区边界导入共用同一坐标门。
bool isLocalGridCrs(const QgsCoordinateReferenceSystem &c) {
  return c.isValid() && !c.isGeographic() &&
         c.mapUnits() == Qgis::DistanceUnit::Meters &&
         c.type() == Qgis::CrsType::Engineering;
}
bool acceptsLocalGrid(const QgsCoordinateReferenceSystem &c) {
  const auto local = QgsCoordinateReferenceSystem::fromWkt(
      DataCatalog::localGridCrsWkt());
  return c == local || isLocalGridCrs(c);
}
QVariantList wellAttributeUpdates(const QVariantList &points, const QVariantList &schema,
                                  const QString &horizon, bool predicted, const QString &method = {}, const QString &jobId = {}) {
  QVariantList updates;
  for (const auto &v : points) {
    const auto point = v.toMap();
    if (point.value("depth_mock").toBool()) continue; // 合成深度只供旧结果展示，不能维护真实井段。
    const auto intervals = QJsonDocument::fromJson(point.value("facies_intervals").toString().toUtf8()).toVariant().toList();
    for (const auto &value : intervals) {
      const auto interval = value.toMap();
      const auto code = interval.value("code");
      const auto name = FaciesCatalog::find(schema, code).value("name").toString();
      const auto hierarchy = FaciesCatalog::attributes(schema, code);
      QVariantMap row{{"well_id", point.value("id")}, {"horizon", horizon},
        {"top_md", interval.value("top")}, {"base_md", interval.value("bottom")},
        {"facies_code", code}, {"facies", hierarchy.value("facies_name")},
        {"sub_facies", hierarchy.value("subfacies")},
        {"micro_facies", hierarchy.value("microfacies")}, {"facies_pattern", QStringLiteral("")}};
      if (predicted) { row.insert("predicted_facies", name); row.insert("model", point.value("prediction_model", method)); row.insert("job_id", point.value("prediction_job_id", jobId)); }
      if (predicted && point.contains("prediction_status")) {
        row.insert("prediction_status", point.value("prediction_status"));
        row.insert("prediction_reason", point.value("prediction_reason"));
      }
      updates << row;
    }
  }
  return updates;
}
} // namespace

QString MappingWorkbench::copyForEditing(const QString &id,
                                         const QStringList &references,
                                         QString *error) {
  auto d = declaration(id);
  if (!ready(d.horizon, error))
    return {};
  if (d.type != "vector") {
    fail(error, tr("请先将相栅格转为矢量面，再创建编辑副本"));
    return {};
  }
  auto *layer =
      qobject_cast<QgsVectorLayer *>(m_layers->instantiate(id, error));
  if (!layer)
    return {};
  if (layer->fields().indexOf("facies_code") < 0) {
    fail(error,
         tr("编辑底图需要 facies_code 相编码字段；请选择相面或预测相点"));
    return {};
  }
  QStringList parents;
  QVariantList referenceVersions;
  auto parent = ensureInputVersion(id, error);
  if (parent.isEmpty())
    return {};
  parents << parent;
  for (const auto &ref : references) {
    auto version = ensureInputVersion(ref, error);
    if (version.isEmpty())
      return {};
    parents << version;
    referenceVersions << QVariantMap{{"version_id", version}, {"role", "REFERENCES"}, {"tombstone", false}};
  }
  const auto draft = QStringLiteral("draft.%1").arg(uid());
  const auto relative =
      QStringLiteral("artifacts/working/%1/features.gpkg").arg(draft);
  const auto path = QDir(m_dir).filePath(relative);
  QDir().mkpath(QFileInfo(path).absolutePath());
  if (!MappingArtifactWriter::vectorSnapshot(layer, path, error))
    return {};
  const auto source = m_catalog->versionById(parent);
  {
    QgsVectorLayer copy(path + "|layername=features", "copy", "ogr");
    if (!MappingFeatureIdentity::ensure(&copy, error)) return {};
    if (!MappingArtifactWriter::syncFaciesAttributes(
            &copy, source.extra.value("facies", facies(d.horizon)).toList(),
            error))
      return {};
  }
  const QVariantMap extra{
      {"draft_id", draft},
      {"computational_parent_versions", QStringList{parent}},
      {"interpretation_reference_versions", referenceVersions},
      {"working_path", relative},
      {"facies", source.extra.value("facies", facies(d.horizon))},
      {"reference_layers", references},
      {"display_mode", displayMode(id)},
      {"hierarchy_model", "shared-polygon-partition-v1"},
      {"mock", source.extra.value("mock")},
      {"result_scope", layer->fields().indexOf("facies_intervals") >= 0 ? QStringLiteral("current_formation") : source.extra.value("result_scope").toString()},
      {"result_kind", layer->fields().indexOf("facies_intervals") >= 0 ? QStringLiteral("formation_prediction") : source.extra.value("result_kind").toString()},
      {"method", "manual-edit-copy"}};
  const auto editTitle = layer->fields().indexOf("facies_intervals") >= 0
                             ? tr("%1 测井预测相修订").arg(d.horizon)
                             : tr("%1 相图修订").arg(d.horizon);
  const auto snapshot =
      record(path, d.horizon, "edited_facies", editTitle, "vector", parents,
             extra, error, "|layername=features", draft);
  if (snapshot.isEmpty())
    return {};
  d.layerId = draft;
  d.source = path + "|layername=features";
  d.title = editTitle + tr(" · 工作副本");
  d.group = "05_PaleoMap";
  if (!m_layers->declare(d, error))
    return {};
  emit productReady(d.horizon, draft);
  return draft;
}
bool MappingWorkbench::saveEditingVersion(const QString &id, QString *error) {
  if (!id.startsWith("draft.")) {
    fail(error, tr("只有人工编辑副本可以保存编辑版本"));
    return false;
  }
  const auto d = declaration(id);
  if (!ready(d.horizon, error))
    return false;
  auto *layer =
      qobject_cast<QgsVectorLayer *>(m_layers->instantiate(id, error));
  if (!layer)
    return false;
  if (layer->isEditable()) {
    fail(error, tr("请先点击“保存编辑”提交当前编辑，再保存图件版本"));
    return false;
  }
  if (!MappingFeatureIdentity::ensure(layer, error)) return false;
  if (!FaciesHierarchyRenderer::validateTopology(layer, error))
    return false;
  const auto previous = versionForLayer(id);
  if (previous.id.isEmpty()) {
    fail(error, tr("找不到编辑副本的来源版本"));
    return false;
  }
  auto features = layer->getFeatures();
  QgsFeature feature;
  while (features.nextFeature(feature)) {
    bool ok = false;
    const double code = feature.attribute("facies_code").toDouble(&ok);
    if ((!feature.attribute("facies_code").isNull() &&
         (!ok || !std::isfinite(code) || code < 1 || code > 32767 ||
          code != std::round(code))) ||
        feature.geometry().isEmpty() || !feature.geometry().isGeosValid()) {
      fail(error, tr("工作副本含空几何、无效几何或非法相编码。请修正属性 / "
                     "几何后再保存图件版本；历史版本未改变。"));
      return false;
    }
  }
  auto *observer = MappingFeatureIdentity::Observer::of(layer);
  const auto spatialChanges = observer ? observer->changes() : QVariantList{};
  if (!MappingArtifactWriter::syncFaciesAttributes(
          layer, previous.extra.value("facies").toList(), error))
    return false;
  auto extra = previous.extra;
  extra.remove(QStringLiteral("evidence")); // rebuild this generation; historical versions stay frozen
  extra.insert("method", "manual-edit-save");
  extra.insert("display_mode", displayMode(id));
  extra.insert("hierarchy_model", "shared-polygon-partition-v1");
  QStringList evidenceParents{previous.id};
  QSet<QString> parentIds{previous.id}, referenceKeys;
  QVariantList referenceVersions;
  const auto rememberReference = [&](const QString &source, const QVariant &roleValue, bool tombstone) {
    if (!parentIds.contains(source)) { parentIds.insert(source); evidenceParents << source; }
    const QString role = roleValue.toString();
    const QString key = source + QChar(0x1f) + role + QChar(0x1f) + QString::number(tombstone);
    if (!referenceKeys.contains(key)) {
      referenceKeys.insert(key);
      referenceVersions << QVariantMap{{"version_id", source}, {"role", role}, {"tombstone", tombstone}};
    }
  };
  QHash<QString, int> qualityCounts;
  QHash<QString, int> stateCounts;
  qint64 evidenceCount = 0;
  auto evidenceFeatures = layer->getFeatures(); QgsFeature evidenceFeature;
  while (evidenceFeatures.nextFeature(evidenceFeature)) {
    QVariantList records; QVariantMap interpretation;
    if (layer->fields().indexOf("facies_evidence") >= 0 &&
        !paleo::evidence::decodeRecords(evidenceFeature.attribute("facies_evidence").toString(), &records, error)) return false;
    if (layer->fields().indexOf("paleo_interpretation") >= 0 &&
        !paleo::evidence::decodeInterpretation(evidenceFeature.attribute("paleo_interpretation").toString(), &interpretation, error)) return false;
    if (!interpretation.isEmpty() &&
        (interpretation.value("feature_uid").toString() != MappingFeatureIdentity::uid(evidenceFeature) ||
         interpretation.value("horizon").toString() != d.horizon)) {
      fail(error, tr("解释身份或层位与当前图斑不一致，请先复核")); return false;
    }
    ++stateCounts[interpretation.value("status", "Proposed").toString()];
    const QVariantMap target{{"feature_uid", MappingFeatureIdentity::uid(evidenceFeature)},
                            {"horizon", d.horizon},
                            {"geometry_hash", MappingFeatureIdentity::geometryFingerprint(evidenceFeature)}};
    for (const auto &finding : paleo::evidence::check(records, interpretation, target)) {
      const auto severity = finding.toMap().value("severity").toString();
      ++qualityCounts[severity];
      if (severity == "Blocking") {
        fail(error, finding.toMap().value("message").toString()); return false;
      }
    }
    evidenceCount += records.size();
    for (const auto &entry : records) {
      const auto row = entry.toMap();
      const auto source = row.value("source_version").toString();
      if (source.isEmpty()) continue;
      if (m_catalog->versionById(source).id.isEmpty()) {
        fail(error, tr("证据引用的来源版本缺失：%1").arg(source)); return false;
      }
      rememberReference(source, row.value("role", "REFERENCES"),
                        row.value("unlinked", row.value("tombstone", false)).toBool());
    }
  }
  QVariantList spatialEvents = previous.extra.value("spatial_edit_events").toList();
  if (observer) {
    const auto operation = uid();
    const QString actor = localOperator();
    for (const auto &change : spatialChanges) {
      auto event = change.toMap();
      QVariantList previousEvidence;
      if (!paleo::evidence::decodeRecords(event.value("before_evidence").toString(), &previousEvidence, error)) return false;
      for (const auto &entry : previousEvidence) {
        const auto row = entry.toMap(); const auto source = row.value("source_version").toString();
        if (source.isEmpty()) continue;
        if (m_catalog->versionById(source).id.isEmpty()) {
          fail(error, tr("已编辑或删除图斑的历史来源版本缺失：%1").arg(source)); return false;
        }
        rememberReference(source, row.value("role", "REFERENCES"), event.value("deleted", false).toBool());
      }
      if (event.contains("before_interpretation")) {
        QVariantMap beforeInterpretation;
        const auto serialized = event.value("before_interpretation");
        if (serialized.typeId() == QMetaType::QVariantMap) {
          beforeInterpretation = serialized.toMap();
          if (!paleo::evidence::validateInterpretation(beforeInterpretation, error)) return false;
        } else if (!paleo::evidence::decodeInterpretation(serialized.toString(), &beforeInterpretation, error)) return false;
        // A removed/merged feature can hold newer unsaved audit records than its
        // last version. Preserve all referenced sources from that captured journal.
        QVector<QVariant> pending{beforeInterpretation};
        int visited = 0;
        while (!pending.isEmpty()) {
          if (++visited > 100000) {
            fail(error, tr("历史解释引用超过安全读取范围，请先整理并保存独立版本")); return false;
          }
          const auto value = pending.takeLast();
          if (value.typeId() == QMetaType::QVariantMap) {
            const auto object = value.toMap();
            const auto source = object.value("source_version").toString();
            if (!source.isEmpty()) {
              if (m_catalog->versionById(source).id.isEmpty()) {
                fail(error, tr("已删除图斑解释历史的来源版本缺失：%1").arg(source)); return false;
              }
              rememberReference(source, object.value("role", "REFERENCES"), true);
            }
            for (auto it = object.cbegin(); it != object.cend(); ++it)
              if (it.value().typeId() == QMetaType::QVariantMap || it.value().typeId() == QMetaType::QVariantList)
                pending << it.value();
          } else if (value.typeId() == QMetaType::QVariantList) {
            for (const auto &item : value.toList()) pending << item;
          }
        }
      }
      event.insert("event_id", uid()); event.insert("operation_id", operation);
      event.insert("actor_id", actor); event.insert("reason", tr("保存原生地图几何编辑；空间关联需重新复核"));
      event.insert("timestamp", QDateTime::currentDateTimeUtc().toString(Qt::ISODateWithMs));
      event.insert("target_object_id", event.value("before_uid").toString().isEmpty()
                       ? event.value("after_uid") : event.value("before_uid"));
      event.insert("target_object_type", "MapFeature");
      event.insert("action", event.value("deleted").toBool() ? "delete_geometry" : "revise_geometry");
      event.insert("before_version_id", previous.id);
      event.insert("before_revision", previous.versionNumber);
      spatialEvents << event;
    }
  }
  extra.insert("spatial_edit_events", spatialEvents);
  extra.insert("computational_parent_versions", QStringList{previous.id});
  extra.insert("interpretation_reference_versions", referenceVersions);
  QVariantMap states, severities;
  for (auto it = stateCounts.cbegin(); it != stateCounts.cend(); ++it) states.insert(it.key(), it.value());
  for (auto it = qualityCounts.cbegin(); it != qualityCounts.cend(); ++it) severities.insert(it.key(), it.value());
  extra.insert("feature_summary", QVariantMap{{"feature_count", layer->featureCount()},
                                              {"evidence_count", evidenceCount}, {"statuses", states},
                                              {"quality_counts", severities}});
  extra.insert("feature_identity", QStringLiteral("paleo_feature_uid"));
  if (layer->fields().indexOf("facies_intervals") >= 0) {
    for (const auto &id : {WellAttributeStore::intervalLayerId(), WellAttributeStore::formationLayerId()}) {
      auto *attributes = qobject_cast<QgsVectorLayer *>(m_layers->layer(id));
      if (attributes && attributes->isEditable()) { fail(error, tr("测井属性表正在编辑，请先保存或放弃编辑")); return false; }
    }
  }
  const auto snapshotPath = m_dir + "/artifacts/staging/" + uid() + ".gpkg";
  if (!MappingArtifactWriter::vectorSnapshot(layer, snapshotPath, error))
    return false;
  const auto output =
      record(snapshotPath, d.horizon, "edited_facies",
             previous.extra.value("title").toString(), "vector",
             evidenceParents, extra, error, "|layername=features", id);
  if (!output.isEmpty()) {
    if (observer) observer->acknowledge();
    styleLayer(id);
    if (layer->fields().indexOf("facies_intervals") >= 0) {
      QVariantList points;
      auto it = layer->getFeatures(); QgsFeature feature;
      while (it.nextFeature(feature)) {
        QVariantMap point;
        for (const auto &field : {"id", "facies_intervals", "depth_mock", "formation_intervals"})
          if (layer->fields().indexOf(field) >= 0) point.insert(field, feature.attribute(field));
        points << point;
      }
      if (!WellAttributeStore::mergeIntervals(m_catalog, m_dir, m_layers,
          wellAttributeUpdates(points, previous.extra.value("facies").toList(), d.horizon, false), error) ||
          !WellAttributeStore::mergeFormations(m_catalog, m_dir, m_layers,
          wellFormationUpdates(points, previous.extra.value("facies").toList(), false), error)) return false;
    }
  }
  return !output.isEmpty();
}
QVariantList MappingWorkbench::removalProducts(const QString &layerId, QString *error) const {
  if (!layerId.startsWith("product.") && !layerId.startsWith("draft.")) {
    fail(error, tr("只有生成的图件与编辑副本可在此删除"));
    return {};
  }
  if (!m_catalog || !m_layers) {
    fail(error, tr("请先打开可写工程"));
    return {};
  }
  const auto target = versionForLayer(layerId);
  if (target.assetId.isEmpty()) {
    fail(error, tr("所选图件尚未登记到数据管理"));
    return {};
  }
  // 一次建反向谱系，按资产广度遍历；每条版本边只处理一次。
  QHash<QString, QVector<CatalogVersion>> childrenByAsset;
  for (const auto &child : m_catalog->versions()) {
    QSet<QString> parents;
    for (const auto &parent : child.parentVersionIds) {
      const auto asset = m_catalog->versionById(parent).assetId;
      if (!asset.isEmpty()) parents.insert(asset);
    }
    for (const auto &asset : parents) childrenByAsset[asset] << child;
  }
  QStringList assets{target.assetId};
  QSet<QString> selected{target.assetId};
  for (qsizetype i = 0; i < assets.size(); ++i)
    for (const auto &child : childrenByAsset.value(assets[i])) {
      if (selected.contains(child.assetId)) continue;
      if (!child.extra.value("mapping_product").toBool()) {
        fail(error, tr("图件被数据管理中的「%1」引用，请先处理该派生资产")
                        .arg(m_catalog->assetById(child.assetId).displayName));
        return {};
      }
      selected.insert(child.assetId);
      assets << child.assetId;
    }
  QVariantList products;
  for (const auto &asset : assets) {
    const auto version = m_catalog->currentVersion(asset);
    products << QVariantMap{{"asset_id", asset},
      {"title", version.extra.value("title", m_catalog->assetById(asset).displayName)},
      {"versions", m_catalog->versionsForAsset(asset).size()}};
  }
  return products;
}
bool MappingWorkbench::removeProduct(const QString &layerId, QString *error, bool includeDerived) {
  if (m_catalog && CatalogPurgeLease::isHeld(m_catalog->catalogPath())) {
    fail(error, tr("受管文件正在清理，请完成后再删除图件"));
    return false;
  }
  const auto products = removalProducts(layerId, error);
  if (products.isEmpty()) return false;
  if (products.size() > 1 && !includeDerived) {
    fail(error, tr("下游图件「%1」以它为来源，请确认同时删除关联派生图件")
                    .arg(products[1].toMap().value("title").toString()));
    return false;
  }
  QStringList assetIds, workingPaths;
  for (const auto &product : products) {
    const auto assetId = product.toMap().value("asset_id").toString();
    assetIds << assetId;
    for (const auto &version : m_catalog->versionsForAsset(assetId)) {
      const auto working = version.extra.value("working_path").toString();
      CatalogVersion check; check.path = working; check.managed = true;
      const auto resolved = DataCatalog::resolvedVersionPath(m_dir, check);
      if (!resolved.isEmpty() && QDir::cleanPath(resolved).startsWith(
              QDir(m_dir).absoluteFilePath("artifacts/working/")))
        workingPaths << QFileInfo(resolved).absolutePath();
    }
  }
  // 同资产的全部声明一并下架（同一资产多版本各自成行）。
  QVector<LayerDeclaration> declared;
  if (!m_layers->tryDeclared(&declared, error)) return false;
  QVector<LayerDeclaration> declarations;
  QStringList declIds;
  const QSet<QString> selectedAssets(assetIds.begin(), assetIds.end());
  for (const auto &d : declared)
    if (selectedAssets.contains(versionForLayer(d.layerId).assetId)) {
      declarations << d;
      declIds << d.layerId;
    }
  // 编辑中的实例不可销毁（removeDeclaration 自身也拦，前置校验让整组
  // 声明要么全下架要么不动）。
  const QSet<QString> selectedDeclarations(declIds.begin(), declIds.end());
  if (auto *project = m_project ? m_project->project() : nullptr)
    for (auto *layer : project->mapLayers())
      if (selectedDeclarations.contains(
              layer->customProperty("paleoLayerId").toString()))
        if (auto *vector = qobject_cast<QgsVectorLayer *>(layer);
            vector && vector->isEditable()) {
          fail(error, tr("请先保存或取消该图层的编辑，再删除图件"));
          return false;
        }
  // 先下架声明、释放 provider，再原子删除整套资产；目录提交失败恢复
  // 声明，避免留下消失的列表项或下次打开又冒出的图件。
  const auto restoreDeclarations = [this, &declarations] {
    for (const auto &d : declarations) m_layers->declare(d);
    emit changed();
  };
  if (!m_layers->removeDeclarations(declIds, error)) return false;
  const auto outcome = paleo::assetops::purgeAssets(m_catalog, m_dir, assetIds, true, true);
  if (outcome.purgedAssetIds.size() != assetIds.size()) {
    restoreDeclarations();
    fail(error, outcome.failedAssets.isEmpty() ? tr("图件删除失败") : outcome.failedAssets.constFirst());
    return false;
  }
  // 主线程只提交目录并释放 QGIS provider。物理清理使用值快照，后台不访问活 catalog。
  if (layerId.startsWith("draft."))
    workingPaths << QDir(m_dir).filePath(QStringLiteral("artifacts/working/") + layerId);
  workingPaths.removeDuplicates();
  const auto projectDir = m_dir;
  const auto retained = m_catalog->versions();
  const auto lease = CatalogPurgeLease::acquire(m_catalog->catalogPath());
  if (!lease) {
    emit errorOccurred(tr("图件记录已删除，后台文件清理未启动，请检查残留文件"));
  } else {
    auto *cleanup = new QFutureWatcher<paleo::assetops::PurgeOutcome>(this);
    connect(cleanup, &QFutureWatcher<paleo::assetops::PurgeOutcome>::finished, this,
        [this, cleanup, projectDir] {
          const auto result = cleanup->result();
          cleanup->deleteLater();
          if (!result.leftoverFiles.isEmpty()) {
            const auto message = tr("图件记录已删除，以下受管文件未能移除：%1").arg(result.leftoverFiles.join("；"));
            if (m_dir == projectDir) emit errorOccurred(message);
            else qWarning("%s", qPrintable(message));
          }
        });
    const auto future = QtConcurrent::run(
        paleo::tasks::pool(m_taskSvc),
        [projectDir, paths = outcome.pendingFiles, retained, workingPaths,
         lease] {
          auto result =
              paleo::assetops::purgeManagedFiles(projectDir, paths, retained);
          for (const auto &working : workingPaths)
            if (QDir(working).exists() && !QDir(working).removeRecursively())
              result.leftoverFiles << working;
          return result;
        });
    paleo::tasks::track(
        m_taskSvc, cleanup, future, tr("回收已删除图件的文件"), false,
        [future] {
          return future.result().leftoverFiles.isEmpty()
                     ? QString()
                     : QStringLiteral("部分图件文件未能回收");
        },
        false);
    cleanup->setFuture(future);
  }
  emit changed();
  if (m_project && !m_project->projectPath().isEmpty() &&
      !m_project->writeProject())
    emit errorOccurred(tr("图件已删除，但工程视图保存失败：%1")
                           .arg(m_project->lastErrors().join("；")));
  return true;
}
QString MappingWorkbench::snapshotConstraints(const QString &h,
                                              const QStringList &parents,
                                              QString *error) {
  if (!ready(h, error))
    return {};
  if (auto *editing = qobject_cast<QgsVectorLayer *>(m_layers->layer(QStringLiteral("constraints.%1").arg(h)));
      editing && editing->isModified()) {
    fail(error, tr("请先保存或撤销约束编辑，再计算并保存等值面"));
    return {};
  }
  auto *store = m_constraints->constraintStore();
  if (!store || m_constraints->loadConstraints(h).isEmpty())
    return {};
  QgsVectorLayer layer(store->gpkgPath() + "|layername=constraints" +
                           paleo::workflow_detail::constraintHorizonSubsetSuffix(h),
                       "constraints", "ogr");
  layer.setCrs(
      QgsCoordinateReferenceSystem::fromWkt(DataCatalog::localGridCrsWkt()));
  QDir().mkpath(m_dir + "/artifacts/staging");
  const auto path = m_dir + "/artifacts/staging/" + uid() + ".gpkg";
  if (!MappingArtifactWriter::vectorSnapshot(&layer, path, error))
    return {};
  int auditEvents = 0;
  if (!store->copyAudit(path, h, error, &auditEvents)) {
    QFile::remove(path);
    return {};
  }
  QStringList upstream = parents;
  const auto old =
      m_constraints
          ->property(("paleo.constraint.snapshot." + h).toUtf8().constData())
          .toString();
  if (!old.isEmpty())
    upstream
        << DerivedAssetRegistrar(m_catalog, m_dir).parentVersionIdsFor({old});
  QVariantMap snapshotExtra{{"method", "constraint-snapshot"}};
  if (auditEvents > 0)
    snapshotExtra.insert(QStringLiteral("constraint_audit_events"), auditEvents);
  const auto id =
      record(path, h, "constraint_snapshot", tr("%1 约束过程").arg(h), "vector",
             upstream, snapshotExtra, error,
             "|layername=features");
  if (!id.isEmpty())
    m_constraints->setProperty(
        ("paleo.constraint.snapshot." + h).toUtf8().constData(),
        filePath(declaration(id).source));
  return id;
}
bool MappingWorkbench::importConstraints(const QString &h, const QString &path,
                                         const QString &role, QString *error) {
  if (!ready(h, error))
    return false;
  // 与既有导入同一坐标门：约束几何必须落在本工程局部米制网格。
  {
    QgsVectorLayer probe(path, QStringLiteral("constraints"),
                         QStringLiteral("ogr"));
    if (probe.isValid() && !acceptsLocalGrid(probe.crs())) {
      fail(error, tr("约束线必须采用本工程的局部米制坐标；请先完成配准"));
      return false;
    }
  }
  const auto records =
      paleo::readConstraintImportFeatures(path, role, nullptr, error);
  if (records.isEmpty())
    return false;
  // Copy the entire source into a managed GPKG (including shapefile sidecars).
  QgsVectorLayer source(path, "constraints", "ogr");
  QDir().mkpath(m_dir + "/artifacts/staging");
  const auto input = m_dir + "/artifacts/staging/" + uid() + ".gpkg";
  if (!MappingArtifactWriter::vectorSnapshot(&source, input, error))
    return false;
  return publishConstraintImport(h, input, path, records, error);
}

bool MappingWorkbench::publishConstraintImport(
    const QString &h, const QString &input, const QString &path,
    const QVector<paleo::ImportedConstraintRecord> &records, QString *error,
    const DerivedFileFingerprint *fingerprint) {
  const QByteArray snapshotKey = ("paleo.constraint.snapshot." + h).toUtf8();
  const auto previousSnapshot = m_constraints->property(snapshotKey.constData());
  DataCatalog::BatchSave transaction(m_catalog);
  const auto inputId = record(input, h, "constraint_source", tr("%1 导入约束").arg(h), "vector", {},
                              {{"source_file", path}, {"method", "constraint-import"}}, error,
                              "|layername=features", {}, fingerprint);
  if (inputId.isEmpty()) { transaction.abort(); return false; }
  const auto source = versionForLayer(inputId);
  QSignalBlocker workflowSignals(m_constraints);
  QSignalBlocker layerSignals(m_layers);
  QStringList created;
  bool ok = true;
  for (const auto &record : records) {
    QString cid;
    if (!m_constraints->addConstraint(h, record.wkt, record.type, -1, error,
                                      &cid, record.params)) {
      ok = false;
      break;
    }
    created << cid;
  }
  const auto rollback = [&] {
    if (auto *store = m_constraints->constraintStore())
      for (const auto &cid : created) store->remove(cid);
    transaction.abort();
    m_constraints->setProperty(snapshotKey.constData(), previousSnapshot);
    m_layers->removeDeclaration(inputId);
    const auto sourcePath = DataCatalog::resolvedVersionPath(m_dir, source);
    QFile::setPermissions(sourcePath, QFileDevice::ReadOwner | QFileDevice::WriteOwner);
    QFile::remove(sourcePath);
    m_constraints->loadConstraints(h);
  };
  if (!ok) { rollback(); return false; }
  const auto snapshot = snapshotConstraints(h, {source.id}, error);
  if (snapshot.isEmpty()) { rollback(); return false; }
  const auto snapshotVersion = versionForLayer(snapshot);
  if (!transaction.flush(error)) {
    m_layers->removeDeclaration(snapshot);
    const auto snapshotPath = DataCatalog::resolvedVersionPath(m_dir, snapshotVersion);
    QFile::setPermissions(snapshotPath, QFileDevice::ReadOwner | QFileDevice::WriteOwner);
    QFile::remove(snapshotPath);
    rollback(); return false;
  }
  const bool saved = true;
  layerSignals.unblock();
  workflowSignals.unblock();
  emit m_layers->layerDeclared(QStringLiteral("constraints.%1").arg(h));
  emit m_constraints->constraintLineUpdated(QString());
  emit changed();
  return saved;
}

// 测区范围/成图边界导入（WS-B/WS-C）：面图层快照进 artifacts/layers/，
// 声明为 horizon 无关的 00_Data 共享层（图层树布局器置顶）。layerId 按
// 文件名稳定——同名重导是原位更新（declare upsert 换源重挂），不同名
// 边界并存供切换。
bool MappingWorkbench::importBoundaryLayer(const QString &path,
                                           QString *error) {
  if (!m_catalog || !m_catalog->isOpen() || m_dir.isEmpty()) {
    fail(error, tr("请先打开可写工程"));
    return false;
  }
  QgsVectorLayer source(path, QStringLiteral("boundary"), QStringLiteral("ogr"));
  if (!source.isValid()) {
    fail(error, tr("边界文件无法读取：%1").arg(path));
    return false;
  }
  if (source.geometryType() != Qgis::GeometryType::Polygon) {
    fail(error, tr("测区边界必须是面图层（多边形）"));
    return false;
  }
  if (!acceptsLocalGrid(source.crs())) {
    fail(error, tr("测区边界必须采用本工程的局部米制坐标；请先完成配准"));
    return false;
  }
  QDir().mkpath(m_dir + "/artifacts/layers");
  const QString copy = QDir(m_dir).filePath(
      QStringLiteral("artifacts/layers/boundary-%1.gpkg").arg(uid().left(8)));
  if (!MappingArtifactWriter::vectorSnapshot(&source, copy, error))
    return false;

  QString stem = QFileInfo(path).completeBaseName();
  stem.replace(QRegularExpression(QStringLiteral("[^A-Za-z0-9_-]")),
               QStringLiteral("_"));
  LayerDeclaration decl;
  decl.layerId = QStringLiteral("boundary.%1").arg(
      stem.isEmpty() ? uid().left(8) : stem);
  decl.horizon.clear();          // 共享层——不随层位切换释放
  decl.type = QStringLiteral("vector");
  decl.source = copy;
  decl.group = QStringLiteral("00_Data");
  decl.title = QFileInfo(path).completeBaseName();
  if (decl.title.isEmpty())
    decl.title = tr("测区边界");
  if (!m_layers->declare(decl, error))
    return false;
  auto *layer = qobject_cast<QgsVectorLayer *>(
      m_layers->instantiate(decl.layerId, error));
  if (!layer)
    return false;
  QgisStyleService::applyBoundaryLayerStyle(layer);
  layer->triggerRepaint();
  emit changed();
  if (m_project && !m_project->projectPath().isEmpty() &&
      !m_project->writeProject())
    emit errorOccurred(tr("边界已导入，但工程视图保存失败：%1")
                           .arg(m_project->lastErrors().join("；")));
  return true;
}

bool MappingWorkbench::startConstraintImport(const QString &h, const QString &path,
                                             const QString &role, QString *error) {
  if (!ready(h, error))
    return false;
  if (m_constraints->factorBusy()) {
    fail(error, tr("已有单因素任务"));
    return false;
  }
  if (!m_taskSvc)
    return importConstraints(h, path, role, error);
  struct Import {
    QString path;
    QVector<paleo::ImportedConstraintRecord> records;
    DerivedFileFingerprint fingerprint;
  };
  auto job = std::make_shared<Import>();
  QDir().mkpath(m_dir + "/artifacts/staging");
  job->path = m_dir + "/artifacts/staging/" + uid() + ".gpkg";
  return m_constraints->runFactorWork(
      tr("%1 · 导入%2").arg(h, role == QLatin1String("barrier") ? tr("间隔线") : tr("方向线")),
      [job, path, role](PaleoTask *task) {
        const auto cancelled = [task] { return task && task->cancelRequested(); };
        if (task)
          task->reportStage("parse", 5);
        QgsVectorLayer source(path, QStringLiteral("constraints"), QStringLiteral("ogr"));
        if (!source.isValid())
          return tr("请选择有效的线矢量文件");
        if (!acceptsLocalGrid(source.crs()))
          return tr("约束线必须采用本工程的局部米制坐标；请先完成配准");
        if (cancelled())
          return tr("导入已取消");
        QgsVectorFileWriter::SaveVectorOptions options;
        options.driverName = "GPKG";
        options.layerName = "features";
        QString error;
        if (QgsVectorFileWriter::writeAsVectorFormatV3(&source, job->path,
                                                       QgsCoordinateTransformContext(), options,
                                                       &error) != QgsVectorFileWriter::NoError)
          return error.isEmpty() ? tr("约束文件保存失败") : error;
        if (task)
          task->reportStage("parse", 50);
        job->records = paleo::readConstraintImportFeatures(job->path + "|layername=features", role,
                                                           nullptr, &error, cancelled);
        if (job->records.isEmpty())
          return error.isEmpty() ? tr("约束文件没有可导入的线") : error;
        if (task)
          task->reportStage("hash", 90);
        job->fingerprint = DerivedFileFingerprint::read(job->path, cancelled, &error);
        return error;
      },
      [this, job, h, path](QString *error) {
        return publishConstraintImport(h, job->path, path, job->records, error, &job->fingerprint);
      },
      [job] { QFile::remove(job->path); }, error);
}
