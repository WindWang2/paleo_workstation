// 层：数据
#include "catalogroles.h"

#include <QMultiHash>
#include <QHash>

namespace
{

QVector<CatalogRoleInfo> builtinCatalogRoles()
{
  // 表序 = 井段 → 地震段 → misc 段；order 全表唯一（井段 0-10 沿树面既有
  // 角色 well_log/tops/time_depth/well_head 的 0/1/2/3 序，等价重构不动）。
  QVector<CatalogRoleInfo> t;
  const auto add = [&t](const char *role, const char *group, int order,
                        const QString &display) {
    t.append(CatalogRoleInfo{QString::fromLatin1(role),
                             QString::fromLatin1(group), order, display});
  };

  // ---- well 族（井节点子序的权威序）----
  add("well_log", "well", 0, QStringLiteral("测井曲线"));
  add("tops", "well", 1, QStringLiteral("井分层"));
  add("time_depth", "well", 2, QStringLiteral("时深关系"));
  add("well_head", "well", 3, QStringLiteral("井身/井位"));
  add("core", "well", 4, QStringLiteral("岩心照片"));
  add("trajectory", "well", 5, QStringLiteral("井斜轨迹"));
  add("cuttings", "well", 6, QStringLiteral("岩屑录井"));
  add("lab_analysis", "well", 7, QStringLiteral("实验分析"));
  add("interpretation", "well", 8, QStringLiteral("井周解释"));
  add("qc", "well", 9, QStringLiteral("质量控制"));
  add("other", "well", 10, QStringLiteral("其他"));

  // ---- seismic 族 ----
  add("seismic_volume", "seismic", 20, QStringLiteral("地震数据体"));
  add("horizon", "seismic", 21, QStringLiteral("层位"));
  add("geometry", "seismic", 22, QStringLiteral("观测系统"));
  add("velocity", "seismic", 23, QStringLiteral("速度场"));
  add("fault", "seismic", 24, QStringLiteral("断层"));
  add("interpretation", "seismic", 25, QStringLiteral("调查解释"));
  add("other", "seismic", 26, QStringLiteral("其他"));

  // ---- misc 族（辅助/格架/部署挂接）----
  add("reference", "misc", 30, QStringLiteral("参考资料"));
  add("framework_unit", "misc", 31, QStringLiteral("格架单元"));
  add("siting_note", "misc", 32, QStringLiteral("部署依据"));

  return t;
}

QVector<CatalogRoleGroup> builtinCatalogRoleGroups()
{
  QVector<CatalogRoleGroup> g;
  g.append(CatalogRoleGroup{QStringLiteral("well"), QStringLiteral("井")});
  g.append(CatalogRoleGroup{QStringLiteral("seismic"), QStringLiteral("地震")});
  g.append(CatalogRoleGroup{QStringLiteral("mapproduct"), QStringLiteral("成果图件")});
  g.append(CatalogRoleGroup{QStringLiteral("misc"), QStringLiteral("辅助")});
  return g;
}

// 进程内只读快照 + 裸角色索引（首次调用构建，之后零开销）。
struct CatalogRoleTable
{
  QVector<CatalogRoleInfo> roles;
  QVector<CatalogRoleGroup> groups;
  QHash<QString, const CatalogRoleInfo *> byRole; // 表序首个定义（同名井段在前）
};

const CatalogRoleTable &table()
{
  static const CatalogRoleTable t = [] {
    CatalogRoleTable out;
    out.roles = builtinCatalogRoles();
    out.groups = builtinCatalogRoleGroups();
    // 取址落哈希：roles 是函数级静态（进程生命周期），指针稳定。
    for (const CatalogRoleInfo &r : out.roles)
      if (!out.byRole.contains(r.role))
        out.byRole.insert(r.role, &r);
    return out;
  }();
  return t;
}

} // namespace

const QVector<CatalogRoleInfo> &catalogRoles() { return table().roles; }

const QVector<CatalogRoleGroup> &catalogRoleGroups() { return table().groups; }

const CatalogRoleInfo *catalogRoleInfo(const QString &role)
{
  return table().byRole.value(role, nullptr);
}

QString catalogRoleGroup(const QString &role)
{
  const CatalogRoleInfo *info = catalogRoleInfo(role);
  return info ? info->group : QString::fromLatin1(kUngroupedRoleGroup);
}

int catalogRoleOrder(const QString &role)
{
  const CatalogRoleInfo *info = catalogRoleInfo(role);
  return info ? info->order : kUngroupedRoleOrder;
}

QString catalogRoleDisplay(const QString &role)
{
  const CatalogRoleInfo *info = catalogRoleInfo(role);
  return info ? info->display : role;
}

QString catalogRoleListForAi()
{
  QStringList segments;
  for (const CatalogRoleGroup &g : catalogRoleGroups())
  {
    QStringList names;
    for (const CatalogRoleInfo &r : catalogRoles())
      if (r.group == g.key)
        names << r.role;
    if (!names.isEmpty())
      segments << QStringLiteral("%1：%2").arg(g.display, names.join(QLatin1Char('/')));
  }
  return segments.join(QStringLiteral("；"));
}
