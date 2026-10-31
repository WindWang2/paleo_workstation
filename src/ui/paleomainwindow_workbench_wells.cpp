// 层：视图
// paleomainwindow_workbench_wells — attachWorkbench 井预测修订域接线段（#473）：
// 面板 dock 创建、工作副本自动保存、测井预测修订链、画布右键改相菜单，
// 逐字平移自 paleomainwindow_workbench.cpp，行为守恒。私有成员访问经
// paleomainwindow.h 的成员函数声明（同 W4 拆分段先例）。
// #538：attachWorkbenchWells 原 288 行单体（file-budget 闸 >200 行）拆出两
// 段接线（面板动作族 / 画布右键改相菜单）为文件内自由函数；私有成员经参数
// 显式传入、私有段回调类型（WorkbenchShowFn 等）展开为等价的 std::function
// 特化——#538 只许改三个 workbench .cpp，不在头文件加成员声明。
#include "paleomainwindow.h"

#include "../domain/faciescatalog.h"
#include "../linkage/selectioncontext.h"
#include "../qgis/draftautosave.h"
#include "../qgis/qgiscanvascontroller.h"
#include "../qgis/qgislayerprofile.h"
#include "../qgis/qgislayerservice.h"
#include "../qgis/qgisprojectservice.h"
#include "../workflow/mappingworkbench.h"
#include "edittools/editingtoolbar.h"
#include "faciespinmenu.h"
#include "io/lasdoc.h" // WellPredictionPanel::setLog 参数类型（ui io 白名单）
#include "notifications/paleonotify.h"
#include "pages/datagraphpage.h"
#include "pages/datapage.h"
#include "pages/mappingworkbenchpage.h"
#include "pages/wellpredictionpanel.h"

#include <QAction>
#include <QDockWidget>
#include <QIcon>
#include <QMenu>
#include <QPointer>
#include <QStatusBar>
#include <functional>
#include <qgslayertree.h>
#include <qgslayertreeview.h>
#include <qgsmapcanvas.h>
#include <qgsmaplayer.h>
#include <qgsmapmouseevent.h>
#include <qgsmaptoolidentify.h>
#include <qgsproject.h>
#include <qgsvectorlayer.h>

namespace {

// #538：wellPanel 信号动作族（logRequested / featureSelected /
// reviseRequested / saveRequested / undoRequested）——自 attachWorkbenchWells
// 拆出，逐字平移（this → self），行为守恒。
void attachWellPanelActions(PaleoMainWindow *self, MappingWorkbench *workbench,
                            WellPredictionPanel *wellPanel,
                            PaleoEditingToolbar *edit,
                            QgisLayerService *layerSvc,
                            const std::function<void(const QString &)> &openWells,
                            const std::function<bool(const QString &)> &beginEdit) {
  QObject::connect(wellPanel, &WellPredictionPanel::logRequested, self,
          [workbench, wellPanel](const QString &version) {
            wellPanel->setLog(workbench->predictionLog(version));
          });
  QObject::connect(wellPanel, &WellPredictionPanel::featureSelected, self,
          [self, wellPanel, layerSvc](qint64 fid) {
            if (auto *v = qobject_cast<QgsVectorLayer *>(
                    layerSvc->layer(wellPanel->layerId())))
              v->selectByIds({fid});
          });
  QObject::connect(wellPanel, &WellPredictionPanel::reviseRequested, self,
          [self, workbench, wellPanel, beginEdit, openWells](
              const QString &well, int interval, int code) {
            QString error;
            auto id = wellPanel->layerId();
            if (!id.startsWith("draft.")) {
              // 直接在预测相上修订：预测原件不可变，首次修订自动创建修订副本
              // 并切换到副本（与画布右键改相同一条路径）。
              id = workbench->copyForEditing(id, {}, &error);
              if (id.isEmpty()) {
                self->statusBar()->showMessage(error, 10000);
                return;
              }
              openWells(id);
            }
            if (beginEdit(id) &&
                workbench->reviseWellInterval(id, well, interval, code, &error))
              self->statusBar()->showMessage(
                  PaleoMainWindow::tr("当前地层的预测相已更新并自动保存到修订副本；可撤销，保存修订版本后登记新版本。"), 10000);
            else
              self->statusBar()->showMessage(
                  error.isEmpty() ? PaleoMainWindow::tr("请先结束其他图层的编辑") : error,
                  10000);
          });
  QObject::connect(wellPanel, &WellPredictionPanel::saveRequested, self,
          [self, workbench, wellPanel, edit, layerSvc] {
            QString error;
            auto id = wellPanel->layerId();
            auto *v = qobject_cast<QgsVectorLayer *>(layerSvc->layer(id));
            if (v && v->isEditable()) {
              if (edit && edit->currentLayer() == v)
                edit->saveEditing();
              else
                self->statusBar()->showMessage(PaleoMainWindow::tr("请先结束其他图层的编辑"), 10000);
            } else if (!workbench->saveEditingVersion(id, &error))
              self->statusBar()->showMessage(error, 10000);
          });
  QObject::connect(wellPanel, &WellPredictionPanel::undoRequested, self,
          [self, workbench, wellPanel, layerSvc] {
            const auto id = wellPanel->layerId();
            auto *v = qobject_cast<QgsVectorLayer *>(layerSvc->layer(id));
            if (v && v->isEditable() && v->undoStack()->canUndo()) {
              v->undoStack()->undo();
              v->triggerRepaint();
              emit workbench->faciesEdited(id);
            } else if (auto *autosave = DraftAutosave::of(v);
                       autosave && autosave->canUndo()) {
              // 已自动保存的修订：按自带历史写回旧值并再次自动保存。
              QString error;
              if (autosave->undo(&error))
                emit workbench->faciesEdited(id);
              else
                self->statusBar()->showMessage(error, 10000);
            }
          });
}

// #538：画布右键「更改此要素的相」菜单——自 attachWorkbenchWells 拆出，逐字
// 平移（this → self，私有成员经参数传入），行为守恒。
void attachCanvasFaciesMenu(PaleoMainWindow *self, MappingWorkbench *workbench,
                            QgisCanvasController *canvasCtl,
                            QgisLayerService *layerSvc,
                            const std::function<bool(const QString &)> &beginEdit,
                            const QPointer<MappingWorkbenchPage> &page) {
  QObject::connect(
      canvasCtl->canvas(), &QgsMapCanvas::contextMenuAboutToShow, self,
      [self, workbench, canvasCtl, layerSvc, beginEdit, page](QMenu *menu, QgsMapMouseEvent *event) {
        auto *v = qobject_cast<QgsVectorLayer *>(
            canvasCtl->canvas()->currentLayer());
        if (!v || v->fields().indexOf("facies_code") < 0)
          return;
        const auto id = v->customProperty("paleoLayerId").toString();
        const auto schema =
            workbench->versionForLayer(id).extra.value("facies").toList();
        if (schema.isEmpty())
          return;
        QgsMapToolIdentify identify(canvasCtl->canvas());
        const auto hits =
            identify.identify(event->pos().x(), event->pos().y(), {v},
                              QgsMapToolIdentify::TopDownAll);
        if (hits.isEmpty())
          return;
        const auto original = hits.first().mFeature;
        // 相类固定显示层级：#741 与固定列表同走 runWorkbenchCommand("pinFaciesLevel")
        //（撤销栈、ErrorHub 失败回报口径一致）；图层 id 显式取右键图层。
        FaciesPinMenu::add(menu, schema, original.attribute("facies_code"),
                           workbench->pinnedLevels(id),
                           [self, workbench, id, page](const QString &key, const QString &mode) {
                             if (page) {
                               emit page->commandRequested(
                                   QStringLiteral("pinFaciesLevel"),
                                   {{"layer", id}, {"pin_key", key}, {"display_mode", mode}});
                               return;
                             }
                             QString error; // 无工作台页（测试壳）：后端直调兜底
                             const bool ok = workbench->setPinnedLevel(id, key, mode, &error);
                             self->statusBar()->showMessage(
                                 ok ? PaleoMainWindow::tr("相图显示层级、标注与图例已更新。") : error, 10000);
                           });
        auto *changes = menu->addMenu(PaleoMainWindow::tr("更改此要素的相"));
        changes->setObjectName("changeFeatureFacies");
        for (const auto &entry : schema) {
          auto f = entry.toMap();
          auto *action = changes->addAction(
              QIcon(FaciesCatalog::resourcePath(f.value("icon", f.value("texture")).toString())),
              f.value("name").toString());
          QObject::connect(
              action, &QAction::triggered, self,
              [self, workbench, layerSvc, id, original, code = f.value("code").toInt(),
               beginEdit] {
                QString error;
                QString target = id;
                QgsFeatureId fid = original.id();
                if (!target.startsWith("draft.")) {
                  target = workbench->copyForEditing(id, {}, &error);
                  auto *copy = qobject_cast<QgsVectorLayer *>(
                      layerSvc->instantiate(target));
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
                  self->statusBar()->showMessage(
                      error.isEmpty() ? PaleoMainWindow::tr("无法修改；请先结束其他图层的编辑")
                                      : error,
                      10000);
                else {
                  // assignFacies 已经发 faciesEdited 刷新样式，不重新定位画布。
                  self->statusBar()->showMessage(
                      PaleoMainWindow::tr("相类别已更新并自动保存到工作副本，可撤销；保存编辑后登记新版本。"),
                      10000);
                }
              });
        }
      });
}

}  // namespace

PaleoMainWindow::WorkbenchWells PaleoMainWindow::attachWorkbenchWells(
    MappingWorkbench *workbench, PaleoEditingToolbar *edit,
    const QList<MappingWorkbenchPage *> &pages, const WorkbenchLegendFn &legend,
    const WorkbenchShowFn &show, const WorkbenchShowFn &compare) {
  auto *wellDock = new QDockWidget(tr("测井预测相与修订"), this);
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
        workbench->versionForLayer(id).extra.value("facies").toList(),
        workbench->versionForLayer(id).extra.value("mock").toBool());
    if (auto *v = qobject_cast<QgsVectorLayer *>(m_layerSvc->layer(id)))
      wellPanel->setUndoAvailable(
          (v->isEditable() && v->undoStack()->canUndo()) ||
          (DraftAutosave::of(v) && DraftAutosave::of(v)->canUndo()));
    wellDock->show();
    wellDock->raise();
  };
  // facies-ux 4：工作副本属性修订随时自动保存（只在已持有编辑锁时提交）。
  const auto attachAutosave = [this, wellPanel](QgsVectorLayer *vector) {
    if (!vector ||
        !vector->customProperty("paleoLayerId").toString().startsWith("draft.") ||
        DraftAutosave::of(vector))
      return;
    auto *autosave = DraftAutosave::attach(vector);
    const QPointer<QgsVectorLayer> guard(vector);
    connect(autosave, &DraftAutosave::failed, this, [this](const QString &error) {
      const auto text = tr("自动保存失败，修改仍保留在编辑缓冲中：%1").arg(error);
      statusBar()->showMessage(text, 15000);
      PaleoNotify::warning(this, tr("自动保存失败"), text);
    });
    connect(autosave, &DraftAutosave::historyChanged, this, [wellPanel, guard] {
      if (guard &&
          guard->customProperty("paleoLayerId").toString() == wellPanel->layerId())
        wellPanel->setUndoAvailable(
            (guard->isEditable() && guard->undoStack()->canUndo()) ||
            DraftAutosave::of(guard)->canUndo());
    });
  };
  if (m_projectSvc && m_projectSvc->project()) {
    connect(m_projectSvc->project(), &QgsProject::layersAdded, this,
            [attachAutosave](const QList<QgsMapLayer *> &layers) {
              for (auto *layer : layers)
                attachAutosave(qobject_cast<QgsVectorLayer *>(layer));
            });
    for (auto *layer : m_projectSvc->project()->mapLayers())
      attachAutosave(qobject_cast<QgsVectorLayer *>(layer));
  }
  connect(wellPanel, &WellPredictionPanel::graphRequested, this,
          [this, workbench](const QString &layer) {
            const auto v = workbench->versionForLayer(layer);
            if (!v.id.isEmpty()) openDataGraph(v.assetId, v.id);
            else statusBar()->showMessage(tr("请先保存修订版本，再追溯其来源"), 8000);
          });
  if (auto *dataPage = findChild<DataPage *>())
    connect(dataPage->graphPage(), &DataGraphPage::workspaceRequested, this,
            [this, workbench, pages, show, compare, openWells](const QString &version, const QString &action) {
              QString error;
              const auto id = workbench->layerForVersion(version, &error);
              if (id.isEmpty()) { statusBar()->showMessage(error, 15000); return; }
              const auto declaration = workbench->declaration(id);
              const auto metadata = workbench->versionForLayer(id).extra;
              const auto kind = metadata.value("kind").toString();
              const QString pageId = metadata.value("result_kind") == "formation_prediction" ||
                  kind.contains("prediction") || kind.contains("well") || kind.contains("seismic")
                  ? QStringLiteral("predict") : kind.startsWith("single_factor") || kind.startsWith("constraint") || kind == "contour_lines"
                  ? QStringLiteral("constraint") : QStringLiteral("compose");
              if (!declaration.horizon.isEmpty()) m_selection->setActiveHorizon(declaration.horizon);
              showPage(pageId);
              for (auto *page : pages) {
                page->setHorizon(m_selection->activeHorizon()); page->refresh(); page->selectLayer(id);
              }
              bool ok = action == "compare" ? compare(id, &error) : show(id, &error);
              if (ok && action == "welltracks") {
                if (workbench->wellPredictions(id).isEmpty()) { ok = false; error = tr("所选版本没有可修订的测井预测结果"); }
                else openWells(id);
              }
              statusBar()->showMessage(ok ? tr("已打开图谱中选择的保存版本") : error, ok ? 8000 : 15000);
            });
  auto beginEdit = [this, edit, attachAutosave](const QString &id) {
    auto *vector = qobject_cast<QgsVectorLayer *>(m_layerSvc->instantiate(id));
    attachAutosave(vector);
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
  // #538：面板信号动作族与画布右键改相菜单拆至文件内自由函数（>200 行闸）。
  attachWellPanelActions(this, workbench, wellPanel, edit, m_layerSvc,
                         openWells, beginEdit);
  connect(
      workbench, &MappingWorkbench::faciesEdited, this,
      [this, workbench, wellPanel, legend](const QString &id) {
        workbench->styleLayer(id);
        m_canvasCtl->canvas()->refresh();
        legend(id);
        if (wellPanel->layerId() == id) {
          wellPanel->setResult(
              id, workbench->wellPredictions(id),
              workbench->versionForLayer(id).extra.value("facies").toList(),
              workbench->versionForLayer(id).extra.value("mock").toBool());
          if (auto *v = qobject_cast<QgsVectorLayer *>(m_layerSvc->layer(id)))
            wellPanel->setUndoAvailable(
                (v->isEditable() && v->undoStack()->canUndo()) ||
                (DraftAutosave::of(v) && DraftAutosave::of(v)->canUndo()));
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
  attachCanvasFaciesMenu(this, workbench, m_canvasCtl, m_layerSvc,
                         beginEdit, QPointer<MappingWorkbenchPage>(pages.value(0)));
  return {wellPanel, wellDock, openWells, beginEdit};
}
