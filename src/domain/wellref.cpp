// 层：数据
#include "wellref.h"

namespace paleo::document {
namespace {
bool identity(const QString &value) {
  return !value.isEmpty() && value.toUtf8().size() <= 256 && !value.contains(QChar(0));
}
bool decimal(const QJsonValue &value, qlonglong &number, bool zero) {
  if (!value.isString()) return false;
  bool parsed = false;
  number = value.toString().toLongLong(&parsed);
  return parsed && number >= (zero ? 0 : 1) && QString::number(number) == value.toString();
}
}
bool WellRef::valid() const {
  if (!identity(documentId) || !identity(catalogEntityId) || modelStoreId != QLatin1String("area-model"))
    return false;
  return !modelAvailable || (modelWellId > 0 && queryHead >= 0 &&
      identity(modelStoreIdentity) && identity(modelGeneration));
}
QJsonObject wellRefJson(const WellRef &ref) {
  return {{"contract_version", 1}, {"document_id", ref.documentId},
      {"catalog_entity_id", ref.catalogEntityId}, {"model_store_id", ref.modelStoreId},
      {"model_store_identity", ref.modelStoreIdentity}, {"model_generation", ref.modelGeneration},
      {"model_well_id", ref.modelWellId > 0 ? QString::number(ref.modelWellId) : QString()},
      {"model_head", ref.queryHead >= 0 ? QString::number(ref.queryHead) : QString()},
      {"model_available", ref.modelAvailable}};
}
bool wellRefFromJson(const QJsonObject &json, WellRef &ref, QString *code) {
  ref = {};
  const auto fail = [&] { if (code) *code = QStringLiteral("invalid_well_ref"); return false; };
  if (json.value("contract_version") != QJsonValue(1) || !json.value("model_available").isBool())
    return fail();
  for (const auto &field : {"document_id", "catalog_entity_id", "model_store_id"})
    if (!json.value(QLatin1String(field)).isString()) return fail();
  ref.documentId = json.value("document_id").toString();
  ref.catalogEntityId = json.value("catalog_entity_id").toString();
  ref.modelStoreId = json.value("model_store_id").toString();
  ref.modelAvailable = json.value("model_available").toBool();
  ref.modelStoreIdentity = json.value("model_store_identity").toString();
  ref.modelGeneration = json.value("model_generation").toString();
  if (ref.modelAvailable && (!decimal(json.value("model_well_id"), ref.modelWellId, false) ||
                             !decimal(json.value("model_head"), ref.queryHead, true))) return fail();
  if (!ref.valid()) return fail();
  if (code) code->clear();
  return true;
}
} // namespace paleo::document
