// 层：视图
// paleomainwindow_attach_validate — 验证与布井辅助接线（W4 拆分段）
#include "paleomainwindow.h"

#include "../qgis/qgiscanvascontroller.h"
#include "../linkage/threewaylocator.h"
#include "../services/previewdoc.h"
#include "../catalog/datacatalog.h"
#include "correlationpanel.h"
#include "datapreview/datapreviewtabs.h"
#include "pages/pagepanels.h"
#include "pages/pageshared.h"
#include "pages/datalist.h"
#include "pages/wellsitingpanel.h"
#include "maptools/sitingpicktool.h"
#include "../workflow/wellsitingworkflow.h"
#include "../workflow/workflows.h"

#include <QFileDialog>
#include <QMessageBox>
#include <QStackedLayout>
#include <QStatusBar>
#include <QTabWidget>

// ---------------------------------------------------------------------------
// 验证页接线：三方定位 + 剖面跳页 + 发布门刷新钩（W4 拆分段）
// ---------------------------------------------------------------------------
void PaleoMainWindow::attachValidatePage(ValidatePage *validatePage,
                                         ValidationWorkflow *validate,
                                         WellCorrelationPanel *corrPanel,
                                         DataImportService *importSvc)
{
  if (validate && validatePage)
  {
    // 问题 → 地图/连井联动（阶段C，预览壳重排）：地图移到井点 + 连井滚到
    // 井/分层；地震测线不再走底栏 gotoLine，由「在数据页看这条剖面」显式
    // 切页打开（threewaylocator.h）。缺面板的字段自动跳过。
    auto *bottomTabs = findChild<QTabWidget *>(QStringLiteral("bottomTabs"));
    auto *threeWay = new ThreeWayLocator(m_canvasCtl, this);
    // 验证定位先挂上声明图层，再缩放。问题行的 layerId 是清单 id，不是临时画布对象。
    connect(validatePage, &ValidatePage::locateRequested, this,
            [this](const QString &layerId, const QString &, const QVariantMap &) {
              revealDeclaredLayer(layerId, false);
            });
    connect(validatePage, &ValidatePage::locateRequested, threeWay,
            &ThreeWayLocator::locate);
    // 联动器的意图信号回壳订阅：底栏切到连井剖面页 + 滚到井分层。
    connect(threeWay, &ThreeWayLocator::bottomTabFocusRequested, this,
            [this, bottomTabs, corrPanel](const QString &tabId) {
              if (tabId == QLatin1String("correlation") && bottomTabs && corrPanel)
              {
                // W4：底栏默认隐藏——定位请求是显式用户意图，连 dock 一起
                // show+raise，不再只切一个看不见的页签。
                if (m_bottomDock)
                {
                  m_bottomDock->show();
                  m_bottomDock->raise();
                }
                bottomTabs->setCurrentWidget(corrPanel);
              }
            });
    connect(threeWay, &ThreeWayLocator::correlationFocusRequested, this,
            [corrPanel](const QString &wellId, const QString &horizon) {
              if (corrPanel)
                corrPanel->scrollToWellTop(wellId, horizon);
            });
    // 「在数据页看这条剖面」：切到数据管理页（预览分栏随之可见），打开/
    // 聚焦地震资产标签并把测线拨到载荷里的 inline/time_ms。载荷没有
    // asset_id——测线号落在哪个 survey 的 inline 范围就开它的链接资产
    // （主关联优先）；一条地震资产都没有就不跳页，不造假定位。
    connect(validatePage, &ValidatePage::seismicSectionRequested, this,
            [this, importSvc](const QVariantMap &payload) {
              if (!m_previewTabs || !importSvc)
                return;
              const int line = payload.value(QStringLiteral("inline"), -1).toInt();
              if (line < 0)
                return;
              const double timeMs =
                  payload.value(QStringLiteral("time_ms"), -1.0).toDouble();
              DataCatalog *cat = m_previewDoc->catalog();
              QString assetId, firstSeismic;
              for (const EntityAssetLink &l : cat->links())
              {
                if (l.unresolved || l.role != QLatin1String("seismic_volume") ||
                    l.assetId.isEmpty())
                  continue;
                if (firstSeismic.isEmpty())
                  firstSeismic = l.assetId;
                const CatalogEntity s = cat->entityById(l.entityId);
                if (s.entityType == QLatin1String("seismic_survey") &&
                    line >= s.inlineMin && line <= s.inlineMax &&
                    (assetId.isEmpty() || l.isPrimary))
                  assetId = l.assetId;
              }
              if (assetId.isEmpty())
                assetId = firstSeismic;
              if (assetId.isEmpty())
                return;
              showPage(QStringLiteral("data"));
              m_previewTabs->openSeismicLine(
                  assetId, QStringLiteral("inline"), line, timeMs);
            });
    // 阶段E 发布门：验证跑完 → 重算逐井残差覆盖（attachMapping 装的钩子，
    // 未装则无事发生）。
    connect(validate, &ValidationWorkflow::validationDone, this,
            [this](int) { if (m_refreshPublishGate) m_refreshPublishGate(); });
  }
}

// ---------------------------------------------------------------------------
// 方向34：井网辅助接线（验证页双页签 + 地图布点 + 导出）
// ---------------------------------------------------------------------------
void PaleoMainWindow::attachWellSiting(WellSitingWorkflow *wf)
{
  m_wellSitingWf = wf;
  if (!wf)
    return;

  // 幂等：页签已建则只刷面板（重挂工程作用域数据）。过滤器注入与
  // plannedWellsChanged→refreshAssetTable 连接也在守卫内——二次调用不叠加。
  if (!m_wellSitingPanel)
  {
    // M2 可见性统一：数据页「计划井」组与 siting 文档 retired 面一致——
    // siting 面删的计划井不滞留数据页；变化即刷资产树（公共槽）。
    if (auto *listPanel = findChild<DataListPanel *>())
    {
      listPanel->setPlannedVisibilityFilter(
          [wf](const QString &id) { return !wf->isPlannedRetired(id); });
      connect(wf, &WellSitingWorkflow::plannedWellsChanged, listPanel,
              [listPanel]() { listPanel->refreshAssetTable(); });
    }

    auto *host = findChild<QWidget *>(QStringLiteral("rightPanelHost"));
    auto *stack = host ? static_cast<QStackedLayout *>(host->layout()) : nullptr;
    if (!stack)
      return;
    const int idx = paleo::pagesinternal::kPageIds.indexOf(QStringLiteral("validate"));
    if (idx < 0 || idx >= stack->count())
      return;
    QLayoutItem *taken = stack->takeAt(idx);
    QWidget *validatePage = taken ? taken->widget() : nullptr;
    delete taken;
    if (!validatePage)
      return;

    auto *tabs = new QTabWidget(host);
    tabs->setObjectName(QStringLiteral("validatePageTabs"));
    tabs->addTab(validatePage, tr("验证"));
    auto *panel = new WellSitingPanel(wf, tabs);
    m_wellSitingPanel = panel;
    tabs->addTab(panel, tr("布井辅助"));
    stack->insertWidget(idx, tabs);
    // 页签不变页义：验证页 ribbon/图层档案映射仍按 "validate" 走。

    // 地图布点意图 → 装拾取工具；拾取点回调面板入 catalog 计划井。
    connect(panel, &WellSitingPanel::mapPlacementRequested, this, [this]() {
      if (!m_canvasCtl || !m_wellSitingPanel)
        return;
      auto *tool = new PaleoSitingPickTool(m_canvasCtl->canvas());
      connect(tool, &PaleoSitingPickTool::pointPicked, this,
              [this](double x, double y) {
                if (m_wellSitingPanel)
                  m_wellSitingPanel->placePlannedAt(x, y);
              });
      m_canvasCtl->setMapTool(tool);
      statusBar()->showMessage(tr("在地图上单击放置计划井（Esc 取消）"), 8000);
    });

    // 导出：面板只报场景，文件对话框由壳统一管。
    connect(panel, &WellSitingPanel::exportRequested, this,
            [this](const QString &kind, const QString &scenarioId) {
              if (!m_wellSitingWf)
                return;
              if (kind == QLatin1String("csv"))
              {
                const QString path = QFileDialog::getSaveFileName(
                    this, tr("导出方案点位表"), QString(), tr("CSV 表 (*.csv)"));
                if (path.isEmpty())
                  return;
                QString err;
                if (!m_wellSitingWf->exportScenarioCsv(scenarioId, path, &err))
                  QMessageBox::warning(this, tr("导出方案点位表"), err);
                else
                  statusBar()->showMessage(tr("已导出：%1").arg(path), 8000);
              }
              else if (kind == QLatin1String("chart"))
              {
                const QString path = QFileDialog::getSaveFileName(
                    this, tr("导出覆盖对比图"), QString(), tr("PNG 图 (*.png)"));
                if (path.isEmpty())
                  return;
                QString err;
                if (!m_wellSitingWf->exportComparisonChart(path, &err))
                  QMessageBox::warning(this, tr("导出覆盖对比图"), err);
                else
                  statusBar()->showMessage(tr("已导出：%1").arg(path), 8000);
              }
            });
  }
  else
  {
    m_wellSitingPanel->reloadFromWorkflow();
  }
}
