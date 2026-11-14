// 层：数据
#include "catalogindex.h"

namespace {
template<class T> CatalogPage<T> page(const QMap<QString, T> &index, const QString &after, int limit) {
  CatalogPage<T> result; result.totalCount = index.size();
  const int size = qBound(1, limit, 256);
  auto it = after.isEmpty() ? index.cbegin() : index.upperBound(after);
  for (; it != index.cend() && result.items.size() < size; ++it) {
    ++result.rowsVisited; result.items.append(it.value()); result.itemKeys.append(it.key()); result.lastKey = it.key();
  }
  result.hasMore = it != index.cend();
  return result;
}
}

CatalogPage<int> CatalogIndex::entityPage(const QString &type, const QString &after, int limit) const {
  if (type.isEmpty()) return page(m_entityOrder, after, limit);
  if (type == QLatin1String("@well-like")) return page(m_wellLikeOrder, after, limit);
  const auto it = m_entitiesOrderedByType.constFind(type);
  return it == m_entitiesOrderedByType.cend() ? CatalogPage<int>{} : page(*it, after, limit);
}
CatalogPage<int> CatalogIndex::assetPage(const QString &type, const QString &after, int limit) const {
  if (type.isEmpty()) return page(m_assetOrder, after, limit);
  const auto it = m_assetsOrderedByType.constFind(type);
  return it == m_assetsOrderedByType.cend() ? CatalogPage<int>{} : page(*it, after, limit);
}
CatalogPage<int> CatalogIndex::linkPage(const QString &entityId, const QString &role, const QString &after, int limit) const {
  const auto it = m_linksOrdered.constFind(qMakePair(entityId, role));
  return it == m_linksOrdered.cend() ? CatalogPage<int>{} : page(*it, after, limit);
}
CatalogPage<QString> CatalogIndex::childrenPage(const QString &parent, const QString &after, int limit) const {
  const auto it = m_childrenOrdered.constFind(parent);
  return it == m_childrenOrdered.cend() ? CatalogPage<QString>{} : page(*it, after, limit);
}
QHash<QString, int> CatalogIndex::assetCountsByType() const {
  QHash<QString, int> out;
  for (auto it = m_assetsOrderedByType.cbegin(); it != m_assetsOrderedByType.cend(); ++it) out.insert(it.key(), it->size());
  return out;
}
CatalogPage<int> CatalogIndex::entityNamePage(const QString &name, const QString &after, int limit) const {
  const auto it = m_wellsByName.constFind(name);
  return it == m_wellsByName.cend() ? CatalogPage<int>{} : page(*it, after, limit);
}
CatalogPage<int> CatalogIndex::modelWellPage(qlonglong id, const QString &after, int limit) const {
  const auto it = m_wellsByModelId.constFind(id);
  return it == m_wellsByModelId.cend() ? CatalogPage<int>{} : page(*it, after, limit);
}
CatalogPage<int> CatalogIndex::assetNamePage(const QString &name, const QString &after, int limit) const {
  const auto it = m_assetsByName.constFind(name);
  return it == m_assetsByName.cend() ? CatalogPage<int>{} : page(*it, after, limit);
}
