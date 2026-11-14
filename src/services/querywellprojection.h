// 层：数据
#pragma once
#include "catalog/datacatalog.h"
#include <memory>

namespace paleo::query {
// Only fields actually returned by project queries; no tracks/curves/payloads.
class WellProjection {
public:
  explicit WellProjection(const QString &projectDir);
  ~WellProjection();
  QString open();
  QString token() const;
  QString verify() const;
  bool exists() const;
  QString overlay(CatalogEntity &entity) const;
  QString matches(const QString &name, qlonglong after, int limit,
                  QVector<qlonglong> &ids, bool &more) const;
private:
  struct Impl;
  std::unique_ptr<Impl> m;
};
}
