// 层：视图
#include "../linkage/selectioncontext.h"
#include "../qgis/factorstylewriter.h"
#include "../qgis/mappingartifactwriter.h"
#include "../qgis/qgiscanvascontroller.h"
#include "../qgis/qgislayerprofile.h"
#include "../qgis/qgislayerservice.h"
#include "../qgis/qgisprojectservice.h"
#include "../workflow/mappingworkbench.h"
#include "decorations/paleodecorations.h"
#include "edittools/editingtoolbar.h"
#include "horizonchipbar.h"
#include "pages/composepage.h"
#include "pages/constraintpage.h"
#include "pages/mappingworkbenchpage.h"
#include "pages/predictpage.h"
#include "paleomainwindow.h"
#include "paleoribbon.h"
#include <QAction>
#include <QDialog>
#include <QFileDialog>
#include <QLabel>
#include <QPointer>
#include <QPushButton>
#include <QStackedLayout>
#include <QStatusBar>
#include <QTabWidget>
#include <QVBoxLayout>
#include <memory>
#include <qgslayertree.h>
#include <qgslayertreeview.h>
#include <qgsmapcanvas.h>
#include <qgsmaptoolpan.h>
#include <qgsproject.h>
#include <qgsrasterlayer.h>
#include <qgsrasterrenderer.h>
#include <qgsvectorlayer.h>

void PaleoMainWindow::attachWorkbench(MappingWorkbench *workbench) {
  if (!workbench || property("workbenchAttached").toBool())
    return;
  auto *host = findChild<QWidget *>("rightPanelHost");
  auto *stack = host ? qobject_cast<QStackedLayout *>(host->layout()) : nullptr;
  if (!stack || stack->count() < 5)
    return;
  setProperty("workbenchAttached", true);
  if (auto *chips = findChild<HorizonChipBar *>())
    chips->setAllowEmptyHorizons(true);
  auto *edit = findChild<PaleoEditingToolbar *>("editingToolbar");
  auto *constraint = findChild<ConstraintPage *>();
  QList<MappingWorkbenchPage *> pages;
  int current = stack->currentIndex();
  const QStringList modes{"predict", "constraint", "compose"};
  for (int i = 0; i < modes.size(); ++i) {
    auto *old = stack->widget(i + 1);
    stack->removeWidget(old);
    auto *tabs = new QTabWidget(host);
    tabs->setObjectName("workbenchTabs." + modes[i]);
    auto *page = new MappingWorkbenchPage(modes[i], workbench, tabs);
    tabs->addTab(page, tr("编图流程"));
    tabs->addTab(old, tr("高级工具"));
    stack->insertWidget(i + 1, tabs);
    pages << page;
  }
  stack->setCurrentIndex(current);
  if (m_decorMgr) {
    m_decorMgr->setNorthArrowEnabled(true);
    m_decorMgr->setScaleBarEnabled(true);
  }
  auto legend = [this, workbench](const QString &id = QString()) {
    if (!m_decorMgr)
      return;
    const auto h = m_selection->activeHorizon();
    auto v = workbench->versionForLayer(id);
    auto d = workbench->declaration(id);
    bool product = !v.id.isEmpty() && d.horizon == h &&
                   v.extra.contains("facies") &&
                   !v.extra.value("kind").toString().startsWith("constraint") &&
                   v.extra.value("kind") != "single_factor_raster";
    auto schema =
        product ? v.extra.value("facies").toList() : workbench->facies(h);
    QString title = h.isEmpty() ? tr("请选择层位") : tr("%1 · 相图例").arg(h);
    if (product)
      title +=
          tr(" · v%1%2")
              .arg(v.versionNumber)
              .arg(v.extra.value("mock").toBool() ? tr(" Mock") : QString());
    if (v.extra.value("kind") == "single_factor_raster") {
      schema.clear();
      if (auto *r = qobject_cast<QgsRasterLayer *>(m_layerSvc->layer(id));
          r && r->renderer())
        for (const auto &item : r->renderer()->legendSymbologyItems())
          schema << QVariantMap{{"name", item.first},
                                {"color", item.second.name()}};
      title = tr("%1 · 数值图例 v%2").arg(h).arg(v.versionNumber);
    } else if (v.extra.value("kind").toString().startsWith("constraint") ||
               v.extra.value("kind") == "contour_lines") {
      schema.clear();
      title = tr("%1 · %2").arg(h, v.extra.value("kind") == "contour_lines"
                                       ? tr("等值线")
                                       : tr("约束线"));
    }
    m_decorMgr->setFaciesLegend(title, h.isEmpty() ? QVariantList() : schema);
  };
  auto shown = std::make_shared<QHash<QString, QString>>();
  connect(m_projectSvc, &QgisProjectService::projectOpened, this,
          [shown] { shown->clear(); });
  auto show = [this, workbench, legend, shown](const QString &id,
                                               QString *error) {
    const auto d = workbench->declaration(id);
    if (!d.horizon.isEmpty() && d.horizon != m_selection->activeHorizon()) {
      if (error)
        *error = tr("请切换至图件所属层位 %1，或打开参考窗口").arg(d.horizon);
      return false;
    }
    auto *layer = m_layerSvc->instantiate(id, error);
    if (!layer)
      return false;
    workbench->styleLayer(id);
    auto *canvas = m_canvasCtl->canvas();
    // Focus one generated result while retaining base data. History stays in
    // the result list and can be compared in independent windows.
    for (const auto &other : m_layerSvc->declared())
      if (other.layerId != id && other.horizon == d.horizon &&
          (other.layerId.startsWith("product.") ||
           other.layerId.startsWith("factor.") ||
           other.layerId.startsWith("contours.") ||
           other.layerId.startsWith("draft."))) {
        if (auto *old = m_layerSvc->layer(other.layerId))
          if (auto *node = m_projectSvc->project()->layerTreeRoot()->findLayer(
                  old->id()))
            node->setItemVisibilityChecked(false);
      }
    if (auto *node =
            m_projectSvc->project()->layerTreeRoot()->findLayer(layer->id()))
      node->setItemVisibilityCheckedParentRecursive(true);
    if (auto *tree = findChild<QgsLayerTreeView *>())
      tree->setCurrentLayer(layer);
    canvas->setCurrentLayer(layer);
    if (m_profileSvc)
      m_profileSvc->captureCurrentAsTheme(
          QgisLayerProfileService::pageThemeName(currentPage()));
    m_canvasCtl->zoomToLayer(id);
    canvas->refresh();
    legend(id);
    shown->insert(d.horizon, id);
    return true;
  };
  auto compare = [this, workbench](const QString &id, QString *error) {
    auto d = workbench->declaration(id);
    QgsMapLayer *layer =
        d.type == "raster"
            ? static_cast<QgsMapLayer *>(new QgsRasterLayer(d.source, d.title))
            : static_cast<QgsMapLayer *>(
                  new QgsVectorLayer(d.source, d.title, "ogr"));
    if (!layer->isValid()) {
      delete layer;
      if (error)
        *error = tr("参考图件无法读取");
      return false;
    }
    MappingArtifactWriter::restoreRasterCrs(layer);
    auto *dialog = new QDialog(this, Qt::Window);
    dialog->setAttribute(Qt::WA_DeleteOnClose);
    dialog->setObjectName("mappingReferenceWindow");
    dialog->setWindowTitle(tr("参考 · %1 · %2").arg(d.horizon, d.title));
    dialog->resize(640, 480);
    layer->setParent(dialog);
    auto *layout = new QVBoxLayout(dialog);
    auto *caption = new QLabel(
        tr("%1 · %2\n独立视图：切换主图层位后仍保留；滚轮缩放，拖动平移。")
            .arg(d.horizon, d.title),
        dialog);
    caption->setWordWrap(true);
    layout->addWidget(caption);
    auto *canvas = new QgsMapCanvas(dialog);
    canvas->setObjectName("referenceCanvas");
    layer->setParent(
        canvas); // keep the layer alive until the renderer has stopped
    canvas->setDestinationCrs(layer->crs());
    canvas->setCanvasColor(Qt::white);
    canvas->setLayers({layer});
    auto extent = layer->extent();
    if (extent.width() < 1 || extent.height() < 1)
      extent.grow(100);
    canvas->setExtent(extent);
    layout->addWidget(canvas, 1);
    canvas->setMapTool(new QgsMapToolPan(canvas));
    auto v = workbench->versionForLayer(id);
    auto f = v.extra.value("facies", workbench->facies(d.horizon)).toList();
    if (!v.extra.value("kind").toString().startsWith("constraint") &&
        v.extra.value("kind") != "single_factor_raster")
      MappingArtifactWriter::applyFaciesStyle(layer, f);
    else {
      f.clear();
      if (v.extra.value("kind") == "single_factor_raster")
        if (auto *r = qobject_cast<QgsRasterLayer *>(layer)) {
          FactorStyleWriter::applyTo(r, v.extra.value("factor_id").toString());
          if (r->renderer())
            for (const auto &item : r->renderer()->legendSymbologyItems())
              f << QVariantMap{{"name", item.first},
                               {"color", item.second.name()}};
        }
    }
    auto *decor = new PaleoDecorationManager(canvas);
    decor->setNorthArrowEnabled(true);
    decor->setScaleBarEnabled(true);
    decor->setFaciesLegend(d.horizon + tr(" · 参考%1")
                                           .arg(v.extra.value("mock").toBool()
                                                    ? tr(" Mock")
                                                    : QString()),
                           f);
    connect(dialog, &QDialog::finished, canvas, [canvas] {
      canvas->stopRendering();
      canvas->setLayers({});
    });
    dialog->show();
    canvas->refresh();
    return true;
  };
  for (auto *page : pages) {
    page->setHorizon(m_selection->activeHorizon());
    connect(m_selection, &SelectionContext::activeHorizonChanged, page,
            &MappingWorkbenchPage::setHorizon);
    connect(
        page, &MappingWorkbenchPage::commandRequested, this,
        [this, workbench, page, show, compare, edit, constraint,
         legend](const QString &action, const QVariantMap &p) {
          QString error;
          bool ok = true;
          const auto h = p.value("horizon").toString(),
                     id = p.value("layer").toString();
          const auto inputs = p.value("inputs").toStringList();
          if (action == "predict")
            ok = workbench->predict(h, p.value("kind").toString(), inputs,
                                    &error);
          else if (action == "cancel")
            workbench->cancelPrediction();
          else if (action == "show")
            ok = show(id, &error);
          else if (action == "compare")
            ok = compare(id, &error);
          else if (action == "polygonize")
            ok = !workbench->polygonize(id, &error).isEmpty();
          else if (action == "copy") {
            const auto draft = workbench->copyForEditing(id, inputs, &error);
            ok = !draft.isEmpty();
            if (ok && edit) {
              show(draft, &error);
              edit->refreshFromProject();
              edit->setCurrentLayer(
                  qobject_cast<QgsVectorLayer *>(m_layerSvc->layer(draft)));
              edit->actionVertexEdit()->trigger();
            }
          } else if (action == "save") {
            auto *vector =
                qobject_cast<QgsVectorLayer *>(m_layerSvc->layer(id));
            if (vector && vector->isEditable()) {
              if (edit && edit->currentLayer() == vector) {
                if (edit->saveEditing())
                  return;
                ok = false;
                error = tr("画布编辑未能提交，请检查编辑提示");
              } else {
                ok = false;
                error = tr("请先保存正在编辑的图层");
              }
            } else
              ok = workbench->saveEditingVersion(id, &error);
          } else if (action == "import") {
            const auto path = QFileDialog::getOpenFileName(
                this, tr("导入局部米制约束线"), QString(),
                tr("矢量文件 (*.gpkg *.geojson *.json *.shp)"));
            if (path.isEmpty())
              return;
            ok = workbench->importConstraints(h, path, &error);
          } else if (action == "draw") {
            if (constraint) {
              constraint->drawConstraintRequested(h, "line", -1);
              page->showMessage(
                  tr("在画布逐点绘制约束线，右键结束；Esc 取消。"));
              return;
            } else {
              ok = false;
              error = tr("约束绘制工具不可用");
            }
          } else if (action == "factor")
            ok = workbench->generateFactor(h, p.value("factor").toString(), p,
                                           &error);
          else if (action == "contours")
            ok = workbench->generateContours(
                h, id, p.value("interval").toDouble(), &error);
          else if (action == "compose")
            ok = !workbench->compose(h, inputs, p, &error).isEmpty();
          else if (action == "schema") {
            ok = workbench->saveFacies(h, p.value("facies").toList(), &error);
            auto *layer = m_canvasCtl->canvas()->currentLayer();
            legend(layer ? layer->customProperty("paleoLayerId").toString()
                         : QString());
          }
          const auto message =
              ok ? (action == "predict"
                        ? tr("预测已提交；完成后自动登记并显示图件。")
                        : tr("操作完成，图件与版本已更新。"))
                 : (error.isEmpty() ? tr("操作未完成，请检查输入。") : error);
          page->showMessage(message);
          statusBar()->showMessage(message, 10000);
        });
  }
  connect(workbench, &MappingWorkbench::productReady, this,
          [this, show, pages](const QString &h, const QString &id) {
            if (h == m_selection->activeHorizon()) {
              QString error;
              if (!show(id, &error))
                statusBar()->showMessage(error, 10000);
            }
            for (auto *page : pages) {
              page->refresh();
              if (page->horizon() == h)
                page->selectLayer(id);
            }
          });
  connect(workbench, &MappingWorkbench::errorOccurred, this,
          [this, pages](const QString &error) {
            statusBar()->showMessage(error, 15000);
            for (auto *page : pages)
              page->showMessage(error);
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
              auto *layer = m_projectSvc->project()->mapLayer(nativeId);
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
  legend();
}
