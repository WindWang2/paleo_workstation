// 层：数据
#include "areawellprojection.h"
#include <paleo_data/service.h>
#include <QDateTime>
#include <QDebug>
#include <QDir>
#include <QFileInfo>
#include <QHash>
#include <QMutex>
#include <QMutexLocker>
#include <cmath>

namespace {
struct Snapshot {
  QString stamp, error; bool exists = false;
  QVector<CatalogEntity> rows;
  QHash<qlonglong, int> ids;
  QHash<QString, int> names;
};
QMutex cacheMutex;
QHash<QString, Snapshot> cache;
Snapshot snapshot(const QString &project) {
  const QString root = QDir(project).absoluteFilePath("artifacts/metadata/area-model");
  Snapshot out; out.exists = QFileInfo::exists(root + "/data.sqlite");
  if (!out.exists) return out;
  for (const auto &suffix : {"/data.sqlite", "/data.sqlite-wal"}) {
    const QFileInfo f(root + suffix);
    out.stamp += QString(":%1:%2:%3:%4").arg(f.size()).arg(f.lastModified().toMSecsSinceEpoch())
        .arg(f.metadataChangeTime().toMSecsSinceEpoch()).arg(f.birthTime().toMSecsSinceEpoch());
  }
  { QMutexLocker lock(&cacheMutex); const auto found = cache.constFind(root);
    if (found != cache.cend() && found->stamp == out.stamp) return *found;
  }
  paleo::data::DataService service; std::string error;
  std::vector<paleo::data::WellInfoRecord> rows;
  if (!service.openProjectReadOnly(root.toUtf8().toStdString(), error) || !service.listWellInfos(rows, error)) {
    out.error = QString::fromUtf8(error); qWarning() << "area well projection:" << out.error;
    return out;
  }
  for (const auto &row : rows) {
    CatalogEntity entity; const auto &w = row.value;
    entity.entityType = QStringLiteral("well"); entity.name = QString::fromUtf8(w.wellName);
    entity.surfaceX = w.wellheadX; entity.surfaceY = w.wellheadY;
    entity.hasSurface = std::isfinite(w.wellheadX) && std::isfinite(w.wellheadY);
    entity.coordinateStatus = entity.hasSurface ? "untransformed" : "missing";
    entity.td = w.totalDrilledDepth;
    entity.extra = {{"areaModelWellId", qlonglong(row.id)},
                    {"wellCoordinatesCrs", QString::fromUtf8(w.coordinateSystem)},
                    {"wellCoordinateUnit", QString::fromUtf8(w.coordinateUnit)},
                    {"wellType", QString::fromUtf8(w.wellType)}, {"fieldName", QString::fromUtf8(w.fieldName)},
                    {"wellRemark", QString::fromUtf8(w.remark)}};
    out.ids.insert(row.id, out.rows.size());
    out.names.insert(entity.name, out.names.contains(entity.name) ? -1 : out.rows.size());
    out.rows << entity;
  }
  { QMutexLocker lock(&cacheMutex); if (cache.size() >= 64) cache.clear(); cache.insert(root, out); }
  return out;
}
CatalogEntity overlay(CatalogEntity stored, const CatalogEntity &model) {
  const bool keepTransform = stored.hasSurface && model.hasSurface && stored.surfaceX == model.surfaceX
      && stored.surfaceY == model.surfaceY && stored.coordinateStatus == "ok"
      && stored.extra.value("wellCoordinatesCrs") == model.extra.value("wellCoordinatesCrs");
  stored.name = model.name; stored.surfaceX = model.surfaceX; stored.surfaceY = model.surfaceY;
  stored.hasSurface = model.hasSurface; stored.td = model.td;
  stored.coordinateStatus = keepTransform ? "ok" : model.coordinateStatus;
  if (!keepTransform) { stored.extra.remove("projectLon"); stored.extra.remove("projectLat"); }
  for (auto it = model.extra.cbegin(); it != model.extra.cend(); ++it) stored.extra.insert(it.key(), it.value());
  return stored;
}
CatalogEntity catalogRecord(CatalogEntity value) {
  // Legacy catalog columns are NOT NULL. hasSurface carries absence there;
  // public reads restore the domain model's NaNs instead of exposing these placeholders.
  if (!std::isfinite(value.surfaceX)) value.surfaceX = 0;
  if (!std::isfinite(value.surfaceY)) value.surfaceY = 0;
  if (!std::isfinite(value.td)) value.td = 0;
  return value;
}
}
namespace {
CatalogEntity resolve(const Snapshot &data, const CatalogEntity &stored) {
  if (stored.entityType != "well") return stored;
  const auto id = stored.extra.value("areaModelWellId").toLongLong();
  if (!data.exists && !id) return stored;
  const int index = id ? data.ids.value(id, -1) : data.names.value(stored.name, -1);
  if (index >= 0) return overlay(stored, data.rows[index]);
  if (id || !data.error.isEmpty()) {
    auto result = stored; result.hasSurface = false; result.surfaceX = result.surfaceY = qQNaN();
    result.extra.remove("projectLon"); result.extra.remove("projectLat");
    result.coordinateStatus = QStringLiteral("missing"); return result;
  }
  return stored; // Legacy/manual wells that are not part of this work-area model.
}
}
CatalogEntity projectAreaWell(const QString &project, const CatalogEntity &stored) {
  if (stored.entityType != "well" || project.isEmpty()) return stored;
  return resolve(snapshot(project), stored);
}
void projectAreaWells(const QString &project, QVector<CatalogEntity> &stored) {
  if (project.isEmpty()) return;
  const auto data = snapshot(project);
  for (auto &entity : stored) entity = resolve(data, entity);
}
bool registerAreaWells(DataCatalog &catalog, QString *error) {
  const auto data = snapshot(catalog.projectDir());
  if (!data.exists) return true;
  if (!data.error.isEmpty()) { if (error) *error = data.error; return false; }
  DataCatalog::BatchSave batch(&catalog);
  const auto existing = catalog.entities("well");
  QHash<qlonglong, int> byId; QHash<QString, int> byName;
  for (int i = 0; i < existing.size(); ++i) {
    const auto id = existing[i].extra.value("areaModelWellId").toLongLong();
    if (id) byId.insert(id, byId.contains(id) ? -1 : i);
    byName.insert(existing[i].name, byName.contains(existing[i].name) ? -1 : i);
  }
  for (const auto &model : data.rows) {
    const auto id = model.extra.value("areaModelWellId").toLongLong();
    const int match = byId.value(id, byName.value(model.name, -2));
    if (match == -1) {
      if (error) *error = QStringLiteral("ambiguous catalog well for area model: ") + model.name;
      batch.abort(); return false;
    }
    if (match >= 0) {
      if (!catalog.updateEntity(catalogRecord(overlay(existing[match], model)), error)) { batch.abort(); return false; }
      continue;
    }
    auto entity = catalogRecord(model); entity.id = catalog.nextEntityId("well");
    if (!catalog.addEntity(entity, error)) { batch.abort(); return false; }
  }
  return batch.flush(error);
}
