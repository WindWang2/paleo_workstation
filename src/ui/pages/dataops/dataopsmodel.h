// 层：视图
// ui/pages/dataops/dataopsmodel — P3 数据管理页操作重构的核心值类型与
// 视图层装配。纯逻辑（无 QWidget 依赖），被 datalist/entitypanel/datapage
// 三个 TU include。
//
// P1（data-commands 2026-11-08）：四个 sidecar 存储对象（TagStore/
// AssetOverrideStore/EntityOverrideStore/RecycleBin 及其值类型）的可执行
// 核心已下沉数据层 services/dataopsstores.h——同命名空间同名类型经下方
// include 对既有消费面零改动可用；本文件保留视图侧行快照装配（AssetRowInfo/
// buildAssetRow，P2 查询所有权不动）与 OperationsHistory。
//
// 设计约束（docs/dataops/MODEL_MAP.md §5）：
//   · catalog 无实体更新/删除、资产删除/改型 API（datacatalog.h 只有
//     add*/attach/setLink*）——类型改写、实体名/坐标改写、软删走 sidecar
//     （.paleo/*.json，与 T28 undo vault 同一模式），写回服务层的正路登记
//     在 docs/dataops/GAPS.md；
//   · sidecar 路径推导与读写由 services/jsonsidecar.h 的 dataopsSidecar*
//     唯一负责；本文件只装配视图快照，不保留持久化转发入口。
#pragma once

#include <QDateTime>
#include <QDir>
#include <QFile>
#include <QFileInfo>
#include <QHash>
#include <QJsonArray>
#include <QJsonDocument>
#include <QJsonObject>
#include <QJsonParseError>
#include <QSet>
#include <QString>
#include <QStringList>
#include <QVariantMap>
#include <QVector>

#include "../../../catalog/datacatalog.h"
#include "../../../services/dataopsstores.h"
#include "../../../services/jsonsidecar.h"

namespace paleo::dataops
{

// ---- 资产行快照（过滤/排序/分组/导出的统一输入） ---------------------------
// 一次从 catalog + 各 sidecar store 装配；视图过滤不再回查 catalog。
struct AssetRowInfo
{
  QString assetId;
  QString displayName;
  QString fileName;      // 当前版本文件名（无版本回退 displayName）
  QString type;          // catalog 原型
  QString effectiveType; // 应用用户改写后的类型（过滤/显示口径）
  QString status;        // "RAW" | "DERIVED"（当前版本 stage）
  bool unresolved = false;     // 任一链接未决
  bool removed = false;        // 软删（可回收清单）
  QStringList roles;           // 去重排序后的链接角色
  QStringList entityNames;     // 已决实体名（无则空）
  QStringList tags;            // 用户标签
  qint64 sizeBytes = 0;        // 当前版本文件大小（受管/外链均 stat）
  QDateTime lastModified;      // 文件 mtime；stat 不到 = 无效
  int versionCount = 0;
  int currentVersionNo = 1;

  bool matchesStatus(const QString &s) const { return status == s; }
};

// ---- 行快照装配 ------------------------------------------------------------
// size/mtime 走 QFileInfo::stat——10k 资产 stat 是一次冷开销；调用方在刷新
// 管线里装配一次，过滤循环不再碰盘（D2.8 性能口径）。
inline AssetRowInfo buildAssetRow(DataCatalog *cat, const CatalogAsset &a,
                                  const TagStore &tags,
                                  const AssetOverrideStore &overrides,
                                  const RecycleBin &bin,
                                  const EntityOverrideStore &entityOverrides, bool statFile = true)
{
  AssetRowInfo row;
  row.assetId = a.id;
  row.displayName = a.displayName;
  row.type = a.type;
  const QString ov = overrides.overriddenType(a.id);
  row.effectiveType = ov.isEmpty() ? a.type : ov;
  row.removed = bin.isRemoved(a.id);
  row.tags = tags.tagsFor(a.id);

  const CatalogVersion v = cat->currentVersion(a.id);
  row.status = v.stage.isEmpty() ? QStringLiteral("RAW") : v.stage;
  row.fileName = v.fileName.isEmpty() ? a.displayName : v.fileName;
  row.currentVersionNo = v.versionNumber > 0 ? v.versionNumber : 1;
  row.versionCount = cat->versionsForAsset(a.id).size();
  if (!statFile)
    row.sizeBytes = -1; // 未读取文件大小，不能显示成零；表视/导出一律按空值呈现。
  if (statFile && !v.path.isEmpty())
  {
    // 外链 = 绝对路径直接 stat；受管 = DataCatalog::resolvedVersionPath
    //（含 symlink 祖先安全校验）拼工程目录。
    QString abs;
    if (v.managed)
    {
      const QString pd = paleo::projectDirForCatalog(cat);
      if (!pd.isEmpty())
        abs = DataCatalog::resolvedVersionPath(pd, v);
    }
    else
      abs = v.path;
    if (!abs.isEmpty())
    {
      const QFileInfo fi(abs);
      if (fi.exists())
      {
        row.sizeBytes = fi.size();
        row.lastModified = fi.lastModified();
      }
    }
  }

  for (const EntityAssetLink &l : cat->linksForAsset(a.id))
  {
    if (l.unresolved)
      row.unresolved = true;
    if (!row.roles.contains(l.role))
      row.roles.append(l.role);
    if (!l.unresolved && !l.entityId.isEmpty())
    {
      const CatalogEntity e = cat->entityById(l.entityId);
      const QString nm = entityOverrides.displayName(e);
      const QString shown = nm.isEmpty() ? (e.name.isEmpty() ? l.entityId : e.name) : nm;
      if (!row.entityNames.contains(shown))
        row.entityNames.append(shown);
    }
  }
  row.roles.sort();
  row.entityNames.sort();
  return row;
}


// ---- D4.10 最近操作历史（会话内环形缓冲；不落盘）---------------------------
class OperationsHistory
{
public:
  static constexpr int kCapacity = 100;
  void push(const QString &entry)
  {
    m_entries.prepend(entry);
    if (m_entries.size() > kCapacity)
      m_entries.removeLast();
  }
  QStringList entries() const { return m_entries; }
  void clear() { m_entries.clear(); }

private:
  QStringList m_entries;
};

} // namespace paleo::dataops
