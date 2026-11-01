// 层：视图
#pragma once

#include <QString>
#include <QVariantMap>

// WorkbenchCommandHandler — 工作台命令处理器接口（#749，承接 #705 命令登记表）。
// 每个工作台控制器实现本接口认领自己的 action 集合；主窗口只负责停靠/菜单
// 装配，把页面 commandRequested 委托给处理器，不再自带分支表。
namespace paleo::workbench {

class WorkbenchCommandHandler
{
  public:
    virtual ~WorkbenchCommandHandler() = default;
    // 本处理器是否认领该 action（登记表口径；未认领的动作由处理器按失败回报）。
    virtual bool handlesAction(const QString &action) const = 0;
    // 执行一条命令；结果文案经处理器自行呈现（页面消息/状态栏/ErrorHub）。
    virtual void handleCommand(const QString &action, const QVariantMap &parameters) = 0;
};

} // namespace paleo::workbench
