// 层：数据
#include "wellsitingplan.h"

namespace paleo::siting {

QVariantMap SitingScenario::toMap() const
{
    QVariantMap m;
    m.insert(QStringLiteral("id"), id);
    m.insert(QStringLiteral("name"), name);
    m.insert(QStringLiteral("planned_well_ids"), plannedWellIds);
    m.insert(QStringLiteral("params"), params);
    m.insert(QStringLiteral("metrics_before"), metricsBefore);
    m.insert(QStringLiteral("metrics_after"), metricsAfter);
    m.insert(QStringLiteral("contributions"), contributions);
    m.insert(QStringLiteral("updated_ms"), updatedMs);
    return m;
}

SitingScenario SitingScenario::fromMap(const QVariantMap &m)
{
    SitingScenario s;
    s.id = m.value(QStringLiteral("id")).toString();
    s.name = m.value(QStringLiteral("name")).toString();
    s.plannedWellIds = m.value(QStringLiteral("planned_well_ids")).toStringList();
    s.params = m.value(QStringLiteral("params")).toMap();
    s.metricsBefore = m.value(QStringLiteral("metrics_before")).toMap();
    s.metricsAfter = m.value(QStringLiteral("metrics_after")).toMap();
    s.contributions = m.value(QStringLiteral("contributions")).toMap();
    s.updatedMs = m.value(QStringLiteral("updated_ms"), 0).toLongLong();
    return s;
}

int ScenarioSet::indexOf(const QString &id) const
{
    for (int i = 0; i < scenarios.size(); ++i)
        if (scenarios.at(i).id == id)
            return i;
    return -1;
}

void ScenarioSet::upsert(const SitingScenario &scenario)
{
    const int at = indexOf(scenario.id);
    if (at >= 0)
        scenarios.replace(at, scenario);
    else
        scenarios.append(scenario);
}

bool ScenarioSet::remove(const QString &id)
{
    const int at = indexOf(id);
    if (at < 0)
        return false;
    scenarios.removeAt(at);
    return true;
}

QVariantMap ScenarioSet::toMap() const
{
    QVariantMap m;
    QVariantList list;
    for (const SitingScenario &s : scenarios)
        list.append(s.toMap());
    m.insert(QStringLiteral("scenarios"), list);
    m.insert(QStringLiteral("retired_well_ids"), retiredWellIds);
    m.insert(QStringLiteral("well_renames"), wellRenames);
    return m;
}

ScenarioSet ScenarioSet::fromMap(const QVariantMap &m)
{
    ScenarioSet set;
    const QVariantList list = m.value(QStringLiteral("scenarios")).toList();
    for (const QVariant &v : list) {
        SitingScenario s = SitingScenario::fromMap(v.toMap());
        if (!s.id.isEmpty())
            set.scenarios.append(s); // 无 id 行不进集——不发明身份
    }
    set.retiredWellIds = m.value(QStringLiteral("retired_well_ids")).toStringList();
    set.wellRenames = m.value(QStringLiteral("well_renames")).toMap();
    return set;
}

} // namespace paleo::siting
