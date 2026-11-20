// 层：视图
// workbenchadmission.cpp — 见 workbenchadmission.h（C-W2 统一准入）。
#include "workbenchadmission.h"

#include <QCoreApplication>

using Context = paleo::workbench::MappingWorkbenchCommandController::Context;
using paleo::workbench::WorkbenchAdmission;
using paleo::workbenchcmd::Cap;
using paleo::workbenchcmd::Effect;

namespace
{
class AdmissionText { Q_DECLARE_TR_FUNCTIONS(PaleoMainWindow) };
}

unsigned WorkbenchAdmission::missingCaps(const workbenchcmd::ActionSpec &spec,
                                         const Context &ctx)
{
    unsigned missing = spec.caps;
    const auto present = [&missing](unsigned cap, bool ok) {
        if (ok)
            missing &= ~cap;
    };
    present(Cap::kCanvas, ctx.canvasCtl != nullptr);
    present(Cap::kLayerService, ctx.layerSvc != nullptr);
    present(Cap::kConstraintPage, ctx.constraint != nullptr);
    present(Cap::kWellPanel, ctx.wellPanel != nullptr);
    present(Cap::kWellDock, ctx.wellDock != nullptr);
    present(Cap::kImpact, ctx.impact != nullptr);
    present(Cap::kShownHistory, ctx.shown != nullptr);
    present(Cap::kShow, static_cast<bool>(ctx.show));
    present(Cap::kCompare, static_cast<bool>(ctx.compare));
    present(Cap::kLegend, static_cast<bool>(ctx.legend));
    present(Cap::kOpenWells, static_cast<bool>(ctx.openWells));
    present(Cap::kBeginEdit, static_cast<bool>(ctx.beginEdit));
    present(Cap::kNavTable, static_cast<bool>(ctx.navigation.showAttributeTable));
    present(Cap::kNavGraph, static_cast<bool>(ctx.navigation.openGraph));
    present(Cap::kNavCatalog, static_cast<bool>(ctx.navigation.focusCatalogVersion));
    present(Cap::kNavExportMap, static_cast<bool>(ctx.navigation.exportMap));
    return missing;
}

WorkbenchAdmission::Verdict WorkbenchAdmission::admit(const workbenchcmd::ActionSpec &spec,
                                                      const Context &ctx)
{
    Verdict v;
    v.allowed = true;
    // 基础上下文：反馈出口与业务宿主都在，命令才有地方执行和回报。
    if (!ctx.dialogParent || !ctx.statusBar || !ctx.workbench || !ctx.page)
    {
        v.allowed = false;
        v.reason = AdmissionText::tr("工作台上下文不完整（窗口/状态栏/工作台/页面）");
        return v;
    }
    if (ctx.projectSwitching && ctx.projectSwitching())
    {
        v.allowed = false;
        v.reason = AdmissionText::tr("工程正在打开或切换，请稍后再试");
        return v;
    }
    // 宿主级门（只读）先于依赖位：只读是全局资格，用户先看到可行动的原因，
    // 而不是因某个面板恰未装配而遮蔽只读事实。
    if (spec.effect == Effect::Mutating && ctx.projectReadOnly && ctx.projectReadOnly())
    {
        v.allowed = false;
        v.reason = AdmissionText::tr(
            "工程处于只读模式，写操作已禁用；可继续浏览、查询和请求取消");
        return v;
    }
    if (const unsigned missing = missingCaps(spec, ctx))
    {
        v.allowed = false;
        v.reason = AdmissionText::tr("命令所需依赖不可用：%1")
                       .arg(paleo::workbenchcmd::missingCapsReason(missing));
        return v;
    }
    return v;
}
