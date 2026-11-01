// 层：数据
#include "domain/shaequals.h"
#include "areasourceprojection.h"
#include "../domain/projectclassifier.h"
#include <paleo_data/service.h>
#include <QDir>
#include <QFileInfo>
#include <QHash>
#include <QSet>
#include <algorithm>

namespace {
using paleo::data::DataEntry;
using paleo::data::SourceIndexEntry;

struct Relations {
  QSet<QString> kinds;
  QHash<QString, QSet<qlonglong>> wellsByKind;
};
struct ModelSnapshot {
  QVector<SourceIndexEntry> sources;
  QHash<qlonglong, Relations> relations;
  QVector<DataEntry> volumes, horizons;
};

QString q(const std::string &value) { return QString::fromUtf8(value); }
QString areaRoot(const QString &project) { return QDir(project).absoluteFilePath("artifacts/metadata/area-model"); }
QString sourcePath(const SourceIndexEntry &source) { return QDir::cleanPath(q(source.path)); }

ProjectClassification classification(const SourceIndexEntry &source, const Relations &relations) {
  if (relations.kinds.contains("volumes")) return {QStringLiteral("seismic"), QFileInfo(q(source.path)).suffix().toLower(), QStringLiteral("input")};
  if (relations.kinds.contains("horizons")) return {QStringLiteral("horizon"), QFileInfo(q(source.path)).suffix().toLower(), QStringLiteral("input")};
  if (relations.kinds.contains("curves")) return {QStringLiteral("well_log"), QFileInfo(q(source.path)).suffix().toLower(), QStringLiteral("input")};
  if (relations.kinds.contains("intervals")) return {QStringLiteral("well_stratification"), QFileInfo(q(source.path)).suffix().toLower(), QStringLiteral("input")};
  if (relations.kinds.contains("time_depth")) return {QStringLiteral("time_depth"), QFileInfo(q(source.path)).suffix().toLower(), QStringLiteral("input")};
  if (relations.kinds.contains("trajectories")) return {QStringLiteral("well_deviation"), QFileInfo(q(source.path)).suffix().toLower(), QStringLiteral("input")};
  if (source.kind == "coordinates") return {QStringLiteral("well_head"), QFileInfo(q(source.path)).suffix().toLower(), QStringLiteral("input")};
  auto out = classifyProjectPath(q(source.path));
  if (relations.kinds.contains("core_photos")) out.type = QStringLiteral("image_reference");
  if (out.type.isEmpty() || out.type == "unknown") {
    if (source.kind == "document") out.type = QStringLiteral("document");
    else if (source.kind == "workbook") out.type = QStringLiteral("outsource_workbook");
    else if (source.kind == "text" || source.kind == "coordinates") out.type = QStringLiteral("tabular");
  }
  if (out.format.isEmpty()) out.format = QFileInfo(q(source.path)).suffix().toLower();
  return out;
}

QString dataRole(const QString &kind) {
  if (kind == "curves") return QStringLiteral("well_log");
  if (kind == "intervals") return QStringLiteral("tops");
  if (kind == "time_depth") return QStringLiteral("time_depth");
  if (kind == "trajectories") return QStringLiteral("trajectory");
  if (kind == "core_photos") return QStringLiteral("core");
  if (kind == "samples") return QStringLiteral("lab_analysis");
  if (kind == "attachments") return QStringLiteral("reference");
  if (kind == "markers") return QStringLiteral("interpretation");
  return {};
}

QString readablePath(const QString &root, const SourceIndexEntry &source) {
  const QString stored = q(source.storedPath).isEmpty() ? q(source.path) : q(source.storedPath);
  return QFileInfo(stored).isAbsolute() ? QDir::cleanPath(stored) : QDir(root).absoluteFilePath(stored);
}

bool loadSnapshot(const QString &root, paleo::data::DataService &service, ModelSnapshot &out, QString *error) {
  std::string detail;
  if (!service.openProjectReadOnly(root.toUtf8().toStdString(), detail)) {
    *error = q(detail); return false;
  }
  std::vector<SourceIndexEntry> sources;
  if (!service.listSourceIndexEntries(sources, detail)) { *error = q(detail); return false; }
  for (const auto &source : sources) {
    if (!source.active || source.status == "failed" || source.status == "ignored") continue;
    out.sources.append(source);
  }
  for (const auto &kind : {"curves", "intervals", "time_depth", "trajectories", "core_photos",
                           "samples", "attachments", "markers", "volumes", "horizons"}) {
    std::vector<DataEntry> rows;
    if (!service.listData(kind, 0, rows, detail)) { *error = q(detail); return false; }
    for (const auto &row : rows) {
      if (row.sourceId <= 0) continue;
      auto &relations = out.relations[row.sourceId];
      relations.kinds.insert(QString::fromLatin1(kind));
      if (row.wellId > 0) relations.wellsByKind[QString::fromLatin1(kind)].insert(row.wellId);
      if (row.kind == "volumes") out.volumes.append(row);
      else if (row.kind == "horizons") out.horizons.append(row);
    }
  }
  return true;
}

QString headerKey(QString value) {
  value.remove(' '); value.remove('\t'); value.remove('\r'); value.remove('\n');
  return value.toLower();
}

bool applyPointMapping(const QString &project, const QString &sourcePath,
                       const QVariantMap &mapping, QString *error) {
  if (error) error->clear();
  const QString wellColumn = mapping.value("well").toString().trimmed();
  const QString xColumn = mapping.value("x").toString().trimmed();
  const QString yColumn = mapping.value("y").toString().trimmed();
  if (sourcePath.isEmpty() || wellColumn.isEmpty() || xColumn.isEmpty() || yColumn.isEmpty()) return true;
  paleo::data::DataService service; std::string detail;
  const QString root = areaRoot(project);
  if (!service.openProject(root.toUtf8().toStdString(), detail)) { *error = q(detail); return false; }
  std::vector<SourceIndexEntry> sources;
  if (!service.listSourceIndexEntries(sources, detail)) { *error = q(detail); return false; }
  qlonglong sourceId = 0;
  for (const auto &source : sources)
    if (QDir::cleanPath(q(source.path)) == QDir::cleanPath(sourcePath)) { sourceId = source.id; break; }
  if (!sourceId) { *error = QStringLiteral("井位来源未进入工区数据模型：") + sourcePath; return false; }
  std::vector<DataEntry> tables;
  if (!service.listSourceData("tables", sourceId, tables, detail)) { *error = q(detail); return false; }
  std::vector<paleo::data::WellInfoRecord> existing;
  if (!service.listWellInfos(existing, detail)) { *error = q(detail); return false; }
  QHash<QString, qlonglong> ids;
  for (const auto &row : existing) ids.insert(DataCatalog::normalizeWellName(q(row.value.wellName)), row.id);

  int mapped = 0;
  for (const auto &table : tables) {
    paleo::data::DataTable head;
    if (!service.readTable("tables", table.id, 0, 60, head, detail)) { *error = q(detail); return false; }
    int well = -1, x = -1, y = -1; qlonglong headerRow = -1;
    for (const auto &row : head.rows) {
      for (int column = 1; column < int(row.size()); ++column) {
        const auto label = headerKey(QString::fromStdString(row[column]));
        if (label == headerKey(wellColumn)) well = column;
        if (label == headerKey(xColumn)) x = column;
        if (label == headerKey(yColumn)) y = column;
      }
      if (well > 0 && x > 0 && y > 0) { headerRow = QString::fromStdString(row.front()).toLongLong(); break; }
      well = x = y = -1;
    }
    if (headerRow < 0) continue;
    for (qlonglong offset = 0; offset < head.totalRows;) {
      paleo::data::DataTable data;
      if (!service.readTable("tables", table.id, offset, 500, data, detail)) { *error = q(detail); return false; }
      if (data.rows.empty()) break;
      for (const auto &row : data.rows) {
        if (QString::fromStdString(row.front()).toLongLong() <= headerRow) continue;
        const auto field = [&](int column) { return column < int(row.size()) ? QString::fromStdString(row[column]).trimmed() : QString(); };
        const QString name = field(well); bool xOk = false, yOk = false;
        const double vx = field(x).toDouble(&xOk), vy = field(y).toDouble(&yOk);
        const QString normalized = DataCatalog::normalizeWellName(name);
        if (normalized.isEmpty() || !xOk || !yOk) continue;
        paleo::data::WellInfo info; const auto id = ids.value(normalized, 0);
        if (id && !service.readWellInfo(id, info, detail)) { *error = q(detail); return false; }
        if (std::isfinite(info.wellheadX) && std::isfinite(info.wellheadY) &&
            (info.wellheadX != vx || info.wellheadY != vy)) continue;
        info.wellName = name.toUtf8().toStdString(); info.wellheadX = vx; info.wellheadY = vy;
        info.coordinateSystem = mapping.value("crs").toString().toUtf8().toStdString();
        info.coordinateUnit = mapping.value("unit").toString().toUtf8().toStdString();
        qlonglong newId = id;
        const bool ok = id ? service.saveWellInfo(id, info, detail) : service.createWell(info, newId, detail);
        if (!ok) { *error = q(detail); return false; }
        ids.insert(normalized, newId); ++mapped;
      }
      offset += data.rows.size();
    }
  }
  if (!mapped) { *error = QStringLiteral("模型井位表中没有符合显式映射的有效井位"); return false; }
  return true;
}

QHash<qlonglong, QString> catalogWellIds(DataCatalog &catalog) {
  QHash<qlonglong, QString> out;
  QHash<QString, QString> names;
  for (const auto &well : catalog.entities(QStringLiteral("well"))) {
    const auto modelId = well.extra.value("areaModelWellId").toLongLong();
    if (modelId) out.insert(modelId, well.id);
    const QString name = DataCatalog::normalizeWellName(well.name);
    names.insert(name, names.contains(name) ? QString() : well.id);
  }
  return out;
}

bool addLink(DataCatalog &catalog, const QHash<qlonglong, CatalogVersion> &versions,
             qlonglong sourceId, const QString &entityType, const QString &entityId,
             const QString &role, QString *error) {
  if (entityId.isEmpty() || role.isEmpty() || !versions.contains(sourceId)) return true;
  const QString assetId = versions.value(sourceId).assetId;
  EntityAssetLink link;
  link.entityType = entityType; link.entityId = entityId; link.assetId = assetId; link.role = role;
  int ordinal = -1;
  for (const auto &old : catalog.linksForEntity(entityId)) {
    if (old.assetId == assetId && old.entityType == entityType && old.role == role) return true;
    if (old.entityType == entityType && old.role == role) {
      ordinal = std::max(ordinal, old.ordinal);
      if (old.isPrimary && !old.unresolved) link.isPrimary = false;
    }
  }
  link.ordinal = ordinal + 1;
  return catalog.addLink(link, error);
}

bool ensureSurvey(DataCatalog &catalog, paleo::data::DataService &service, const DataEntry &row,
                  QHash<qlonglong, QString> &surveyIds, QString *error) {
  if (surveyIds.contains(row.id)) return true;
  paleo::data::SeismicVolume volume;
  std::string detail;
  if (!service.readVolume(row.id, volume, detail)) { *error = q(detail); return false; }
  QString surveyId;
  const auto existing = catalog.entities(QStringLiteral("seismic_survey"));
  for (const auto &entity : existing) {
    if (entity.extra.value("areaModelVolumeId").toLongLong() == row.id ||
        entity.name == q(volume.volumeName)) { surveyId = entity.id; break; }
  }
  if (surveyId.isEmpty()) {
    CatalogEntity entity;
    entity.id = QStringLiteral("survey-%1").arg(q(volume.volumeName));
    if (catalog.hasEntity(entity.id)) entity.id = catalog.nextEntityId(QStringLiteral("survey"));
    entity.entityType = QStringLiteral("seismic_survey");
    entity.name = q(volume.volumeName);
    entity.inlineMin = volume.geometry.inlineMin; entity.inlineMax = volume.geometry.inlineMax;
    entity.xlineMin = volume.geometry.crosslineMin; entity.xlineMax = volume.geometry.crosslineMax;
    const QString timeUnit = q(volume.sampling.timeUnit);
    if (timeUnit.compare("ms", Qt::CaseInsensitive) == 0) {
      entity.sampleIntervalUs = volume.sampling.sampleInterval * 1000.0;
      entity.startTimeMs = volume.sampling.firstTime;
    } else if (timeUnit.compare("us", Qt::CaseInsensitive) == 0) {
      entity.sampleIntervalUs = volume.sampling.sampleInterval;
      entity.startTimeMs = volume.sampling.firstTime / 1000.0;
    }
    entity.extra.insert(QStringLiteral("areaModelVolumeId"), qlonglong(row.id));
    surveyId = entity.id;
    if (!catalog.addEntity(entity, error)) return false;
  }
  surveyIds.insert(row.id, surveyId);
  return true;
}

bool addModelLinks(DataCatalog &catalog, paleo::data::DataService &service,
                   const ModelSnapshot &model, const QHash<qlonglong, CatalogVersion> &versions,
                   QString *error) {
  const auto wellIds = catalogWellIds(catalog);
  for (auto source = model.relations.cbegin(); source != model.relations.cend(); ++source) {
    for (auto kind = source->wellsByKind.cbegin(); kind != source->wellsByKind.cend(); ++kind) {
      const QString role = dataRole(kind.key());
      for (const auto modelWellId : kind.value()) {
        if (!wellIds.contains(modelWellId)) continue;
        if (!addLink(catalog, versions, source.key(), QStringLiteral("well"),
                     wellIds.value(modelWellId), role, error)) return false;
      }
    }
  }
  QHash<qlonglong, QString> surveyIds;
  for (const auto &row : model.volumes) {
    if (!ensureSurvey(catalog, service, row, surveyIds, error)) return false;
    if (!addLink(catalog, versions, row.sourceId, QStringLiteral("seismic_survey"),
                 surveyIds.value(row.id), QStringLiteral("seismic_volume"), error)) return false;
  }
  for (const auto &row : model.horizons) {
    if (!addLink(catalog, versions, row.sourceId, QStringLiteral("seismic_survey"),
                 surveyIds.value(row.parentId), QStringLiteral("horizon"), error)) return false;
  }
  return true;
}
}

bool applyAreaPointMapping(const QString &project, const QString &sourcePath,
                           const QVariantMap &mapping, QString *error) {
  return applyPointMapping(project, sourcePath, mapping, error);
}

bool registerAreaSources(DataCatalog &catalog, QString *error) {
  if (error) error->clear();
  const QString root = areaRoot(catalog.projectDir());
  if (!QFileInfo::exists(root + "/data.sqlite")) return true;
  paleo::data::DataService service;
  ModelSnapshot model;
  if (!loadSnapshot(root, service, model, error)) return false;

  DataCatalog::BatchSave batch(&catalog);
  QHash<qlonglong, CatalogVersion> bySource;
  QHash<QString, QVector<CatalogVersion>> byPath;
  for (const auto &version : catalog.versions()) {
    const auto sourceId = version.extra.value("areaModelSourceId").toLongLong();
    if (sourceId) bySource.insert(sourceId, version);
    byPath[QDir::cleanPath(version.sourceUri)].append(version);
  }
  for (const auto &source : model.sources) {
    const QString path = sourcePath(source);
    CatalogVersion previous = bySource.value(source.id);
    if (previous.id.isEmpty()) {
      const auto candidates = byPath.value(path);
      for (auto it = candidates.crbegin(); it != candidates.crend(); ++it) {
        if (previous.id.isEmpty() && paleo::domain::shaEquals(it->sha256, q(source.sha256))) previous = *it;
      }
      if (previous.id.isEmpty() && !candidates.isEmpty()) previous = candidates.last();
    }
    if (!previous.id.isEmpty() && paleo::domain::shaEquals(previous.sha256, q(source.sha256))) {
      if (previous.extra.value("areaModelSourceId").toLongLong() != source.id) {
        if (!catalog.updateVersionExtra(previous.id, "areaModelSourceId", qlonglong(source.id), error)) {
          batch.abort(); return false;
        }
        previous.extra.insert("areaModelSourceId", qlonglong(source.id));
      }
      bySource.insert(source.id, previous);
      continue;
    }

    const auto cls = classification(source, model.relations.value(source.id));
    CatalogAsset asset;
    asset.id = previous.id.isEmpty() ? catalog.nextAssetId() : previous.assetId;
    asset.type = cls.type; asset.format = cls.format; asset.displayName = QFileInfo(q(source.path)).fileName();
    if (previous.id.isEmpty() && !catalog.addAsset(asset, error)) { batch.abort(); return false; }

    CatalogVersion version;
    version.id = catalog.nextVersionId(); version.assetId = asset.id; version.stage = QStringLiteral("RAW");
    version.managed = false; version.path = readablePath(root, source);
    version.sourceUri = q(source.path); version.sha256 = q(source.sha256);
    version.fileName = QFileInfo(q(source.path)).fileName();
    version.extra.insert("areaModelSourceId", qlonglong(source.id));
    version.extra.insert("areaModelSourceKind", q(source.kind));
    if (!previous.id.isEmpty()) {
      const auto current = catalog.currentVersion(asset.id);
      version.versionNumber = current.versionNumber + 1;
      version.parentVersionIds = QStringList{current.id};
    }
    if (!catalog.addVersion(version, error)) { batch.abort(); return false; }
    bySource.insert(source.id, version); byPath[path].append(version);
  }
  if (!addModelLinks(catalog, service, model, bySource, error)) { batch.abort(); return false; }
  return batch.flush(error);
}
