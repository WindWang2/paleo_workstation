// 层：数据
#include "querywellprojection.h"
#include "domain/wellref.h"
#include <paleo_data/service.h>
#include <QDir>
#include <QFileInfo>
#include <QJsonDocument>

namespace paleo::query {
bool currentWellboreMapping(const DataCatalog &catalog, const document::WellRef &well) {
  const auto checkpoint = catalog.projectionCheckpoint("area-sources");
  const auto entity = catalog.storedEntityById(well.catalogEntityId);
  return checkpoint.valid() && checkpoint.storeId == well.modelStoreIdentity && checkpoint.generation == well.modelGeneration &&
         entity.entityType == "well" && entity.extra.value("areaModelWellId").toLongLong() == well.modelWellId &&
         entity.extra.value("areaModelGeneration").toString() == well.modelGeneration && entity.extra.value("areaModelWellActive",true).toBool();
}
bool currentWellboreLas(const DataCatalog &catalog, const document::WellRef &well,
                       const CatalogVersion &version, qlonglong sourceId) {
  return sourceId > 0 && version.stage == "RAW" && version.extra.value("areaModelSourceId").toLongLong() == sourceId &&
         version.extra.value("areaModelGeneration").toString() == well.modelGeneration &&
         QFileInfo(version.fileName).suffix().compare("las",Qt::CaseInsensitive) == 0 &&
         catalog.assetById(version.assetId).type == "well_log" && catalog.currentVersion(version.assetId).id == version.id;
}
namespace {
QString headToken(const data::ModelChangeHead &head) {
  return QString::fromUtf8(QJsonDocument(QJsonObject{{"store",QString::fromStdString(head.storeId)},
    {"generation",QString::fromStdString(head.generation)},{"sequence",QString::number(head.sequence)}}).toJson(QJsonDocument::Compact));
}
}
struct WellProjection::Impl {
  QString root, snapshot; bool present = false;
  data::ModelChangeHead head;
  data::DataService service;
};
WellProjection::WellProjection(const QString &root) : m(std::make_unique<Impl>()) {
  m->root = QDir(root).absoluteFilePath(QStringLiteral("artifacts/metadata/area-model"));
}
WellProjection::~WellProjection() = default;
QString WellProjection::open() {
  m->present = QFileInfo::exists(m->root + QStringLiteral("/data.sqlite"));
  if (!m->present) {m->snapshot = QStringLiteral("absent"); return {};}
  std::string error; data::ModelChangeHead head;
  if (!m->service.openProjectReadOnly(m->root.toUtf8().toStdString(), error) ||
      !m->service.readModelChangeHead(head, error) || !head.available)
    return QStringLiteral("model_query_unavailable");
  if (head.storeId.empty() || head.generation.empty() || head.storeId.size() > 256 || head.generation.size() > 256 || head.sequence < 0)
    return QStringLiteral("model_query_unavailable");
  m->snapshot = headToken(head); m->head = head; return {};
}
QString WellProjection::token() const { return m->snapshot; }
bool WellProjection::exists() const { return m->present; }
bool WellProjection::matchesGeneration(const CatalogProjectionCheckpoint &checkpoint, const QString &generation) const {
  return m->head.available && checkpoint.valid() && checkpoint.generation == generation &&
         checkpoint.storeId == QString::fromStdString(m->head.storeId) && generation == QString::fromStdString(m->head.generation);
}
QJsonObject WellProjection::sourceBore(qlonglong wellId, const CatalogVersion &version) const {
  const auto sourceId = version.extra.value("areaModelSourceId").toLongLong();
  if (sourceId <= 0 || version.stage != "RAW" || QFileInfo(version.fileName).suffix().compare("las", Qt::CaseInsensitive)) return {};
  if (version.extra.value("areaModelGeneration").toString() != QString::fromStdString(m->head.generation))
    return {{"status", "stale_model"}};
  data::SourceIndexEntry source;
  data::SourceIndexReadOptions options; options.maxTextBytes = 8192; options.maxFieldBytes = 8192;
  std::optional<data::WellboreRef> bore; std::string error;
  if (!m->service.readSourceIndexEntry(sourceId, source, error, options)) return {{"status", "unavailable"}};
  if (!source.active || version.sha256.isEmpty() || QString::fromStdString(source.sha256) != version.sha256)
    return {{"status", "stale_source"}};
  if (!m->service.findWellbore(data::SourceWellboreKey{sourceId, "file"}, bore, error)) return {{"status", "unavailable"}};
  if (!bore) return {{"status", "unconfirmed"}};
  if (bore->wellId != wellId) return {{"status", "conflict"}};
  return {{"status", "confirmed"}, {"key", QString::fromStdString(bore->key)}};
}
QString WellProjection::borePage(qlonglong wellId, const QString &after, int limit, QJsonArray &items, bool &more) const {
  items = {}; more = false;
  data::WellborePage page; std::string error;
  if (!m->service.listWellbores(wellId, after.toUtf8().toStdString(), limit, page, error))
    return QStringLiteral("wellbore_query_unavailable");
  for (const auto &bore : page.items) {
    items.append(QJsonObject{{"key", QString::fromStdString(bore.ref.key)}, {"name", QString::fromStdString(bore.displayName)},
                             {"main", bore.ref.key == "main"}, {"declared", bore.declared}});
  }
  more = !page.nextKey.empty(); return {};
}
QString WellProjection::verify() const {
  if (!m->present) return QFileInfo::exists(m->root + QStringLiteral("/data.sqlite")) ? QStringLiteral("model_revision_changed") : QString();
  std::string error; data::ModelChangeHead head;
  if (!m->service.readModelChangeHead(head, error) || !head.available) return QStringLiteral("model_query_unavailable");
  return headToken(head) == m->snapshot ? QString() : QStringLiteral("model_revision_changed");
}
QString WellProjection::overlay(CatalogEntity &entity) const {
  entity.extra.insert(QStringLiteral("queryModelAvailable"), false);
  if (!m->present || entity.entityType != QLatin1String("well")) return {};
  const auto id = entity.extra.value(QStringLiteral("areaModelWellId")).toLongLong();
  if (id < 0) return {}; // A broken explicit mapping is not permission to match a different well by name.
  data::WellQueryValue row; bool found = false; std::string error;
  if (!m->service.readWellQueryValue(id, entity.name.toUtf8().toStdString(), row, found, error))
    return error == "WellQueryValueBudgetExceeded" ? QStringLiteral("model_value_budget_exceeded") : QStringLiteral("model_query_unavailable");
  if (found) {
    entity.name = QString::fromUtf8(row.name); entity.td = row.totalDepth;
    entity.extra.insert(QStringLiteral("areaModelWellId"), qlonglong(row.id));
    entity.extra.insert(QStringLiteral("queryModelAvailable"), true);
  }
  // A missing mapped model well retains the stored name/TD in the existing projection.
  return {};
}
QString WellProjection::matches(const QString &name, qlonglong after, int limit,
                                QVector<qlonglong> &ids, bool &more) const {
  ids.clear(); more = false; if (!m->present) return {};
  std::vector<long long> rows; std::string error;
  if (!m->service.findWellIdsByQueryName(name.toUtf8().toStdString(), after, limit, rows, more, error))
    return QStringLiteral("model_name_index_unavailable");
  for (const auto id : rows) ids.append(id);
  return {};
}
}
