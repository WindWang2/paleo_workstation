// 层：数据
#include "catalogindex.h"

#include "datacatalog.h" // CatalogEntity/… 完整定义（头内只用前置声明防环）

void CatalogIndex::clear()
{
  m_entityRow.clear();
  m_assetRow.clear();
  m_versionRow.clear();
  m_versionsByAsset.clear();
  m_linksByEntity.clear();
  m_linksByAsset.clear();
  m_entitiesByType.clear();
  m_childrenByParent.clear();
  m_entityCount.clear();
  m_linkCount = 0;
}

void CatalogIndex::rebuild(const QVector<CatalogEntity> &entities,
                           const QVector<CatalogAsset> &assets,
                           const QVector<CatalogVersion> &versions,
                           const QVector<EntityAssetLink> &links)
{
  clear();
  for (int i = 0; i < entities.size(); ++i)
    entityAdded(i, entities.at(i).id, entities.at(i).entityType);
  for (int i = 0; i < assets.size(); ++i)
    assetAdded(i, assets.at(i).id, assets.at(i).type);
  for (int i = 0; i < versions.size(); ++i)
    versionAdded(i, versions.at(i));
  for (int i = 0; i < links.size(); ++i)
    linkAdded(i, links.at(i));
}

void CatalogIndex::entityAdded(int row, const QString &id, const QString &type)
{
  if (id.isEmpty())
    return;
  m_entityRow.insert(id, row);
  if (!type.isEmpty())
    m_entitiesByType[type].append(row);
}

void CatalogIndex::assetAdded(int row, const QString &id, const QString &type)
{
  Q_UNUSED(type);
  if (id.isEmpty())
    return;
  m_assetRow.insert(id, row);
}

void CatalogIndex::versionAdded(int row, const CatalogVersion &v)
{
  if (v.id.isEmpty())
    return;
  m_versionRow.insert(v.id, row);
  if (!v.assetId.isEmpty())
    m_versionsByAsset[v.assetId].append(row); // 行号升序（追加语义）
  for (const QString &p : v.parentVersionIds)
    if (!p.isEmpty())
      m_childrenByParent[p].append(v.id); // 行序 = 入边发现序（BFS 确定性）
}

void CatalogIndex::indexLinkRow(int row, const EntityAssetLink &l)
{
  if (!l.entityId.isEmpty())
    m_linksByEntity[l.entityId].append(row);
  if (!l.assetId.isEmpty())
    m_linksByAsset[l.assetId].append(row);
}

void CatalogIndex::linkAdded(int row, const EntityAssetLink &l)
{
  indexLinkRow(row, l);
  m_linkCount = row + 1 > m_linkCount ? row + 1 : m_linkCount;
}

void CatalogIndex::linksMutated(const QVector<EntityAssetLink> &links)
{
  // 链接行序不变，只重挂 entity/asset 邻接。
  m_linksByEntity.clear();
  m_linksByAsset.clear();
  m_linkCount = links.size();
  for (int i = 0; i < links.size(); ++i)
    indexLinkRow(i, links.at(i));
}

void CatalogIndex::versionsMutated(const QVector<CatalogVersion> &versions)
{
  // stale/extra 就地改写不动索引键（id/assetId/parents 不变）；保守起见
  // 重建版本侧三表（<10k 版本亚毫秒级）。
  m_versionRow.clear();
  m_versionsByAsset.clear();
  m_childrenByParent.clear();
  for (int i = 0; i < versions.size(); ++i)
    versionAdded(i, versions.at(i));
}

QHash<QString, int> CatalogIndex::entityCountsByType() const
{
  QHash<QString, int> out;
  for (auto it = m_entitiesByType.constBegin(); it != m_entitiesByType.constEnd(); ++it)
    out.insert(it.key(), it.value().size());
  return out;
}

bool CatalogIndex::verifyAgainst(const QVector<CatalogEntity> &entities,
                                 const QVector<CatalogAsset> &assets,
                                 const QVector<CatalogVersion> &versions,
                                 const QVector<EntityAssetLink> &links,
                                 QString *mismatch) const
{
  const auto fail = [mismatch](const QString &what) {
    if (mismatch)
      *mismatch = what;
    return false;
  };
  // 行号表对账。
  for (int i = 0; i < entities.size(); ++i)
    if (entityRow(entities.at(i).id) != i && entityRow(entities.at(i).id) >= 0)
      return fail(QStringLiteral("entity row %1").arg(entities.at(i).id));
  for (int i = 0; i < assets.size(); ++i)
    if (assetRow(assets.at(i).id) != i && assetRow(assets.at(i).id) >= 0)
      return fail(QStringLiteral("asset row %1").arg(assets.at(i).id));
  for (int i = 0; i < versions.size(); ++i)
    if (versionRow(versions.at(i).id) != i && versionRow(versions.at(i).id) >= 0)
      return fail(QStringLiteral("version row %1").arg(versions.at(i).id));
  // 邻接表对账（线性重算 vs 索引）。
  QHash<QString, QVector<int>> wantVersions, wantLinksE, wantLinksA;
  for (int i = 0; i < versions.size(); ++i)
    if (!versions.at(i).assetId.isEmpty())
      wantVersions[versions.at(i).assetId].append(i);
  for (int i = 0; i < links.size(); ++i)
  {
    if (!links.at(i).entityId.isEmpty())
      wantLinksE[links.at(i).entityId].append(i);
    if (!links.at(i).assetId.isEmpty())
      wantLinksA[links.at(i).assetId].append(i);
  }
  if (wantVersions != m_versionsByAsset)
    return fail(QStringLiteral("versionsByAsset"));
  if (wantLinksE != m_linksByEntity)
    return fail(QStringLiteral("linksByEntity"));
  if (wantLinksA != m_linksByAsset)
    return fail(QStringLiteral("linksByAsset"));
  for (int i = 0; i < versions.size(); ++i)
    for (const QString &p : versions.at(i).parentVersionIds)
    {
      const QStringList want = m_childrenByParent.value(p);
      if (!want.contains(versions.at(i).id))
        return fail(QStringLiteral("childrenByParent %1").arg(p));
    }
  if (m_linkCount != links.size())
    return fail(QStringLiteral("linkCount"));
  return true;
}
