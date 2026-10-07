// 层：视图
// token 例外：DESIGN 数据符号例外：编图参考地图固定纸面与地质预览色样。（tools/ui-token-exceptions.json 精确计数）。
#include "../domain/faciescatalog.h"
#include "../domain/facieshierarchy.h"
#include "../domain/singlefactorrequest.h"
#include "../linkage/selectioncontext.h"
#include "../qgis/facieshierarchyrenderer.h"
#include "../qgis/factorstylewriter.h"
#include "../qgis/mapcanvaslink.h"
#include "../qgis/mappingartifactwriter.h"
#include "../qgis/qgiscanvascontroller.h"
#include "../qgis/qgislayerprofile.h"
#include "../qgis/qgislayerservice.h"
#include "../qgis/qgisprojectservice.h"
#include "../services/paleotaskservice.h" // QPointer<PaleoTask>::running 需完整类型
#include "../workflow/mappingworkbench.h"
#include "datapreview/datapreviewtabs.h"
#include "decorations/paleodecorations.h"
#include "edittools/editingtoolbar.h"
#include "horizonchipbar.h"
#include "pages/composepage.h"
#include "pages/constraintpage.h"
#include "pages/datapage.h"
#include "pages/mappingworkbenchpage.h"
#include "pages/pageshared.h"
#include "pages/predictpage.h"
#include "pages/wellpredictionpanel.h"
#include "paleomainwindow.h"
#include "paleoribbon.h"
#include <QAction>
#include <QCheckBox>
#include <QDialog>
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
#include <memory>
#include <qgslayertree.h>
#include <qgslayertreeview.h>
#include <qgsmapcanvas.h>
#include <qgsmapmouseevent.h>
#include <qgsmaptoolidentify.h>
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
  if (!stack || stack->count() < paleo::pagesinternal::kPageIds.size())
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
  }
  stack->setCurrentIndex(current);
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
  auto legend = [this, workbench](const QString &id = QString()) {
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
    auto schema =
        product ? v.extra.value("facies").toList() : workbench->facies(h);
    QString title = h.isEmpty() ? tr("请选择层位") : tr("%1 · 相图例").arg(h);
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
      title = tr("%1 · %2 · v%3")
                  .arg(h, FaciesHierarchy::title(workbench->resolvedLevel(id)))
                  .arg(v.versionNumber);
      if (v.extra.value("mock").toBool())
        title += tr(" · Mock");
    }
    m_decorMgr->setFaciesLegend(title, h.isEmpty() ? QVariantList() : schema);
  };
  auto shown = std::make_shared<QHash<QString, QString>>();
  if (m_projectSvc) {
    connect(m_projectSvc, &QgisProjectService::projectOpened, this,
            [shown] { shown->clear(); });
  }
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
    // the result list and can be compared in independent windows.
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
    }
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
    dialog->setProperty("paleo.referenceVersion",
                        workbench->versionForLayer(id).id);
    dialog->setWindowTitle(tr("参考 · %1 · %2").arg(d.horizon, d.title));
    dialog->resize(640, 480);
    layer->setParent(dialog);
    auto *layout = new QVBoxLayout(dialog);
    auto *caption = new QLabel(
        tr("%1 · %2\n与主图联动范围及光标；取消联动后可独立缩放和平移。")
            .arg(d.horizon, d.title),
        dialog);
    caption->setWordWrap(true);
    layout->addWidget(caption);
    auto *linked = new QCheckBox(tr("联动主图范围与光标"), dialog);
    linked->setObjectName("referenceLinked");
    linked->setChecked(true);
    layout->addWidget(linked);
    auto *canvas = new QgsMapCanvas(dialog);
    canvas->setObjectName("referenceCanvas");
    layer->setParent(
        canvas); // keep the layer alive until the renderer has stopped
    if (m_projectSvc) {
      canvas->setProject(m_projectSvc->project());
      canvas->mapSettings().setTransformContext(m_projectSvc->project()->transformContext());
    }
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
    const auto compareKind = v.extra.value("kind").toString();
    const auto compareSource = v.extra.value("value_source").toString();
    const bool compareAnalysis =
        paleo::singlefactor::isAnalysisFactorRaster(compareKind, compareSource);
    const bool compareCartographic =
        paleo::singlefactor::rejectsQuantitativeUse(compareKind, compareSource);
    const bool faciesReference =
        !compareKind.startsWith(QLatin1String("constraint")) &&
        !compareAnalysis && !compareCartographic && !f.isEmpty();
    if (faciesReference)
      f = FaciesHierarchyRenderer::apply(
          layer, f, FaciesHierarchy::resolveLevel("auto", canvas->scale()));
    else {
      f.clear();
      if (compareAnalysis || compareCartographic)
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
    auto *link = new MapCanvasLink(m_canvasCtl->canvas(), canvas, dialog);
    connect(linked, &QCheckBox::toggled, link, &MapCanvasLink::setEnabled);
    if (faciesReference) {
      const auto schema =
          v.extra.value("facies", workbench->facies(d.horizon)).toList();
      const auto refreshReference = [layer, canvas, decor, schema, d] {
        const auto level =
            FaciesHierarchy::resolveLevel("auto", canvas->scale());
        if (layer->customProperty("paleo/faciesResolvedLevel").toString() !=
            level) {
          const auto entries =
              FaciesHierarchyRenderer::apply(layer, schema, level);
          decor->setFaciesLegend(
              d.horizon + " · " + FaciesHierarchy::title(level), entries);
        }
      };
      connect(canvas, &QgsMapCanvas::scaleChanged, dialog, refreshReference);
      refreshReference();
    }
    connect(dialog, &QDialog::finished, link,
            [link] { link->setEnabled(false); });
    connect(dialog, &QDialog::finished, canvas, [canvas] {
      canvas->stopRendering();
      canvas->setLayers({});
    });
    dialog->show();
    canvas->refresh();
    return true;
  };
  auto *wellDock = new QDockWidget(tr("测井相预测与修订"), this);
  wellDock->setObjectName("wellPredictionDock");
  auto *wellPanel = new WellPredictionPanel(wellDock);
  wellDock->setWidget(wellPanel);
  addDockWidget(Qt::BottomDockWidgetArea, wellDock);
  wellDock->hide();
  if (auto *dataPage = findChild<DataPage *>())
    connect(dataPage, &DataPage::mappingReferenceRequested, this,
            [this, workbench, compare](const QString &version) {
              const QPointer<QgsMapLayer> target =
                  m_canvasCtl->canvas()->currentLayer();
              QString error;
              const auto id = workbench->layerForVersion(version, &error);
              if (id.isEmpty() || !compare(id, &error)) {
                statusBar()->showMessage(error, 15000);
                return;
              }
              showPage("compose");
              if (target) {
                if (auto *node =
                        m_projectSvc->project()->layerTreeRoot()->findLayer(
                            target->id()))
                  node->setItemVisibilityCheckedParentRecursive(true);
                if (auto *tree = findChild<QgsLayerTreeView *>())
                  tree->setCurrentLayer(target);
                m_canvasCtl->canvas()->setCurrentLayer(target);
                m_canvasCtl->canvas()->refresh();
                if (m_profileSvc)
                  m_profileSvc->captureCurrentAsTheme(
                      QgisLayerProfileService::pageThemeName(currentPage()));
              }
              statusBar()->showMessage(tr("已打开所选保存版本的联动参考图。"),
                                       8000);
            });
  auto openWells = [this, workbench, wellPanel, wellDock](const QString &id) {
    const auto wells = workbench->wellPredictions(id);
    if (wells.isEmpty())
      return;
    wellPanel->setResult(
        id, wells,
        workbench->versionForLayer(id).extra.value("facies").toList());
    if (auto *v = qobject_cast<QgsVectorLayer *>(m_layerSvc->layer(id)))
      wellPanel->setUndoAvailable(v->isEditable() && v->undoStack()->canUndo());
    wellDock->show();
    wellDock->raise();
  };
  auto beginEdit = [this, edit](const QString &id) {
    auto *vector = qobject_cast<QgsVectorLayer *>(m_layerSvc->instantiate(id));
    if (edit && vector) {
      edit->refreshFromProject();
      edit->setCurrentLayer(vector);
      const bool ok = edit->startEditing();
      if (ok && !edit->actionSelect()->isChecked())
        edit->actionSelect()->trigger();
      return ok;
    }
    return false;
  };
  connect(wellPanel, &WellPredictionPanel::logRequested, this,
          [workbench, wellPanel](const QString &version) {
            wellPanel->setLog(workbench->predictionLog(version));
          });
  connect(wellPanel, &WellPredictionPanel::featureSelected, this,
          [this, wellPanel](qint64 fid) {
            if (auto *v = qobject_cast<QgsVectorLayer *>(
                    m_layerSvc->layer(wellPanel->layerId())))
              v->selectByIds({fid});
          });
  connect(wellPanel, &WellPredictionPanel::reviseRequested, this,
          [this, workbench, wellPanel, beginEdit, openWells](
              const QString &well, int interval, int code) {
            QString error;
            auto id = wellPanel->layerId();
            if (!id.startsWith("draft.")) {
              // 直接在预测相上修订：预测原件不可变，首次修订自动创建修订副本
              // 并切换到副本（与画布右键改相同一条路径）。
              id = workbench->copyForEditing(id, {}, &error);
              if (id.isEmpty()) {
                statusBar()->showMessage(error, 10000);
                return;
              }
              openWells(id);
            }
            if (beginEdit(id) &&
                workbench->reviseWellInterval(id, well, interval, code, &error))
              statusBar()->showMessage(
                  tr("井段与地图相点已更新；保存修订版本后登记文件。"), 10000);
            else
              statusBar()->showMessage(
                  error.isEmpty() ? tr("请先结束其他图层的编辑") : error,
                  10000);
          });
  connect(wellPanel, &WellPredictionPanel::saveRequested, this,
          [this, workbench, wellPanel, edit] {
            QString error;
            auto id = wellPanel->layerId();
            auto *v = qobject_cast<QgsVectorLayer *>(m_layerSvc->layer(id));
            if (v && v->isEditable()) {
              if (edit && edit->currentLayer() == v)
                edit->saveEditing();
              else
                statusBar()->showMessage(tr("请先结束其他图层的编辑"), 10000);
            } else if (!workbench->saveEditingVersion(id, &error))
              statusBar()->showMessage(error, 10000);
          });
  connect(wellPanel, &WellPredictionPanel::undoRequested, this,
          [this, workbench, wellPanel] {
            const auto id = wellPanel->layerId();
            auto *v = qobject_cast<QgsVectorLayer *>(m_layerSvc->layer(id));
            if (v && v->isEditable() && v->undoStack()->canUndo()) {
              v->undoStack()->undo();
              v->triggerRepaint();
              emit workbench->faciesEdited(id);
            }
          });
  connect(
      workbench, &MappingWorkbench::faciesEdited, this,
      [this, workbench, wellPanel, legend](const QString &id) {
        workbench->styleLayer(id);
        m_canvasCtl->canvas()->refresh();
        legend(id);
        if (wellPanel->layerId() == id) {
          wellPanel->setResult(
              id, workbench->wellPredictions(id),
              workbench->versionForLayer(id).extra.value("facies").toList());
          if (auto *v = qobject_cast<QgsVectorLayer *>(m_layerSvc->layer(id)))
            wellPanel->setUndoAvailable(v->isEditable() &&
                                        v->undoStack()->canUndo());
        }
      });
  connect(m_selection, &SelectionContext::activeHorizonChanged, wellPanel,
          [wellPanel, wellDock] {
            wellPanel->clear();
            wellDock->hide();
          });
  if (m_projectSvc) {
    connect(m_projectSvc, &QgisProjectService::projectOpened, wellPanel,
            [wellPanel, wellDock] {
              wellPanel->clear();
              wellDock->hide();
            });
  }
  connect(
      m_canvasCtl->canvas(), &QgsMapCanvas::contextMenuAboutToShow, this,
      [this, workbench, show, beginEdit](QMenu *menu, QgsMapMouseEvent *event) {
        auto *v = qobject_cast<QgsVectorLayer *>(
            m_canvasCtl->canvas()->currentLayer());
        if (!v || v->fields().indexOf("facies_code") < 0)
          return;
        const auto id = v->customProperty("paleoLayerId").toString();
        const auto schema =
            workbench->versionForLayer(id).extra.value("facies").toList();
        if (schema.isEmpty())
          return;
        QgsMapToolIdentify identify(m_canvasCtl->canvas());
        const auto hits =
            identify.identify(event->pos().x(), event->pos().y(), {v},
                              QgsMapToolIdentify::TopDownAll);
        if (hits.isEmpty())
          return;
        auto *changes = menu->addMenu(tr("更改此要素的相"));
        changes->setObjectName("changeFeatureFacies");
        const auto original = hits.first().mFeature;
        for (const auto &entry : schema) {
          auto f = entry.toMap();
          auto *action = changes->addAction(
              QIcon(FaciesCatalog::resourcePath(f.value("icon", f.value("texture")).toString())),
              f.value("name").toString());
          connect(
              action, &QAction::triggered, this,
              [this, workbench, id, original, code = f.value("code").toInt(),
               show, beginEdit] {
                QString error;
                QString target = id;
                QgsFeatureId fid = original.id();
                if (!target.startsWith("draft.")) {
                  target = workbench->copyForEditing(id, {}, &error);
                  auto *copy = qobject_cast<QgsVectorLayer *>(
                      m_layerSvc->instantiate(target));
                  if (copy) {
                    auto it = copy->getFeatures();
                    QgsFeature f;
                    fid = FID_NULL;
                    while (it.nextFeature(f))
                      if (f.geometry().asWkb() == original.geometry().asWkb() &&
                          f.attribute("facies_code") ==
                              original.attribute("facies_code")) {
                        fid = f.id();
                        break;
                      }
                  }
                }
                if (target.isEmpty() || fid == FID_NULL || !beginEdit(target) ||
                    !workbench->assignFacies(target, {fid}, code, &error))
                  statusBar()->showMessage(
                      error.isEmpty() ? tr("无法修改；请先结束其他图层的编辑")
                                      : error,
                      10000);
                else {
                  show(target, &error);
                  statusBar()->showMessage(
                      tr("相类别已更新，可撤销；保存编辑后登记新版本。"),
                      10000);
                }
              });
        }
      });
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
  for (auto *page : pages) {
    page->setHorizon(m_selection->activeHorizon());
    connect(m_selection, &SelectionContext::activeHorizonChanged, page,
            &MappingWorkbenchPage::setHorizon);
    connect(
        page, &MappingWorkbenchPage::commandRequested, this,
        [this, workbench, page, show, compare, edit, constraint, legend,
         openWells, beginEdit](const QString &action, const QVariantMap &p) {
          QString error;
          bool ok = true;
          const auto h = p.value("horizon").toString(),
                     id = p.value("layer").toString();
          const auto inputs = p.value("inputs").toStringList();
          if (action == "predict")
            ok = workbench->predict(h, p.value("kind").toString(), inputs,
                                    &error, p.value("horizon_file").toString());
          else if (action == "cancel")
            workbench->cancelPrediction();
          else if (action == "show")
            ok = show(id, &error);
          else if (action == "catalog") {
            const auto version = workbench->versionForLayer(id);
            auto *dataPage = findChild<DataPage *>();
            if (!dataPage || version.id.isEmpty()) {
              ok = false;
              error = tr("所选图件尚未登记到数据管理");
            } else {
              showPage("data");
              if (m_previewTabs)
                m_previewTabs->openVersion(version.id);
              dataPage->focusVersion(version.assetId, version.id);
            }
          } else if (action == "labels")
            ok = workbench->setLabelMode(id, p.value("label_mode").toInt(),
                                         &error);
          else if (action == "welltracks")
            openWells(id);
          else if (action == "selectHierarchy") {
            ok = workbench->selectHierarchyMembers(
                id, p.value("edit_level").toString(), &error);
          } else if (action == "displayLevel") {
            ok = workbench->setDisplayMode(
                id, p.value("display_mode").toString(), &error);
          } else if (action == "addEvidence" || action == "removeEvidence") {
            if (!beginEdit(id)) {
              ok = false;
              error = tr("请先结束其他图层的编辑");
            } else if (action == "addEvidence")
              ok = workbench->addEvidence(id, workbench->selectedFeatures(id),
                                          p, &error);
            else
              ok = workbench->removeEvidence(
                  id, workbench->selectedFeatures(id),
                  p.value("evidence_id").toString(), &error);
          } else if (action == "assignFacies") {
            auto *v =
                qobject_cast<QgsVectorLayer *>(m_layerSvc->instantiate(id));
            if (!v || v->selectedFeatureIds().isEmpty()) {
              ok = false;
              error = tr("请先用画布选择工具选中要素");
            } else if (!beginEdit(id)) {
              ok = false;
              error = tr("请先结束其他图层的编辑");
            } else
              ok = workbench->assignHierarchy(
                  id, v->selectedFeatureIds().values(), p.value("code").toInt(),
                  p.value("edit_level", "micro_facies").toString(), &error);
          } else if (action == "references") {
            for (const auto &ref : inputs) {
              if (!compare(ref, &error)) {
                ok = false;
                break;
              }
            }
          } else if (action == "compare")
            ok = compare(id, &error);
          else if (action == "polygonize")
            ok = !workbench->polygonize(id, &error).isEmpty();
          else if (action == "copy") {
            const auto draft = workbench->copyForEditing(id, inputs, &error);
            ok = !draft.isEmpty();
            if (ok && edit) {
              show(draft, &error);
              ok = beginEdit(draft);
              if (!ok)
                error = tr("副本已创建，请先结束其他图层的编辑");
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
            ok = workbench->importConstraints(h, path, QStringLiteral("auto"),
                                              &error);
            // auto 判定失败 → 询问用户角色后重试（方向线 / 打断线）。
            if (!ok && error.contains(tr("无法判断约束线类型"))) {
              const QString choice = QInputDialog::getItem(
                  this, tr("约束线类型"), tr("无法自动判断，请选择约束线类型："),
                  {tr("方向线"), tr("打断线")}, 0, false, &ok);
              if (ok)
                ok = workbench->importConstraints(
                    h, path,
                    choice == tr("方向线") ? QStringLiteral("direction")
                                          : QStringLiteral("barrier"),
                    &error);
              else
                return;
            }
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
          } else if (action == "factor") {
            if (m_factorTask && m_factorTask->running()) {
              ok = false;
              error = tr("已有单因素计算在进行");
            } else
              ok = workbench->generateFactor(h, p.value("factor").toString(), p,
                                             &error);
          }
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
          QString message;
          if (!ok)
            message = error.isEmpty() ? tr("操作未完成，请检查输入。") : error;
          else if (action == "predict")
            message = tr("预测已提交；完成后自动登记并显示图件。");
          else if (action == "cancel")
            message = tr("已请求取消预测。");
          else if (action == "show")
            message = tr("已在画布显示并定位选中图件。");
          else if (action == "compare" || action == "references")
            message =
                tr("已打开联动参考窗口，平移、缩放与光标位置随主图同步。");
          else if (action == "catalog")
            message = tr("已在数据管理中定位图件的保存版本和来源谱系。");
          else if (action == "displayLevel")
            message = tr("相图显示层级、标注与图例已更新。");
          else if (action == "addEvidence" || action == "removeEvidence")
            message = tr("解释证据已更新，可撤销；保存图件版本后持久化。");
          else if (action == "labels")
            message = tr("选中图件的标注已更新。");
          else if (action == "schema")
            message = tr("当前层位相分类已保存，已有图件保留原分类。");
          else if (action == "draw")
            message = tr("请在画布逐点绘制约束线，右键结束，Esc 取消。");
          else if (action == "assignFacies")
            message = tr("选中要素的相类别已更新，请保存编辑以登记新版本。");
          else if (action == "welltracks")
            message = tr("已打开井道与测井相修订面板。");
          else
            message = tr("操作完成，图件与版本已更新。");
          page->showMessage(message);
          statusBar()->showMessage(message, 10000);
        });
  }
  connect(workbench, &MappingWorkbench::productReady, this,
          [this, show, pages, workbench, openWells,
           wellDock](const QString &h, const QString &id) {
            if (h == m_selection->activeHorizon()) {
              QString error;
              if (!show(id, &error))
                statusBar()->showMessage(error, 10000);
            }
            if (h == m_selection->activeHorizon() &&
                !workbench->wellPredictions(id).isEmpty())
              openWells(id);
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
  legend();
}
