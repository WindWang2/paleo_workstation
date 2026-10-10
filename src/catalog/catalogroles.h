// 层：数据
#pragma once
#include <QString>
#include <QVector>

// catalog/ — 链接角色的树/AI 消费面词表（方向 92 单源化）。
//
// 契约：EntityAssetLink.role 的「分组族 / 树排序序 / 用户面显示名」唯一权威。
// 数据树（datalist_tree）的井节点角色序、AI 工程查询工具（domaintools）的
// 角色清单描述、以及未来任何按角色分组/排序的消费方，一律查本表——不再各自
// 手写清单。与 RoleRegistry（roleregistry.h）的分工：RoleRegistry 是工程作用域
// 词表描述面（project_area.json 可覆盖，管准入描述/成员序语义），本表是编译期
// 常量（管树序/分组/显示名）；工程自定义角色不在本表内，树侧落「未分组」
// 可见桶（不静默藏匿）。
//
// 纪律：**新增角色先入表**——新链接角色合入时必须在 builtinCatalogRoles()
// 加一行（带分组/序/显示名），词表外角色不进树角色序也不进 AI 清单。
// 排序序全表唯一井跨族可比（well 段 0-10、seismic 段 20+、misc 段 30+，
// 段间留空隙便于插入），树排序直接取值；词表外角色统一 kUngroupedRoleOrder
// （恒大于全部表内序）落未分组桶。
//
// 显示名口径：以树面既渲染文案为准（tops=井分层、core=岩心照片，与
// RoleRegistry.display 的 分层顶/岩心 存在历史分叉——方向 92 以树面为准，
// 等价重构不改用户可见文本）；树未渲染的角色沿用 RoleRegistry 显示名。

// 词表外角色的分组键（未分组桶）。
inline constexpr const char *kUngroupedRoleGroup = "ungrouped";
// 词表外角色的树排序序——恒大于全部表内序（兜底桶收尾）。
inline constexpr int kUngroupedRoleOrder = 1000;

struct CatalogRoleInfo
{
  QString role;    // 链接角色名（EntityAssetLink.role）
  QString group;   // 分组族键（catalogRoleGroups() 之一）
  int order = 0;   // 树排序序（全表唯一；井节点子序按它升序）
  QString display; // 用户面显示名（中文，树/AI 共用）
};

struct CatalogRoleGroup
{
  QString key;     // well | seismic | mapproduct | misc
  QString display; // 分组显示名（AI 描述里的族名）
};

// 常量词表（表序即井段→地震段→misc 段；同名跨族角色井段在前，与
// RoleRegistry::find 的裸查约定一致）。
const QVector<CatalogRoleInfo> &catalogRoles();
// 分组族表。mapproduct 族当前无链接角色（成果图件组按资产类型归集，不经
// 链接角色）——键预留：未来产物挂接角色入该族，AI 清单生成自动带出。
const QVector<CatalogRoleGroup> &catalogRoleGroups();

// 裸角色查询（表序首个定义；词表外回 nullptr）。
const CatalogRoleInfo *catalogRoleInfo(const QString &role);
// 便捷查询面：词表外 → kUngroupedRoleGroup / kUngroupedRoleOrder / 原样返回。
QString catalogRoleGroup(const QString &role);
int catalogRoleOrder(const QString &role);
QString catalogRoleDisplay(const QString &role);
// AI 工具描述用的分组角色清单：按族表序输出「族名：role1/role2/…」，空族
// 跳过；词表外角色不出现（它们走树的未分组桶，不进 AI 词面）。
QString catalogRoleListForAi();
