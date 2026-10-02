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
  m_rowsBySha.clear();
  m_entitySeqByPrefix.clear();
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
  indexEntitySeq(id); // WP2：前缀序号登记（nextEntityId O(1) 面）
  if (!type.isEmpty())
    m_entitiesByType[type].append(row);
}

// WP2："<prefix>-N" 拆解登记。取「最后一个 '-'」为界——与旧 nextEntityId
// 线性扫描（startsWith(prefix+'-') + 余段 toInt）对所有非空 prefix 的输入
// 同判定：余段非纯数字（含 '-')时 toInt 失败不入序号面；无 '-' 的 id 对任何
// 非空 prefix 查询都不命中（扫描版 startsWith 同样不命中）。唯一分歧是
// 空 prefix（"-5" 这类 id）：扫描版会抬高空前缀序号、索引版不登记——
// 现有调用方只有 "well"/"aux"，不可达（review 核验）。
void CatalogIndex::indexEntitySeq(const QString &id)
{
  const int dash = id.lastIndexOf(QLatin1Char('-'));
  if (dash <= 0)
    return; // 无前缀（或空前缀）不参与序号面
  bool ok = false;
  const int n = id.mid(dash + 1).toInt(&ok);
  if (!ok || n <= 0)
    return;
  const QString prefix = id.left(dash);
  const int cur = m_entitySeqByPrefix.value(prefix, 0);
  if (n > cur)
    m_entitySeqByPrefix.insert(prefix, n);
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
  if (!v.sha256.isEmpty())                    // WP2：sha 命中行集（升序）
    m_rowsBySha[v.sha256.toLower()].append(row);
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
  // 重建版本侧三表（<10k 版本亚毫秒级）。WP2：sha 行集随版本侧一并重建。
  m_versionRow.clear();
  m_versionsByAsset.clear();
  m_childrenByParent.clear();
  m_rowsBySha.clear();
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
  QHash<QString, QVector<int>> wantVersions, wantLinksE, wantLinksA, wantSha;
  for (int i = 0; i < versions.size(); ++i)
  {
    if (!versions.at(i).assetId.isEmpty())
      wantVersions[versions.at(i).assetId].append(i);
    if (!versions.at(i).sha256.isEmpty())
      wantSha[versions.at(i).sha256.toLower()].append(i);
  }
  for (int i = 0; i < links.size(); ++i)
  {
    if (!links.at(i).entityId.isEmpty())
      wantLinksE[links.at(i).entityId].append(i);
    if (!links.at(i).assetId.isEmpty())
      wantLinksA[links.at(i).assetId].append(i);
  }
  if (wantVersions != m_versionsByAsset)
    return fail(QStringLiteral("versionsByAsset"));
  if (wantSha != m_rowsBySha)
    return fail(QStringLiteral("rowsBySha"));
  if (wantLinksE != m_linksByEntity)
    return fail(QStringLiteral("linksByEntity"));
  if (wantLinksA != m_linksByAsset)
    return fail(QStringLiteral("linksByAsset"));
  // WP2：前缀序号面对账（线性重算每前缀最大 N）。
  QHash<QString, int> wantSeq;
  for (int i = 0; i < entities.size(); ++i)
  {
    const QString &id = entities.at(i).id;
    const int dash = id.lastIndexOf(QLatin1Char('-'));
    if (dash <= 0)
      continue;
    bool ok = false;
    const int n = id.mid(dash + 1).toInt(&ok);
    if (!ok || n <= 0)
      continue;
    const QString prefix = id.left(dash);
    if (n > wantSeq.value(prefix, 0))
      wantSeq.insert(prefix, n);
  }
  if (wantSeq != m_entitySeqByPrefix)
    return fail(QStringLiteral("entitySeqByPrefix"));
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
