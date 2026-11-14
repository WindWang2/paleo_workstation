// 层：数据
#include "querywellprojection.h"
#include <paleo_data/service.h>
#include <QDir>
#include <QFileInfo>
#include <QJsonDocument>

namespace paleo::query {
namespace {
QString headToken(const data::ModelChangeHead &head) {
  return QString::fromUtf8(QJsonDocument(QJsonObject{{"store",QString::fromStdString(head.storeId)},
    {"generation",QString::fromStdString(head.generation)},{"sequence",QString::number(head.sequence)}}).toJson(QJsonDocument::Compact));
}
}
struct WellProjection::Impl {
  QString root, snapshot; bool present = false;
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
  m->snapshot = headToken(head); return {};
}
QString WellProjection::token() const { return m->snapshot; }
bool WellProjection::exists() const { return m->present; }
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
