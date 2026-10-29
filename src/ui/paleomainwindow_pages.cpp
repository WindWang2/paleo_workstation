// 层：视图
// paleomainwindow_pages — 页切换域（方向 83 拆 TU）：showPage（六页/启动页/
// 地层对比 Web 页的中央面与 dock 显隐编排）、地层对比页进出时的 dock
// 保存/恢复、任务驱动的底栏自动露出。自本体移入，逻辑逐行保持。
#include "paleomainwindow.h"
#include "paleomainwindow_internal.h"

#include "../metadata/paleoprojectstore.h" // showStartup 只读态现取 store 真源（白名单六头）
#include "datapreview/datapreviewtabs.h" // m_previewTabs 的 findChild 需完整类型（上行转换）
#include "pages/pageshared.h" // kPageIds（页序表）
#include "paleoribbon.h"      // makeKeyboardReachable（页签命令钮进 Tab 链）
#include "pages/stratigraphicwebpage.h" // correlation 页 activate()
#include "shortcuts/shortcutcatalog.h" // 方向63：Ctrl+S 让渡（correlation 页）
#include "taskpanel.h" // syncBottomDockForTasks 切到任务页
#include "../qgis/qgiscanvascontroller.h" // 非编辑页停用活动工具
#include "../qgis/qgislayerprofile.h" // applyPageProfile（页面档案）
#include "../qgis/qgisprojectservice.h" // 工程在场判定（startup 逃逸）
#include "../services/paleotaskservice.h" // PaleoTask::running/quiet
#include "../workflow/mappingworkbench.h" // 编图链三页测井基础图层保证在场

#include <QAction>
#include <QTabBar>
#include <QSignalBlocker>
#include <QDockWidget>
#include <QMainWindow>
#include <QStackedLayout>
#include <QStackedWidget>
#include <QTabWidget>
#include <QToolButton>

using paleo::mainwindow_internal::pageDockTitles;

namespace
{
// 页作用域的数字化工具面：编辑/编图工具只属于编图链三页
// （预测/约束/编图——PALEO_QGIS_PLAN §9 的物源线、相界编辑所在）。
// 数据管理页是纯数据面（预览+实体视图），验证页是检查面：
// 两页画布只作展示，编辑 dock 隐藏，且进入时停用活动画布工具。
const QStringList kEditingToolPages = {
  QStringLiteral("predict"),
  QStringLiteral("constraint"),
  QStringLiteral("compose"),
};
} // namespace

namespace paleo::mainwindow_internal
{

void applyPageDockPolicy(const QString &pageId, const PageDockState &s,
                         QMainWindow *window)
{
  if (s.right)
    s.right->setProgrammaticVisible(pageId != QLatin1String("startup") &&
        pageId != QLatin1String("correlation") && s.right->userWantsVisible() && !s.previewMaximized);
  s.dataList->setProgrammaticVisible(pageId == QLatin1String("data") &&
                                     s.dataList->userWantsVisible() && !s.previewMaximized);

  // 数据页面纯三栏布局（数据导航树 | 预览可视化 | 属性面板），不要图层树与底部的横向面板（日志/任务等）。
  // 离开数据页面切到编图页面时，恢复用户期望的图层树与底栏状态。
  if (pageId == QLatin1String("data"))
  {
    if (s.left)
      s.left->setProgrammaticVisible(false);
    if (s.bottom)
      s.bottom->setProgrammaticVisible(false);
    if (s.wellSection)
      s.wellSection->setProgrammaticVisible(false);
  }
  else
  {
    if (s.left && s.left->userWantsVisible())
      s.left->setProgrammaticVisible(true);
    // W2：任务驱动露出的底栏（m_bottomDockAutoShown）不随切页收回。
    if (s.bottom && (s.bottom->userWantsVisible() || s.bottomAutoShown))
      s.bottom->setProgrammaticVisible(true);
    if (s.wellSection && s.wellSection->userWantsVisible())
      s.wellSection->setProgrammaticVisible(true);
  }
  // 编图参考面板随地图工作区进出；保留关闭意愿，恢复布局也走同一规则。
  if (window)
    for (auto *dock : window->findChildren<PaleoDockWidget *>())
      if (dock->property("paleo.mappingReference").toBool())
        dock->setProgrammaticVisible(pageId != QLatin1String("data") &&
            pageId != QLatin1String("correlation") && dock->userWantsVisible());
}

void hideAllDocksForCorrelation(QMainWindow *win)
{
  // Web 工作台拥有井组、属性和任务面；整片中央区域让给它。
  for (auto *dock : win->findChildren<QDockWidget *>(QString(), Qt::FindDirectChildrenOnly))
    if (auto *paleoDock = qobject_cast<PaleoDockWidget *>(dock))
      paleoDock->setProgrammaticVisible(false);
    else
      dock->hide();
}

} // namespace paleo::mainwindow_internal

void PaleoMainWindow::restoreCorrelationDocks()
{
  if (auto *save = findChild<QAction *>(QStringLiteral("saveProjectAction")))
    paleo::shortcuts::setActionShortcutActive(QStringLiteral("main.project.save"), save, true);
  for (const auto &dock : m_correlationHiddenDocks)
    if (dock)
    {
      // 与 applyPageDockPolicy 同一纪律：Web 页期间用户经「布局」菜单收起面板
      // （toggleViewAction → setVisible 记下 userWantsVisible=false）后，离开
      // 时不得被恢复强制拉回。
      if (auto *paleoDock = qobject_cast<PaleoDockWidget *>(dock.data()))
      {
        if (paleoDock->userWantsVisible())
          paleoDock->setProgrammaticVisible(true);
      }
      else
        dock->show();
    }
  m_correlationHiddenDocks.clear();
}

void PaleoMainWindow::showPage(const QString &pageId)
{
  const int idx = paleo::pagesinternal::kPageIds.indexOf(pageId);
  if (idx < 0)
  {
    qWarning() << "PaleoMainWindow::showPage — unknown page id:" << pageId;
    return;
  }
  if (pageId != QLatin1String("data") && m_previewMaximized)
    if (auto *button = m_previewTabs->findChild<QToolButton *>(QStringLiteral("previewMaxButton")))
      button->setChecked(false);
  const bool enteringCorrelation = pageId == QLatin1String("correlation") &&
                                   m_currentPage != pageId;
  const bool leavingCorrelation = m_currentPage == QLatin1String("correlation") &&
                                  pageId != m_currentPage;
  if (leavingCorrelation)
    restoreCorrelationDocks();
  if (enteringCorrelation)
  {
    m_beforeCorrelationWindowState = saveState();
    for (auto *dock : findChildren<QDockWidget *>(QString(), Qt::FindDirectChildrenOnly))
      if (!dock->isHidden())
        m_correlationHiddenDocks.append(dock);
  }
  m_currentPage = pageId;
  if (m_visualizationTabs) {
    const QString surface = pageId == QLatin1String("data") ? QStringLiteral("data") :
                            pageId == QLatin1String("correlation") ? QStringLiteral("correlation") : QStringLiteral("compose");
    QSignalBlocker blocker(m_visualizationTabs);
    int tab = -1;
    for (int i=0; i<m_visualizationTabs->count(); ++i)
      if (m_visualizationTabs->tabData(i).toMap().value(QStringLiteral("surface")).toString() == surface) tab = i;
    if (tab < 0) {
      tab = m_visualizationTabs->addTab(surface == "data" ? tr("数据可视化") : surface == "correlation" ? tr("地层对比") : tr("QGIS 地理视图"));
    }
    m_visualizationTabs->setTabData(tab, QVariantMap{{QStringLiteral("surface"), surface}, {QStringLiteral("page"), pageId}});
    m_visualizationTabs->setCurrentIndex(tab);
  }
  if (m_workbench) m_workbench->setManualEvidenceRequired(pageId == QLatin1String("compose"));
  // Web 页自己的 Ctrl+S 保存独立解释，Paleo 工程的保存快捷键让出。
  if (auto *save = findChild<QAction *>(QStringLiteral("saveProjectAction")))
    paleo::shortcuts::setActionShortcutActive(QStringLiteral("main.project.save"), save,
                                              true);

  // Visualization activation preserves the user's selected Ribbon tools.
  // 各 attach 后续建的命令钮也要进 Tab 链（幂等，便宜的补漏点）。
  PaleoRibbon::makeKeyboardReachable(this);

  // Scroll hosts isolate content hints; switching a page never resizes a dock.
  if (auto *host = findChild<QWidget *>(QStringLiteral("rightPanelHost")))
    if (auto *stack = qobject_cast<QStackedLayout *>(host->layout()))
      stack->setCurrentIndex(idx);
  if (m_rightDock)
    m_rightDock->setWindowTitle(pageDockTitles().at(idx));

  // ---- m2(D): 页面图层档案——切到编图页即应用该页档案（数据页 no-op）。
  // 档案表在 QgisLayerProfileService（predict/constraint/compose/validate）。
  applyCurrentPageProfile();

  // A workflow step implies the workspace: leave the startup page once a
  // project exists (with no project the startup page stays — nothing to show).
  if (m_centerStack && m_centerStack->currentIndex() == 0 && m_projectSvc &&
      !m_projectSvc->projectPath().isEmpty())
    m_centerStack->setCurrentIndex(1);

  // 数据管理页是列表面（数据列表 + 数据预览），其余四页是画布面。井上
  // 图层状态不受影响——回到画布页照常看图。
  if (m_workspaceStack)
    m_workspaceStack->setCurrentIndex(pageId == QLatin1String("data") ? 1 :
                                      pageId == QLatin1String("correlation") ? 2 : 0);
  if (m_previewTabs)
    m_previewTabs->setVisible(pageId == QLatin1String("data"));

  paleo::mainwindow_internal::applyPageDockPolicy(
      pageId, {m_leftDock, m_dataListDock, m_bottomDock, m_wellSectionDock,
               m_previewMaximized, m_bottomDockAutoShown, m_rightDock}, this);

  if (pageId == QLatin1String("correlation"))
  {
    paleo::mainwindow_internal::hideAllDocksForCorrelation(this);
    m_centerStack->setCurrentIndex(1); // 无需先打开 QGIS/Paleo 工程
    // Native correlation shares the current Paleo project. No external process.
  }
  else if (leavingCorrelation && (!m_projectSvc || m_projectSvc->projectPath().isEmpty()) &&
           m_centerStack && m_centerStack->currentIndex() != 0)
    m_centerStack->setCurrentIndex(0);

  // 图层平台：页面档案——不同页面激活不同图层组（QgsMapThemeCollection，
  // data 页 no-op）。本函数前段 applyCurrentPageProfile() 已按 m_currentPage
  // 应用过同一档案（含 kPageIds 守卫）；此前这里再直调一次 = 每次切页双份
  // 摆树 + 双份画布刷新（profileApplied ×2），已删。

  // 编图链三页的图层列表必须带测井基础图层（存在才物化；2026-10-10 用户
  // 要求）。覆盖首预测落盘后与跨工程重开两种时机。
  if (m_workbench && (pageId == QLatin1String("predict") ||
                      pageId == QLatin1String("constraint") ||
                      pageId == QLatin1String("compose")))
    m_workbench->ensureWellBaseLayers();

  // 页作用域工具面：编辑命令组只在编图链三页的 ribbon 里。落到非编辑页
  // 时停用活动画布工具——各工具 deactivate() 统一发 abort 信号，约束捕获/
  // 编辑会话经 owner 的 abort 路径拆台（等价 §42.15 的 Esc）。
  if (!kEditingToolPages.contains(pageId) && m_canvasCtl)
    m_canvasCtl->deactivateTool();
}

void PaleoMainWindow::syncBottomDockForTasks()
{
  if (!m_bottomDock)
    return;
  bool anyRunning = false;
  if (m_taskSvc)
    for (const PaleoTask *t : m_taskSvc->tasks())
      // quiet 交互任务（三维切片/剖面解码/预取）不拉起任务中心——交互控件
      // 自带进度语义，弹出反而打断操作。
      anyRunning |= t->running() && !t->quiet();
  if (anyRunning)
  {
    // 有活动任务且底栏藏着 → 程序化露出并切到任务页（不动
    // userWantsVisible）。用户中途手动关掉即尊重其选择，不再反复拉起。
    if (m_currentPage != QLatin1String("correlation") &&
        !m_bottomDockAutoShown && !m_bottomDock->isVisible())
    {
      m_bottomDockAutoShown = true;
      m_bottomDock->setProgrammaticVisible(true);
      if (auto *tabs = findChild<QTabWidget *>(QStringLiteral("bottomTabs")))
        if (auto *panel = findChild<TaskPanel *>(QStringLiteral("taskPanel")))
          tabs->setCurrentWidget(panel);
      m_bottomDock->raise();
    }
  }
  else if (m_bottomDockAutoShown)
  {
    // 任务清空 → 恢复用户原可见态（数据页的纯三栏布局照常隐藏底栏）。
    m_bottomDockAutoShown = false;
    const bool want = m_currentPage != QLatin1String("data") &&
                      m_currentPage != QLatin1String("correlation") &&
                      m_bottomDock->userWantsVisible();
    m_bottomDock->setProgrammaticVisible(want);
  }
}

void PaleoMainWindow::showStartup()
{
  if (m_currentPage == QLatin1String("correlation"))
    restoreCorrelationDocks();
  // 只读工程（另一实例持锁打开）回起始页不得重置只读态——现取 store 真源，
  // 否则编辑工具条重新点亮、进入最终无法保存的编辑会话。
  setProjectReadOnly(m_projectStore && m_projectStore->isReadOnly());
  m_currentPage = QStringLiteral("startup");
  if (m_leftDock) m_leftDock->setProgrammaticVisible(false);
  if (m_rightDock) m_rightDock->setProgrammaticVisible(false);
  if (m_dataListDock)
    m_dataListDock->setProgrammaticVisible(false);
  if (m_centerStack)
    m_centerStack->setCurrentIndex(0);
}
