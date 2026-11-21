// 层：视图
#pragma once

#include "mappingworkbenchcommandcontroller.h"
#include "workbenchactioncatalog.h"

#include <QString>

namespace paleo::workbench
{

// WorkbenchAdmission — 工作台命令的统一准入适配器（C-W2，#988/#994 关联）。
// 页面按钮、Ribbon 镜像与 controller 直接 handleCommand 的触发路径都在进入
// 业务分支前经过同一份判断；展示状态（按钮 enabled/tooltip）只是快照，
// 这里是执行时刻对真实上下文的复核：
//   1) 基础上下文（dialogParent/statusBar/workbench/page 存活）；
//   2) 工程打开/切换中 → 一律拒绝（避免作用于新会话）；
//   3) 只读工程拒绝 Mutating（Read 放行：浏览/查询/导航/请求取消）——
//      宿主级门先于依赖位，不因个别面板未装配而遮蔽只读事实；
//   4) 目录声明的必需能力（spec.caps → QPointer 存活 / 回调可调用），
//      缺失给具体依赖名，不落 bad_function_call。
// 同一函数也在模态边界（删除确认、文件/参数对话框）返回后再次调用——
// 嵌套事件循环期间宿主/页面/依赖可能被销毁、只读或工程会话可能变化，
// 冻结的旧上下文不能继续当执行授权。
class WorkbenchAdmission
{
  public:
    struct Verdict
    {
        bool allowed = false;
        QString reason; // 拒绝时的具体原因（空 = 放行）
    };

    static Verdict admit(const workbenchcmd::ActionSpec &spec,
                         const MappingWorkbenchCommandController::Context &ctx);

    // 能力位核对：返回 ctx 中缺失的能力位集合（0 = 齐备）。
    static unsigned missingCaps(const workbenchcmd::ActionSpec &spec,
                                const MappingWorkbenchCommandController::Context &ctx);
};

} // namespace paleo::workbench
