// 层：数据
#include "areawellprojection.h"
#include "wellavailability.h"
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
QString databaseStamp(const QString &root) {
  QString stamp;
  for (const auto &suffix : {"/data.sqlite", "/data.sqlite-wal"}) {
    const QFileInfo f(root + suffix);
    stamp += QString(":%1:%2:%3:%4").arg(f.size()).arg(f.lastModified().toMSecsSinceEpoch())
        .arg(f.metadataChangeTime().toMSecsSinceEpoch()).arg(f.birthTime().toMSecsSinceEpoch());
  }
  return stamp;
}
Snapshot snapshot(const QString &project) {
  const QString root = QDir(project).absoluteFilePath("artifacts/metadata/area-model");
  Snapshot out; out.exists = QFileInfo::exists(root + "/data.sqlite");
  if (!out.exists) return out;
  out.stamp = databaseStamp(root);
  { QMutexLocker lock(&cacheMutex); const auto found = cache.constFind(root);
    if (found != cache.cend() && found->stamp == out.stamp) return *found;
  }
  paleo::data::DataService service; std::string error;
  std::vector<paleo::data::WellInfoRecord> rows;
  if (!service.openProjectReadOnly(root.toUtf8().toStdString(), error) || !service.listWellInfos(rows, error)) {
    out.error = QString::fromUtf8(error); qWarning() << "area well projection:" << out.error;
    return out;
  }
  QHash<qlonglong,WellDataAvailability> availability;
  if(!readWellDataAvailability(service,availability,&out.error))return out;
  for (const auto &row : rows) {
    CatalogEntity entity; const auto &w = row.value;
    entity.entityType = QStringLiteral("well"); entity.name = QString::fromUtf8(w.wellName);
    entity.surfaceX = w.wellheadX; entity.surfaceY = w.wellheadY;
    entity.hasSurface = std::isfinite(w.wellheadX) && std::isfinite(w.wellheadY);
    entity.coordinateStatus = entity.hasSurface ? "untransformed" : "missing";
    entity.td = w.totalDrilledDepth;
    entity.extra = {{"areaModelWellId", qlonglong(row.id)},
                    {"wellHasTrackData",availability.value(row.id).tracks},
                    {"wellHasCurveData",availability.value(row.id).curves},
                    {"wellDataSummary",availability.value(row.id).label},
                    {"wellCoordinatesCrs", QString::fromUtf8(w.coordinateSystem)},
                    {"wellCoordinateUnit", QString::fromUtf8(w.coordinateUnit)},
                    {"wellType", QString::fromUtf8(w.wellType)}, {"fieldName", QString::fromUtf8(w.fieldName)},
                    {"wellRemark", QString::fromUtf8(w.remark)}};
    out.ids.insert(row.id, out.rows.size());
    out.names.insert(entity.name, out.names.contains(entity.name) ? -1 : out.rows.size());
    out.rows << entity;
  }
  if (out.stamp == databaseStamp(root)) {
    QMutexLocker lock(&cacheMutex); if (cache.size() >= 64) cache.clear(); cache.insert(root, out);
  }
  return out;
}
}
CatalogEntity overlayAreaWell(CatalogEntity stored, const CatalogEntity &model) {
  const bool keepTransform = stored.hasSurface && model.hasSurface && stored.surfaceX == model.surfaceX
      && stored.surfaceY == model.surfaceY && stored.coordinateStatus == "ok"
      && stored.extra.value("wellCoordinatesCrs") == model.extra.value("wellCoordinatesCrs")
      && stored.extra.value("wellCoordinateUnit") == model.extra.value("wellCoordinateUnit");
  stored.name = model.name; stored.surfaceX = model.surfaceX; stored.surfaceY = model.surfaceY;
  stored.hasSurface = model.hasSurface; stored.td = model.td;
  stored.coordinateStatus = keepTransform ? "ok" : model.coordinateStatus;
  if (!keepTransform) { stored.extra.remove("projectLon"); stored.extra.remove("projectLat"); }
  for (auto it = model.extra.cbegin(); it != model.extra.cend(); ++it) stored.extra.insert(it.key(), it.value());
  return stored;
}
namespace {
CatalogEntity catalogRecord(CatalogEntity value) {
  // Legacy catalog columns are NOT NULL. hasSurface carries absence there;
  // public reads restore the domain model's NaNs instead of exposing these placeholders.
  if (!std::isfinite(value.surfaceX)) value.surfaceX = 0;
  if (!std::isfinite(value.surfaceY)) value.surfaceY = 0;
  if (!std::isfinite(value.td)) value.td = 0;
  return value;
}
bool sameWellProjection(const CatalogEntity &stored, const CatalogEntity &updated) {
  // overlayAreaWell copies every other field from the stored record unchanged.
  return stored.name == updated.name && stored.surfaceX == updated.surfaceX
      && stored.surfaceY == updated.surfaceY && stored.hasSurface == updated.hasSurface
      && stored.td == updated.td && stored.coordinateStatus == updated.coordinateStatus
      && stored.extra == updated.extra;
}
CatalogEntity resolve(const Snapshot &data, const CatalogEntity &stored) {
  if (stored.entityType != "well") return stored;
  const auto id = stored.extra.value("areaModelWellId").toLongLong();
  if (!data.exists && !id) return stored;
  const int index = id ? data.ids.value(id, -1) : data.names.value(stored.name, -1);
  if (index >= 0) return overlayAreaWell(stored, data.rows[index]);
  if (id || !data.error.isEmpty()) {
    auto result = stored; result.hasSurface = false; result.surfaceX = result.surfaceY = qQNaN();
    result.extra.remove("projectLon"); result.extra.remove("projectLat");
    result.extra.insert("wellHasTrackData",false); result.extra.insert("wellHasCurveData",false);
    result.extra.insert("wellDataSummary",QStringLiteral("模型资料不可用"));
    result.coordinateStatus = QStringLiteral("missing"); return result;
  }
  return stored; // Legacy/manual wells that are not part of this work-area model.
}
}
CatalogEntity projectAreaWell(const QString &project, const CatalogEntity &stored) {
  if (stored.entityType != "well" || project.isEmpty()) return stored;
  return resolve(snapshot(project), stored);
}
bool cachedAreaWellRows(const QString &project, QVector<CatalogEntity> &rows) {
  const QString root = QDir(project).absoluteFilePath("artifacts/metadata/area-model");
  const QString stamp = databaseStamp(root);
  QMutexLocker lock(&cacheMutex);
  const auto found = cache.constFind(root);
  if (found == cache.cend() || found->stamp != stamp) return false;
  rows = found->rows;
  return true;
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
  // Public reads already overlay the latest model and can hide stale persisted values.
  // Pass 1 binds by areaModelWellId and refreshes the maps. Pass 2 may adopt a card
  // only when that card has no model id and the name hits exactly one such card.
  // A card that already has a different model id is never overwritten.
  auto existing = catalog.storedEntities("well");
  QHash<qlonglong, int> byId; QHash<QString, int> unboundByName;
  for (int i = 0; i < existing.size(); ++i) {
    const auto id = existing[i].extra.value("areaModelWellId").toLongLong();
    if (id) byId.insert(id, byId.contains(id) ? -1 : i);
    else unboundByName.insert(existing[i].name, unboundByName.contains(existing[i].name) ? -1 : i);
  }
  auto persist = [&](int match, const CatalogEntity &model) {
    const auto updated = catalogRecord(overlayAreaWell(existing[match], model));
    if (!sameWellProjection(existing[match], updated) && !catalog.updateEntity(updated, error)) return false;
    const QString oldName = existing[match].name;
    const auto oldId = existing[match].extra.value("areaModelWellId").toLongLong();
    existing[match] = updated;
    if (!oldId && unboundByName.value(oldName) == match) unboundByName.remove(oldName);
    const auto newId = updated.extra.value("areaModelWellId").toLongLong();
    if (newId) byId.insert(newId, match);
    return true;
  };
  auto ambiguous = [&](const QString &name) {
    if (error) *error = QStringLiteral("ambiguous catalog well for area model: ") + name;
    batch.abort();
    return false;
  };
  QVector<int> pending;
  for (int mi = 0; mi < data.rows.size(); ++mi) {
    const auto &model = data.rows[mi];
    const auto id = model.extra.value("areaModelWellId").toLongLong();
    if (!id || !byId.contains(id)) { pending.append(mi); continue; }
    if (byId.value(id) < 0) return ambiguous(model.name);
    if (!persist(byId.value(id), model)) { batch.abort(); return false; }
  }
  for (int mi : pending) {
    const auto &model = data.rows[mi];
    const auto id = model.extra.value("areaModelWellId").toLongLong();
    if (id && byId.contains(id)) {
      if (byId.value(id) < 0) return ambiguous(model.name);
      if (!persist(byId.value(id), model)) { batch.abort(); return false; }
      continue;
    }
    const int nameMatch = unboundByName.value(model.name, -2);
    if (nameMatch == -1) return ambiguous(model.name);
    if (nameMatch >= 0) {
      if (!persist(nameMatch, model)) { batch.abort(); return false; }
      continue;
    }
    auto entity = catalogRecord(model); entity.id = catalog.nextEntityId("well");
    if (!catalog.addEntity(entity, error)) { batch.abort(); return false; }
    existing.append(entity);
    if (id) byId.insert(id, existing.size() - 1);
  }
  return batch.flush(error);
}
