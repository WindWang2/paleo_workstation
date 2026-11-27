// 层：视图
#pragma once

#include "workbenchcommandhandler.h"

#include <QHash>
#include <QList>
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
class QStatusBar;
class QWidget;
class QgisCanvasController;
class QgisLayerService;
class PaleoTaskService;

namespace paleo::workbench {

struct ImpactOutcome;

// One report shared by the mapping pages in the current project session.
class MappingWorkbenchImpactFeedback : public QObject
{
  public:
    MappingWorkbenchImpactFeedback(MappingWorkbench *workbench, QStatusBar *statusBar,
                                   QObject *parent);
    void addPage(MappingWorkbenchPage *page);
    QVariantMap reportFor(const QString &horizon, const QString &layer) const;

  private:
    void updateAvailability();
    void updatePage(MappingWorkbenchPage *page) const;
    void publish(const ImpactOutcome &outcome);

    QPointer<MappingWorkbench> m_workbench;
    QPointer<QStatusBar> m_statusBar;
    QList<QPointer<MappingWorkbenchPage>> m_pages;
    QVariantMap m_report;
};

// MappingWorkbenchCommandController — 制图工作台命令控制器（#749）。
// 承接原 PaleoMainWindow::runWorkbenchCommand（#473/#538 分发链）：查询/编辑/
// 生命周期三族分派 + 公共结果文案。每个 MappingWorkbenchPage 一个实例
//（parent = 页面，随页面析构），主窗口只做装配与 commandRequested 委托。
class MappingWorkbenchCommandController : public QObject, public WorkbenchCommandHandler
{
    Q_OBJECT
  public:
    using LegendFn = std::function<void(const QString &)>;
    using ShowFn = std::function<bool(const QString &, QString *)>;
    using OpenWellsFn = std::function<void(const QString &)>;
    using BeginEditFn = std::function<bool(const QString &)>;

    struct Navigation
    {
        std::function<void(const QString &)> showAttributeTable;
        std::function<void(const QString &, const QString &)> openGraph;
        std::function<bool(const QString &, const QString &)> focusCatalogVersion;
        std::function<void()> exportMap;
    };

    struct Context
    {
        QPointer<MappingWorkbench> workbench;
        QPointer<MappingWorkbenchPage> page;
        QPointer<QWidget> dialogParent;
        QPointer<QStatusBar> statusBar;
        // C-W4（#988）：可销毁 QObject 一律 QPointer——模态边界（删除确认、
        // 文件/参数对话框）返回后按 WorkbenchAdmission::admit 复核存活，
        // 不再依赖装配时刻的裸指针。
        QPointer<QgisCanvasController> canvasCtl;
        QPointer<QgisLayerService> layerSvc;
        QPointer<MappingWorkbenchImpactFeedback> impact;
        std::function<bool()> factorBusy;
        std::function<PaleoTaskService *()> taskService;
        Navigation navigation;
        QPointer<PaleoEditingToolbar> edit;
        QPointer<ConstraintPage> constraint;
        QPointer<WellPredictionPanel> wellPanel;
        QPointer<QDockWidget> wellDock;
        LegendFn legend;
        ShowFn show;
        ShowFn compare;
        ShowFn comparePrimary;
        OpenWellsFn openWells;
        BeginEditFn beginEdit;
        std::shared_ptr<QHash<QString, QString>> shown;
        // C-W2：宿主状态钩子（触发时刻现读，不是装配快照）。留空的测试壳按
        // 可写、非切换处理；生产装配由 attachWorkbenchCommands 注入。
        std::function<bool()> projectReadOnly;  // 工程只读（Mutating 拒绝）
        std::function<quint64()> projectSession;
        std::function<bool()> projectSwitching; // 工程打开/切换中（一律拒绝）
    };

    explicit MappingWorkbenchCommandController(Context context, QObject *parent = nullptr);

    bool handlesAction(const QString &action) const override;
    void handleCommand(const QString &action, const QVariantMap &parameters) override;

  private:
    Context m_ctx;
};

} // namespace paleo::workbench
