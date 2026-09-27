// 层：数据
#pragma once
#include <QString>
#include <QStringList>
#include <QVector>

#include "datacatalog.h"
#include "roleregistry.h"

// catalog/ — 实体数据视图（docs/DATA_FABRIC_ADOPTION.md B 包；上游
// paleo_project catalog/entity_views.py 的角色槽模型裁剪移植）。纯查询
// 门面：只装配 catalog 内存中的实体/链接/版本，不读盘、不改 catalog、
// 不触碰上游的 working-copy / run / filesystem-stat 重面。
//
// RoleSlot 是「(实体, 角色)」展示槽：roleRegistry().forEntity(entityType)
// 枚举出的每个合法角色都占一槽——空角色也在（缺源可见性，词表驱动的
// UI 能显示「该角色暂无资产」而非假装角色不存在）。槽内分三桶：
//   · primary   —— 该槽的已决主关联（同角色至多一条是 addLink/attachLink/
//     setLinkPrimary 强制的结构不变量）；没有时 assetId 为空。
//   · members   —— 已决非主成员，按链接 ordinal 升序（well_log 等
//     ordered=true 角色的业务序；ordinal 全 0 的旧数据保持入库序）。
//   · unresolved —— entityId 指向本实体但 unresolved 的链接（上游歧义
//     链接携带候选实体 id，同一约定）。entityId 为空的未决链接不属于
//     任何实体——走 cat.unresolvedLinks() 全局集合，不在槽里凭空出现。
// 链接角色不在词表时合成兜底槽尾置（display 取角色名本身）——链接
// 不静默丢弃，与上游 _slots_for_entity 同一约定。
struct RoleSlot
{
  RoleDef def;                         // 词表定义；合成槽只有 role/entityTypes/display
  EntityAssetLink primary;             // 已决主关联；无则 assetId 为空
  QVector<EntityAssetLink> members;    // 已决非主成员，按 ordinal 升序
  QVector<EntityAssetLink> unresolved; // 指向本实体但未决的链接
};

// EntityView = 一个实体的「资产全貌 + 派生血缘诊断」截面：
//   · slots            —— 见上。
//   · derivedProducts  —— 以本实体已决资产版本为种子、经 parentVersionIds
//     反查可达的全部 DERIVED 版本（下游闭包 ∩ DERIVED；多跳穿透
//     INTERMEDIATE/OUTPUT，种子自身不算）。
//   · missingSources   —— 视图内版本（实体已决资产版本 ∪ 下游闭包）的
//     parentVersionIds 中指向不存在版本的 id，去重升序——悬空的血缘
//     引用如实列出（「声明了父版本但库里没有」是诚实的坏状态）。
//
// 成员名是 roleSlots 而非 slots——后者与 Qt 的 slots 宏（Q_SLOTS）冲突。
struct EntityView
{
  CatalogEntity entity;                    // id 为空 = 未知实体（如实空视图）
  QVector<RoleSlot> roleSlots;
  QVector<CatalogVersion> derivedProducts;
  QStringList missingSources;
};

// 组装 entityId 的实体视图。空/未知 entityId → 如实空视图（entity.id 为空、
// slots/derivedProducts/missingSources 皆空），不编造槽位不报错。
EntityView entityDataView(const DataCatalog &cat, const QString &entityId);
