#pragma once
#include <QString>
#include <QStringList>
#include <QVector>

class QJsonObject;

// catalog/ — 工程作用域角色词表（docs/DATA_FABRIC_ADOPTION.md A 包；
// 上游 paleo_project libs/data_suite role_registry 的 Qt 移植裁剪）。
// 词表是「实体→资产」链接角色的唯一权威：EntityAssetLink.role 仍是自由
// 字符串，词表负责描述它——允许挂哪些实体类型、成员数上限、是否 ordinal
// 有序、UI 中文名与建议阶段。只做查询面不做准入闸：未知角色的老/新文档
// 照常装载 round-trip，addLink 不查表（冲突检测与实体视图归后续 B 包）。
//
// 同名角色可跨词表复用（interpretation / other 同时属于 well 与
// seismic_survey），按 (实体类型, 角色) 定位才精确；裸角色 find() 回表序
// 首个定义——内置表 well 段在前，与上游「裸查命中 well 词表」约定一致。
// maxCount 只表达成员数上限（0=不限/0..N，1=单成员/0..1）；上游
// primary_policy=required_single 不映射进它——「同一角色一条主关联」的
// 不变量已由 addLink/attachLink/setLinkPrimary 在结构上强制，词表不复述。

struct RoleDef
{
  QString role;            // well_head | well_log | trajectory | tops |
                           // time_depth | seismic_volume | horizon | …
  QStringList entityTypes; // 允许挂接的实体类型
  int maxCount = 0;        // 0 = 不限（0..N）；1 = 单成员（0..1）
  bool ordered = false;    // 同角色多成员需 ordinal 排序
  QString display;         // UI 显示名（中文）
  QString stageDefault;    // 建议阶段（RAW/DERIVED/…）
  QString description;
};

class RoleRegistry
{
  public:
    // 内置词表（不读盘）：well 9 角色 + seismic_survey 7 角色，表序即词表
    // 序（"other" 收尾；同名跨表角色 well 段在前）。
    static RoleRegistry defaults();
    // 工程自定义覆盖：以 defaults() 为底逐键应用，键 = 角色名，值须为对象
    //（非对象跳过）。定义内写了 entity_types → 只补 (role, entityTypes)
    // 精确同域的定义；不写 → 补该角色名下全部定义（省略字段保留原值）。
    // 无匹配定义 → 追加为新角色（词表序尾部）。
    static RoleRegistry fromJson(const QJsonObject &roles);
    // 裸角色查询回表序首个定义；未知角色回 nullptr。
    const RoleDef *find(const QString &role) const;
    // 该实体类型可挂的角色定义（含表序）；无词表的类型/空类型如实回空集。
    QVector<RoleDef> forEntity(const QString &entityType) const;
    bool isKnown(const QString &role) const;

  private:
    QVector<RoleDef> m_defs; // 构造后只读——find() 指针在本对象生命周期内稳定
};
