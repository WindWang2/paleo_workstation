// 层：数据
#include "wellfaciesrequest.h"
#include <QJsonArray>
#include <QSet>

namespace paleo::wellfacies {
bool UnitScope::valid() const {
  if (!unit.isValid() || unit.revision.isEmpty() || occurrenceKeys.size() > 32) return false;
  if(unit.aggregate?unit.subIndex!=0:unit.subIndex<1||unit.subIndex>4)return false;
  QSet<QString> keys;
  for (const auto &key : occurrenceKeys) {
    if (key.isEmpty() || key.toUtf8().size() > 256 || keys.contains(key)) return false;
    keys.insert(key);
  }
  return true;
}
QJsonObject unitScopeJson(const UnitScope &scope) {
  const auto &unit=scope.unit;
  return {{"schemeId",unit.schemeId},{"revision",unit.revision},{"intervalId",unit.intervalId},
    {"subIntervalId",unit.subIntervalId},{"aggregate",unit.aggregate},{"subIndex",unit.subIndex},
    {"occurrenceKeys",QJsonArray::fromStringList(scope.occurrenceKeys)}};
}
bool requestFromJson(const QJsonObject &json, Request &request, QString *code) {
  request = {};
  const auto fail = [&](const QString &reason) { if (code) *code = reason; return false; };
  const QSet<QString> known{"contract_version", "client_request_id", "endpoint_id", "model", "model_version", "well_ref",
                            "context", "unit_scope", "curve_ids", "lithology_track_id"};
  for (auto it = json.begin(); it != json.end(); ++it)
    if (!known.contains(it.key())) return fail(QStringLiteral("unknown_well_request_field"));
  if (json.value("contract_version") != QJsonValue(2)) return fail(QStringLiteral("unsupported_well_contract"));
  const auto wellJson=json.value("well_ref").toObject();
  const QSet<QString> wellFields{"contract_version","document_id","catalog_entity_id","model_store_id","model_store_identity",
    "model_generation","model_well_id","model_head","model_available"};
  for(auto it=wellJson.begin();it!=wellJson.end();++it)if(!wellFields.contains(it.key()))return fail("unknown_well_reference_field");
  if (!document::wellRefFromJson(wellJson, request.well, code)) return false;
  if(!request.well.modelAvailable||request.well.modelStoreId!=QLatin1String("area-model"))return fail("well_model_unavailable");
  QString contextError;
  const auto contextJson=json.value("context").toObject();
  const QSet<QString> contextFields{"documentId","modelStoreId","purpose","schemeId","schemeRevision","intervalId","subIntervalId","aggregate"};
  for(auto it=contextJson.begin();it!=contextJson.end();++it)if(!contextFields.contains(it.key()))return fail("unknown_well_context_field");
  for(const auto &field:{"documentId","modelStoreId","purpose","schemeId","schemeRevision","intervalId","subIntervalId"}) {
    const auto value=contextJson.value(QLatin1String(field));
    if(!value.isString()||value.toString().toUtf8().size()>256||value.toString().contains(QChar(0)))return fail("invalid_well_context");
  }
  if (!document::interpretationContextFromJson(contextJson, request.context, &contextError) ||
      request.context.purpose != document::RelationPurpose::FaciesAnalysis ||
      request.context.documentId != request.well.documentId ||
      request.context.modelStoreId != request.well.modelStoreId)
    return fail(QStringLiteral("invalid_well_context"));
  const auto scope = json.value("unit_scope").toObject();
  const QSet<QString> scopeFields{"schemeId","revision","intervalId","subIntervalId","aggregate","subIndex","occurrenceKeys"};
  for(auto it=scope.begin();it!=scope.end();++it)if(!scopeFields.contains(it.key()))return fail("unknown_unit_scope_field");
  if (!scope.value("aggregate").isBool()) return fail(QStringLiteral("invalid_unit_scope"));
  for(const auto &field:{"schemeId","revision","intervalId","subIntervalId"}) {
    const auto value=scope.value(QLatin1String(field));
    if(!value.isString()||value.toString().toUtf8().size()>256||value.toString().contains(QChar(0)))return fail("invalid_unit_scope");
  }
  const auto subIndex=scope.value("subIndex");
  if(!subIndex.isDouble()||subIndex.toDouble()!=subIndex.toInt(-1))return fail("invalid_unit_scope");
  request.scope.unit = strat::fromMap(scope.toVariantMap());
  if (scope.contains("occurrenceKeys") && !scope.value("occurrenceKeys").isArray())
    return fail(QStringLiteral("invalid_unit_scope"));
  for (const auto &key : scope.value("occurrenceKeys").toArray()) {
    if (!key.isString()) return fail(QStringLiteral("invalid_unit_scope"));
    request.scope.occurrenceKeys.append(key.toString());
  }
  const auto &unit = request.scope.unit;
  if (!request.scope.valid() || request.context.schemeId != unit.schemeId ||
      request.context.schemeRevision != unit.revision || request.context.intervalId != unit.intervalId ||
      request.context.subIntervalId != unit.subIntervalId || request.context.aggregate != unit.aggregate)
    return fail(QStringLiteral("unit_context_mismatch"));
  for (const auto &field : {"client_request_id", "model", "model_version"}) {
    const auto value = json.value(QLatin1String(field));
    if (!value.isString() || value.toString().isEmpty() || value.toString().toUtf8().size() > 256 ||
        value.toString().contains(QChar(0))) return fail(QStringLiteral("well_request_identity_required"));
  }
  request.clientRequestId = json.value("client_request_id").toString();
  request.endpointId=json.value("endpoint_id").toString();
  if(request.endpointId.size()!=64)return fail("endpoint_identity_required");
  for(const auto &ch:request.endpointId)if(!QStringLiteral("0123456789abcdef").contains(ch))return fail("endpoint_identity_required");
  request.modelId = json.value("model").toString();
  request.modelVersion = json.value("model_version").toString();
  if (json.contains("lithology_track_id")) {
    const auto value=json.value("lithology_track_id"); bool parsed=false;
    request.lithologyTrackId=value.toString().toLongLong(&parsed);
    if (!value.isString() || !parsed || request.lithologyTrackId<=0 ||
        QString::number(request.lithologyTrackId)!=value.toString()) return fail("invalid_lithology_ref");
  }
  if (json.contains("curve_ids") && !json.value("curve_ids").isObject()) return fail(QStringLiteral("invalid_curve_ref"));
  const auto curves = json.value("curve_ids").toObject();
  if (curves.size() > 64) return fail(QStringLiteral("curve_budget_exceeded"));
  for (auto it = curves.begin(); it != curves.end(); ++it) {
    bool parsed = false;
    const auto id = it.value().toString().toLongLong(&parsed);
    if (!it.value().isString() || !parsed || id <= 0 || QString::number(id) != it.value().toString() ||
        it.key().isEmpty() || it.key().toUtf8().size() > 128) return fail(QStringLiteral("invalid_curve_ref"));
    request.curveIds.insert(it.key(), id);
  }
  if (code) code->clear();
  return true;
}

bool declaredSemanticToken(const QString &value) {
  const auto token=value.trimmed();
  static const QSet<QString> unknown{"-","unknown","unspecified","undefined","n/a","na","?",
    QStringLiteral("未知"),QStringLiteral("未确认"),QStringLiteral("未声明")};
  return !token.isEmpty()&&!token.contains(QChar(0))&&!unknown.contains(token.toCaseFolded());
}
QString modelReadiness(const WellFaciesModel &model, const QString &id, const QString &version) {
  if(!model.contractReadinessCode.isEmpty())return model.contractReadinessCode;
  if (model.id.isEmpty() || model.id != id || model.id.contains(QChar(0)) || model.id.toUtf8().size()>256 ||
      model.version.isEmpty() || model.version.contains(QChar(0)) || model.version.toUtf8().size()>256 || model.version != version)
    return QStringLiteral("exact_model_unavailable");
  if (model.curves.isEmpty() || model.curves.size() > 64 || model.window < 1 || model.window > 4096 ||
      model.labels.isEmpty() || model.labels.size() > 512)
    return QStringLiteral("model_schema_unavailable");
  QSet<QString> curves, labels;
  for (const auto &curve : model.curves) {
    const auto key = curve.trimmed().toCaseFolded();
    if (key.isEmpty() || curve.trimmed()!=curve || curve.contains(QChar(0)) || key.toUtf8().size() > 128 || curves.contains(key))
      return QStringLiteral("invalid_model_features");
    curves.insert(key);
    const auto unit = model.curveUnits.value(curve).trimmed();
    if (!declaredSemanticToken(unit) || unit.toUtf8().size()>128)
      return QStringLiteral("model_feature_units_unavailable");
  }
  for (const auto &label : model.labels) {
    if (label.isEmpty() || label.trimmed()!=label || label.contains(QChar(0)) ||
        label.toUtf8().size() > 512 || labels.contains(label))
      return QStringLiteral("invalid_model_labels");
    labels.insert(label);
  }
  return {};
}

QJsonObject frozenModelJson(const WellFaciesModel &model) {
  QJsonObject units;
  for (auto it = model.curveUnits.begin(); it != model.curveUnits.end(); ++it) units.insert(it.key(), it.value());
  return {{"id", model.id}, {"version", model.version}, {"curves", QJsonArray::fromStringList(model.curves)},
          {"curveUnits", units}, {"window", model.window}, {"labels", QJsonArray::fromStringList(model.labels)}};
}
bool frozenModelFromJson(const QJsonObject &json,WellFaciesModel &model) {
  model={};model.id=json.value("id").toString();model.version=json.value("version").toString();
  model.window=json.value("window").toInt(-1);
  for(const auto &value:json.value("curves").toArray()){if(!value.isString())return false;model.curves.append(value.toString());}
  for(const auto &value:json.value("labels").toArray()){if(!value.isString())return false;model.labels.append(value.toString());}
  const auto units=json.value("curveUnits").toObject();
  for(auto it=units.begin();it!=units.end();++it){if(!it.value().isString())return false;model.curveUnits.insert(it.key(),it.value().toString());}
  return !model.id.isEmpty()&&modelReadiness(model,model.id,model.version).isEmpty()&&frozenModelJson(model)==json;
}
} // namespace paleo::wellfacies
