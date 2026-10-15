// 层：功能
#include "mappingworkbench.h"
#include "../domain/interpretationevidence.h"
#include "../domain/predictionevidence.h"
#include "../domain/faciescatalog.h"
#include "../domain/facieshierarchy.h"
#include "../qgis/mappingfeatureidentity.h"
#include "../qgis/evidencemapadapter.h"
#include "../qgis/qgislayerservice.h"
#include <QJsonDocument>
#include <QFileInfo>
#include <QCryptographicHash>
#include <QSet>
#include <QUuid>
#include <cmath>
#include <functional>
#include <qgsvectorlayer.h>

namespace {
bool reject(QString *error, const QString &message) { if (error) *error = message; return false; }
QString attribute(const QgsFeature &f, const QString &name) {
  const int index = f.fields().indexOf(name);
  return index < 0 ? QString() : f.attribute(index).toString();
}
bool read(const QgsFeature &f, QVariantList *records, QVariantMap *interpretation, QString *error) {
  return paleo::evidence::decodeRecords(attribute(f, "facies_evidence"), records, error) &&
         paleo::evidence::decodeInterpretation(attribute(f, "paleo_interpretation"), interpretation, error);
}
QVariantMap target(const QgsFeature &f, const CatalogVersion &v, const QString &horizon) {
  return {{"feature_uid", MappingFeatureIdentity::uid(f)}, {"horizon", horizon},
    {"version_id", v.id}, {"asset_id", v.assetId}, {"geometry_hash", MappingFeatureIdentity::geometryFingerprint(f)},
    {"facies_code", f.attribute("facies_code")}, {"facies_schema_hash", paleo::evidence::fingerprint(v.extra.value("facies"))}};
}
bool store(QgsVectorLayer *v, QgsFeatureId fid, const paleo::evidence::Revision &r) {
  for (const auto &field : {"facies_evidence", "paleo_interpretation"})
    if (v->fields().indexOf(field) < 0 && !v->addAttribute(QgsField(field, QMetaType::Type::QString))) return false;
  return v->changeAttributeValues(fid, {{v->fields().indexOf("facies_evidence"), paleo::evidence::encode(r.records)},
    {v->fields().indexOf("paleo_interpretation"), paleo::evidence::encode(r.interpretation)}});
}
}
QString MappingWorkbench::localOperator() const {
  const auto user = qEnvironmentVariable("USER", qEnvironmentVariable("USERNAME", "operator"));
  return QStringLiteral("local:%1").arg(user.trimmed().isEmpty() ? QStringLiteral("operator") : user);
}
QVariantList MappingWorkbench::evidence(const QString &id, const QList<qint64> &ids) const {
  QVariantList result;
  auto *v = m_layers ? qobject_cast<QgsVectorLayer *>(m_layers->layer(id)) : nullptr;
  if (!v) return result;
  const auto version = versionForLayer(id);
  const auto schema = version.extra.value("facies").toList();
  const auto horizon = declaration(id).horizon;
  int count = 0;
  for (auto fid : ids) {
    if (++count > 100) break; // bounded detail view; bulk edits have separate validation.
    const auto f = v->getFeature(fid); QVariantList records; QVariantMap interpretation; QString error;
    if (!read(f, &records, &interpretation, &error)) {
      result << QVariantMap{{"id", "corrupt:" + QString::number(fid)}, {"feature_id", fid},
        {"text", error}, {"quality_error", true}, {"needs_review", true}};
      continue;
    }
    for (const auto &entry : records) {
      auto row = entry.toMap(); if (row.value("unlinked").toBool()) continue;
      row.insert("feature_id", fid); row.insert("feature_uid", MappingFeatureIdentity::uid(f));
      const auto current = FaciesHierarchy::key(FaciesCatalog::find(schema, f.attribute("facies_code")), row.value("level").toString());
      const auto findings = paleo::evidence::check({row}, interpretation, target(f, version, horizon));
      row.insert("quality", findings);
      row.insert("needs_review", (!row.value("category_key").toString().isEmpty() && row.value("category_key").toString() != current) ||
        !findings.isEmpty() || (f.fields().indexOf(MappingFeatureIdentity::ReviewField) >= 0 && f.attribute(MappingFeatureIdentity::ReviewField).toBool()));
      row.insert("interpretation_id", interpretation.value("id")); row.insert("assertion_id", interpretation.value("assertion_id"));
      row.insert("interpretation_revision", interpretation.value("revision", 0)); row.insert("interpretation_status", interpretation.value("status", "Proposed"));
      result << row;
    }
  }
  return result;
}
QVariantMap MappingWorkbench::featureEvidence(const QString &id, const QList<qint64> &ids) const {
  QVariantMap out{{"layer_id", id}, {"horizon", declaration(id).horizon}, {"records", QVariantList{}}};
  const auto version = versionForLayer(id);
  out.insert("version_id", version.id); out.insert("asset_id", version.assetId);
  out.insert("generation_evidence", version.extra.value("evidence"));
  QVariantList generation = version.extra.value("evidence").toMap().value("records").toList();
  if (generation.isEmpty()) { // compatible v1 without extension records.
    for (const auto &input : version.extra.value("evidence").toMap().value("inputs").toList()) {
      auto row = input.toMap(); row.insert("id", "source:" + paleo::evidence::fingerprint(row.value("version_id")));
      row.insert("source_version", row.value("version_id")); row.insert("source_asset", row.value("asset_id"));
      row.insert("relation", row.value("role", "DERIVED_FROM")); row.insert("text", row.value("name"));
      row.insert("origin", "computed_input"); generation << row;
    }
  }
  out.insert("generation_records", generation);
  // Existing well entities provide precise navigation identities. Participation
  // in generation is distinct from a scientifically determined spatial effect.
  if (m_catalog) {
    const int limit = qMin(200, int(generation.size()));
    for (int n = 0; n < limit; ++n) {
      auto row = generation[n].toMap();
      const auto assetId = row.value("source_asset", row.value("asset_id")).toString();
      QStringList wells;
      for (const auto &link : m_catalog->linksForAsset(assetId))
        if (link.entityType == "well" && !link.unresolved) wells << link.entityId;
      wells.removeDuplicates();
      if (wells.size() == 1) {
        const auto entity = m_catalog->entityById(wells.first());
        row.insert("entity_id", entity.id); row.insert("well_name", entity.name);
        row.insert("has_coordinates", entity.hasSurface); row.insert("x", entity.surfaceX); row.insert("y", entity.surfaceY);
      } else if (!wells.isEmpty()) row.insert("entity_ids", wells);
      generation[n] = row;
    }
    out.insert("generation_records", generation);
  }
  auto *v = m_layers ? qobject_cast<QgsVectorLayer *>(m_layers->layer(id)) : nullptr;
  if (!v || ids.isEmpty()) {
    out.insert("message", v ? tr("请在地图选择图斑以查看局部证据；以下为成果生成来源") : tr("地图要素暂不可用；可查看已保存的成果来源")); return out;
  }
  if (ids.size() != 1) out.insert("message", tr("已选 %1 个要素；详情显示首个，证据列表最多显示 100 个要素").arg(ids.size()));
  const auto f = v->getFeature(ids.first());
  if (!f.isValid()) { out.insert("error", tr("选中要素已不存在")); return out; }
  const auto t = target(f, version, declaration(id).horizon);
  for (auto it = t.cbegin(); it != t.cend(); ++it) out.insert(it.key(), it.value());
  QVariantList records; QVariantMap interpretation; QString error;
  if (!read(f, &records, &interpretation, &error)) { out.insert("error", error); return out; }
  const QVariantMap spatialTarget{{"wkb", f.geometry().asWkb()}, {"crs_wkt", v->crs().toWkt()}};
  QSet<QString> spatialSources;
  const auto spatial = [&](const QVariantMap &row) {
    return resolveEvidenceSource(row, spatialTarget, false, &spatialSources);
  };
  // Catalog answers use stable IDs and O(1) lookups; no active QGIS object is
  // passed to the background graph workflow.
  for (auto &value : records) value = spatial(value.toMap());
  QVariantList spatialCandidates;
  for (int n = 0; n < qMin(8, int(generation.size())); ++n) {
    auto row = spatial(generation[n].toMap()); generation[n] = row;
    for (const auto &candidate : row.value("spatial_query").toMap().value("features").toList()) {
      auto local = candidate.toMap();
      if (local.value("identity_state").toString() != "stable") continue;
      local.insert("id", "spatial:" + paleo::evidence::fingerprint(QVariantList{row.value("source_version"), local.value("feature_uid")}));
      local.insert("source_feature_uid", local.take("feature_uid")); local.insert("source_version", row.value("source_version"));
      local.insert("source_asset", row.value("source_asset")); local.insert("source_layer", row.value("source_layer"));
      local.insert("source_feature_horizon", local.value("horizon"));
      local.insert("name", tr("空间候选 · %1").arg(local.value("source_feature_uid").toString().left(12)));
      local.insert("relation", "REFERENCES"); local.insert("origin", "spatial_candidate"); local.insert("type", row.value("type", "reference"));
      spatialCandidates << local;
    }
  }
  out.insert("generation_records", generation); out.insert("spatial_candidates", spatialCandidates);
  auto findings = paleo::evidence::check(records, interpretation, t);
  if (f.fields().indexOf(MappingFeatureIdentity::ReviewField) >= 0 && f.attribute(MappingFeatureIdentity::ReviewField).toBool())
    findings << QVariantMap{{"severity", "Warning"}, {"code", "identity_changed"}, {"message", tr("图斑经过分割或合并，继承关系和空间证据需要复核")}};
  out.insert("records", records); out.insert("interpretation", interpretation); out.insert("quality", findings);
  out.insert("feature_id", f.id());
  if (MappingFeatureIdentity::uid(f).isEmpty()) out.insert("message", tr("早期图斑无稳定身份；创建编辑副本后可建立持久关联"));
  return out;
}
QVariantMap MappingWorkbench::resolveEvidenceSource(const QVariantMap &record,
    const QVariantMap &spatialTarget, bool loadSavedSource, QSet<QString> *spatialSources,
    QHash<QString, QVariantMap> *spatialResults) const {
  auto row = record;
  // These are query results, never caller assertions or durable observations.
  for (const auto &key : {"source_missing", "source_stale", "source_horizon", "source_current_version",
      "source_facies_schema_hash", "source_feature_missing", "source_feature_horizon",
      "spatial_intersection", "spatial_state", "spatial_message", "spatial_query"}) row.remove(key);
  row.insert("spatial_state", "unknown");
  if (row.value("unlinked").toBool()) return row;
  const auto sourceId = row.value("source_version", row.value("version_id")).toString();
  const auto source = m_catalog ? m_catalog->versionById(sourceId) : CatalogVersion();
  if (!sourceId.isEmpty()) {
    row.insert("source_missing", source.id.isEmpty() || (!source.path.isEmpty() &&
        !QFileInfo::exists(DataCatalog::resolvedVersionPath(m_dir, source).section('|', 0, 0))));
    row.insert("source_stale", source.extra.value("stale").toBool());
    row.insert("source_asset", source.assetId);
    row.insert("source_horizon", source.extra.value("horizon"));
    if (m_catalog && !source.id.isEmpty()) row.insert("source_current_version", m_catalog->currentVersion(source.assetId).id);
    if (source.extra.contains("facies")) row.insert("source_facies_schema_hash", paleo::evidence::fingerprint(source.extra.value("facies")));
  }
  if (sourceId.isEmpty() && !row.value("source_asset").toString().isEmpty()) {
    // Older asset-level references retain their original identity; resolving
    // currentVersion here would silently rewrite what the reviewer accepted.
    row.insert("source_missing", !m_catalog || m_catalog->assetById(row.value("source_asset").toString()).id.isEmpty());
  }
  const auto pinnedUid = row.value("source_feature_uid").toString().trimmed();
  if (loadSavedSource && pinnedUid.isEmpty()) return row; // catalog health is enough for an unpinned reference.
  const auto spatialKey = paleo::evidence::fingerprint(QVariantList{sourceId, pinnedUid,
      QString::fromLatin1(QCryptographicHash::hash(spatialTarget.value("wkb").toByteArray(), QCryptographicHash::Sha256).toHex()),
      spatialTarget.value("crs_wkt")});
  const auto applySpatial = [&](const QString &layerId, const QVariantMap &result) {
    const auto state = result.value("state").toString();
    row.insert("source_layer", layerId); row.insert("spatial_state", state); row.insert("spatial_query", result);
    row.insert("spatial_message", result.value("message"));
    row.insert("source_feature_horizon", result.value("pinned_feature").toMap().value("horizon"));
    if (state == "intersects" || state == "disjoint") row.insert("spatial_intersection", state == "intersects");
    if (state == "missing" || state == "ambiguous") row.insert("source_feature_missing", true);
  };
  if (loadSavedSource && spatialResults && spatialResults->contains(spatialKey)) {
    const auto cached = spatialResults->value(spatialKey);
    applySpatial(cached.value("layer_id").toString(), cached.value("result").toMap()); return row;
  }
  QString sourceLayer = "product." + sourceId;
  auto *loaded = m_layers ? m_layers->layer(sourceLayer) : nullptr;
  if (!loaded && m_layers) {
    const auto alias = row.value("source_layer", source.extra.value("layer_id")).toString();
    if (!alias.isEmpty() && !alias.startsWith("draft.") && !sourceId.isEmpty() && versionForLayer(alias).id == sourceId) {
      auto *candidate = m_layers->layer(alias);
      auto *vector = qobject_cast<QgsVectorLayer *>(candidate);
      if (!vector || vector->readOnly()) { sourceLayer = alias; loaded = candidate; }
    }
  }
  if (loadSavedSource && !loaded && !source.id.isEmpty() && m_layers) {
    QString error;
    // Product IDs are exact indexed identities. For raw/reference versions,
    // use only a declared alias recorded with that version; do not scan every
    // project layer to guess a spatial representation.
    if (source.extra.value("mapping_product").toBool())
      sourceLayer = const_cast<MappingWorkbench *>(this)->layerForVersion(sourceId, &error);
    else {
      const auto alias = row.value("source_layer", source.extra.value("layer_id")).toString();
      sourceLayer = !alias.startsWith("draft.") && !alias.isEmpty() && versionForLayer(alias).id == sourceId ? alias : QString();
      if (sourceLayer.isEmpty()) error = tr("原始来源未登记可核查的空间图层；请先加载该确切版本");
    }
    if (!sourceLayer.isEmpty()) loaded = m_layers->instantiate(sourceLayer, &error);
    if (!loaded) row.insert("spatial_message", error);
  }
  if (!loaded) {
    if (row.value("spatial_message").toString().isEmpty())
      row.insert("spatial_message", tr("来源尚未加载；定位来源后可核查局部空间关联"));
    return row; // unavailable is unknown, not proof that a pinned feature was deleted.
  }
  if (loadSavedSource) {
    // Acceptance checks only the pinned identity. An unbound adapter uses a
    // provider expression with LIMIT 2, detects duplicate UIDs, and avoids
    // building an index for every feature in a source layer. The eight-layer
    // detail-view budget must never make a valid interpretation unconfirmable.
    EvidenceMapAdapter exact(nullptr);
    const auto result = exact.spatialCandidates(loaded, spatialTarget, pinnedUid, 1);
    applySpatial(sourceLayer, result);
    // Bound transient memory independently of selected-feature × evidence count.
    // A full cache only costs additional exact queries; it never skips a check.
    if (spatialResults && spatialResults->size() < 1024)
      spatialResults->insert(spatialKey, {{"layer_id", sourceLayer}, {"result", result}});
    return row;
  }
  if (spatialSources && !spatialSources->contains(sourceLayer) && spatialSources->size() >= 8) {
    row.insert("spatial_message", tr("单次核查最多查询 8 个来源图层；请分组复核后重试"));
    return row;
  }
  if (spatialSources) spatialSources->insert(sourceLayer);
  auto *adapter = qobject_cast<EvidenceMapAdapter *>(m_evidenceSpatialAdapters.value(sourceLayer).data());
  if (!adapter) {
    if (m_evidenceSpatialAdapters.size() >= 16) {
      const auto first = m_evidenceSpatialAdapters.begin(); delete first.value().data(); m_evidenceSpatialAdapters.erase(first);
    }
    adapter = new EvidenceMapAdapter(nullptr, const_cast<MappingWorkbench *>(this)); m_evidenceSpatialAdapters.insert(sourceLayer, adapter);
  }
  adapter->bindLayer(qobject_cast<QgsVectorLayer *>(loaded));
  const auto result = adapter->spatialCandidates(loaded, spatialTarget, pinnedUid, 20);
  applySpatial(sourceLayer, result);
  return row;
}
bool MappingWorkbench::addEvidence(const QString &id, const QList<qint64> &ids, const QVariantMap &entry, QString *error) {
  auto p = entry;
  // #442：忽略调用方传入的身份，一律写 localOperator()。
  p.insert("actor_id", localOperator());
  p.insert("reason", entry.value("reason", entry.value("text")));
  return changeEvidence(id, ids, "link", p, error);
}
bool MappingWorkbench::removeEvidence(const QString &id, const QList<qint64> &ids, const QString &evidenceId, QString *error) {
  return changeEvidence(id, ids, "unlink", {{"evidence_id", evidenceId}, {"actor_id", localOperator()}, {"reason", tr("解除错误的证据关联")}}, error);
}
bool MappingWorkbench::changeEvidence(const QString &id, const QList<qint64> &ids, const QString &action, const QVariantMap &p, QString *error) {
  if (!id.startsWith("draft.") || !ready(declaration(id).horizon, error)) return reject(error, tr("请先创建可写的编辑副本"));
  auto *v = qobject_cast<QgsVectorLayer *>(m_layers->instantiate(id, error));
  if (!v || v->readOnly() || ids.isEmpty() || ids.size() > 10000) return reject(error, tr("请选择 1–10000 个编辑副本要素"));
  const auto version = versionForLayer(id); const auto horizon = declaration(id).horizon;
  if (p.contains("target_version") && p.value("target_version").toString() != version.id)
    return reject(error, tr("图件版本已经改变，请刷新后重试"));
  auto change = p;
  QString unsavedSource;
  if (action == "link") {
    const auto level = p.value("level", resolvedLevel(id)).toString();
    if (!FaciesHierarchy::levels().contains(level)) return reject(error, tr("请选择相解释层级"));
    change.insert("level", level);
    const auto sourceLayer = p.value("source_layer").toString();
    if (sourceLayer == id) return reject(error, tr("证据来源不能是当前工作副本"));
    if (!sourceLayer.isEmpty()) {
      const auto d = declaration(sourceLayer);
      if (!d.horizon.isEmpty() && d.horizon != horizon) return reject(error, tr("证据来源层位与图斑不一致"));
      // Existing saved sources only. Adding a relationship must not create an
      // input version as a side effect of a failed revision check.
      const auto source = versionForLayer(sourceLayer);
      if (source.id.isEmpty() || sourceLayer.startsWith("draft.")) unsavedSource = sourceLayer;
      else { change.insert("source_version", source.id); change.insert("source_asset", source.assetId); }
      change.insert("source_title", d.title); change.insert("source_horizon", d.horizon);
    }
    const auto sourceId = change.value("source_version").toString();
    if (!sourceId.isEmpty() && (!m_catalog || m_catalog->versionById(sourceId).id.isEmpty())) return reject(error, tr("引用的来源版本不存在"));
  }
  struct Row { QgsFeatureId fid; paleo::evidence::Revision revision; };
  QVector<Row> rows; QSet<qint64> seen; QSet<QString> spatialSources;
  QHash<QString, QVariantMap> spatialResults;
  const bool confirming = action == "review" && p.value("status").toString() == "Confirmed";
  for (auto fid : ids) {
    if (seen.contains(fid)) continue; seen.insert(fid);
    const auto f = v->getFeature(fid);
    if (!f.isValid()) return reject(error, tr("选中要素已不存在"));
    QVariantList records; QVariantMap interpretation;
    if (!read(f, &records, &interpretation, error)) return false;
    auto current = target(f, version, horizon);
    const QVariantMap spatialTarget{{"wkb", f.geometry().asWkb()}, {"crs_wkt", v->crs().toWkt()}};
    // The detail view and acceptance use the same authoritative source checks.
    // Unlink must remain available even when the source itself is gone.
    if (action != "unlink") for (auto &value : records) {
      auto row = resolveEvidenceSource(value.toMap(), spatialTarget, true, &spatialSources, &spatialResults);
      if (confirming && !row.value("unlinked").toBool() && !row.value("source_feature_uid").toString().isEmpty() &&
          row.value("spatial_state").toString() == "unknown")
        return reject(error, tr("来源图元核查尚未完成，不能确认解释：%1").arg(row.value("spatial_message").toString()));
      value = row;
    }
    auto featureChange = change;
    if (action == "link" && unsavedSource.isEmpty()) {
      featureChange = resolveEvidenceSource(featureChange, spatialTarget, true, &spatialSources, &spatialResults);
      auto checked = featureChange;
      if (checked.value("id", checked.value("evidence_id")).toString().isEmpty()) checked.insert("id", "pending-link");
      for (const auto &finding : paleo::evidence::check({checked}, {}, current))
        if (finding.toMap().value("severity").toString() == "Blocking")
          return reject(error, finding.toMap().value("message").toString());
    }
    if (p.contains("source_feature_uid") && (action == "link" || action == "update_evidence")) {
      auto candidate = featureChange;
      if (action == "update_evidence") for (const auto &record : records) {
        const auto row = record.toMap();
        if (row.value("id").toString() == p.value("evidence_id").toString()) {
          candidate = row;
          candidate.insert("source_feature_uid", p.value("source_feature_uid").toString().trimmed());
        }
      }
      const auto sourceUid = candidate.value("source_feature_uid").toString().trimmed();
      if (!sourceUid.isEmpty()) {
        if (!unsavedSource.isEmpty()) {
          // Preflight the imported working layer before saving its immutable
          // input snapshot; the recursive saved-version pass below rechecks it.
          auto *source = m_layers->instantiate(unsavedSource, error);
          if (!source) return false;
          EvidenceMapAdapter locator(nullptr);
          const auto result = locator.spatialCandidates(source, spatialTarget, sourceUid, 1);
          candidate.insert("spatial_state", result.value("state"));
          candidate.insert("spatial_message", result.value("message"));
          candidate.insert("source_feature_horizon", result.value("pinned_feature").toMap().value("horizon"));
          candidate.insert("spatial_intersection", result.value("state") == "intersects");
        } else {
          if (candidate.value("source_version").toString().isEmpty()) return reject(error, tr("指定来源图元需要已保存的来源版本"));
          candidate = resolveEvidenceSource(candidate, spatialTarget, true, &spatialSources, &spatialResults);
        }
        const auto state = candidate.value("spatial_state").toString();
        if (state == "missing" || state == "ambiguous" || state == "unknown") return reject(error, candidate.value("spatial_message").toString());
        const auto sourceHorizon = candidate.value("source_feature_horizon").toString();
        if (!sourceHorizon.isEmpty() && sourceHorizon != horizon) return reject(error, tr("来源图元属于其他层位，不能关联当前解释"));
        featureChange.insert("source_feature_uid", sourceUid);
        featureChange.insert("source_feature_horizon", sourceHorizon);
        featureChange.insert("spatial_intersection", state == "intersects");
        featureChange.insert("source_feature_missing", false);
      } else {
        // Removing a pin removes its cached health as well.
        featureChange.insert("source_feature_uid", QString());
        featureChange.insert("source_feature_horizon", QString());
        featureChange.remove("spatial_intersection");
      }
    }
    if (action == "link") {
      const auto category = FaciesCatalog::find(version.extra.value("facies").toList(), f.attribute("facies_code"));
      featureChange.insert("category_path", FaciesHierarchy::path(category).join(" / "));
      featureChange.insert("category_key", FaciesHierarchy::key(category, change.value("level").toString()));
    }
    const auto expected = p.value("expected_revisions").toMap();
    const auto uid = MappingFeatureIdentity::uid(f);
    if (p.contains("target_feature_uid") && p.value("target_feature_uid").toString() != uid)
      return reject(error, tr("选中图斑已经改变，请刷新后重试"));
    if (p.contains("expected_revisions") && !expected.contains(uid)) return reject(error, tr("缺少选中要素的修订号，请刷新后重试"));
    const auto revisionValue = expected.contains(uid) ? expected.value(uid) : p.value("expected_revision", interpretation.value("revision", 0));
    bool revisionOk = false; const double numericRevision = revisionValue.toDouble(&revisionOk);
    if (!revisionOk || !std::isfinite(numericRevision) || numericRevision < 0 || numericRevision != std::floor(numericRevision) || numericRevision > 9007199254740991.0)
      return reject(error, tr("预期修订号无效，请刷新后重试"));
    const auto rev = revisionValue.toULongLong();
    Row row{fid, {}};
    // #442：变更证据一律使用本机 localOperator()，忽略调用方 actor_id。
    if (!paleo::evidence::revise(records, interpretation, current, action, featureChange,
        localOperator(), p.value("reason").toString(), rev, &row.revision, error)) return false;
    rows << row;
  }
  if (!unsavedSource.isEmpty()) {
    // Validate every target/revision/reason before registering an unversioned
    // imported source. The existing registrar saves real immutable bytes, then
    // the same revision checks are repeated against that saved version. A
    // rejected/stale relationship must never create an input as a side effect.
    const auto sourceVersion = ensureInputVersion(unsavedSource, error);
    if (sourceVersion.isEmpty()) return false;
    const auto savedLayer = layerForVersion(sourceVersion, error);
    if (savedLayer.isEmpty()) return false;
    auto resolved = p; resolved.insert("source_layer", savedLayer);
    return changeEvidence(id, ids, action, resolved, error);
  }
  if (!v->isEditable() && !v->startEditing()) return reject(error, tr("无法开始编辑"));
  v->beginEditCommand(tr("修订解释证据与审核记录"));
  for (const auto &row : rows) if (!store(v, row.fid, row.revision)) {
    v->destroyEditCommand(); return reject(error, tr("证据修订失败，本次修改已撤销"));
  }
  v->endEditCommand(); emit faciesEdited(id); return true;
}
bool MappingWorkbench::recordInterpretationEdit(const QString &id, qint64 fid, int beforeCode, int afterCode, QString *error) {
  if (beforeCode == afterCode) return true;
  auto *v = qobject_cast<QgsVectorLayer *>(m_layers->layer(id));
  if (!v || !v->isEditable()) return reject(error, tr("解释修改需要当前 QGIS 编辑事务"));
  const auto f = v->getFeature(fid); QVariantList records; QVariantMap i;
  if (!read(f, &records, &i, error)) return false;
  paleo::evidence::Revision r;
  if (!paleo::evidence::revise(records, i, target(f, versionForLayer(id), declaration(id).horizon), "edit",
      {{"before_facies_code", beforeCode}, {"facies_code", afterCode}}, localOperator(),
      tr("人工指定相类型：%1 → %2").arg(beforeCode).arg(afterCode), i.value("revision", 0).toULongLong(), &r, error)) return false;
  return store(v, fid, r);
}
bool MappingWorkbench::recordSpatialInterpretationEdit(const QString &id, qint64 fid, const QString &kind, QString *error) {
  auto *v = m_layers ? qobject_cast<QgsVectorLayer *>(m_layers->layer(id)) : nullptr;
  if (!id.startsWith("draft.") || !v || !v->isEditable() || v->readOnly()) return reject(error, tr("空间解释修订需要可写工作副本"));
  const auto f = v->getFeature(fid); QVariantList records; QVariantMap i;
  if (!f.isValid() || !read(f, &records, &i, error)) return false;
  const auto t = target(f, versionForLayer(id), declaration(id).horizon);
  const auto currentUid = t.value("feature_uid").toString();
  if (kind == "classification" && i.contains("facies_code") && i.value("facies_code").toInt() == f.attribute("facies_code").toInt()) return true;
  QVariantMap change{{"spatial_change", kind}, {"facies_code", f.attribute("facies_code")}, {"geometry_hash", t.value("geometry_hash")}};
  const auto *observer = MappingFeatureIdentity::Observer::of(v);
  if (observer) change.insert("before", observer->editBefore(fid));
  auto fresh = [] { return QUuid::createUuid().toString(QUuid::WithoutBraces); };
  if (i.isEmpty() || i.value("feature_uid").toString() != currentUid) {
    // A split/merge materializes a NEW interpretation. Preserve a real parent
    // record rather than rewriting its history as a child. An empty parent is
    // not stored as an inherited object; the new identity still needs a valid shell.
    const auto inherited = i;
    i = {{"id", fresh()}, {"assertion_id", fresh()}, {"feature_uid", currentUid},
      {"horizon", t.value("horizon")}, {"status", "Proposed"}, {"revision", 0}};
    if (!inherited.isEmpty()) {
      i.insert("inherited_interpretation", inherited);
      i.insert("parent_interpretation_id", inherited.value("id"));
      change.insert("parent_interpretation_id", inherited.value("id"));
      for (auto &value : records) {
        auto row = value.toMap(); row.insert("inherited_from_feature_uid", row.value("feature_uid", inherited.value("feature_uid")));
        row.insert("feature_uid", currentUid); value = row;
      }
    }
  }
  if (kind == "merge" && observer) {
    const auto parentUids = QJsonDocument::fromJson(f.attribute(MappingFeatureIdentity::ParentsField).toString().toUtf8()).toVariant().toList();
    QSet<QString> parents; for (const auto &uid : parentUids) parents.insert(uid.toString());
    QVariantList parentInterpretations, carriedEvents;
    QSet<QString> interpretationIds, seenParentUids, recordIds, carriedEventIds, ancestorIds;
    std::function<void(const QVariantMap &)> takeEvents = [&](const QVariantMap &object) {
      for (const auto &key : {QStringLiteral("history"), QStringLiteral("carried_events")}) {
        for (const auto &eventValue : object.value(key).toList()) {
          const auto event = eventValue.toMap();
          const auto eventId = event.value("event_id").toString();
          if (eventId.isEmpty() || carriedEventIds.contains(eventId)) continue;
          carriedEventIds.insert(eventId);
          carriedEvents << QVariantMap{{"event_id", eventId}, {"action", event.value("action")},
            {"actor_id", event.value("actor_id")}, {"reason", event.value("reason")},
            {"timestamp", event.value("timestamp")},
            {"target_object_id", event.value("target_object_id")},
            {"target_object_type", event.value("target_object_type")},
            {"before_revision", event.value("before_revision")},
            {"after_revision", event.value("after_revision")}};
        }
      }
      const auto interpretationId = object.value("id").toString();
      if (!interpretationId.isEmpty()) ancestorIds.insert(interpretationId);
      for (const auto &parent : object.value("parent_interpretations").toList()) takeEvents(parent.toMap());
    };
    auto acceptParent = [&](QVariantMap inherited) {
      takeEvents(inherited);
      inherited.remove("history");
      inherited.remove("carried_events");
      inherited.remove("ancestor_interpretation_ids");
      inherited.remove("parent_interpretations");
      inherited.remove("inherited_interpretation");
      inherited.remove("inherited_evidence");
      const auto parentUid = inherited.value("feature_uid").toString();
      const auto parentId = inherited.value("id").toString();
      if (parentUid.isEmpty() || !parents.contains(parentUid) || seenParentUids.contains(parentUid) ||
          parentId.isEmpty() || interpretationIds.contains(parentId)) return;
      interpretationIds.insert(parentId); seenParentUids.insert(parentUid);
      parentInterpretations << inherited;
    };
    acceptParent(i.value("inherited_interpretation").toMap());
    for (const auto &value : records) recordIds.insert(value.toMap().value("id").toString());
    for (const auto &value : observer->changes()) {
      const auto parent = value.toMap();
      const auto parentUid = parent.value("before_uid").toString();
      if (!parents.contains(parentUid)) continue;
      QVariantMap inherited; QVariantList inheritedRecords;
      if (!paleo::evidence::decodeInterpretation(parent.value("before_interpretation").toString(), &inherited, error) ||
          !paleo::evidence::decodeRecords(parent.value("before_evidence").toString(), &inheritedRecords, error)) return false;
      if (inherited.isEmpty()) {
        inherited = {{"id", fresh()}, {"assertion_id", fresh()}, {"feature_uid", parentUid},
          {"horizon", t.value("horizon")}, {"status", "Proposed"}, {"revision", 0},
          {"facies_code", parent.value("before_facies_code")},
          {"geometry_hash", parent.value("before_fingerprint")},
          {"synthesized_from_feature", true}};
      }
      acceptParent(inherited);
      if (parentUid == i.value("inherited_interpretation").toMap().value("feature_uid").toString()) continue;
      for (const auto &value : inheritedRecords) {
        auto row = value.toMap(); const auto originalId = row.value("id").toString();
        row.insert("inherited_from_feature_uid", parentUid); row.insert("feature_uid", currentUid);
        if (recordIds.contains(originalId)) {
          row.insert("evidence_id", row.value("evidence_id", originalId));
          row.insert("id", fresh());
        }
        recordIds.insert(row.value("id").toString()); records << row;
      }
    }
    // Immediate parents stay one level deep. Unsaved review events and ancestor
    // ids are a flat, event-id-deduped log: nested copies would grow every merge,
    // and those events were never written to a saved parent version.
    i.remove("inherited_interpretation");
    i.insert("parent_interpretations", parentInterpretations);
    QStringList ancestors = ancestorIds.values();
    ancestors.sort();
    i.insert("ancestor_interpretation_ids", ancestors);
    i.insert("carried_events", carriedEvents);
    change.insert("parent_interpretation_count", parentInterpretations.size());
  }
  paleo::evidence::Revision result;
  if (!paleo::evidence::revise(records, i, t, "edit", change, localOperator(),
      tr("QGIS 原生空间编辑（%1），继承的证据需要复核").arg(kind), i.value("revision", 0).toULongLong(), &result, error)) return false;
  return store(v, fid, result);
}
