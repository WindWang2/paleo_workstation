// 层：视图
// workbenchactioncatalog.cpp — 见 workbenchactioncatalog.h（C-W1 单份动作描述）。
#include "workbenchactioncatalog.h"

#include <QCoreApplication>
#include <QHash>
#include <QSet>
#include <QStringList>

namespace paleo::workbenchcmd
{
namespace
{
// 唯一事实表。副作用与能力位依据 mappingworkbenchcommandcontroller.cpp 各分支
// 的实际使用逐条核对（族名不推断只读性——predict/recompute 等查询族动作按
// 实际产出记 Mutating）。
const ActionSpec kActions[] = {
    // ---- EditFlow（runMappingEditFlow：分支自行 notify 收尾）----
    {"captureEditEvidence", Family::EditFlow, Effect::Read, Outcome::FlowOwned, kNone},
    {"setEditEvidence", Family::EditFlow, Effect::Mutating, Outcome::FlowOwned, kNone},
    {"clearEditEvidence", Family::EditFlow, Effect::Mutating, Outcome::FlowOwned, kNone},
    {"setAutoExplain", Family::EditFlow, Effect::Read, Outcome::FlowOwned, kNone},
    {"adoptEditExplanation", Family::EditFlow, Effect::Mutating, Outcome::FlowOwned, kNone},
    {"generateEditExplanation", Family::EditFlow, Effect::Read, Outcome::FlowOwned, kNone},
    {"cancelEditExplanation", Family::EditFlow, Effect::Read, Outcome::FlowOwned, kNone},
    {"checkTopology", Family::EditFlow, Effect::Read, Outcome::FlowOwned, kNone},
    // 导出把成果写出工程之外，不改工程数据：只读工程放行（浏览/查询契约）。
    {"exportVector", Family::EditFlow, Effect::Read, Outcome::FlowOwned, kNone},
    {"exportMap", Family::EditFlow, Effect::Read, Outcome::FlowOwned,
     kLayerService | kCanvas | kNavExportMap},

    // ---- Query（runWorkbenchQueryFamily）----
    {"predict", Family::Query, Effect::Mutating, Outcome::Queued, kNone},
    {"clusterWellAssignment", Family::Query, Effect::Mutating, Outcome::Standard, kNone},
    {"clusterManualAssignment", Family::Query, Effect::Mutating, Outcome::Standard, kLayerService | kShow},
    // 打开属性表面需要导航回调：缺了只开图层不显表，成功文案会失真——按必需依赖拒绝。
    {"wellattributes", Family::Query, Effect::Mutating, Outcome::Standard, kNavTable},
    {"wellformations", Family::Query, Effect::Mutating, Outcome::Standard, kNavTable},
    // 请求取消已在运行的预测：只读契约明确放行。
    {"cancel", Family::Query, Effect::Read, Outcome::Standard, kNone},
    {"show", Family::Query, Effect::Read, Outcome::Standard, kShow},
    {"graph", Family::Query, Effect::Read, Outcome::Standard, kNavGraph},
    {"catalog", Family::Query, Effect::Read, Outcome::Standard, kNavCatalog},
    {"reverseQuery", Family::Query, Effect::Read, Outcome::Standard, kNone},
    {"impactAnalysis", Family::Query, Effect::Mutating, Outcome::Queued, kNone},
    {"compareRecompute", Family::Query, Effect::Mutating, Outcome::Queued, kNone},
    {"recompute", Family::Query, Effect::Mutating, Outcome::Detailed, kImpact},

    // ---- Edit（runWorkbenchEditFamily）----
    {"labels", Family::Edit, Effect::Mutating, Outcome::Standard, kNone},
    // 井道面板打开是导航（不写数据）。
    {"welltracks", Family::Edit, Effect::Read, Outcome::Standard, kOpenWells},
    {"selectHierarchy", Family::Edit, Effect::Mutating, Outcome::Standard, kNone},
    {"displayLevel", Family::Edit, Effect::Mutating, Outcome::Standard, kNone},
    {"pinFaciesLevel", Family::Edit, Effect::Mutating, Outcome::Standard, kNone},
    {"featureDisplayLevel", Family::Edit, Effect::Mutating, Outcome::Standard, kBeginEdit},
    {"addEvidence", Family::Edit, Effect::Mutating, Outcome::Standard, kBeginEdit},
    {"removeEvidence", Family::Edit, Effect::Mutating, Outcome::Standard, kBeginEdit},
    {"updateEvidence", Family::Edit, Effect::Mutating, Outcome::Standard, kBeginEdit},
    {"verifyEvidence", Family::Edit, Effect::Mutating, Outcome::Standard, kBeginEdit},
    {"reviewInterpretation", Family::Edit, Effect::Mutating, Outcome::Standard, kBeginEdit},
    {"assignFacies", Family::Edit, Effect::Mutating, Outcome::Standard,
     kLayerService | kBeginEdit},
    {"referencePanel", Family::Edit, Effect::Read, Outcome::Standard, kCompare},
    {"references", Family::Edit, Effect::Read, Outcome::Standard, kCompare},
    {"compare", Family::Edit, Effect::Read, Outcome::Standard, kCompare},
    {"consistency", Family::Edit, Effect::Mutating, Outcome::Queued, kNone},
    {"polygonize", Family::Edit, Effect::Mutating, Outcome::Queued, kNone},
    {"resumeEdit", Family::Edit, Effect::Mutating, Outcome::Standard, kShow | kBeginEdit},
    {"copy", Family::Edit, Effect::Mutating, Outcome::Standard, kNone},
    {"save", Family::Edit, Effect::Mutating, Outcome::Standard, kLayerService},

    // ---- Lifecycle（runWorkbenchLifecycleFamily）----
    {"newMap", Family::Lifecycle, Effect::Mutating, Outcome::Standard, kShow | kBeginEdit},
    {"delete", Family::Lifecycle, Effect::Mutating, Outcome::Removal,
     kCanvas | kWellPanel | kWellDock | kLegend | kShownHistory},
    {"import", Family::Lifecycle, Effect::Mutating, Outcome::Standard, kNone},
    {"draw", Family::Lifecycle, Effect::Mutating, Outcome::Standard, kConstraintPage},
    {"factor", Family::Lifecycle, Effect::Mutating, Outcome::Queued, kNone},
    {"contours", Family::Lifecycle, Effect::Mutating, Outcome::Queued, kNone},
    {"compose", Family::Lifecycle, Effect::Mutating, Outcome::Queued, kNone},
    {"schema", Family::Lifecycle, Effect::Mutating, Outcome::Standard, kCanvas | kLegend},
};

const unsigned kKnownCaps = kCanvas | kLayerService | kConstraintPage | kWellPanel |
                            kWellDock | kImpact | kShownHistory | kShow | kCompare |
                            kLegend | kOpenWells | kBeginEdit | kNavTable | kNavGraph |
                            kNavCatalog | kNavExportMap;

struct CapName
{
    unsigned cap;
    const char *name;
};
const CapName kCapNames[] = {
    {kCanvas, QT_TRANSLATE_NOOP("PaleoMainWindow", "画布控制器")},
    {kLayerService, QT_TRANSLATE_NOOP("PaleoMainWindow", "图层服务")},
    {kConstraintPage, QT_TRANSLATE_NOOP("PaleoMainWindow", "约束编图页")},
    {kWellPanel, QT_TRANSLATE_NOOP("PaleoMainWindow", "测井预测修订面板")},
    {kWellDock, QT_TRANSLATE_NOOP("PaleoMainWindow", "测井预测窗口")},
    {kImpact, QT_TRANSLATE_NOOP("PaleoMainWindow", "影响分析反馈")},
    {kShownHistory, QT_TRANSLATE_NOOP("PaleoMainWindow", "已显示图件登记")},
    {kShow, QT_TRANSLATE_NOOP("PaleoMainWindow", "图件显示入口")},
    {kCompare, QT_TRANSLATE_NOOP("PaleoMainWindow", "证据参考入口")},
    {kLegend, QT_TRANSLATE_NOOP("PaleoMainWindow", "图例刷新入口")},
    {kOpenWells, QT_TRANSLATE_NOOP("PaleoMainWindow", "井道面板入口")},
    {kBeginEdit, QT_TRANSLATE_NOOP("PaleoMainWindow", "编辑会话入口")},
    {kNavTable, QT_TRANSLATE_NOOP("PaleoMainWindow", "属性表导航")},
    {kNavGraph, QT_TRANSLATE_NOOP("PaleoMainWindow", "图谱导航")},
    {kNavCatalog, QT_TRANSLATE_NOOP("PaleoMainWindow", "数据管理导航")},
    {kNavExportMap, QT_TRANSLATE_NOOP("PaleoMainWindow", "图件输出入口")},
};

const QHash<QString, const ActionSpec *> &index()
{
    static const QHash<QString, const ActionSpec *> lookup = [] {
        QHash<QString, const ActionSpec *> hash;
        for (const auto &spec : catalog())
            hash.insert(QString::fromLatin1(spec.id), &spec);
        return hash;
    }();
    return lookup;
}
} // namespace

const QVector<ActionSpec> &catalog()
{
    static const QVector<ActionSpec> actions = [] {
        QVector<ActionSpec> out;
        const int count = int(sizeof(kActions) / sizeof(kActions[0]));
        out.reserve(count);
        for (int i = 0; i < count; ++i)
            out.append(kActions[i]);
        return out;
    }();
    return actions;
}

const ActionSpec *find(const QString &action) { return index().value(action, nullptr); }

QString missingCapsReason(unsigned caps)
{
    QStringList missing;
    for (const auto &entry : kCapNames)
        if (caps & entry.cap)
            missing << QCoreApplication::translate("PaleoMainWindow", entry.name);
    return missing.join(QStringLiteral("、"));
}

QStringList catalogIntegrity()
{
    QStringList problems;
    QSet<QString> seen;
    for (const auto &spec : catalog())
    {
        const QString id = QString::fromLatin1(spec.id);
        if (id.isEmpty() || seen.contains(id))
            problems << QStringLiteral("动作 id 重复或为空：%1").arg(id);
        seen.insert(id);
        if (spec.family != Family::EditFlow && spec.family != Family::Query &&
            spec.family != Family::Edit && spec.family != Family::Lifecycle)
            problems << QStringLiteral("%1：未知处理族").arg(id);
        if (spec.effect != Effect::Read && spec.effect != Effect::Mutating)
            problems << QStringLiteral("%1：未知副作用分类").arg(id);
        if (spec.outcome != Outcome::Standard && spec.outcome != Outcome::Queued &&
            spec.outcome != Outcome::Detailed && spec.outcome != Outcome::Removal &&
            spec.outcome != Outcome::FlowOwned)
            problems << QStringLiteral("%1：未知结果策略").arg(id);
        if (spec.caps & ~kKnownCaps)
            problems << QStringLiteral("%1：未知能力位").arg(id);
    }
    return problems;
}

} // namespace paleo::workbenchcmd
