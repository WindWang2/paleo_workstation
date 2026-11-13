// 层：数据
#include "datacatalog.h"

namespace {
template<class T> CatalogPage<T> values(const CatalogPage<int> &page, const QVector<T> &table) {
  CatalogPage<T> result;
  result.lastKey = page.lastKey; result.totalCount = page.totalCount;
  result.itemKeys = page.itemKeys;
  result.hasMore = page.hasMore; result.rowsVisited = page.rowsVisited;
  for (const int row : page.items) result.items.append(table.at(row));
  return result;
}
}
CatalogPage<CatalogEntity> DataCatalog::entityPage(const QString &type, const QString &after, int limit) const {
  if (!noteRead("entityPage")) return {};
  return values(m_idx.entityPage(type, after, limit), m_entities);
}
CatalogPage<CatalogAsset> DataCatalog::assetPage(const QString &type, const QString &after, int limit) const {
  if (!noteRead("assetPage")) return {};
  return values(m_idx.assetPage(type, after, limit), m_assets);
}
CatalogPage<EntityAssetLink> DataCatalog::linkPage(const QString &id, const QString &role, const QString &after, int limit) const {
  if (!noteRead("linkPage")) return {};
  return values(m_idx.linkPage(id, role, after, limit), m_links);
}
CatalogPage<QString> DataCatalog::versionParentsPage(const QString &id, const QString &after, int limit) const {
  CatalogPage<QString> result;
  if (!noteRead("versionParentsPage")) return result;
  const int row = m_idx.versionRow(id);
  if (row < 0) return result;
  const auto &parents = m_versions.at(row).parentVersionIds;
  bool valid = after.isEmpty(); const auto start = after.isEmpty() ? 0 : after.toLongLong(&valid);
  if (!valid || start < 0 || start > parents.size()) return result;
  result.totalCount = parents.size(); const auto end = qMin<qsizetype>(parents.size(), start + qBound(1, limit, 256));
  for (qsizetype i = start; i < end; ++i) {
    result.items.append(parents.at(i)); result.itemKeys.append(QString::number(i + 1)); ++result.rowsVisited;
  }
  result.lastKey = QString::number(end); result.hasMore = end < parents.size(); return result;
}
CatalogPage<QString> DataCatalog::versionChildrenPage(const QString &id, const QString &after, int limit) const {
  return noteRead("versionChildrenPage") ? m_idx.childrenPage(id, after, limit) : CatalogPage<QString>{};
}
QHash<QString, int> DataCatalog::assetCountsByType() const {
  return noteRead("assetCountsByType") ? m_idx.assetCountsByType() : QHash<QString, int>{};
}
CatalogPage<CatalogEntity> DataCatalog::wellNamePage(const QString &name, const QString &after, int limit) const {
  return noteRead("wellNamePage") ? values(m_idx.entityNamePage(name, after, limit), m_entities) : CatalogPage<CatalogEntity>{};
}
CatalogPage<CatalogEntity> DataCatalog::modelWellPage(qlonglong id, const QString &after, int limit) const {
  return noteRead("modelWellPage") ? values(m_idx.modelWellPage(id, after, limit), m_entities) : CatalogPage<CatalogEntity>{};
}
CatalogPage<CatalogAsset> DataCatalog::assetNamePage(const QString &name, const QString &after, int limit) const {
  return noteRead("assetNamePage") ? values(m_idx.assetNamePage(name, after, limit), m_assets) : CatalogPage<CatalogAsset>{};
}
