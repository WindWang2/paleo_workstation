// 层：数据
#include "projectquery_internal.h"
#include <QJsonDocument>

namespace paleo::query {
namespace {
QJsonObject base(const QString &topic) {
  return {{"ok",true},{"code","ok"},{"contract","paleo.project.query.page.v1"},
    {"tool","paleo.query_project"},{"topic",topic}};
}
QString bindModel(QueryState &state, WellProjection &model) {
  const auto error = model.open(); if (!error.isEmpty()) return error;
  if (!state.modelToken.isEmpty() && state.modelToken != model.token()) return QStringLiteral("model_revision_changed");
  state.modelToken = model.token(); return {};
}
}
QJsonObject ProjectQueryCapability::Impl::listPage(QueryState &state, const QJsonObject &args,
                                                  int rows, int bytes, bool &more) {
  auto result = base(state.topic); QJsonArray values;
  if (state.topic == QLatin1String("summary")) {
    QJsonObject entities, assets;
    const auto entityCounts = catalog->entityCountsByType(); const auto assetCounts = catalog->assetCountsByType();
    for (auto it = entityCounts.cbegin(); it != entityCounts.cend(); ++it) entities.insert(it.key(), it.value());
    for (auto it = assetCounts.cbegin(); it != assetCounts.cend(); ++it) assets.insert(it.key(), it.value());
    result.insert("entities", entities); result.insert("assets_by_type", assets);
    result.insert("revision", catalog->catalogRevision()); result.insert("returnedCount", 0); result.insert("totalCount", 0);
    return result;
  }
  if (state.topic == QLatin1String("wells")) {
    WellProjection model(catalog->projectDir()); const auto error = bindModel(state, model);
    if (!error.isEmpty()) return queryFailure(error);
    const auto page = catalog->entityPage(QStringLiteral("@well-like"), state.after, rows);
    for (auto entity : page.items) {
      const auto failed = model.overlay(entity); if (!failed.isEmpty()) return queryFailure(failed);
      QJsonObject item{{"id",entity.id},{"name",entity.name.isEmpty()?entity.id:entity.name},{"type",entity.entityType},
        {"td",entity.tdValue()?QJsonValue(*entity.tdValue()):QJsonValue(QJsonValue::Null)}};
      const auto ref = queryWellReference(entity, state); if (ref.isEmpty()) return queryFailure("invalid_well_ref");
      item.insert("well_ref", ref);
      if (!appendWithinBudget(values, item, bytes, 1536)) {
        if (values.isEmpty()) return queryFailure("query_entry_too_large");
        more = true; break;
      }
      state.after = entity.id;
    }
    const auto failed = model.verify(); if (!failed.isEmpty()) return queryFailure(failed);
    more = more || page.hasMore; result.insert("wells", values);
    result.insert("count", page.totalCount); result.insert("totalCount", page.totalCount);
  } else if (state.topic == QLatin1String("assets") || state.topic == QLatin1String("horizons")) {
    const auto type = state.topic == QLatin1String("horizons") ? QStringLiteral("horizon") : args.value("asset_type").toString();
    const auto page = catalog->assetPage(type, state.after, rows);
    for (const auto &asset : page.items) {
      QJsonObject item{{"name",queryDisplayName(asset)}};
      if (state.topic == QLatin1String("horizons")) item.insert("asset_id", asset.id);
      else { item.insert("id", asset.id); item.insert("type", asset.type); item.insert("format", asset.format); }
      queryVersionCitation(item, catalog, asset.id);
      if (!appendWithinBudget(values, item, bytes, 1536)) {
        if (values.isEmpty()) return queryFailure("query_entry_too_large");
        more = true; break;
      }
      state.after = asset.id;
    }
    more = more || page.hasMore; result.insert(state.topic, values);
    result.insert("count", page.totalCount); result.insert("totalCount", page.totalCount);
  } else if (state.topic == QLatin1String("version_parents")) {
    const auto id = args.value("version_id").toString();
    if (id.isEmpty() || id.size() > 256) return queryFailure("invalid_version_ref");
    if (catalog->versionById(id).id != id) return queryFailure("version_not_found");
    if (args.contains("catalog_revision") && args.value("catalog_revision").toInt(-1) != catalog->catalogRevision())
      return queryFailure("stale_revision");
    const auto page = catalog->versionParentsPage(id, state.after, rows);
    const qlonglong start = state.after.isEmpty() ? 0 : state.after.toLongLong();
    for (const auto &parent : page.items) {
      QJsonObject item{{"version_id",parent}};
      if (!appendWithinBudget(values, item, bytes, 1536)) {
        if (values.isEmpty()) return queryFailure("query_entry_too_large");
        more = true; break;
      }
    }
    state.after = QString::number(start + values.size()); more = more || page.hasMore;
    result.insert("contract", "catalog.version.parents.page.v1"); result.insert("version_id", id);
    result.insert("parents", values); result.insert("count", page.totalCount); result.insert("totalCount", page.totalCount);
  } else return queryFailure("query_topic_invalid");
  result.insert("returnedCount", values.size()); return result;
}
}
