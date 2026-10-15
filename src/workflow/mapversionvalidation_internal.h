// 层：功能
#pragma once
#include "../catalog/datacatalog.h"
#include "../domain/interpretationevidence.h"
#include "../qgis/mappingfeatureidentity.h"
#include <QSet>
#include <QJsonArray>
#include <QJsonDocument>
#include <qgsfeature.h>
#include <qgsvectorlayer.h>
#include <QPointer>

class MapVersionController;

// A short-lived, typed proof for the immediate native commit only. Ordinary
// edits cannot manufacture it through layer properties. Native commit guards
// may reuse the already completed scan; any business mutation invalidates it.
class MapVersionEvidenceCommitProof final : public QObject {
public:
  static bool validFor(QgsVectorLayer *layer, const QString &horizon, const DataCatalog *catalog) {
    for (auto *child : layer->children())
      if (auto *proof = dynamic_cast<MapVersionEvidenceCommitProof *>(child))
        if (proof->m_valid && proof->m_horizon == horizon && proof->m_catalog == catalog &&
            (!catalog || proof->m_revision == catalog->catalogRevision())) return true;
    return false;
  }
private:
  friend class MapVersionController;
  MapVersionEvidenceCommitProof(QgsVectorLayer *layer, const QString &horizon, DataCatalog *catalog)
      : QObject(layer), m_layer(layer), m_horizon(horizon), m_catalog(catalog), m_revision(catalog ? catalog->catalogRevision() : 0) {
    QSet<int> businessFields;
    for (const auto &name : {MappingFeatureIdentity::UidField, MappingFeatureIdentity::ParentsField,
        MappingFeatureIdentity::RootsField, QString("facies_evidence"), QString("paleo_interpretation"),
        QString("horizon"), QString("facies_code")}) {
      const int index = layer->fields().indexOf(name); if (index >= 0) businessFields.insert(index);
    }
    const auto invalidate = [this] { m_valid = false; };
    connect(layer, &QgsVectorLayer::attributeValueChanged, this,
      [this, businessFields](QgsFeatureId, int index, const QVariant &) { if (businessFields.contains(index)) m_valid = false; });
    connect(layer, &QgsVectorLayer::geometryChanged, this, invalidate);
    connect(layer, &QgsVectorLayer::featureAdded, this, invalidate);
    connect(layer, &QgsVectorLayer::featureDeleted, this, invalidate);
    connect(layer, &QgsVectorLayer::updatedFields, this, invalidate);
    connect(layer, &QgsVectorLayer::subsetStringChanged, this, invalidate);
    connect(layer, &QgsMapLayer::dataChanged, this, invalidate);
    connect(layer, &QgsMapLayer::dataSourceChanged, this, invalidate);
    if (catalog) connect(catalog, &DataCatalog::changed, this, invalidate);
    // Constructed after existing consumers, this is the final synchronous
    // beforeCommit guard. A consumer which mutates business data after the
    // workbench's guard cannot commit under the now-invalid scan proof.
    connect(layer, &QgsVectorLayer::beforeCommitChanges, this, [this] {
      if (!m_valid && m_layer && m_layer->allowCommit()) {
        m_ownsRejection = true; m_layer->setAllowCommit(false);
      }
    });
  }
  void releaseGuard() {
    // Restore only our own temporary denial after the failed save returns.
    // Other guards' rejections remain owned by those guards.
    if (m_ownsRejection && m_layer) m_layer->setAllowCommit(true);
  }
  bool m_valid = true;
  bool m_ownsRejection = false;
  QPointer<QgsVectorLayer> m_layer;
  QString m_horizon;
  QPointer<DataCatalog> m_catalog;
  int m_revision;
};

// Shared by synchronous saves and the cancellable owner-thread query. No live
// layer is retained here; each feature is consumed once and only indexes/counts
// survive the batch. Catalog answers are deduplicated by immutable version ID.
struct MapVersionEvidenceScan {
  QSet<QString> layerUids, inputVersions;
  QVariantMap statuses;
  qint64 featureCount = 0;

  bool append(const QgsFeature &feature, const QString &horizon, DataCatalog *catalog, QString *error,
              bool requireStableIdentity = false) {
    const auto fail = [&](const QString &message) { if (error) *error = message; return false; };
    ++featureCount;
    const auto uid = MappingFeatureIdentity::uid(feature);
    if (requireStableIdentity && uid.isEmpty())
      return fail(QObject::tr("图元身份缺失，请修复工作副本后保存"));
    if (!uid.isEmpty() && layerUids.contains(uid))
      return fail(QObject::tr("证据图层中存在重复图元身份，请在工作副本中复核后保存"));
    if (!uid.isEmpty()) layerUids.insert(uid);
    for (const auto &field : {MappingFeatureIdentity::ParentsField, MappingFeatureIdentity::RootsField}) {
      if (feature.fields().indexOf(field) < 0) continue;
      const auto raw = feature.attribute(field).toString().trimmed(); if (raw.isEmpty()) continue;
      QJsonParseError parse; const auto json = QJsonDocument::fromJson(raw.toUtf8(), &parse);
      if (parse.error != QJsonParseError::NoError || !json.isArray())
        return fail(QObject::tr("图元来源身份损坏，原关联未覆盖"));
      for (const auto &parent : json.array()) if (!parent.isString() || parent.toString().trimmed().isEmpty())
        return fail(QObject::tr("图元来源身份损坏，原关联未覆盖"));
    }
    QVariantList records; QVariantMap interpretation;
    if (feature.fields().indexOf("facies_evidence") >= 0 &&
        !paleo::evidence::decodeRecords(feature.attribute("facies_evidence").toString(), &records, error)) return false;
    if (feature.fields().indexOf("paleo_interpretation") >= 0 &&
        !paleo::evidence::decodeInterpretation(feature.attribute("paleo_interpretation").toString(), &interpretation, error)) return false;
    const QVariantMap target{{"horizon", horizon}, {"feature_uid", uid},
                             {"geometry_hash", MappingFeatureIdentity::geometryFingerprint(feature)}};
    if (!interpretation.isEmpty() && (interpretation.value("feature_uid") != target.value("feature_uid") ||
        interpretation.value("horizon") != target.value("horizon")))
      return fail(QObject::tr("解释身份或层位与当前图斑不一致，请先复核"));
    for (const auto &finding : paleo::evidence::check(records, interpretation, target))
      if (finding.toMap().value("severity").toString() == "Blocking")
        return fail(finding.toMap().value("message").toString());
    const auto status = interpretation.value("status", "Proposed").toString();
    statuses.insert(status, statuses.value(status).toLongLong() + 1);
    for (const auto &entry : records) {
      const auto row = entry.toMap(); const auto source = row.value("source_version").toString();
      if (!row.value("feature_uid").toString().isEmpty() && row.value("feature_uid").toString() != uid)
        return fail(QObject::tr("证据关联指向其他图元，不能提交"));
      if (row.value("unlinked", row.value("tombstone", false)).toBool() || source.isEmpty() || inputVersions.contains(source)) continue;
      if (catalog && catalog->versionById(source).id.isEmpty())
        return fail(QObject::tr("图斑证据引用的来源版本缺失：%1").arg(source));
      inputVersions.insert(source);
    }
    return true;
  }
  QVariantList citedSources(DataCatalog *catalog) const {
    QStringList ids = inputVersions.values();
    ids.sort();
    QVariantList out;
    for (const auto &id : ids) {
      QVariantMap row{{"version_id", id}};
      if (catalog) {
        const auto version = catalog->versionById(id);
        if (!version.id.isEmpty()) {
          row.insert("asset_id", version.assetId);
          row.insert("sha256", version.sha256);
          row.insert("horizon", version.extra.value("horizon").toString());
        }
      }
      out << row;
    }
    return out;
  }
  QVariantMap summary() const { return {{"feature_count", featureCount}, {"statuses", statuses}}; }
};

// Append-only retention on the cited catalog version. The frozen cited_sources
// list is the publish gate; this extra only stops a later purge. A missing
// update must not pretend the map row was never saved.
inline QStringList mapResultRetention(const QVariant &value) {
  QStringList tokens = value.toStringList();
  if (!tokens.isEmpty()) return tokens;
  for (const auto &entry : value.toList()) {
    const auto token = entry.toString();
    if (!token.isEmpty()) tokens << token;
  }
  return tokens;
}
inline bool retainMapCitations(DataCatalog *catalog, const QString &horizon, int version,
                                const QVariantMap &provenance, QString *error) {
  if (!catalog || version <= 0) return true;
  const auto token = horizon + QLatin1Char('/') + QString::number(version);
  for (const auto &entry : provenance.value(QStringLiteral("cited_sources")).toList()) {
    const auto id = entry.toMap().value(QStringLiteral("version_id")).toString();
    if (id.isEmpty()) continue;
    const auto current = catalog->versionById(id);
    if (current.id.isEmpty()) {
      if (error) *error = QObject::tr("图斑证据引用的来源版本缺失：%1").arg(id);
      return false;
    }
    auto tokens = mapResultRetention(current.extra.value(QStringLiteral("retained_by_map_results")));
    if (tokens.contains(token)) continue;
    tokens << token;
    QVariantList stored;
    for (const auto &item : tokens) stored << item;
    if (!catalog->updateVersionExtra(id, {{QStringLiteral("retained_by_map_results"), stored}}, error))
      return false;
  }
  return true;
}
