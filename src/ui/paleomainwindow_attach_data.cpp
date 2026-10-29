// 层：视图
// paleomainwindow_attach_data — 数据管理页接线（W4 拆分段）
#include "paleomainwindow.h"
#include "pages/datagraphpage.h"
#include "taskpanel.h"

#include "paleotheme.h"
#include "../qgis/qgiscanvascontroller.h"
#include "../qgis/qgisprojectservice.h"
#include "../services/toolavailability.h"  // 导入门控取禁令理由需完整类型
#include "../qgis/qgislayerservice.h"
#include "../linkage/selectioncontext.h"
#include "../domain/projectclassifier.h"
#include "../services/previewdoc.h"
#include "../catalog/datacatalog.h"
#include "dialogs/folderconfirm.h"
#include "dialogs/importsourcedialog.h"
#include "dialogs/offlinebasemapdialog.h"
#include "correlationpanel.h"
#include "datapreview/datapreviewtabs.h"
#include "pages/pagepanels.h"
#include "pages/dataops/dataopsimportqueue.h"
#include "pages/datalist.h"
#include "../workflow/folderimport.h"

#include <qgsmapcanvas.h>
#include <qgsproject.h>
#include <qgsmaplayer.h>
#include <qgslayertree.h>
#include <qgslayertreelayer.h>
#include <qgsmessagelog.h>

#include <QDialog>
#include <QDialogButtonBox>
#include <QFileDialog>
#include <QLabel>
#include <QPushButton>
#include <QStatusBar>
#include <QTabWidget>
#include <QTimer>
#include <QVBoxLayout>
#include <QComboBox>
#include <QCheckBox>
#include <QFileInfo>
#include <QLineEdit>

// ---------------------------------------------------------------------------
// 数据页接线：门面绑定 / catalog 联动 / 预览分栏 / 导入意图（W4 拆分段）
// ---------------------------------------------------------------------------
void PaleoMainWindow::attachDataProjectOpen(DataPage *dataPage)
{
  if (!m_projectSvc) return;
  dataPage->setProjectService(m_projectSvc);
  connect(m_projectSvc, &QgisProjectService::openFinished, this, [this](bool success) {
    if (!success) return;
    const auto session = m_projectSvc->sessionId();
    QTimer::singleShot(16, this, [this, session] {
      if (m_projectSvc && m_projectSvc->sessionId() == session) {
        // #481 P3-3：自动装载记账随会话重置（新工程重扫）。
        m_seismicAutoLoadedPaths.clear();
        syncSeismicVolumeToDocks(false);
        probeSurveyBoundsAsync();
      }
    });
  });
}

// 测区角点补探（轻量道头探测，规则网格秒回）：数据服务投影不带角点时
// 目录实体 corners 为空，看板/全景首开只能画井位并集框——开工程后探一次
// 并写回实体（成功回调里 catalog 已更新，页面随 changed 信号刷新）。
// 空回调：只为写回，无页面待更新。
void PaleoMainWindow::probeSurveyBoundsAsync()
{
  DataCatalog *cat = m_previewDoc ? m_previewDoc->catalog() : nullptr;
  if (!cat || !cat->isOpen())
    return;
  for (const CatalogAsset &asset : cat->assets())
    if (asset.type == QLatin1String("seismic")) {
      m_previewDoc->requestSurveyBounds(asset.id, {});
      break;
    }
}

// 数据页导入按钮的门控开关（#481 P2-4）：禁用存 tooltip、启用原样还原；
// 与工程门取与——目录禁令解除≠全局门放开，谁的禁令用谁的理由。
void PaleoMainWindow::setImportToolsEnabled(bool enabled)
{
  bool toolsOk = true;
  QString toolsReason;
  if (enabled && m_tools)
    toolsOk = m_tools->check(QString(), &toolsReason);
  for (const char *name : {"importAddData", "importOfflineBasemap", "importWells", "importWellLogs", "importSeismic",
                           "importBoundary", "importFolder", "importFiles", "importPaleomap", "importPaleomapFolder"})
    if (auto *btn = findChild<QPushButton *>(QLatin1String(name)))
    {
      if (enabled && toolsOk)
      {
        btn->setEnabled(true);
        if (btn->property("paleoGateSavedTip").isValid())
        {
          btn->setToolTip(btn->property("paleoGateSavedTip").toString());
          btn->setProperty("paleoGateSavedTip", QVariant());
        }
      }
      else
      {
        if (!btn->property("paleoGateSavedTip").isValid())
          btn->setProperty("paleoGateSavedTip", btn->toolTip());
        btn->setEnabled(false);
        btn->setToolTip(enabled && !toolsOk
                            ? toolsReason
                            : tr("数据目录打开失败 — 导入暂不可用（见状态栏）"));
      }
    }
}

void PaleoMainWindow::attachDataPage(DataPage *dataPage,
                                     WellCorrelationPanel *corrPanel,
                                     DataImportService *importSvc,
                                     PaleoTaskService *taskSvc)
{
  attachPaleomapLibrary(dataPage);
  attachDataProjectOpen(dataPage);
  // Panel intents → workflows / selection. Params stay minimal for the shell
  // milestone — full parameter dialogs are per-panel follow-up work.
  if (importSvc && dataPage)
  {
    // §3/§4 数据契约接线：数据页绑定导入服务，资产表跟 catalog 走，
    // 列表选中在中央预览标签（地图下方分栏）打开（确认入库后才开标签）。
    dataPage->setProperty("paleo.page.importsvc", QVariant::fromValue<QObject *>(m_previewDoc));
    connect(m_previewDoc->catalog(), &DataCatalog::changed, this,
            [this, dataPage]() {
              QMetaObject::invokeMethod(dataPage, "refreshAssetTable");
              syncSeismicVolumeToDocks(false);
            });
    dataPage->refreshAssetTable();
    syncSeismicVolumeToDocks(false);
    // B2（wave/deepen-perf）：导入队列生产 runner（GAPS G-2.3 收口）——串行
    // 驱动 + loud 任务池逐文件导入 + 取消/重试状态机。面板持有 runner 闭包
    //（shared_ptr Hub 续命，GUI 线程投递），适配器本体随本栈析构不悬空。
    if (taskSvc && dataPage->listPanel())
    {
      paleo::dataops::FolderImportQueueAdapter importRunner(m_previewDoc, taskSvc);
      importRunner.attach(dataPage->listPanel()->importQueuePanel());
    }
    // D6 地图→表联动：画布上拾取的实体（WellMapLink → ctx）→ 资产表选中
    // 其已决关联的行；选中走同一条 assetActivated → 预览照开。
    if (m_selection)
      connect(m_selection, &SelectionContext::selectionChanged, dataPage,
              [dataPage](const QStringList &ids, const QString &origin) {
                if (origin != QLatin1String("datapreview") && origin != QLatin1String("datatree"))
                  dataPage->selectAssetsForEntities(ids);
              });

    // ---- T20 余项：catalogOpenFailed 的状态栏露出 ----
    // 常驻红胶囊（DESIGN.md error token）写打开失败原因；恢复（工程重开且
    // catalog 打开成功）前，数据页的导入类动作保持禁用——失败的 catalog 拒
    // 绝写入，导入必然失败，不给用户一个假入口。
    auto *catalogError =
        PaleoTheme::capsuleLabel(QString(), PaleoTheme::CapsuleKind::Error, this);
    catalogError->setObjectName(QStringLiteral("statusCatalogError"));
    catalogError->hide();
    statusBar()->addPermanentWidget(catalogError);
    // #481 P2-4：与 attach.cpp 的 applyProjectGate 同一套 tooltip 存还纪律
    //（门态存 paleoGateSavedTip、开门还原），且开门与工程门取与——目录禁令
    // 解除≠全局门放开，谁的禁令用谁的理由（旧实现开门清空原 tooltip、不
    // 看工程门就 enable）。
    const auto setImportsEnabled = [this](bool enabled) { setImportToolsEnabled(enabled); };
    connect(m_previewDoc, &PreviewDocService::catalogOpenFailed, this,
            [catalogError, setImportsEnabled](const QString &error) {
              catalogError->setText(tr("数据目录打开失败：%1").arg(error));
              catalogError->show();
              setImportsEnabled(false);
            });
    // 恢复：projectOpened 后（AppContext 已同步重设 projectDir）错误清空 →
    // 收告警、放开导入。
    if (m_projectSvc)
      connect(m_projectSvc, &QgisProjectService::projectOpened, this,
              [this, dataPage, importSvc, catalogError, setImportsEnabled](const QString &) {
                if (m_previewTabs && m_projectSvc)
                  m_previewTabs->setProject(m_projectSvc->project());
                if (m_previewDoc->catalogOpenError().isEmpty())
                {
                  catalogError->hide();
                  setImportsEnabled(true);
                }
                else
                  setImportsEnabled(false); // 换了个工程仍然失败 → 保持禁用
                dataPage->refreshAssetTable();
                syncSeismicVolumeToDocks(false);
              });
    DataPreviewTabs *preview = m_previewTabs;
    if (preview)
    {
      preview->setDocService(m_previewDoc); connect(dataPage->listPanel(), &DataListPanel::assetAssociationChanged, preview, &DataPreviewTabs::refreshAssetAssociations);
      preview->setTaskService(taskSvc); // D1：剖面索引/解码异步化（nullptr 时保持同步）
      const auto openGraph = [this](const QString &asset, const QString &version) {
        openDataGraph(asset, version);
      };
      connect(dataPage, &DataPage::graphRequested, this, openGraph);
      connect(dataPage, &DataPage::entityGraphRequested, this,
              [this](const QString &entity, const QString &asset, const QString &version) { openDataGraph(asset, version, entity); });
      connect(preview, &DataPreviewTabs::graphRequested, this, openGraph);
      connect(dataPage->graphPage(), &DataGraphPage::taskCenterRequested,
              this, &PaleoMainWindow::openTaskCenter);
      connect(dataPage->graphPage(), &DataGraphPage::managementRequested, this,
              [this, dataPage](const QString &action, const QString &asset, const QString &version) {
        auto *list = dataPage->listPanel();
        if (!asset.isEmpty()) {
          if (!m_previewDoc || m_previewDoc->catalog()->assetById(asset).id.isEmpty()) return;
          dataPage->selectAsset(asset);
          if (!version.isEmpty()) dataPage->focusVersion(asset, version);
          if (list->currentAssetSelection() != QSet<QString>{asset}) {
            statusBar()->showMessage(tr("所选资料已移入回收清单，无法执行文件操作"), 8000); return;
          }
        }
        if (action == "health") list->showHealthCheck();
        else if (action == "recycle") list->showRecycleBin();
        else if (action == "imports") list->showImportLedger();
        else if (action == "storage") list->showStorageGovernance();
        else if (!asset.isEmpty() && action == "versions") list->showVersionTable();
        else if (!asset.isEmpty() && action == "role") list->editRoleForSelection();
        else if (!asset.isEmpty() && action == "tag") list->batchAddTag();
        else if (!asset.isEmpty() && action == "remove") list->batchRemoveSoft();
      });
      attachProjectDashboard(dataPage, preview);
      // D11 临时配准：GeoJSON 标签手工仿射 → DERIVED + 水印图层。
      connect(preview, &DataPreviewTabs::provisionalRegistrationRequested, this,
              [this, importSvc](const QString &assetId, const QVariantMap &params) {
                applyProvisionalRegistration(importSvc, assetId, params);
              });
      connect(dataPage, &DataPage::versionActivated, preview, &DataPreviewTabs::openVersion);
      connect(preview, &DataPreviewTabs::versionContextChanged, dataPage, &DataPage::focusVersion);
      connect(dataPage, &DataPage::assetActivated, preview, &DataPreviewTabs::openAsset);
      connect(dataPage, &DataPage::assetActivated, dataPage,
              [preview, dataPage](const QString &assetId) {
                dataPage->selectAsset(assetId);
                // 已有预览可能保留历史版本；列表重选不能用 latest 覆盖它。
                auto *tabs = preview->findChild<QTabWidget *>(QStringLiteral("dataPreviewTabs"));
                if (tabs && preview->assetIdAt(tabs->currentIndex()) == assetId)
                {
                  const QString vid = preview->versionIdAt(tabs->currentIndex());
                  if (!vid.isEmpty()) dataPage->focusVersion(assetId, vid);
                }
              });
      connect(dataPage, &DataPage::assetWellActivated, preview, &DataPreviewTabs::openAssetForWell);
      connect(dataPage, &DataPage::modelWellChartRequested, preview, &DataPreviewTabs::openModelWell);
      connect(dataPage, &DataPage::seismicLineActivated, preview,
              [preview](const QString &aid, const QString &mode) {
                preview->openSeismicLine(aid, mode, 0, 0.0);
              });

      connect(preview, &DataPreviewTabs::requestShowOnMainCanvas, this, [this]() {
        if (m_workspaceStack)
          m_workspaceStack->setCurrentIndex(0);
      });
      if (auto *inner = preview->findChild<QTabWidget *>(QStringLiteral("dataPreviewTabs")))
      {
        connect(inner, &QTabWidget::currentChanged, this, [preview, dataPage](int idx) {
          if (idx >= 0 && preview && dataPage && !preview->property("paleo.versionNavigation").toBool())
          {
            const QString aid = preview->assetIdAt(idx);
            if (!aid.isEmpty())
            {
              const QString vid = preview->versionIdAt(idx);
              if (!vid.isEmpty()) dataPage->focusVersion(aid, vid);
              else dataPage->selectAsset(aid);
            }
          }
        });
      }
      // well_head 预览 / 井树选中 → 地图高亮该井（§4；Direction B 经 SelectionContext）。
      if (m_selection)
      {
        connect(dataPage, &DataPage::wellSelected, this,
                [this](const QString &wellEntityId) {
                  m_selection->setSelection({wellEntityId}, QStringLiteral("datatree"));
                });
        connect(preview, &DataPreviewTabs::wellSelected, this,
                [this, dataPage](const QString &wellEntityId) {
                  dataPage->focusWell(wellEntityId); m_selection->setSelection({wellEntityId}, QStringLiteral("datapreview"));
                });
      }
      // horizon 预览「在地图上显示」（§4/T29 双向同步）：实例化派生栅格 →
      // 缩放到该图层 → 闪烁定位 ~400ms → 勾上图层树节点 → 按钮置「已在
      // 地图上」；图层树里取消勾选时按钮态跟随（node visibilityChanged，
      // QGIS 4：可见性归图层树管，不在 QgsMapLayer 上）。
      if (m_layerSvc)
        connect(preview, &DataPreviewTabs::showHorizonOnMapRequested, this,
                [this, preview](const QString &layerId) {
                  QString err;
                  QgsMapLayer *layer = m_layerSvc->instantiate(layerId, &err);
                  if (!layer)
                  {
                    QgsMessageLog::logMessage(tr("在地图上显示失败：%1").arg(err),
                                              QStringLiteral("Paleo"), Qgis::Critical);
                    return;
                  }
                  if (m_canvasCtl)
                  {
                    m_canvasCtl->zoomToLayer(layerId);
                    flashHorizonLayer(layer);
                  }
                  QgsProject *proj = m_projectSvc ? m_projectSvc->project() : nullptr;
                  QgsLayerTree *treeRoot = proj ? proj->layerTreeRoot() : nullptr;
                  if (QgsLayerTreeLayer *node =
                          treeRoot ? treeRoot->findLayer(layer->id()) : nullptr)
                  {
                    node->setItemVisibilityChecked(true); // 显示意图（可能已在）
                    // 重复点击同一图层不叠加 connect：节点属性作去重标记
                    // （UniqueConnection 只支持成员函数槽，lambda 不可用）。
                    if (!node->property("paleo.visSync").toBool())
                    {
                      node->setProperty("paleo.visSync", true);
                      connect(node, &QgsLayerTreeNode::visibilityChanged, preview,
                              [preview, layerId](QgsLayerTreeNode *n) {
                                preview->setHorizonOnMap(
                                    layerId, n->itemVisibilityChecked());
                              });
                    }
                  }
                  preview->setHorizonOnMap(layerId, true);
                });
    }
    wireDataImportRequests(dataPage, importSvc, preview);
  }
    // Imported assets feed the bottom-dock correlation panel and the central
    // preview tabs (§4: the preview tab opens only after the import is
    // confirmed; 地震预览不再走底栏面板，预览壳直接按资产渲染测线控件)。
    DataPreviewTabs *previewForImport = m_previewTabs;
    // m_previewDoc 只在上方 importSvc 分支里创建；无导入服务的壳在此处
    // connect(nullptr)——产生空连接告警且接线静默失效，一并挪进守卫。
    if (m_previewDoc)
      connect(m_previewDoc, &PreviewDocService::assetImported, this,
              [this, corrPanel, previewForImport](const QString &kind, const QString &assetId, const QString &) {
                // 文件夹导入期间不逐文件开标签——确认后只开井口标签（§4）。
                if (previewForImport && !m_folderImportActive)
                  previewForImport->openAsset(assetId);
                // LAS imports pull the GR curve into the column when present
                // (#156：井集与工程打开同一来源——catalog 全部 well_log)。
                if (corrPanel && kind == QLatin1String("well_log"))
                  refreshCorrelationWells();
              });
}

void PaleoMainWindow::openDataGraph(const QString &asset, const QString &version,
                                   const QString &entity, bool lineage) {
  auto *dataPage = findChild<DataPage *>();
  if (!dataPage || !m_previewTabs || !m_previewDoc || !m_previewDoc->catalog()) return;
  auto *catalog = m_previewDoc->catalog();
  if ((!version.isEmpty() && catalog->versionById(version).id.isEmpty()) ||
      (!asset.isEmpty() && catalog->assetById(asset).id.isEmpty()) ||
      (!entity.isEmpty() && catalog->entityById(entity).id.isEmpty())) {
    statusBar()->showMessage(tr("关联资料已移除或不属于当前工区"), 8000); return;
  }
  showPage(QStringLiteral("data"));
  if (!version.isEmpty()) dataPage->focusVersion(catalog->versionById(version).assetId, version);
  else if (!asset.isEmpty()) dataPage->selectAsset(asset);
  else if (!entity.isEmpty()) dataPage->selectAssetsForEntities({entity});
  auto *graph = dataPage->graphPage();
  graph->setPresentation(dataPage->listPanel()->graphPresentation());
  m_previewTabs->openWorkspace(graph, tr("资料图谱"));
  if (lineage) graph->openLineage(asset, version);
  else graph->openRelations(asset, version, entity);
  if (m_workspaceStack) m_workspaceStack->setCurrentWidget(m_previewTabs);
}
void PaleoMainWindow::openTaskCenter() {
  if (!m_bottomDock) return;
  m_bottomDockAutoShown = false;
  m_bottomDock->setProgrammaticVisible(true);
  if (auto *tabs = findChild<QTabWidget *>(QStringLiteral("bottomTabs")))
    if (auto *panel = findChild<TaskPanel *>(QStringLiteral("taskPanel"))) {
      tabs->setCurrentWidget(panel); panel->refresh();
    }
  m_bottomDock->raise();
}

void PaleoMainWindow::wireDataImportRequests(DataPage *dataPage, DataImportService *importSvc, DataPreviewTabs *preview) {
    connect(dataPage, &DataPage::importRequested, this,
            [this, importSvc, preview](const QString &kind) {
              if (kind.startsWith(QLatin1String("paleomap"))) return;
              if (kind == QLatin1String("offline_basemap")) { openOfflineBasemapImport(m_projectSvc, m_layerSvc, m_taskSvc, this); return; }
              if (!m_projectSvc || m_projectSvc->projectPath().isEmpty() || isProjectReadOnly() || m_projectSvc->isOpening()) return;
              const auto session = m_projectSvc->sessionId();
              ImportSourceDialog source(kind, this);
              if (source.exec() != QDialog::Accepted) return;
              if (m_projectSvc->sessionId() != session) { PaleoNotify::warning(this, tr("导入资料"), tr("工程已变化，请重新选择资料。")); return; }
              if (!source.folder().isEmpty()) { runFolderImportAt(importSvc, source.folder(), kind); return; }
              const auto picked = source.files();
              if (picked.isEmpty()) return;
              // Single files share the batch preview, ownership, hash and retry contract.
              runFilesImportAt(importSvc, picked, kind);
            });
}

void PaleoMainWindow::attachProjectDashboard(DataPage *dataPage, DataPreviewTabs *preview)
{
  connect(dataPage, &DataPage::surveyAreaActivated, preview, &DataPreviewTabs::openSurveyArea);
  connect(dataPage, &DataPage::projectDashboardActivated, preview, &DataPreviewTabs::openProjectDashboard);
  if (!m_projectSvc) return;
  preview->setProject(m_projectSvc->project());
  const auto schedule = [this, dataPage, preview] {
    const auto session = m_projectSvc->sessionId();
    const auto page = currentPage();
    // projectOpened 的同步槽先完成目录绑定；新建、同步打开与后台打开共用此入口。
    QTimer::singleShot(0, this, [this, dataPage, preview, session, page] {
      if (!m_projectSvc || m_projectSvc->sessionId() != session || m_projectSvc->projectPath().isEmpty()) return;
      if (preview->tabCount() > 0 || currentPage() != page) return; // 保留显式导航。
      showPage(QStringLiteral("data"));
      preview->openProjectDashboard();
      dataPage->selectAsset(QStringLiteral("project_dashboard"));
    });
  };
  connect(m_projectSvc, &QgisProjectService::projectOpened, this, schedule);
  if (!m_projectSvc->projectPath().isEmpty()) schedule();
}
