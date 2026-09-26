#include "entityview.h"

#include <QHash>
#include <QSet>

EntityView entityDataView(const DataCatalog &cat, const QString &entityId)
{
  EntityView view;
  if (entityId.isEmpty())
    return view;
  view.entity = cat.entityById(entityId);
  if (view.entity.id.isEmpty())
    return EntityView(); // 未知实体 → 如实空视图（不编造槽位）

  // ---- 角色槽：词表枚举先行，空角色也占位 ----
  QHash<QString, int> slotOf; // role → roleSlots 下标；同名定义只取首个（词表序）
  for (const RoleDef &def : cat.roleRegistry().forEntity(view.entity.entityType))
  {
    if (slotOf.contains(def.role))
      continue;
    RoleSlot slot;
    slot.def = def;
    slotOf.insert(def.role, view.roleSlots.size());
    view.roleSlots.append(slot);
  }

  // ---- 链接分桶：linksForEntity 已按 ordinal 排好，成员桶自然有序 ----
  QStringList assetIds; // 已决链接的资产（未决资产不算本实体数据源）
  for (const EntityAssetLink &l : cat.linksForEntity(entityId))
  {
    int si = slotOf.value(l.role, -1);
    if (si < 0)
    {
      // 未知角色（外来词表/未登记的工程自定义）：合成槽尾置，链接不丢。
      RoleSlot slot;
      slot.def.role = l.role;
      slot.def.entityTypes = QStringList{view.entity.entityType};
      slot.def.display = l.role;
      si = view.roleSlots.size();
      slotOf.insert(l.role, si);
      view.roleSlots.append(slot);
    }
    RoleSlot &slot = view.roleSlots[si];
    if (l.unresolved)
    {
      slot.unresolved.append(l);
      continue;
    }
    if (!assetIds.contains(l.assetId))
      assetIds.append(l.assetId);
    if (l.isPrimary && slot.primary.assetId.isEmpty())
      slot.primary = l;
    else
      slot.members.append(l); // 主关联违例时的多余主链接也如实落成员桶
  }

  // ---- 派生产物：实体已决资产的全部版本为种子做下游闭包 ----
  // 种子序确定性：assetIds 入库序 × versionsForAsset 表序（QStringList
  // contains 线性去重，目录规模下足够）。
  QStringList seedIds;
  for (const QString &assetId : assetIds)
    for (const CatalogVersion &v : cat.versionsForAsset(assetId))
      if (!seedIds.contains(v.id))
        seedIds.append(v.id);

  QVector<CatalogVersion> downstreamAll; // 闭包全阶段，missingSources 也要扫
  {
    QSet<QString> seen;
    for (const QString &seedId : seedIds)
      for (const CatalogVersion &d : cat.downstreamClosure(seedId))
        if (!seen.contains(d.id))
        {
          seen.insert(d.id);
          downstreamAll.append(d);
          if (d.stage == QLatin1String("DERIVED"))
            view.derivedProducts.append(d);
        }
  }

  // ---- 缺失源：视图内版本（实体资产版本 ∪ 下游闭包）指向不存在版本的
  //      parentVersionIds，去重排序后如实列出 ----
  QSet<QString> missing;
  const auto probeParents = [&cat, &missing](const CatalogVersion &v) {
    for (const QString &pid : v.parentVersionIds)
      if (!pid.isEmpty() && cat.versionById(pid).id.isEmpty())
        missing.insert(pid);
  };
  for (const QString &assetId : assetIds)
    for (const CatalogVersion &v : cat.versionsForAsset(assetId))
      probeParents(v);
  for (const CatalogVersion &d : downstreamAll)
    probeParents(d);
  view.missingSources = missing.values();
  view.missingSources.sort();
  return view;
}
