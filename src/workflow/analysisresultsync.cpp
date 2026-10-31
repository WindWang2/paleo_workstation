// 层：功能
#include "analysisresultsync.h"
#include "catalog/datacatalog.h"
#include "services/subintervalfactors.h"
#include "paleo_data/service.h"
#include <QFileInfo>
#include <QJsonDocument>
#include <QJsonObject>
#include <QJsonArray>
#include <QSet>
#include <QCryptographicHash>

namespace {
QVariantMap identity(DataCatalog *catalog, const CatalogVersion &v, QSet<QString> &seen) {
  if (seen.contains(v.id)) return {};
  seen.insert(v.id);
  auto unit = v.extra.value("unitSelection").toMap();
  if (unit.isEmpty()) unit = v.extra.value("algorithm_parameters").toMap().value("unitSelection").toMap();
  if (!unit.isEmpty()) return unit;
  QVariantMap inherited;
  for (const auto &id : v.parentVersionIds) {
    auto branch = seen;
    const auto candidate = identity(catalog, catalog->versionById(id), branch);
    if (candidate.isEmpty()) continue;
    if (!inherited.isEmpty() && inherited != candidate) return {};
    inherited = candidate;
  }
  return inherited;
}
QString resultKind(const QString &type) {
  if (type == "prediction_raster" || type == "onnx_prediction") return "seismic_facies";
  if (type == "well_litho_intervals" || type == "well_facies_prediction") return "well_facies";
  if (type == "facies_polygons") return "map";
  if (type == "facies_fusion_raster") return "composite";
  if (type.contains("factor") || type.startsWith("thickness_")) return "single_factor";
  return {};
}
bool stale(DataCatalog *catalog, const CatalogVersion &v, QSet<QString> &seen) {
  if (v.id.isEmpty() || seen.contains(v.id)) return true;
  seen.insert(v.id);
  if (v.extra.value("stale").toBool()) return true;
  for (const auto &id : v.parentVersionIds) {
    auto branch = seen;
    const auto parent = catalog->versionById(id);
    if (catalog->currentVersion(parent.assetId).id != id || stale(catalog, parent, branch)) return true;
  }
  return false;
}
}

QString syncAnalysisResults(DataCatalog *catalog, const QString &projectDir,
                            const std::function<bool()> &cancelled) {
  if (!catalog || !catalog->isOpen() || catalog->refusesWrites() || projectDir.isEmpty()) return {};
  // Legacy projects remain readable; do not create a second area model.
  if (!QFileInfo::exists(projectDir + "/artifacts/metadata/area-model/data.sqlite")) return {};
  paleo::data::DataService ds;
  QString error;
  if (!paleo::strat::openProjectDataService(projectDir, ds, &error)) return error;
  QSet<QString> registered;
  QSet<QString> invalidated;
  QHash<QString, paleo::data::StratigraphicScheme> heads;
  for (const auto &scheme : paleo::strat::listProjectSchemes(ds, &error)) {
    if (cancelled && cancelled()) return {};
    paleo::data::StratigraphicScheme head;
    if (!paleo::strat::loadProjectScheme(ds, scheme.id, scheme.headRevision, head, &error)) return error;
    heads.insert(scheme.id, head);
    std::vector<paleo::data::SubIntervalResultBinding> bindings;
    std::string detail;
    if (!ds.listSubIntervalResults(scheme.id.toStdString(), {}, bindings, detail))
      return QString::fromStdString(detail);
    for (const auto &b : bindings) {
      const auto id = QString::fromStdString(b.id);
      registered.insert(id);
      if (b.status == "stale" || b.status == "legacy_unresolved") invalidated.insert(id);
      if (!invalidated.contains(id) && id.startsWith("catalog.") && catalog->versionById(id.mid(8)).id.isEmpty()) {
        if (!ds.invalidateSubIntervalResult(b.id, detail)) return QString::fromStdString(detail);
        invalidated.insert(id); // #890：记账，后续同 id 不再重复 invalidate
      }
    }
  }
  if (!error.isEmpty()) return error;
  for (const auto &v : catalog->versions()) {
    if (cancelled && cancelled()) return {};
    const QString kind = resultKind(catalog->assetById(v.assetId).type);
    if (kind.isEmpty() || v.stage == "RAW") continue;
    QSet<QString> seen;
    const auto unit = paleo::strat::fromMap(identity(catalog, v, seen));
    if (!unit.isValid() || unit.schemeId.isEmpty() || unit.revision.isEmpty()) continue;
    const QString id = "catalog." + v.id;
    QSet<QString> dependencies;
    bool outdated = stale(catalog, v, dependencies);
    if (registered.contains(id)) {
      if (outdated && !invalidated.contains(id)) {
        std::string failure;
        if (!ds.invalidateSubIntervalResult(id.toStdString(), failure)) return QString::fromStdString(failure);
      }
      continue;
    }
    paleo::data::SubIntervalResultBinding b, stored;
    if (heads.contains(unit.schemeId) && heads.value(unit.schemeId).revision.value != unit.revision.toStdString()) {
      paleo::data::StratigraphicScheme source;
      if (!paleo::strat::loadProjectScheme(ds, unit.schemeId, unit.revision, source, &error)) return error;
      for (const auto &sub : paleo::data::framework::impactedSubIntervals(source, heads.value(unit.schemeId))) {
        if (sub.value == unit.subIntervalId.toStdString()) outdated = true;
        const auto *old = paleo::data::framework::findSubInterval(source, sub);
        if (unit.aggregate && old && old->interval.value == unit.intervalId.toStdString()) outdated = true;
      }
    }
    b.id = id.toStdString(); b.kind = kind.toStdString();
    b.schemeId = unit.schemeId.toStdString(); b.schemeRevision.value = unit.revision.toStdString();
    b.interval.value = unit.intervalId.toStdString(); b.subInterval.value = unit.subIntervalId.toStdString();
    b.aggregate = unit.aggregate; b.assetRef = v.id.toStdString();
    const QString well = v.extra.value("wellEntityId").toString();
    if (!well.isEmpty()) b.wellId = paleo::strat::stableWellKey(well);
    QVariantList inputs;
    for (const auto &parent : v.parentVersionIds)
      inputs.append(QVariantMap{{"id", parent}, {"sha256", catalog->versionById(parent).sha256}});
    const auto fingerprint = QJsonDocument(QJsonObject{{"unit", QJsonObject::fromVariantMap(paleo::strat::toMap(unit))},
        {"inputs", QJsonArray::fromVariantList(inputs)}, {"parameters", QJsonObject::fromVariantMap(v.extra)}}).toJson(QJsonDocument::Compact);
    b.inputFingerprint = QCryptographicHash::hash(fingerprint, QCryptographicHash::Sha256).toHex().toStdString();
    b.status = outdated ? "stale" : "current";
    QVariantMap detail = v.extra;
    detail.insert("assetId", v.assetId); detail.insert("versionId", v.id);
    detail.insert("parentVersionIds", v.parentVersionIds);
    b.detailJson = QJsonDocument(QJsonObject::fromVariantMap(detail)).toJson(QJsonDocument::Compact).toStdString();
    std::string failure;
    if (!ds.bindSubIntervalResult(b, stored, failure)) return QString::fromStdString(failure);
    registered.insert(id);
  }
  return {};
}
