// 层：数据
#include "projectquery_internal.h"
#include <QJsonDocument>

namespace paleo::query {
QString ProjectQueryCapability::Impl::resolveWell(QueryState &state, const QJsonObject &args,
                                                 WellProjection &model, bool &more) {
  if (!state.target.isEmpty()) return {};
  if (args.contains("well_id")) {
    const auto id = args.value("well_id").toString(); const auto well = catalog->storedEntityById(id);
    if (id.isEmpty() || well.id != id || well.entityType != QLatin1String("well")) return QStringLiteral("well_not_found");
    state.target = id; return {};
  }
  const auto name = args.value("well_name").toString(); const auto key = DataCatalog::normalizeWellName(name);
  if (key.isEmpty()) return QStringLiteral("well_ref_required");
  int work = 256;
  while (!state.storedSearchDone && work > 0) {
    const auto page = catalog->wellNamePage(key, state.wellAfter, qMin(work, 32));
    for (auto well : page.items) {
      --work; const auto error = model.overlay(well); if (!error.isEmpty()) return error;
      state.wellAfter = well.id;
      if (DataCatalog::normalizeWellName(well.name) == key) state.candidates.insert(well.id);
      if (state.candidates.size() > 1) return QStringLiteral("well_ambiguous");
    }
    state.storedSearchDone = !page.hasMore;
  }
  if (!model.exists()) state.modelSearchDone = true;
  while (!state.modelSearchDone && work > 0) {
    if (!state.pendingModel) {
      QVector<qlonglong> ids; bool tail = false;
      const auto error = model.matches(name, state.modelAfter, 1, ids, tail); --work;
      if (!error.isEmpty()) return error;
      if (ids.isEmpty()) { state.modelSearchDone = true; break; }
      state.pendingModel = ids.first(); state.pendingModelIsLast = !tail;
      state.modelEntityAfter.clear();
    }
    const auto page = catalog->modelWellPage(state.pendingModel, state.modelEntityAfter, qMax(1, qMin(work, 32)));
    --work;
    for (auto well : page.items) {
      --work; const auto error = model.overlay(well); if (!error.isEmpty()) return error;
      state.modelEntityAfter = well.id;
      if (DataCatalog::normalizeWellName(well.name) == key) state.candidates.insert(well.id);
      if (state.candidates.size() > 1) return QStringLiteral("well_ambiguous");
    }
    if (!page.hasMore) {
      state.modelAfter = state.pendingModel; state.pendingModel = 0;
      state.modelSearchDone = state.pendingModelIsLast;
    }
  }
  more = !state.storedSearchDone || !state.modelSearchDone;
  if (more) return {};
  if (state.candidates.isEmpty()) return QStringLiteral("well_not_found");
  state.target = *state.candidates.cbegin(); return {};
}

QJsonObject ProjectQueryCapability::Impl::wellDetails(QueryState &state, const QJsonObject &args,
                                                     int rows, int bytes, bool &more) {
  const auto reference = args.value(args.contains("well_id") ? "well_id" : "well_name");
  if (QJsonDocument(QJsonArray{reference}).toJson(QJsonDocument::Compact).size() > bytes / 4)
    return queryFailure("query_entry_too_large");
  WellProjection model(catalog->projectDir()); auto error = model.open();
  if (!error.isEmpty()) return queryFailure(error);
  if (!state.modelToken.isEmpty() && state.modelToken != model.token()) return queryFailure("model_revision_changed");
  state.modelToken = model.token();
  error = resolveWell(state, args, model, more);
  if (!error.isEmpty()) {
    auto failure = queryFailure(error);
    if (error == QLatin1String("well_not_found")) {
      QJsonArray known;
      const auto sample = catalog->entityPage(QStringLiteral("well"), {}, 10);
      for (auto entity : sample.items) {
        const auto invalid = model.overlay(entity); if (!invalid.isEmpty()) return queryFailure(invalid);
        known.append(entity.name.isEmpty() ? entity.id : entity.name);
        if (QJsonDocument(known).toJson(QJsonDocument::Compact).size() > bytes / 4) {known.removeLast(); break;}
      }
      failure.insert("known_wells", known);
      failure.insert("known_wells_complete", known.size() == sample.totalCount);
    }
    const auto invalid = model.verify(); return invalid.isEmpty() ? failure : queryFailure(invalid);
  }
  QJsonObject result{{"ok",true},{"code","ok"},{"contract","paleo.project.query.page.v1"},
    {"tool","paleo.query_project"},{"topic","well_details"}};
  if (more) {
    error = model.verify(); if (!error.isEmpty()) return queryFailure(error);
    result.insert("resolution", "searching"); result.insert("returnedCount", 0); result.insert("totalCount", QJsonValue::Null);
    return result;
  }
  auto well = catalog->storedEntityById(state.target); error = model.overlay(well);
  if (!error.isEmpty()) return queryFailure(error);
  const auto page = catalog->linkPage(state.target, args.value("role").toString(), state.after, rows);
  QJsonArray entries;
  for (int i = 0; i < page.items.size(); ++i) {
    const auto &link = page.items.at(i); const auto asset = catalog->assetById(link.assetId);
    QJsonObject entry{{"role",link.role},{"asset",queryDisplayName(asset)},{"asset_id",link.assetId},{"primary",link.isPrimary}};
    queryVersionCitation(entry, catalog, link.assetId);
    if (!appendWithinBudget(entries, entry, bytes, 1536)) {
      if (entries.isEmpty()) return queryFailure("query_entry_too_large");
      more = true; break;
    }
    state.after = page.itemKeys.at(i);
  }
  error = model.verify(); if (!error.isEmpty()) return queryFailure(error);
  more = more || page.hasMore;
  result.insert("well", well.name.isEmpty() ? well.id : well.name); result.insert("well_id", well.id);
  const auto ref = queryWellReference(well, state); if (ref.isEmpty()) return queryFailure("invalid_well_ref");
  result.insert("well_ref", ref);
  result.insert("links", entries); result.insert("count", page.totalCount); result.insert("returnedCount", entries.size());
  result.insert("totalCount", page.totalCount); return result;
}
}
