// 层：数据
#pragma once
#include <QJsonObject>
#include <QString>

namespace paleo::document {
// A query-selected well identity, not a historical numeric-value revision.
// modelStoreId is the logical slot; modelStoreIdentity is the SDK's durable store UUID.
struct WellRef {
  QString documentId, catalogEntityId, modelStoreId, modelStoreIdentity, modelGeneration;
  qlonglong modelWellId = 0, queryHead = -1;
  bool modelAvailable = false;
  bool valid() const;
  bool operator==(const WellRef &) const = default;
};
QJsonObject wellRefJson(const WellRef &ref);
bool wellRefFromJson(const QJsonObject &json, WellRef &ref, QString *code = nullptr);
} // namespace paleo::document
