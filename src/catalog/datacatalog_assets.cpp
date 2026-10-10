// 层：数据
#include "datacatalog.h"
#include "../metadata/storeerrors_internal.h"

#include <QSet>

#include <algorithm>

namespace
{
using paleo::store_detail::setError;
} // namespace

// 资产族（方向 99 拆分）：addAsset/assets/assetById/物理删除（方向 30 回收站
// 的 catalog 面）/受 "ast-N" id 分配。

bool DataCatalog::addAsset(const CatalogAsset &a, QString *error)
{
  if (!checkWriteThread("addAsset", error))
    return false;
  if (!ensureOpen(error))
    return false;
  if (a.id.isEmpty() || !assetById(a.id).id.isEmpty())
  {
    setError(error, QStringLiteral("asset id is empty or duplicate: %1").arg(a.id));
    return false;
  }
  // T17 同型：显式给的 "ast-N" 也推进序号——否则 nextAssetId() 回发已用 id。
  {
    bool ok = false;
    const int n = a.id.startsWith(QStringLiteral("ast-")) ? a.id.mid(4).toInt(&ok) : 0;
    if (ok && n > 0)
      m_assetSeq = qMax(m_assetSeq, n);
  }
  m_assets.append(a);
  m_idx.assetAdded(m_assets.size() - 1, a.id, a.type); // D5.2
  m_dirtyAssets.insert(a.id);
  if (save(error))
  {
    CatalogOp op;
    op.kind = CatalogOp::Kind::AddAsset;
    op.asset = a;
    recordOp(std::move(op));
    return true;
  }
  m_assets.removeLast();
  m_idx.rebuild(m_entities, m_assets, m_versions, m_links);
  return false;
}

QVector<CatalogAsset> DataCatalog::assets() const
{
  noteRead("assets");
  return m_assets;
}

CatalogAsset DataCatalog::assetById(const QString &id) const
{
  noteRead("assetById");
  const int row = m_idx.assetRow(id); // D5.1 O(1)
  return row >= 0 ? m_assets.at(row) : CatalogAsset();
}

// ---- 方向 30：物理删除资产（回收站「物理删除」的 catalog 面）----
bool DataCatalog::removeAsset(const QString &assetId, QString *error)
{
  return removeAssets({assetId}, error);
}
bool DataCatalog::removeAssets(const QStringList &assetIds, QString *error)
{
  if (!checkWriteThread("removeAssets", error))
    return false;
  if (!ensureOpen(error))
    return false;
  if (m_staging)
  {
    setError(error, QStringLiteral(
                        "staging copy does not support removeAsset (journal has no delete op)"));
    return false;
  }
  const QSet<QString> selected(assetIds.begin(), assetIds.end());
  if (selected.isEmpty()) return true;
  for (const QString &assetId : selected)
    if (assetById(assetId).id.isEmpty()) {
      setError(error, QStringLiteral("asset not found: %1").arg(assetId));
      return false;
    }
  // 血缘保护：待删版本被他资产的版本列为 parentVersionIds → 拒绝。
  // 同资产互引不拦（随资产一起删）。
  QVector<CatalogVersion> doomedVersions;
  for (const auto &v : m_versions) if (selected.contains(v.assetId)) doomedVersions.append(v);
  for (const CatalogVersion &v : doomedVersions)
    for (const QString &cid : m_idx.childVersionIds(v.id))
    {
      const int row = m_idx.versionRow(cid);
      if (row < 0 || selected.contains(m_versions.at(row).assetId))
        continue;
      setError(error, QStringLiteral("version %1 (v%2) is referenced as parent by "
                                     "asset %3 — delete the derived asset first")
                       .arg(v.id, QString::number(v.versionNumber),
                             m_versions.at(row).assetId));
      return false;
    }
  // 回滚快照：删除是罕见路径，O(N) 拷贝换 save 失败时的精确还原（与
  // 索引 rebuild 的「罕见路径花全量换正确性」同一纪律）。
  const QVector<CatalogAsset> prevAssets = m_assets;
  const QVector<CatalogVersion> prevVersions = m_versions;
  const QVector<EntityAssetLink> prevLinks = m_links;
  const CatalogIndex prevIdx = m_idx;
  const bool prevLinksFullRewrite = m_linksFullRewrite;
  // Compact once: interleaved bulk removals must not repeatedly shift tails.
  m_links.erase(std::remove_if(m_links.begin(), m_links.end(),
    [&](const auto &l) { return selected.contains(l.assetId); }), m_links.end());
  m_versions.erase(std::remove_if(m_versions.begin(), m_versions.end(),
    [&](const auto &v) { return selected.contains(v.assetId); }), m_versions.end());
  m_assets.erase(std::remove_if(m_assets.begin(), m_assets.end(),
    [&](const auto &a) { return selected.contains(a.id); }), m_assets.end());
  m_idx.rebuild(m_entities, m_assets, m_versions, m_links);
  const auto previousRemovedAssets = m_removedAssets;
  const auto previousRemovedVersions = m_removedVersions;
  m_removedAssets.unite(selected);
  for (const CatalogVersion &v : doomedVersions)
    m_removedVersions.insert(v.id);
  m_linksFullRewrite = true;
  if (save(error))
  {
    ++m_mutationSeq; // 同 mutator 口径：成功变更推进基线序号（无 journal op）
    return true;
  }
  m_assets = prevAssets;
  m_versions = prevVersions;
  m_links = prevLinks;
  m_idx = prevIdx;
  m_removedAssets = previousRemovedAssets;
  m_removedVersions = previousRemovedVersions;
  m_linksFullRewrite = prevLinksFullRewrite;
  return false;
}

QString DataCatalog::nextAssetId()
{
  noteRead("nextAssetId");
  ++m_mutationSeq; // 分配 id 也是 owner 侧状态变化（提交基线要看见）
  QString id;
  do { id = QStringLiteral("ast-%1").arg(++m_assetSeq); }
  while (!assetById(id).id.isEmpty());
  return id;
}
