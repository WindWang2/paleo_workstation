// 层：视图
// token 例外：DESIGN 数据符号例外：剖面路径的 QGIS 地图橡皮筋描边。（tools/ui-token-exceptions.json 精确计数）。
#include "linkage/seismicmaplink.h"
#include "linkage/selectioncontext.h"
#include "paleoicons.h"
#include "paleomainwindow.h"
#include "qgis/qgiscanvascontroller.h"
#include "qgis/seismicsectiontool.h"
#include "seismicsection/sectionsetupdialog.h"
#include "seismicsection/seismicsectiondockwidget.h"
#include "services/previewdoc.h"
#include "workflow/sectionworkbench.h"
#include <QAction>
#include <QFileInfo>
#include <QPointer>
#include <QStatusBar>
#include <qgsmapcanvas.h>
#include <qgsrubberband.h>

void PaleoMainWindow::attachSections(SeismicMapLink *link) {
  m_sectionLink = link;
  if (!link || !m_seismicSectionDock || !m_previewDoc)
    return;
  auto *dock = m_seismicSectionDock;
  if (m_seismicTaskSvc)
    dock->setTaskService(m_seismicTaskSvc.get());
  auto *workbench = new SectionWorkbench(m_previewDoc->catalog(), this);
  m_sectionWorkbench = workbench; // 连井剖面共享逐井时深校正
  auto *setup = new SectionSetupDialog(this);
  auto route = std::make_shared<std::vector<glm::dvec2>>();
  auto routeHorizon = std::make_shared<QString>();
  auto *band =
      new QgsRubberBand(m_canvasCtl->canvas(), Qgis::GeometryType::Line);
  band->setColor(QColor("#1B73D0"));
  band->setWidth(2);
  auto refresh = [workbench, setup] {
    setup->setWells(workbench->wells());
    setup->setSavedSections(workbench->savedSections());
  };
  auto report = [this, setup](const QString &message) {
    setup->setMessage(message);
    statusBar()->showMessage(message, 10000);
  };
  auto open = [setup, refresh] {
    refresh();
    setup->show();
    setup->raise();
    setup->activateWindow();
  };
  // 绘制期间对话框隐藏；取消/失败时唤回（消息落在对话框状态行，藏着看不见）。
  auto drawingFromSetup = std::make_shared<bool>(false);
  auto *action = new QAction(tr("连井 / 时深对齐"), this);
  action->setObjectName("sectionWorkbenchAction");
  action->setIcon(PaleoIcons::qgisTheme("mActionElevationProfile.svg"));
  action->setToolTip(
      tr("按井顺序或地图折线提取地震剖面，叠加测井与分层并校正时深关系"));
  connect(action, &QAction::triggered, this, open);
  connect(dock, &seismic::SeismicSectionDockWidget::setupRequested, this, open);
  connect(link, &SeismicMapLink::sectionVolumeChanged, dock,
          &seismic::SeismicSectionDockWidget::setVolume);
  connect(link, &SeismicMapLink::sectionVolumeChanged, this, [route, band] {
    route->clear();
    band->reset(Qgis::GeometryType::Line);
  });
  auto catalogPath =
      std::make_shared<QString>(m_previewDoc->catalog()->catalogPath());
  connect(m_previewDoc->catalog(), &DataCatalog::changed, this,
          [this, catalogPath, link, refresh] {
            const auto path = m_previewDoc->catalog()->catalogPath();
            if (*catalogPath != path) {
              *catalogPath = path;
              link->setActiveVolume(nullptr);
              refresh();
            }
          });
  auto locate = [link, dock](bool clicked, int trace, double time, double depth,
                             float amp, double x, double y) {
    if (dock->canvas()->orientation() ==
        seismic::SectionOrientation::TimeSlice) {
      const auto grid = link->gridGeometry();
      if (!grid.valid)
        return;
      const double inl = y, xl = x;
      grid.inlineXlineToXy(inl, xl, &x, &y);
    }
    if (clicked)
      link->onSectionTraceClicked(trace, time, depth, amp, x, y);
    else
      link->onSectionTraceHovered(trace, time, depth, amp, x, y);
  };
  connect(dock->canvas(), &seismic::SeismicSectionCanvas::traceHovered, link,
          [locate](int a, double b, double c, float d, double e, double f) {
            locate(false, a, b, c, d, e, f);
          });
  connect(dock->canvas(), &seismic::SeismicSectionCanvas::traceClicked, link,
          [locate](int a, double b, double c, float d, double e, double f) {
            locate(true, a, b, c, d, e, f);
          });
  connect(link, &SeismicMapLink::sectionExtractedFromMap, this,
          [report, setup, drawingFromSetup](bool ok, const QString &error) {
            if (*drawingFromSetup)
            {
              *drawingFromSetup = false;
              if (!ok)
              {
                setup->show();
                setup->raise();
              }
            }
            if (!ok)
              report(error);
          });
  // Esc 取消绘制 → 唤回为绘制而隐藏的对话框。
  connect(link, &SeismicMapLink::sectionCaptureCancelled, this,
          [setup, report, drawingFromSetup] {
            if (!*drawingFromSetup)
              return;
            *drawingFromSetup = false;
            report(QObject::tr("已取消剖面绘制"));
            setup->show();
            setup->raise();
          });
  // 「清除剖面连线」：地图 rubber band + 路线 + dock 侧 route 一并清，
  // 「保存剖面新版本」随 hasRoute() 复归而失效。
  connect(setup, &SectionSetupDialog::clearRequested, this,
          [band, route, dock, report] {
            route->clear();
            band->reset(Qgis::GeometryType::Line);
            band->hide();
            dock->clearRoute();
            report(QObject::tr("已清除剖面连线"));
          });
  // 地图连线跟随剖面面板显隐：dock 收起时线随之隐藏，重新展开且有路线时恢复。
  connect(dock, &QDockWidget::visibilityChanged, this, [band, route](bool visible) {
    band->setVisible(visible && !route->empty());
  });
  connect(link, &SeismicMapLink::sectionExtractRequested, dock,
          [this, dock, workbench, route, routeHorizon, band,
           setup](std::shared_ptr<const seismic::SgyVolume> volume,
                  std::vector<glm::ivec2> points, QString title,
                  std::vector<glm::dvec2> line) {
            *route = line;
            *routeHorizon =
                m_selection ? m_selection->activeHorizon() : QString();
            band->reset(Qgis::GeometryType::Line);
            for (const auto &p : line)
              band->addPoint(QgsPointXY(p.x, p.y));
            band->show();
            dock->extractSectionFromVolumeAsync(volume, points, title, line,
                                                workbench->sectionWells());
            setup->setMessage(
                QObject::tr("正在提取剖面；沿线井按各自时深关系叠加。"));
            dock->show();
            dock->raise();
          });
  connect(dock, &seismic::SeismicSectionDockWidget::sectionExtractionFinished,
          this, [dock, workbench, report](bool ok, const QString &error) {
            if (ok && dock->hasRoute()) {
              dock->refreshWellOverlay(workbench->sectionWells());
              report(QObject::tr("剖面已更新。可在“井与分层”设置偏距范围；未对"
                                 "齐或超范围的数据不叠加。"));
            } else if (!ok)
              report(error);
          });
  connect(setup, &SectionSetupDialog::drawRequested, this,
          [this, setup, link, report, drawingFromSetup] {
            if (!link->activeVolume() || !link->gridGeometry().valid) {
              report(tr("请先导入带有效坐标的地震体"));
              return;
            }
            *drawingFromSetup = true;
            showPage("constraint");
            setup->hide();
            link->requestSectionCapture();
            m_canvasCtl->canvas()->setFocus();
            report(tr("左键添加节点，右键完成；退格撤回节点，Esc 取消。"));
          });
  // R4 信号化（方向 49）：任意剖面捕获工具归壳持有——linkage 只发
  // sectionCaptureRequested 意图（先例 threewaylocator）；工具的路径完成
  // 接回 linkage 的折线提取入口，Esc 取消直通其取消信号。
  connect(link, &SeismicMapLink::sectionCaptureRequested, this, [this] {
    QgsMapCanvas *canvas = m_canvasCtl ? m_canvasCtl->canvas() : nullptr;
    if (!canvas)
      return;
    if (!m_sectionCaptureTool)
    {
      m_sectionCaptureTool = new SeismicSectionTool(canvas);
      connect(m_sectionCaptureTool, &SeismicSectionTool::sectionPathCaptured,
              m_sectionLink, &SeismicMapLink::onSectionPathCaptured);
      connect(m_sectionCaptureTool, &SeismicSectionTool::captureCancelled,
              m_sectionLink, &SeismicMapLink::sectionCaptureCancelled);
      // 画布先死则置空不 delete（工具析构会碰已死场景的橡皮带——
      // tst_sectionlifecycle 既有坑序；正常收尾走 ~PaleoMainWindow）。
      connect(canvas, &QObject::destroyed, this, [this]() {
        m_sectionCaptureTool = nullptr;
      });
    }
    canvas->setMapTool(m_sectionCaptureTool);
  });
  connect(setup, &SectionSetupDialog::buildRequested, this,
          [link, workbench, report](const QStringList &ids) {
            QString error;
            const auto points = workbench->wellRoute(ids, &error);
            if (points.empty()) {
              report(error);
              return;
            }
            QVector<QgsPointXY> line;
            for (const auto &p : points)
              line << QgsPointXY(p.x, p.y);
            link->triggerSectionFromMapPolyline(
                line, QObject::tr("连井剖面 · %1 口井").arg(ids.size()));
          });
  connect(
      setup, &SectionSetupDialog::calibrationRequested, this,
      [dock, workbench, refresh, report](const QString &id, bool constant,
                                         double velocity, double shift) {
        QString error;
        if (!workbench->setCalibration(id, constant, velocity, shift, &error)) {
          report(error);
          return;
        }
        dock->refreshWellOverlay(workbench->sectionWells());
        refresh();
        report(QObject::tr("时深对齐已应用；保存剖面新版本后可在工程中恢复。"));
      });
  connect(setup, &SectionSetupDialog::saveRequested, this,
          [this, dock, route, routeHorizon, workbench, refresh,
           report](const QString &name) {
            if (!dock->hasRoute()) {
              report(tr("请先完成连井或任意折线剖面提取"));
              return;
            }
            QString error;
            const auto path =
                dock->volume()
                    ? QString::fromStdString(dock->volume()->Path().string())
                    : QString();
            if (!workbench->save(name, *route, path, *routeHorizon, &error)) {
              report(error);
              return;
            }
            refresh();
            report(tr("路线、逐井时深校正与来源关系已保存为工程新版本。"));
          });
  connect(setup, &SectionSetupDialog::restoreRequested, this,
          [workbench, link, routeHorizon, refresh, report](const QString &id) {
            if (!link->activeVolume()) {
              report(QObject::tr("请先加载剖面使用的地震体"));
              return;
            }
            QString error;
            const auto state = workbench->restore(
                id, &error,
                QString::fromStdString(link->activeVolume()->Path().string()));
            if (state.isEmpty()) {
              report(error);
              return;
            }
            refresh();
            QVector<QgsPointXY> line;
            for (const auto &v : state.value("route").toList()) {
              auto p = v.toMap();
              line << QgsPointXY(p.value("x").toDouble(),
                                 p.value("y").toDouble());
            }
            link->triggerSectionFromMapPolyline(line,
                                                QObject::tr("恢复的剖面版本"));
            *routeHorizon = state.value("horizon").toString();
          });
  if (link->activeVolume())
    dock->setVolume(link->activeVolume());
}
