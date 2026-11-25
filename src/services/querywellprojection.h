// 层：数据
#pragma once
#include "catalog/datacatalog.h"
#include <QJsonArray>
#include <memory>

namespace paleo::document { struct WellRef; }
namespace paleo::query {
bool currentWellboreMapping(const DataCatalog &catalog, const document::WellRef &well);
bool currentWellboreLas(const DataCatalog &catalog, const document::WellRef &well,
                       const CatalogVersion &version, qlonglong sourceId);
// Only fields actually returned by project queries; no tracks/curves/payloads.
class WellProjection {
public:
  explicit WellProjection(const QString &projectDir);
  ~WellProjection();
  QString open();
  QString token() const;
  QString verify() const;
  bool exists() const;
  bool matchesGeneration(const CatalogProjectionCheckpoint &checkpoint, const QString &generation) const;
  QJsonObject sourceBore(qlonglong wellId, const CatalogVersion &version) const;
  QString overlay(CatalogEntity &entity) const;
  QString borePage(qlonglong wellId, const QString &after, int limit, QJsonArray &items, bool &more) const;
  QString matches(const QString &name, qlonglong after, int limit,
                  QVector<qlonglong> &ids, bool &more) const;
private:
  struct Impl;
  std::unique_ptr<Impl> m;
};
}
