// 层：数据
#pragma once
#include <QList>
#include <QString>
#include <QStringList>
#include <QVariantMap>

// domain/wellsitingplan — 井网部署方案的纯值类型（方向 34）。
// 方案 = 计划井（catalog 实体 entityType=="planned"）集合 + 指标快照；
// 与断层集同款约定：无 Qgs* 依赖，经 QVariantMap/JSON 往返以落
// project.sqlite（WellSitingStore）。
//
// planned 井本体在 catalog（创建后不可变——catalog 无 update/remove 面），
// 这里只存实体 id 引用与指标快照，绝不复制坐标（单一事实源）。可见性
// 面同此：retiredIds 记「已删除」的计划井 id，renames 记显示名覆盖
//（id → 名）——siting 查询面据此过滤/改称，catalog 实体保持审计痕迹。
//
// 指标字段口径（metricsBefore/metricsAfter 的键，workflow 是字段权威）：
//   hole_area_total/hole_count/coverage_ratio/domain_mean_distance/
//   inter_well_mean_spacing/weighted_density/note
//   （与 algorithms/wellsiting::ScenarioMetrics 一一对应）。
namespace paleo::siting {

struct SitingScenario {
    QString id;                 // "scenario-N"（workflow 分配）
    QString name;               // 用户命名（默认「方案 N」）
    QStringList plannedWellIds; // catalog planned 实体 id
    QVariantMap params;         // WellSitingParams::toMap()（复算/图表用）
    QVariantMap metricsBefore;  // 基线指标快照（实井）
    QVariantMap metricsAfter;   // 方案指标快照（实井 + 本方案候选井）
    QVariantMap contributions;  // id → {hole_area_m2, mean_distance_m}
    qint64 updatedMs = 0;       // lastModified（排序/显示用）

    QVariantMap toMap() const;
    static SitingScenario fromMap(const QVariantMap &m);
};

// 方案集：upsert 以 id 为键；列表序 = 保存序。文档级还携带计划井的
// 可见性面（retired/renames）——与方案同库同文档。
struct ScenarioSet {
    QList<SitingScenario> scenarios;
    QStringList retiredWellIds;      // 「已删除」的 planned 实体 id
    QVariantMap wellRenames;         // planned id → 显示名

    bool isEmpty() const { return scenarios.isEmpty(); }
    int indexOf(const QString &id) const;
    void upsert(const SitingScenario &scenario); // 同 id 替换保位，新 id 追加
    bool remove(const QString &id);
    bool isRetired(const QString &wellId) const
    {
        return retiredWellIds.contains(wellId);
    }
    QString displayName(const QString &wellId, const QString &fallback) const
    {
        return wellRenames.value(wellId).toString().isEmpty() ? fallback
                                                              : wellRenames.value(wellId).toString();
    }

    QVariantMap toMap() const;
    static ScenarioSet fromMap(const QVariantMap &m);
};

} // namespace paleo::siting
