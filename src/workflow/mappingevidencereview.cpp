// 层：功能
#include "mappingworkbench.h"
#include "../domain/interpretationevidence.h"
#include "../domain/predictionevidence.h"
#include "../domain/faciescatalog.h"
#include "../domain/facieshierarchy.h"
#include "../qgis/mappingfeatureidentity.h"
#include "../qgis/qgislayerservice.h"
#include <QJsonDocument>
#include <QSet>
#include <QUuid>
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
  // Catalog answers use stable IDs and O(1) lookups; no active QGIS object is
  // passed to the background graph workflow.
  for (auto &value : records) {
    auto row = value.toMap(); const auto source = row.value("source_version").toString();
    if (!source.isEmpty() && m_catalog) {
      const auto input = m_catalog->versionById(source);
      row.insert("source_missing", input.id.isEmpty()); row.insert("source_stale", input.extra.value("stale").toBool());
      row.insert("source_asset", input.assetId);
      row.insert("source_horizon", input.extra.value("horizon"));
      row.insert("source_current_version", m_catalog->currentVersion(input.assetId).id);
    }
    value = row;
  }
  auto findings = paleo::evidence::check(records, interpretation, t);
  if (f.fields().indexOf(MappingFeatureIdentity::ReviewField) >= 0 && f.attribute(MappingFeatureIdentity::ReviewField).toBool())
    findings << QVariantMap{{"severity", "Warning"}, {"code", "identity_changed"}, {"message", tr("图斑经过分割或合并，继承关系和空间证据需要复核")}};
  out.insert("records", records); out.insert("interpretation", interpretation); out.insert("quality", findings);
  out.insert("feature_id", f.id());
  if (MappingFeatureIdentity::uid(f).isEmpty()) out.insert("message", tr("早期图斑无稳定身份；创建编辑副本后可建立持久关联"));
  return out;
}
bool MappingWorkbench::addEvidence(const QString &id, const QList<qint64> &ids, const QVariantMap &entry, QString *error) {
  auto p = entry;
  p.insert("actor_id", entry.value("actor_id", localOperator()));
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
      if (source.id.isEmpty()) unsavedSource = sourceLayer;
      else { change.insert("source_version", source.id); change.insert("source_asset", source.assetId); }
      change.insert("source_title", d.title); change.insert("source_horizon", d.horizon);
    }
    const auto sourceId = change.value("source_version").toString();
    if (!sourceId.isEmpty() && (!m_catalog || m_catalog->versionById(sourceId).id.isEmpty())) return reject(error, tr("引用的来源版本不存在"));
  }
  struct Row { QgsFeatureId fid; paleo::evidence::Revision revision; };
  QVector<Row> rows; QSet<qint64> seen;
  for (auto fid : ids) {
    if (seen.contains(fid)) continue; seen.insert(fid);
    const auto f = v->getFeature(fid);
    if (!f.isValid()) return reject(error, tr("选中要素已不存在"));
    QVariantList records; QVariantMap interpretation;
    if (!read(f, &records, &interpretation, error)) return false;
    auto current = target(f, version, horizon);
    // Resolve live source health before acceptance, rather than trusting UI.
    for (auto &value : records) {
      auto r = value.toMap(); const auto source = r.value("source_version").toString();
      if (!source.isEmpty()) r.insert("source_missing", !m_catalog || m_catalog->versionById(source).id.isEmpty());
      value = r;
    }
    auto featureChange = change;
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
    const auto rev = expected.contains(uid) ? expected.value(uid).toULongLong() : p.value("expected_revision", interpretation.value("revision", 0)).toULongLong();
    Row row{fid, {}};
    if (!paleo::evidence::revise(records, interpretation, current, action, featureChange,
        p.value("actor_id").toString(), p.value("reason").toString(), rev, &row.revision, error)) return false;
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
  if (!i.isEmpty() && i.value("feature_uid").toString() != currentUid) {
    // A split/merge materializes a NEW interpretation. Preserve the parent's
    // complete immutable record rather than rewriting its history as a child.
    const auto inherited = i;
    auto fresh = [] { return QUuid::createUuid().toString(QUuid::WithoutBraces); };
    i = {{"id", fresh()}, {"assertion_id", fresh()}, {"feature_uid", currentUid},
      {"horizon", t.value("horizon")}, {"status", "Proposed"}, {"revision", 0},
      {"inherited_interpretation", inherited}, {"parent_interpretation_id", inherited.value("id")}};
    change.insert("parent_interpretation_id", inherited.value("id"));
    for (auto &value : records) {
      auto row = value.toMap(); row.insert("inherited_from_feature_uid", row.value("feature_uid", inherited.value("feature_uid")));
      row.insert("feature_uid", currentUid); value = row;
    }
  }
  if (kind == "merge" && observer) {
    const auto parentUids = QJsonDocument::fromJson(f.attribute(MappingFeatureIdentity::ParentsField).toString().toUtf8()).toVariant().toList();
    QSet<QString> parents; for (const auto &uid : parentUids) parents.insert(uid.toString());
    QVariantList parentInterpretations;
    QSet<QString> interpretationIds, recordIds;
    for (const auto &value : records) recordIds.insert(value.toMap().value("id").toString());
    for (const auto &value : observer->changes()) {
      const auto parent = value.toMap();
      if (!parents.contains(parent.value("before_uid").toString())) continue;
      QVariantMap inherited; QVariantList inheritedRecords;
      if (!paleo::evidence::decodeInterpretation(parent.value("before_interpretation").toString(), &inherited, error) ||
          !paleo::evidence::decodeRecords(parent.value("before_evidence").toString(), &inheritedRecords, error)) return false;
      if (!inherited.isEmpty() && !interpretationIds.contains(inherited.value("id").toString())) {
        interpretationIds.insert(inherited.value("id").toString()); parentInterpretations << inherited;
      }
      if (parent.value("before_uid").toString() == i.value("inherited_interpretation").toMap().value("feature_uid").toString()) continue;
      for (const auto &value : inheritedRecords) {
        auto row = value.toMap(); const auto originalId = row.value("id").toString();
        row.insert("inherited_from_feature_uid", parent.value("before_uid")); row.insert("feature_uid", currentUid);
        if (recordIds.contains(originalId)) {
          row.insert("evidence_id", row.value("evidence_id", originalId));
          row.insert("id", QUuid::createUuid().toString(QUuid::WithoutBraces));
        }
        recordIds.insert(row.value("id").toString()); records << row;
      }
    }
    i.insert("parent_interpretations", parentInterpretations);
    change.insert("parent_interpretation_count", parentInterpretations.size());
  }
  paleo::evidence::Revision result;
  if (!paleo::evidence::revise(records, i, t, "edit", change, localOperator(),
      tr("QGIS 原生空间编辑（%1），继承的证据需要复核").arg(kind), i.value("revision", 0).toULongLong(), &result, error)) return false;
  return store(v, fid, result);
}
