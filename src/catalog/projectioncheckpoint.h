// 层：数据
#pragma once
#include <QJsonObject>
#include <QString>

struct CatalogProjectionCheckpoint {
  QString storeId, generation;
  qint64 sequence = -1;
  bool valid() const {return !storeId.isEmpty() && !generation.isEmpty() && sequence>=0;}
  bool operator==(const CatalogProjectionCheckpoint &) const = default;
  QJsonObject toJson() const;
  static bool fromJson(const QJsonObject &json,CatalogProjectionCheckpoint &result);
};
bool validProjectionCheckpoints(const QJsonObject &json);
