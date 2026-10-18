// 层：视图
#include <QApplication>

#include "../domain/faciescatalog.h"
#include "../domain/faciestaxonomy.h"
#include "../domain/facieshierarchy.h"
#include "../domain/singlefactorrequest.h"
#include "../linkage/selectioncontext.h"
#include "../qgis/qgislayerorganizer.h"
#include "pages/faciesconsistencydialog.h"
#include "../qgis/draftautosave.h"
#include "../qgis/facieshierarchyrenderer.h"
#include "../qgis/factorstylewriter.h"
#include "../qgis/mappingartifactwriter.h"
#include "../qgis/qgiscanvascontroller.h"
#include "../qgis/qgislayerprofile.h"
#include "../qgis/qgislayerservice.h"
#include "../qgis/qgisprojectservice.h"
#include "../services/paleotaskservice.h" // QPointer<PaleoTask>::running 需完整类型
#include "../services/previewdoc.h"
#include "../workflow/mappingworkbench.h"
#include "../workflow/evidenceinteraction.h"
#include "datapreview/datapreviewtabs.h"
#include "decorations/paleodecorations.h"
#include "edittools/editingtoolbar.h"
#include "horizonchipbar.h"
#include "notifications/paleonotify.h"
#include "pages/composepage.h"
#include "pages/constraintpage.h"
#include "pages/datapage.h"
#include "pages/datagraphpage.h"
#include "pages/mappingworkbenchpage.h"
#include "pages/mappingreferencepanel.h"
#include "pages/pageshared.h"
#include "pages/predictpage.h"
#include "pages/wellpredictionpanel.h"
#include "paleomainwindow.h"
#include "paleoribbon.h"
#include "taskpanel.h" // 智能预测开跑切到任务页
#include <QAction>
#include <QDockWidget>
#include <QFileDialog>
#include <QInputDialog>
#include <QLabel>
#include <QMenu>
#include <QPointer>
#include <QPushButton>
#include <QStackedLayout>
#include <QStatusBar>
#include <QTabWidget>
#include <QUndoStack>
#include <QVBoxLayout>
#include <algorithm>
#include <memory>
#include <qgslayertree.h>
#include <qgslayertreeview.h>
#include <qgscoordinatetransform.h>
#include <qgsexception.h>
#include <qgsmapcanvas.h>
#include <qgsmapmouseevent.h>
#include <qgsmaptoolidentify.h>
#include <qgsmaptoolpan.h>
#include <qgsproject.h>
#include <qgsrasterlayer.h>
#include <qgsrasterrenderer.h>
#include <qgsvectorlayer.h>

// ---------------------------------------------------------------------------
// #473 拆分：attachWorkbench 入口只留幂等守卫 + 页签构建 + 顺序编排
//（本函数原 1060 行）。各子域编排体逐字平移（行为守恒）：
//   - 证据面板/图谱联动 → attachWorkbenchEvidence（_evidence.cpp）
//   - 井预测修订域（dock/自动保存/右键改相）→ attachWorkbenchWells（_wells.cpp）
//   - 命令分发 lambda 链 → runWorkbenchCommand（_commands.cpp）
//   - 图例/上图/参考窗（显示编排域）→ 下方三个工厂（留在本 TU，含
//     Qt::white 纸面色 token 例外所在行）
// ---------------------------------------------------------------------------
void PaleoMainWindow::attachWorkbench(MappingWorkbench *workbench, EvidenceInteraction *evidence) {
  if (!workbench || property("workbenchAttached").toBool())
    return;
  auto *host = findChild<QWidget *>("rightPanelHost");
  auto *stack = host ? qobject_cast<QStackedLayout *>(host->layout()) : nullptr;
  if (!stack || stack->count() < paleo::pagesinternal::kPageIds.size())
    return;
  setProperty("workbenchAttached", true);
  m_workbench = workbench;
  // G09：工程切换后旧影响报告失效——重算入口必须基于当前工程的快照。
  connect(workbench, &MappingWorkbench::catalogBound, this,
          [this] { m_lastImpact.clear(); });
  // 智能预测在后台执行：开跑即弹出底栏任务中心并切到任务页（底栏原本就
  // 开着也切页），显示任务行与进度；收尾由 syncBottomDockForTasks 恢复。
  connect(workbench, &MappingWorkbench::predictionBusyChanged, this,
          [this](bool busy) {
            if (!busy || !m_bottomDock)
              return;
            if (!m_bottomDock->isVisible()) {
              m_bottomDockAutoShown = true;
              m_bottomDock->setProgrammaticVisible(true);
            }
            if (auto *tabs = findChild<QTabWidget *>(QStringLiteral("bottomTabs")))
              if (auto *panel = findChild<TaskPanel *>(QStringLiteral("taskPanel")))
                tabs->setCurrentWidget(panel);
            m_bottomDock->raise();
          });
  if (auto *chips = findChild<HorizonChipBar *>())
    chips->setAllowEmptyHorizons(true);
  auto *edit = findChild<PaleoEditingToolbar *>("editingToolbar");
  auto *constraint = findChild<ConstraintPage *>();
  QList<MappingWorkbenchPage *> pages;
  int current = stack->currentIndex();
  const QStringList modes{"predict", "constraint", "compose"};
  for (int i = 0; i < modes.size(); ++i) {
    const int index = paleo::pagesinternal::kPageIds.indexOf(modes[i]);
    auto *old = stack->widget(index);
    stack->removeWidget(old);
    auto *tabs = new QTabWidget(host);
    tabs->setObjectName("workbenchTabs." + modes[i]);
    auto *page = new MappingWorkbenchPage(modes[i], workbench, tabs);
    if (modes[i] == QLatin1String("constraint")) {
      tabs->addTab(old, tr("编图流程"));
      tabs->addTab(page, tr("图件与版本"));
    } else {
      tabs->addTab(page, tr("编图流程"));
      tabs->addTab(old, tr("高级工具"));
    }
    stack->insertWidget(index, tabs);
    pages << page;
    // X9：后台索引同步完成后，页面若有未完成的反查则自动重发一次。
    connect(workbench, &MappingWorkbench::evidenceIndexSynced, page,
            [page](const QVariantMap &) { page->reissueReverseQuery(); });
  }
  stack->setCurrentIndex(current);
  // #473：证据面板/图谱联动接线（原 if (evidence) 块逐字平移）。
  if (evidence)
    attachWorkbenchEvidence(evidence, pages);
  if (m_decorMgr) {
    m_decorMgr->setNorthArrowEnabled(true);
    m_decorMgr->setScaleBarEnabled(true);
  }
  // WS-C5：约束页「导入边界…」——面图层进 00_Data 共享置顶区（图层树
  // 布局器摆位），structural_idw 的 boundaryLayerId 从其声明选择。
  if (constraint)
    connect(constraint, &ConstraintPage::boundaryImportRequested, this,
            [this, workbench]() {
              const auto path = QFileDialog::getOpenFileName(
                  this, tr("导入测区边界面图层"), QString(),
                  tr("矢量文件 (*.gpkg *.geojson *.json *.shp)"));
              if (path.isEmpty())
                return;
              QString error;
              if (workbench->importBoundaryLayer(path, &error))
                statusBar()->showMessage(tr("测区边界已导入"), 8000);
              else
                statusBar()->showMessage(
                    error.isEmpty() ? tr("测区边界导入失败") : error, 15000);
            });
  // #473：图例/上图/参考窗三个编排体拆至下方工厂（本 TU）。legend 薄转发
  // 保持默认实参（std::function 不携带默认实参），下游 legend()/legend(id)
  // 调用点零改动。
  const auto legendCore = makeWorkbenchLegend(workbench);
  auto legend = [legendCore](const QString &id = QString()) { legendCore(id); };
  auto shown = std::make_shared<QHash<QString, QString>>();
  if (m_projectSvc) {
    connect(m_projectSvc, &QgisProjectService::projectOpened, this,
            [shown] { shown->clear(); });
  }
  const auto show = makeWorkbenchShow(workbench, legend, shown);
  const auto compare = makeWorkbenchCompare(workbench);
  if (edit) {
    connect(edit->actionAddPolygon(), &QAction::triggered, this, [this, compare] {
      if (currentPage() != QLatin1String("compose")) return;
      // 首次绘面给出入口；用户关闭过参考面板后，不因再次绘面反复拉起。
      if (!findChildren<MappingReferencePanel *>().isEmpty()) return;
      QString error;
      compare(QString(), &error);
      m_canvasCtl->canvas()->setFocus();
    });
  }
  // #473：井预测修订域拆至 attachWorkbenchWells；返回句柄沿用原局部名，
  // 下游 connect 捕获列表零改动。
  const auto wells = attachWorkbenchWells(workbench, edit, pages, legend, show, compare);
  auto *wellPanel = wells.panel;
  auto *wellDock = wells.dock;
  const auto openWells = wells.openWells;
  const auto beginEdit = wells.beginEdit;
  workbench->updateDisplayScale(m_canvasCtl->canvas()->scale());
  connect(m_canvasCtl->canvas(), &QgsMapCanvas::scaleChanged, workbench,
          &MappingWorkbench::updateDisplayScale);
  connect(workbench, &MappingWorkbench::displayChanged, this,
          [this, legend](const QString &id) {
            auto *layer = m_canvasCtl->canvas()->currentLayer();
            if (layer &&
                layer->customProperty("paleoLayerId").toString() == id) {
              legend(id);
              if (m_profileSvc)
                m_profileSvc->captureCurrentAsTheme(
                    QgisLayerProfileService::pageThemeName(currentPage()));
            }
          });
  connect(workbench, &MappingWorkbench::consistencyReady, this,
          [this, show](const QVariantMap &report) {
            auto *dialog = new FaciesConsistencyDialog(this);
            dialog->setReport(report);
            const auto wellLayer = report.value("well_layer").toString();
            connect(
                dialog, &FaciesConsistencyDialog::wellActivated, this,
                [this, show, wellLayer](const QString &, double x, double y) {
                  QString ignored;
                  show(wellLayer, &ignored);
                  QgsPointXY point(x, y);
                  auto *canvas = m_canvasCtl->canvas();
                  if (auto *layer = m_layerSvc->layer(wellLayer)) {
                    try {
                      point = QgsCoordinateTransform(
                                  layer->crs(),
                                  canvas->mapSettings().destinationCrs(),
                                  m_projectSvc->project())
                                  .transform(point);
                    } catch (const QgsCsException &) {
                    }
                  }
                  canvas->setCenter(point);
                  m_canvasCtl->canvas()->refresh();
                });
            dialog->show();
          });
  for (auto *page : pages) {
    page->setHorizon(m_selection->activeHorizon());
    connect(m_selection, &SelectionContext::activeHorizonChanged, page,
            &MappingWorkbenchPage::setHorizon);
    // #473：命令分发 lambda 链拆至 runWorkbenchCommand——捕获面转参数，
    // 捕获列表保持原名原序。
    connect(
        page, &MappingWorkbenchPage::commandRequested, this,
        [this, workbench, page, show, compare, edit, constraint, legend,
         openWells, beginEdit, shown, wellPanel, wellDock](const QString &action, const QVariantMap &p) {
          runWorkbenchCommand(workbench, page, edit, constraint, wellPanel,
                              wellDock, legend, show, compare, openWells,
                              beginEdit, shown, action, p);
        });
  }
  connect(workbench, &MappingWorkbench::polygonizeFinished, this,
          [this, pages](const QString &, const QString &productId,
                        const QString &error, bool cancelled) {
            if (!productId.isEmpty())
              return; // 成功走 productReady 收尾（上图 + 选中）
            const QString message =
                cancelled ? tr("相栅格转相面已取消或已过期（%1）。").arg(error)
                          : tr("相栅格转相面失败：%1").arg(error);
            for (auto *page : pages)
              page->showMessage(message);
            statusBar()->showMessage(message, 10000);
          });
  connect(workbench, &MappingWorkbench::productReady, this,
          [this, show, pages, workbench, openWells,
           wellDock](const QString &h, const QString &id) {
            if (h == m_selection->activeHorizon()) {
              QString error;
              if (!show(id, &error))
                statusBar()->showMessage(error, 10000);
            }
            if (h == m_selection->activeHorizon()) {
              if (workbench->versionForLayer(id).extra.value("result_scope").toString() == "current_formation")
                wellDock->hide();
              else if (!workbench->wellPredictions(id).isEmpty())
                openWells(id);
            }
            for (auto *page : pages) {
              page->refresh();
              if (page->horizon() == h) {
                page->selectLayer(id);
                page->showMessage(tr("已生成图件：%1").arg(workbench->declaration(id).title));
              }
            }
          });
  connect(workbench, &MappingWorkbench::errorOccurred, this,
          [this, pages](const QString &error) {
            statusBar()->showMessage(error, 15000);
            for (auto *page : pages)
              page->showMessage(error);
            PaleoNotify::warning(this, tr("编图操作未完成"), error);
          });
  connect(m_selection, &SelectionContext::activeHorizonChanged, this,
          [legend] { legend(); });
  connect(m_selection, &SelectionContext::activeHorizonChanged, this,
          [this, show, shown, pages](const QString &h) {
            QMetaObject::invokeMethod(
                this,
                [this, show, shown, pages, h] {
                  if (m_selection->activeHorizon() != h || !shown->contains(h))
                    return;
                  QString error;
                  const auto id = shown->value(h);
                  if (show(id, &error))
                    for (auto *page : pages)
                      if (page->horizon() == h)
                        page->selectLayer(id);
                },
                Qt::QueuedConnection);
          });
  if (auto *tree = findChild<QgsLayerTreeView *>())
    connect(tree, &QgsLayerTreeView::currentLayerChanged, this,
            [legend](QgsMapLayer *layer) {
              legend(layer ? layer->customProperty("paleoLayerId").toString()
                           : QString());
            });
  if (edit)
    connect(edit, &PaleoEditingToolbar::editingStopped, this,
            [this, workbench, pages](const QString &nativeId, bool saved) {
              if (!saved)
                return;
              auto *layer = (m_projectSvc && m_projectSvc->project())
                                ? m_projectSvc->project()->mapLayer(nativeId)
                                : nullptr;
              const auto id =
                  layer ? layer->customProperty("paleoLayerId").toString()
                        : QString();
              if (!id.startsWith("draft."))
                return;
              QString error;
              const bool ok = workbench->saveEditingVersion(id, &error);
              statusBar()->showMessage(
                  ok ? tr("画布编辑已保存并生成图件新版本。") : error, 15000);
              for (auto *page : pages)
                page->showMessage(
                    ok ? tr("画布编辑已保存，并登记了新的图件版本。") : error);
            });
  // The visible parameter tab owns the ribbon action's readiness and intent.
  // Remirroring replaces the old event filter and connection atomically.
  struct Binding {
    QString action;
    int page;
    QString command;
    QString legacy;
  };
  const QList<Binding> bindings{
      {"ribbonRunPrediction", 0, "predict", "runButton"},
      {"ribbonDrawConstraint", 1, "draw", "drawButton"},
      {"ribbonRunIdw", 1, "factor", "runIdwButton"},
      {"ribbonFuse", 2, "compose", "fuseButton"},
      {"ribbonPolygonize", 2, "polygonize", "polygonizeButton"},
      {"ribbonSaveVersion", 2, "save", "saveVersionButton"}};
  for (const auto &b : bindings) {
    // 单因素主流程与 ribbon 已绑定同一 ConstraintPage；图件页不替换方法入口。
    if (b.page == 1) continue;
    auto *action = findChild<QAction *>(b.action);
    auto *tabs = findChild<QTabWidget *>("workbenchTabs." + modes[b.page]);
    if (!action || !tabs)
      continue;
    auto *primary = pages[b.page]->commandButton(b.command);
    auto *legacy = tabs->widget(1)->findChild<QPushButton *>(b.legacy);
    action->setMenu(nullptr);
    auto bind = [action, primary, legacy](int index) {
      PaleoRibbon::mirror(action, index == 0 ? primary : legacy, true, false);
    };
    connect(tabs, &QTabWidget::currentChanged, this, bind);
    bind(tabs->currentIndex());
  }
  if (auto *tabs = findChild<QTabWidget *>("workbenchTabs.compose")) {
    for (const auto &name :
         {"ribbonThicknessChain", "ribbonExportPdf", "ribbonPublish"})
      if (auto *action = findChild<QAction *>(name)) {
        action->setVisible(tabs->currentIndex() == 1);
        connect(tabs, &QTabWidget::currentChanged, action,
                [action](int index) { action->setVisible(index == 1); });
      }
  }
  if (auto *category = categoryForPage(QStringLiteral("compose")))
    if (auto *panel = category->findChild<SARibbonPanel *>(QStringLiteral("ribbonPanel.compose.chain"))) {
      auto *action = new QAction(tr("参考证据"), this);
      action->setObjectName(QStringLiteral("ribbonReferenceEvidence"));
      PaleoRibbon::mirror(action, pages[2]->commandButton("referencePanel"), false, false);
      action->setWhatsThis(tr("打开参考证据面板，在面板内添加视图并选择证据，与主图联动对照绘制。"));
      panel->addSmallAction(action);
    }
  legend();
}

// ---------------------------------------------------------------------------
// #473 显示编排域工厂（逐字平移自 attachWorkbench 原地 lambda，行为守恒）：
// 图例 schema/标题组装（legend）、上图互斥与定制绘制序（show）、联动参考
// 窗构造（compare，含参考画布纸面色 token 例外行）。
// ---------------------------------------------------------------------------
PaleoMainWindow::WorkbenchLegendFn PaleoMainWindow::makeWorkbenchLegend(
    MappingWorkbench *workbench) {
  return [this, workbench](const QString &id = QString()) {
    if (!m_decorMgr)
      return;
    const auto h = m_selection->activeHorizon();
    auto v = workbench->versionForLayer(id);
    auto d = workbench->declaration(id);
    const auto legendKind = v.extra.value("kind").toString();
    const auto legendSource = v.extra.value("value_source").toString();
    const bool analysisRaster =
        paleo::singlefactor::isAnalysisFactorRaster(legendKind, legendSource);
    const bool cartographic =
        paleo::singlefactor::rejectsQuantitativeUse(legendKind, legendSource);
    bool product = !v.id.isEmpty() && d.horizon == h &&
                   v.extra.contains("facies") &&
                   !legendKind.startsWith(QLatin1String("constraint")) &&
                   !analysisRaster && !cartographic;
    QVariantList schema;
    if (product) {
      // 旧成果冻结的平级分类也迁进三级体系，图例不出现未定义相。
      FaciesTaxonomy::upgrade(v.extra.value("facies").toList(), &schema);
    } else {
      schema = workbench->facies(h);
    }
    QString title = h.isEmpty() ? tr("请选择层位") : tr("%1 · 三级相图例").arg(h);
    if (product)
      title +=
          tr(" · v%1%2")
              .arg(v.versionNumber)
              .arg(v.extra.value("mock").toBool() ? tr(" Mock") : QString());
    if (analysisRaster || cartographic) {
      schema.clear();
      if (auto *r = qobject_cast<QgsRasterLayer *>(m_layerSvc->layer(id));
          r && r->renderer())
        for (const auto &item : r->renderer()->legendSymbologyItems())
          schema << QVariantMap{{"name", item.first},
                                {"color", item.second.name()}};
      title = cartographic
                  ? tr("%1 · 解释性制图 v%2").arg(h).arg(v.versionNumber)
                  : tr("%1 · 数值图例 v%2").arg(h).arg(v.versionNumber);
    } else if (legendKind.startsWith(QLatin1String("constraint")) ||
               legendKind == QLatin1String("contour_lines")) {
      schema.clear();
      title = tr("%1 · %2").arg(h, legendKind == QLatin1String("contour_lines")
                                       ? tr("等值线")
                                       : tr("约束线"));
    }
    if (product) {
      schema = workbench->displayLegend(id);
      auto *wellLayer = qobject_cast<QgsVectorLayer *>(m_layerSvc->layer(id));
      const bool wellResult = wellLayer && wellLayer->fields().indexOf("facies_intervals") >= 0;
      title = tr("%1 · %2 · v%3")
                  .arg(h, wellResult ? tr("测井预测 · %1").arg(FaciesHierarchy::title(workbench->resolvedLevel(id))) : FaciesHierarchy::title(workbench->resolvedLevel(id)))
                  .arg(v.versionNumber);
      if (v.extra.value("mock").toBool())
        title += tr(" · Mock");
    }
    m_decorMgr->setFaciesLegend(title, h.isEmpty() ? QVariantList() : schema,
                                product ? m_layerSvc->layer(id) : nullptr);
  };
}

PaleoMainWindow::WorkbenchShowFn PaleoMainWindow::makeWorkbenchShow(
    MappingWorkbench *workbench, const WorkbenchLegendFn &legend,
    const std::shared_ptr<QHash<QString, QString>> &shown) {
  return [this, workbench, legend, shown](const QString &id,
                                          QString *error) {
    const auto d = workbench->declaration(id);
    if (!d.horizon.isEmpty() && d.horizon != m_selection->activeHorizon()) {
      if (error)
        *error = tr("请切换至图件所属层位 %1，或打开参考面板").arg(d.horizon);
      return false;
    }
    auto *layer = m_layerSvc->instantiate(id, error);
    if (!layer)
      return false;
    // 初次实例化和编辑/比例尺变化已负责样式；显示缓存图层无需重建。
    auto *canvas = m_canvasCtl->canvas();
    const bool wasFrozen = canvas->isFrozen();
    canvas->freeze(true);
    if (!canvas->mapTool()) {
      auto *pan = canvas->findChild<QgsMapToolPan *>("workbenchPan");
      if (!pan) {
        pan = new QgsMapToolPan(canvas);
        pan->setParent(canvas);
        pan->setObjectName("workbenchPan");
      }
      canvas->setMapTool(pan);
    }
    // Focus one generated result while retaining base data. History stays in
    // the result list and can be compared in independent panels.
    auto *treeRoot = (m_projectSvc && m_projectSvc->project())
                         ? m_projectSvc->project()->layerTreeRoot()
                         : nullptr;
    if (treeRoot) {
      for (const auto &other : m_layerSvc->declared())
        if (other.layerId != id && other.horizon == d.horizon &&
            (other.layerId.startsWith("product.") ||
             other.layerId.startsWith("factor.") ||
             other.layerId.startsWith("contours.") ||
             other.layerId.startsWith("cartographic.") ||
             other.layerId.startsWith("draft."))) {
          if (auto *old = m_layerSvc->layer(other.layerId))
            if (auto *node = treeRoot->findLayer(old->id()))
              node->setItemVisibilityChecked(false);
        }
      if (auto *node = treeRoot->findLayer(layer->id()))
        node->setItemVisibilityCheckedParentRecursive(true);
      if (auto *vector = qobject_cast<QgsVectorLayer *>(layer);
          vector && vector->fields().indexOf("facies_intervals") >= 0) {
        // 共享井位/维护表位于树顶部；显示预测时把本图件放到绘制顺序首位，
        // 否则同坐标的基础井点会盖住预测点及其相标注。
        auto order = treeRoot->layerOrder();
        order.removeAll(layer);
        order.prepend(layer);
        treeRoot->setCustomLayerOrder(order);
        treeRoot->setHasCustomLayerOrder(true);
      } else {
        // 自定义绘制顺序一旦生效，新实例化的产层会被追加到序列表尾
        //（画在最底），落在不透明的离线地形底图/阴影之下即整层不可见。
        // 按树序找位（facies-ux 2）：布局器已把图件收进所属地层组并按组序
        // 摆好，绘制序跟随树序——相面压在同层位相栅格之上、底图之上、
        // 井位等共享矢量之下。
        QgisLayerOrganizer::slotIntoCustomOrder(treeRoot, layer);
      }
    }
    if (auto *tree = findChild<QgsLayerTreeView *>())
      tree->setCurrentLayer(layer);
    canvas->setCurrentLayer(layer);
    if (m_profileSvc)
      m_profileSvc->captureCurrentAsTheme(
          QgisLayerProfileService::pageThemeName(currentPage()));
    m_canvasCtl->zoomToLayer(id);
    canvas->freeze(wasFrozen);
    if (!wasFrozen) canvas->refresh();
    legend(id);
    shown->insert(d.horizon, id);
    return true;
  };
}

PaleoMainWindow::WorkbenchShowFn PaleoMainWindow::makeWorkbenchCompare(
    MappingWorkbench *workbench) {
  Q_UNUSED(workbench); // #516：参考窗口体系由 MappingReferencePanel 承载
  return [this](const QString &id, QString *error) {
    return showMappingReference(id, error);
  };
}
