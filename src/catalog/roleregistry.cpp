// 层：数据
#include "roleregistry.h"

#include <QJsonArray>
#include <QJsonObject>

namespace
{
  // 内置词表（上游 roles.py _WELL_ROLE_DEFS / _SURVEY_ROLE_DEFS 逐字段移植，
  // 去掉 format_hints——文件格式推断是 C 包 IngestPlan 的事，本包不消费）。
  // 表序即词表序：well 段在前、"other" 收尾；maxCount 全 0（上游成员数
  // 一律 0..N，required_single 是主关联策略而非成员上限，见头注释）。
  QVector<RoleDef> builtinTable()
  {
    QVector<RoleDef> t;
    const auto add = [&t](const QString &role, const QString &entityType,
                          bool ordered, const QString &display,
                          const QString &description) {
      t.append(RoleDef{role, {entityType}, 0, ordered, display,
                       QStringLiteral("RAW"), description});
    };

    add(QStringLiteral("well_head"), QStringLiteral("well"), false,
        QStringLiteral("井身/井位"),
        QStringLiteral("井位/井史主记录（SMI DAT / XML 交付）；多文件时一个 primary。"));
    add(QStringLiteral("well_log"), QStringLiteral("well"), true,
        QStringLiteral("测井曲线"),
        QStringLiteral("一口井可有多个测井文件（LAS/DLIS…）；ordinal 表达加载顺序。"));
    add(QStringLiteral("trajectory"), QStringLiteral("well"), false,
        QStringLiteral("井斜轨迹"),
        QStringLiteral("井斜/轨迹数据；primary 为当前生效版本，其余为历史。"));
    add(QStringLiteral("tops"), QStringLiteral("well"), false,
        QStringLiteral("分层顶"),
        QStringLiteral("地层顶数据，多版本并存。"));
    add(QStringLiteral("time_depth"), QStringLiteral("well"), false,
        QStringLiteral("时深关系"),
        QStringLiteral("时深曲线/校验炮；primary 即当前 active 版本。"));
    add(QStringLiteral("core"), QStringLiteral("well"), false,
        QStringLiteral("岩心"),
        QStringLiteral("岩心描述/图像数据。"));
    add(QStringLiteral("interpretation"), QStringLiteral("well"), false,
        QStringLiteral("井周解释"),
        QStringLiteral("井尺度解释成果（多方案并存）。"));
    add(QStringLiteral("qc"), QStringLiteral("well"), false,
        QStringLiteral("质量控制"),
        QStringLiteral("QC 报告与附件。"));
    add(QStringLiteral("other"), QStringLiteral("well"), false,
        QStringLiteral("其他"),
        QStringLiteral("兜底角色。"));

    add(QStringLiteral("seismic_volume"), QStringLiteral("seismic_survey"), false,
        QStringLiteral("地震数据体"),
        QStringLiteral("SEG-Y 体（或其转码 store）；primary 为当前体。"));
    add(QStringLiteral("geometry"), QStringLiteral("seismic_survey"), false,
        QStringLiteral("观测系统"),
        QStringLiteral("采集几何/观测系统描述。"));
    add(QStringLiteral("velocity"), QStringLiteral("seismic_survey"), false,
        QStringLiteral("速度场"),
        QStringLiteral("速度模型/速度谱。"));
    add(QStringLiteral("horizon"), QStringLiteral("seismic_survey"), false,
        QStringLiteral("层位"),
        QStringLiteral("地震层位解释。"));
    add(QStringLiteral("fault"), QStringLiteral("seismic_survey"), false,
        QStringLiteral("断层"),
        QStringLiteral("断层解释。"));
    add(QStringLiteral("interpretation"), QStringLiteral("seismic_survey"), false,
        QStringLiteral("调查解释"),
        QStringLiteral("调查尺度解释成果。"));
    add(QStringLiteral("other"), QStringLiteral("seismic_survey"), false,
        QStringLiteral("其他"),
        QStringLiteral("兜底角色。"));
    return t;
  }
} // namespace

RoleRegistry RoleRegistry::defaults()
{
  RoleRegistry reg;
  reg.m_defs = builtinTable();
  return reg;
}

RoleRegistry RoleRegistry::fromJson(const QJsonObject &roles)
{
  RoleRegistry reg = defaults();
  for (auto it = roles.constBegin(); it != roles.constEnd(); ++it)
  {
    const QString name = it.key();
    // 空角色名与非对象值都不产出定义——坏形状静默跳过，不发明语义。
    if (name.isEmpty() || !it.value().isObject())
      continue;
    const QJsonObject o = it.value().toObject();
    const bool scoped = o.contains(QStringLiteral("entity_types"));
    QStringList types;
    if (scoped)
      for (const auto &v : o.value(QStringLiteral("entity_types")).toArray())
        types.append(v.toString());

    bool matched = false;
    for (RoleDef &def : reg.m_defs)
    {
      if (def.role != name)
        continue;
      if (scoped && def.entityTypes != types)
        continue;
      // 覆盖 = 字段级补丁：写了的键替换，没写的保留原值（尤其省略
      // entity_types 时原挂接域不动）。
      if (scoped)
        def.entityTypes = types;
      if (o.contains(QStringLiteral("max_count")))
        def.maxCount = o.value(QStringLiteral("max_count")).toInt();
      if (o.contains(QStringLiteral("ordered")))
        def.ordered = o.value(QStringLiteral("ordered")).toBool();
      if (o.contains(QStringLiteral("display")))
        def.display = o.value(QStringLiteral("display")).toString();
      if (o.contains(QStringLiteral("stage_default")))
        def.stageDefault = o.value(QStringLiteral("stage_default")).toString();
      if (o.contains(QStringLiteral("description")))
        def.description = o.value(QStringLiteral("description")).toString();
      matched = true;
      if (scoped)
        break; // (role, entityTypes) 至多一条定义
    }
    if (!matched)
    {
      RoleDef def;
      def.role = name;
      def.entityTypes = types;
      def.maxCount = o.value(QStringLiteral("max_count")).toInt();
      def.ordered = o.value(QStringLiteral("ordered")).toBool();
      def.display = o.value(QStringLiteral("display")).toString();
      def.stageDefault =
          o.value(QStringLiteral("stage_default")).toString(QStringLiteral("RAW"));
      def.description = o.value(QStringLiteral("description")).toString();
      reg.m_defs.append(def);
    }
  }
  return reg;
}

const RoleDef *RoleRegistry::find(const QString &role) const
{
  for (const RoleDef &def : m_defs)
    if (def.role == role)
      return &def;
  return nullptr;
}

QVector<RoleDef> RoleRegistry::forEntity(const QString &entityType) const
{
  QVector<RoleDef> out;
  // 空类型是调用方 bug——如实回空集，与 linksForEntity("") 同一约定。
  if (entityType.isEmpty())
    return out;
  for (const RoleDef &def : m_defs)
    if (def.entityTypes.contains(entityType))
      out.append(def);
  return out;
}

bool RoleRegistry::isKnown(const QString &role) const
{
  return find(role) != nullptr;
}
