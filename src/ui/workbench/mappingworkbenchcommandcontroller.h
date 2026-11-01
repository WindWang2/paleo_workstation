// 层：视图
#pragma once

#include "workbenchcommandhandler.h"

#include <QHash>
#include <QObject>
#include <QPointer>
#include <functional>
#include <memory>

class MappingWorkbench;
class MappingWorkbenchPage;
class PaleoEditingToolbar;
class ConstraintPage;
class WellPredictionPanel;
class QDockWidget;
class PaleoMainWindow;

namespace paleo::workbench {

// MappingWorkbenchCommandController — 制图工作台命令控制器（#749）。
// 承接原 PaleoMainWindow::runWorkbenchCommand（#473/#538 分发链）：查询/编辑/
// 生命周期三族分派 + 公共结果文案。每个 MappingWorkbenchPage 一个实例
//（parent = 页面，随页面析构），主窗口只做装配与 commandRequested 委托。
// 宿主私有面（画布/图层服务/预览/任务句柄/编辑流入口）经 friend 读取，
// 行为与拆分前逐字等价。
class MappingWorkbenchCommandController : public QObject, public WorkbenchCommandHandler
{
    Q_OBJECT
  public:
    using LegendFn = std::function<void(const QString &)>;
    using ShowFn = std::function<bool(const QString &, QString *)>;
    using OpenWellsFn = std::function<void(const QString &)>;
    using BeginEditFn = std::function<bool(const QString &)>;

    // 原 lambda 捕获面（this 之外全部显式化）。
    struct Context
    {
        MappingWorkbench *workbench = nullptr;
        MappingWorkbenchPage *page = nullptr;
        PaleoEditingToolbar *edit = nullptr;
        ConstraintPage *constraint = nullptr;
        WellPredictionPanel *wellPanel = nullptr;
        QDockWidget *wellDock = nullptr;
        LegendFn legend;
        ShowFn show;
        ShowFn compare;
        OpenWellsFn openWells;
        BeginEditFn beginEdit;
        std::shared_ptr<QHash<QString, QString>> shown;
    };

    MappingWorkbenchCommandController(PaleoMainWindow *host, Context context, QObject *parent = nullptr);

    bool handlesAction(const QString &action) const override;
    void handleCommand(const QString &action, const QVariantMap &parameters) override;

  private:
    QPointer<PaleoMainWindow> m_host;
    Context m_ctx;
};

} // namespace paleo::workbench
