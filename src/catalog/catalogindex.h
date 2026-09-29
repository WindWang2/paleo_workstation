// 层：数据
#pragma once
#include <QHash>
#include <QPair>
#include <QString>
#include <QStringList>
#include <QVector>

struct CatalogEntity;
struct CatalogAsset;
struct CatalogVersion;
struct EntityAssetLink;

// catalog/ — 实体↔资产↔版本三级邻接索引（wave/io-perf-cache D5.1/D5.4）。
//
// DataCatalog 的四张 QVector 是唯一事实源；本索引是纯派生加速结构：
//   id → 行号          （entityById/assetById/versionById O(1)）
//   assetId → 版本行集 （versionsForAsset/currentVersion 不再全表扫）
//   entityId → 链接行集（linksForEntity）
//   assetId → 链接行集 （linksForAsset）
//   实体类型 → 行集/计数（entities(type)/entityCounts O(1)，D5.3）
//   versionId → 子版本集（downstreamClosure 的邻接表，D5.4）
//
// 维护口径（D5.2 增量失效）：addXxx 单点增量；链接标志批量变（attach/
// unresolved/primary）走 linksMutated 整表重挂（只有链接表重索引，实体/版本
// 索引不动）；回滚路径（save 失败还原快照）走 rebuild——罕见路径花全量换
// 正确性。一致性可由 verifyAgainst() 全量对账（测试/诊断面）。
class CatalogIndex
{
  public:
    void clear();

    // 全量重建（open 装载完 / 回滚后）。
    void rebuild(const QVector<CatalogEntity> &entities,
                 const QVector<CatalogAsset> &assets,
                 const QVector<CatalogVersion> &versions,
                 const QVector<EntityAssetLink> &links);

    // ---- 增量维护 ----
    void entityAdded(int row, const QString &id, const QString &type);
    void assetAdded(int row, const QString &id, const QString &type);
    void versionAdded(int row, const CatalogVersion &v);
    void linkAdded(int row, const EntityAssetLink &l);
    // 链接表被就地改写（attachLink/setLinkUnresolved/setLinkPrimary/addLink 的
    // 旧主关联降级）——整表重挂（行序不变）。
    void linksMutated(const QVector<EntityAssetLink> &links);
    // 版本表被就地改写（stale 标记——不影响索引键，但保险起见提供）。
    void versionsMutated(const QVector<CatalogVersion> &versions);

    // ---- O(1) 查询（-1 = 无；行集升序）----
    int entityRow(const QString &id) const { return m_entityRow.value(id, -1); }
    int assetRow(const QString &id) const { return m_assetRow.value(id, -1); }
    int versionRow(const QString &id) const { return m_versionRow.value(id, -1); }
    QVector<int> versionRowsForAsset(const QString &assetId) const
    {
      return m_versionsByAsset.value(assetId);
    }
    QVector<int> linkRowsForEntity(const QString &entityId) const
    {
      return m_linksByEntity.value(entityId);
    }
    QVector<int> linkRowsForAsset(const QString &assetId) const
    {
      return m_linksByAsset.value(assetId);
    }
    QVector<int> entityRowsByType(const QString &type) const
    {
      return m_entitiesByType.value(type);
    }
    // D5.4：parentVersionIds 反查邻接（row 升序入边）。
    QStringList childVersionIds(const QString &versionId) const
    {
      return m_childrenByParent.value(versionId);
    }

    // D5.3 类型计数（不再全量算）。
    int entityCountByType(const QString &type) const
    {
      return m_entitiesByType.value(type).size();
    }
    QHash<QString, int> entityCountsByType() const;
    int linkCount() const { return m_linkCount; }

    // 对账：索引 vs 四表线性扫描（测试/诊断；true = 完全一致）。
    bool verifyAgainst(const QVector<CatalogEntity> &entities,
                       const QVector<CatalogAsset> &assets,
                       const QVector<CatalogVersion> &versions,
                       const QVector<EntityAssetLink> &links,
                       QString *mismatch = nullptr) const;

  private:
    void indexLinkRow(int row, const EntityAssetLink &l);

    QHash<QString, int> m_entityRow;
    QHash<QString, int> m_assetRow;
    QHash<QString, int> m_versionRow;
    QHash<QString, QVector<int>> m_versionsByAsset;
    QHash<QString, QVector<int>> m_linksByEntity;
    QHash<QString, QVector<int>> m_linksByAsset;
    QHash<QString, QVector<int>> m_entitiesByType;
    QHash<QString, QStringList> m_childrenByParent;
    QHash<QString, int> m_entityCount; // 冗余计数（entitiesByType.size() 即是——
                                       // 保字段省一次哈希）
    int m_linkCount = 0;
};
