// 层：视图
// paleomainwindow_attach_mapping — 编图流水线与版本域接线（W4 拆分段）
#include "paleomainwindow.h"

#include "../qgis/qgiscanvascontroller.h"
#include "../qgis/qgisprojectservice.h"
#include "../qgis/qgislayerservice.h"
#include "../domain/arearules.h"
#include "../linkage/selectioncontext.h"
#include "../metadata/mapversionstore.h"
#include "../services/projectdata.h"
#include "../catalog/datacatalog.h"
#include "../workflow/mappingworkflow.h"
#include "../workflow/mappingworkbench.h"
#include "../workflow/mapexport.h"
#include "../workflow/geologicalexport.h"
#include "../services/dataexport.h"
#include "../qgis/geologicalmaplayout.h"
#include "dialogs/geologicalexportdialog.h"
#include "../workflow/mapversioncontroller.h"
#include "evolution/evolutionplayerpanel.h"
#include "layout/layoutexportactions.h"
#include "layout/mapbookcontroller.h"
#include "pages/pagepanels.h"

#include <qgsmapcanvas.h>
#include <qgslayoutitemlabel.h>
#include <qgsmaplayer.h>
#include <qgsvectorlayer.h>
#include <qgsproject.h>
#include <qgslayoutitemmap.h>
#include <qgsprintlayout.h>
#include <qgsmessagelog.h>

#include "notifications/paleonotify.h"
#include <QDir>
#include <QFileInfo>
#include <QLabel>
#include <QPushButton>
#include <memory>

#include "../services/paleotaskservice.h"
#include <QDateTime>
#include <QStatusBar>
#include <QTemporaryFile>

// ---------------------------------------------------------------------------
// wave/mapping-pipeline 阶段C+E — 编图链 / 层位图导出 / 版本状态机接线。
// W4：attachMapping 按段拆（发布门 → 导出接线 → 版本状态机），入口只排
// 顺序 + 早退守卫；发布门本体存成员 m_refreshPublishGate（其他段经成员
// 重算，不靠 lambda 捕获串接）。
// ---------------------------------------------------------------------------
void PaleoMainWindow::attachMapping(MappingWorkflow *mapping, MapVersionController *versions,
                                    MapVersionStore *versionStore, ProjectDataFacade *projectData,
                                    DataCatalog *catalog)
{
  auto *composePage = findChild<ComposePage *>();
  if (!composePage || !mapping || !versions)
    return;

  attachMappingPublishGate(composePage, versions, versionStore, projectData, catalog);
  attachMappingExport(composePage, mapping, versionStore, projectData, catalog);
  attachMappingVersions(composePage, versions);
  connect(composePage, &ComposePage::cancelSnapshotRequested, versions, &MapVersionController::cancelSnapshot);
  connect(versions, &MapVersionController::snapshotQueryProgress, this,
          [this, composePage](const QString &h, qint64 checked, qint64 total) {
    if (m_selection && m_selection->activeHorizon() == h)
      if (auto *label = composePage->findChild<QLabel *>("statusLabel"))
        label->setText(tr("正在校验证据：%1/%2 个要素，可取消").arg(checked).arg(total));
  });
  // A terminal result from an earlier project session is deliberately dropped,
  // but its busy transition must still release the current page's save gate.
  connect(versions, &MapVersionController::snapshotBusyChanged, this, [this](bool) {
    if (m_refreshPublishGate) m_refreshPublishGate();
  });
  connect(versions, &MapVersionController::snapshotFinished, this,
          [this, composePage](const QString &h, const QString &operation, bool success, bool cancelled, const QString &error) {
    if (m_selection && m_selection->activeHorizon() == h) {
      if (auto *label = composePage->findChild<QLabel *>("statusLabel"))
        label->setText(success ? operation == "save" ? tr("成果版本已保存") : tr("成果快照已发布") :
            cancelled ? tr("快照任务已取消，原成果保持不变") : error);
    }
    if (m_refreshPublishGate) m_refreshPublishGate();
  });
  if (m_mapBookCtl)
    m_mapBookCtl->setCatalog(catalog); // #148：逐版产物登记进 catalog

  // 工程打开时恢复发布门状态（版本行的 PDF 资产 + 残差覆盖重算）。
  if (m_projectSvc)
    connect(m_projectSvc, &QgisProjectService::projectOpened, this,
            [this]() { if (m_refreshPublishGate) m_refreshPublishGate(); });
  // 发布门按当前层位取版本/残差——chip 切层后必须重算，否则按钮态与
  // 版本胶囊滞留旧层位（对照下方 setThicknessHorizon 的同源联动）。
  if (m_selection)
    connect(m_selection, &SelectionContext::activeHorizonChanged, this,
            [this]() { if (m_refreshPublishGate) m_refreshPublishGate(); });
  if (m_refreshPublishGate)
    m_refreshPublishGate(); // 初始态：缺什么写什么，按钮禁用
}

// ---------------------------------------------------------------------------
// 发布门（§177/§260）：版本行上的 PDF 资产 id + 逐井残差摘要完整性共同
// 决定按钮状态；导出成功 / 保存 / 发布 / 验证跑完 / 工程打开后都重算。
// ---------------------------------------------------------------------------
void PaleoMainWindow::attachMappingPublishGate(ComposePage *composePage,
                                               MapVersionController *versions,
                                               MapVersionStore *versionStore,
                                               ProjectDataFacade *projectData,
                                               DataCatalog *catalog)
{
  const auto status = [composePage](const QString &text) {
    if (auto *label = composePage->findChild<QLabel *>(QStringLiteral("statusLabel")))
      label->setText(text);
  };
  const auto activeHorizon = [this]() -> QString {
    return m_selection ? m_selection->activeHorizon() : QString();
  };
  // 发布门（§177/§260）：版本行上的 PDF 资产 id + 逐井残差摘要完整性共同
  // 决定按钮状态；tooltip 写缺的那条。导出成功 / 保存 / 发布 / 验证跑完 /
  // 工程打开后都重算 —— 门是「当前状态」而不是一次性开关。
  const auto refreshPublishGate = [this, versions, versionStore, projectData, catalog]() {
    auto *page = findChild<ComposePage *>();
    if (!page)
      return;
    page->setSnapshotBusy(versions->snapshotBusy());
    const QString h = m_selection ? m_selection->activeHorizon() : QString();
    const MapVersion v = (versionStore && !h.isEmpty()) ? versionStore->latest(h)
                                                       : MapVersion();
    const QString summary = h.isEmpty()
        ? QString()
        : MapVersionController::residualSummaryJson(projectData, h);
    int covered = -1, total = -1;
    MapVersionStore::residualSummaryComplete(summary, &covered, &total);
    page->setPublishState(!v.pdfAssetId.isEmpty(), covered, total);
    page->setVersionState(v.version, v.state == QLatin1String("Published"));
    // B 包 staleness-lite advisory：目标工程里有过时下游产物 → 发布按钮
    // tooltip 如实列出（可见但不阻断——enable 态仍由 setPublishState 决定）。
    const QString advisory = MapVersionController::stalePublishAdvisory(catalog);
    if (!advisory.isEmpty())
      if (auto *btn = page->findChild<QPushButton *>(QStringLiteral("publishButton")))
        btn->setToolTip(btn->toolTip().isEmpty()
                            ? advisory
                            : btn->toolTip() + QLatin1Char('\n') + advisory);
    page->setSnapshotBusy(versions->snapshotBusy());
  };
  m_refreshPublishGate = refreshPublishGate;

  // 逐井残差摘要随发布冻结进版本行。B 包 staleness-lite：有过时下游产物
  // 时确认文案如实列出（advisory——不阻断，Ok/Cancel 照常由人决断）。
  connect(composePage, &ComposePage::publishRequested, this,
          [this, versions, versionStore, projectData, catalog, composePage,
           activeHorizon, status]() {
            const QString h = activeHorizon();
            if (h.isEmpty())
            {
              status(tr("先选择层位再发布"));
              return;
            }
            const QString summary =
                MapVersionController::residualSummaryJson(projectData, h);
            int covered = -1, total = -1;
            MapVersionStore::residualSummaryComplete(summary, &covered, &total);
            const MapVersion v = versionStore ? versionStore->latest(h) : MapVersion();
            const QString pdfName =
                QFileInfo(versionStore ? versionStore->latestLayoutProduct(h) : QString())
                    .fileName();
            const QString advisory =
                MapVersionController::stalePublishAdvisory(catalog);
            const bool proceed = PaleoNotify::ask(
                this, tr("发布版本"),
                tr("发布 %1 v%2？\n\nPDF：%3\n覆盖井数：%4/%5\n\n发布后快照只读，"
                   "继续编辑请保存新版本。")
                    .arg(h)
                    .arg(v.version)
                    .arg(pdfName.isEmpty() ? tr("（未登记）") : pdfName)
                    .arg(covered < 0 ? 0 : covered)
                    .arg(total < 0 ? 0 : total)
                    + (advisory.isEmpty()
                           ? QString()
                           : tr("\n\n注意：") + advisory),
                PaleoNotify::AskButtons::OkCancel, PaleoNotify::AskDefault::Reject);
            if (!proceed)
              return;
            QString err;
            if (versions->startPublish(h, summary, &err)) status(tr("正在准备发布快照，可在任务中心查看进度或取消"));
            else
              status(err.isEmpty() ? tr("发布失败") : err);
            if (m_refreshPublishGate) m_refreshPublishGate();
          });

}

// ---------------------------------------------------------------------------
// 导出接线：链路状态 / 厚度链 / 层位图 PDF 导出 + catalog 登记（W4 拆分段）
// ---------------------------------------------------------------------------
void PaleoMainWindow::attachMappingExport(ComposePage *composePage,
                                          MappingWorkflow *mapping,
                                          MapVersionStore *versionStore,
                                          ProjectDataFacade *projectData,
                                          DataCatalog *catalog)
{
  const auto status = [composePage](const QString &text) {
    if (auto *label = composePage->findChild<QLabel *>(QStringLiteral("statusLabel")))
      label->setText(text);
  };
  const auto activeHorizon = [this]() -> QString {
    return m_selection ? m_selection->activeHorizon() : QString();
  };

  // 链路状态文案走 statusLabel（成功/失败都落页面，再进日志）。
  connect(mapping, &MappingWorkflow::chainDone, this,
          [this, composePage, status](const QString &h, const QString &layerId) {
            status(tr("编图链完成：%1 → %2").arg(h, layerId));
            composePage->refreshFactors();
            revealDeclaredLayer(layerId, true);
          });
  connect(mapping, &MappingWorkflow::chainFailed, this,
          [status](const QString &, const QString &error) { status(error); });

  // 「选层位 → 算厚度 → IDW → 转相面」：层位来自 chip 的 activeHorizon，
  // 提示里的目标层位随工程参数（AreaRules targetHorizon）。
  connect(composePage, &ComposePage::thicknessChainRequested, this,
          [this, mapping, activeHorizon, status]() {
            const QString h = activeHorizon();
            if (h.isEmpty())
            {
              status(tr("先在顶部 chip 选择层位（本阶段目标 %1）")
                         .arg(AreaRules::active().targetHorizon));
              return;
            }
            QString err;
            if (!mapping->startThicknessChain(h, &err))
              QgsMessageLog::logMessage(err, QStringLiteral("Paleo"), Qgis::MessageLevel::Warning);
          });

  // D8 使能态：无层位时按钮禁用（tooltip 写原因）；chip 切换即时联动命名。
  composePage->setThicknessHorizon(activeHorizon());
  if (m_selection)
    connect(m_selection, &SelectionContext::activeHorizonChanged, composePage,
            &ComposePage::setThicknessHorizon);

  // 层位图 PDF（阶段C+E）：导出 → catalog OUTPUT 资产登记 → 布局产物记录 →
  // 发布门重算。失败弹「导出失败 + 原因 + 重试」；成功弹路径 + SHA-256。
  connect(composePage, &ComposePage::exportPdfRequested, this,
          [this, versionStore, catalog, activeHorizon, status]() {
    const auto horizon = activeHorizon();
    auto *canvas = m_canvasCtl ? m_canvasCtl->canvas() : nullptr;
    if (!canvas || !m_projectSvc || !m_projectSvc->project() || !catalog || !catalog->isOpen()) {
      status(tr("请先打开工程并选择地图")); return;
    }
    auto *project = m_projectSvc->project();
    const auto layers = canvas->layers();
    const auto extent = canvas->extent();
    for (auto *layer : layers) {
      if (auto *vector = qobject_cast<QgsVectorLayer *>(layer);
          (vector && vector->isEditable()) ||
          (m_workbench && m_workbench->hasUnpublishedEdits(layer->customProperty("paleoLayerId").toString()))) {
        status(tr("请先保存地图中正在编辑的图层，再输出图件。")); return;
      }
    }
    const auto session = m_taskSvc ? m_taskSvc->session() : 0;
    QPointer<QgsProject> projectGuard(project);
    QList<QPointer<QgsMapLayer>> layerGuards;
    for (auto *layer : layers) layerGuards << layer;
    PaleoGeologicalMapLayout::Settings settings;
    settings.horizon = horizon;
    settings.kind = PaleoStandardElements::FigureKind::WellPosition;
    QString primaryName;
    for (auto *layer : layers) {
      const auto id = layer->customProperty("paleoLayerId").toString();
      if (id.startsWith("facies.") || (m_layerSvc && m_layerSvc->declaration(id).group == "05_PaleoMap")) {
        settings.kind = PaleoStandardElements::FigureKind::Facies; primaryName = layer->name(); break;
      }
      if (layer->type() == Qgis::LayerType::Raster && layer->providerType() == "gdal") {
        settings.kind = PaleoStandardElements::FigureKind::SingleFactor; primaryName = layer->name();
      }
    }
    settings.title = (horizon + " " + PaleoStandardElements::figureKindTitle(settings.kind)).trimmed();
    if (!primaryName.isEmpty()) settings.subtitle = tr("层位：%1 · %2 · 当前地图快照").arg(horizon, primaryName);
    settings.rotation = canvas->rotation();
    settings.version = versionStore && !horizon.isEmpty() ? versionStore->currentVersion(horizon) : 0;
    settings.author = project->customVariables().value("paleoAuthor").toString();
    settings.reviewer = project->customVariables().value("paleoReviewer").toString();
    QStringList inputs, unbound;
    QStringList paths;
    for (auto *layer : layers) paths << layer->source().section('|', 0, 0);
    const auto pathVersions = paleo::exporting::versionIdsForPaths(*catalog, paths);
    for (auto *layer : layers) {
      const auto explicitVersion = layer->customProperty("paleoVersionId").toString();
      if (!explicitVersion.isEmpty() && !catalog->versionById(explicitVersion).id.isEmpty()) {
        inputs << explicitVersion; continue;
      }
      const auto matches = pathVersions.value(layer->source().section('|', 0, 0));
      if (matches.size() == 1) inputs << matches.first();
      else unbound << layer->name();
    }
    inputs.removeDuplicates();
    GeologicalExportDialog dialog(settings,
        [projectGuard, layerGuards, extent](const PaleoGeologicalMapLayout::Settings &settings, QString *error)
            -> std::unique_ptr<PaleoLayoutExport::LayoutHandle> {
          QList<QgsMapLayer *> current;
          for (const auto &layer : layerGuards) {
            if (!layer) { if (error) *error = tr("地图数据已变化，请重新打开导出"); return nullptr; }
            current << layer.data();
          }
          return PaleoGeologicalMapLayout::buildHandle(projectGuard, current, extent, settings, error);
        }, !inputs.isEmpty(), this, unbound, catalog->projectDir());
    if (dialog.exec() != QDialog::Accepted) return;
    if (!m_taskSvc || m_taskSvc->session() != session || !projectGuard) { status(tr("工程已切换，请重新打开导出")); return; }
    auto request = dialog.request();
    request.sourceVersionIds = inputs;
    request.context = {{"project", project->title()}, {"horizon", horizon},
        {"map_version", settings.version}, {"scope", "current_visible_saved_layers"},
        {"unbound_layers", unbound}, {"validation", "unverified"}};
    QString error;
    auto *task = PaleoGeologicalExport::start(dialog.layout(), catalog, m_taskSvc, request, this,
        [this, versionStore, status, horizon](const PaleoGeologicalExport::Result &result) {
      if (result.cancelled) { status(tr("图件导出已取消或工程已切换")); return; }
      if (!result.error.isEmpty()) { status(result.error); PaleoNotify::warning(this, tr("图件导出失败"), result.error); return; }
      if (versionStore && !horizon.isEmpty() && !result.pdfAssetId.isEmpty()) {
        QString error;
        if (!versionStore->recordLayoutProduct(horizon, result.pdfPath, result.pdfAssetId, result.pdfSha256, &error))
          PaleoNotify::warning(this, tr("图件登记"), error);
      }
      const auto summary = tr("已导出 %1 个文件：%2").arg(result.files.size()).arg(result.directory);
      status(summary);
      PaleoNotify::report(this, tr("地质图导出完成"), summary + "\n" + result.warnings.join("\n"));
      if (m_refreshPublishGate) m_refreshPublishGate();
    }, &error);
    status(task ? tr("地质图正在后台导出，可在任务中心查看进度或取消") : error);
  });

  // 方向35：演化动览「定格导出」——当前帧（activeHorizon 的画布现状）抓 PNG
  // 落 catalog OUTPUT 受管资产（PNG 须显式登记为 png，不冒充 pdf）。catalog
  // 缺席（无工程数据目录）→ 如实拒绝，不落无主文件。
  if (auto *player = findChild<EvolutionPlayerPanel *>(QStringLiteral("evolutionPlayer")))
  {
    connect(player, &EvolutionPlayerPanel::frameExportRequested, this,
            [this, catalog](const QString &h) {
              const auto note = [this](const QString &text) {
                if (statusBar())
                  statusBar()->showMessage(text, 8000);
              };
              if (h.isEmpty())
              {
                note(tr("动览尚未定格到任何层位"));
                return;
              }
              if (!m_canvasCtl || !m_canvasCtl->canvas())
              {
                note(tr("画布不可用，无法抓帧"));
                return;
              }
              const QString projectDir =
                  m_projectSvc ? QFileInfo(m_projectSvc->projectPath()).absolutePath()
                               : QString();
              if (!catalog || projectDir.isEmpty())
              {
                note(tr("未打开工程数据目录，演化帧无法登记为导出资产"));
                return;
              }
              const auto image = m_canvasCtl->canvas()->grab().toImage();
              const auto name = QStringLiteral("frame_%1_%2.png")
                                    .arg(h)
                                    .arg(QDateTime::currentMSecsSinceEpoch());
              QString error;
              auto *task = startMapFrameExport(
                  image, m_taskSvc, catalog, projectDir, name, this,
                  [note](const MapProductExportOutcome &outcome) {
                    if (outcome.cancelled)
                      note(QObject::tr("演化帧导出已取消或工程已切换"));
                    else if (outcome.assetId.isEmpty())
                      note(outcome.error);
                    else
                      note(QObject::tr("演化帧已登记：%1")
                               .arg(outcome.managedPath));
                  },
                  &error);
              note(task ? tr("演化帧正在后台导出，可在任务中心查看") : error);
            });
  }

  // 保存版本：commit + 版本号递增（undo 清空在 controller 内，§1223）；
}

// ---------------------------------------------------------------------------
// 版本状态机：保存版本（commit + 递增）（W4 拆分段）
// ---------------------------------------------------------------------------
void PaleoMainWindow::attachMappingVersions(ComposePage *composePage,
                                            MapVersionController *versions)
{
  const auto status = [composePage](const QString &text) {
    if (auto *label = composePage->findChild<QLabel *>(QStringLiteral("statusLabel")))
      label->setText(text);
  };
  const auto activeHorizon = [this]() -> QString {
    return m_selection ? m_selection->activeHorizon() : QString();
  };

  connect(composePage, &ComposePage::saveVersionRequested, this,
          [this, versions, composePage, activeHorizon, status]() {
            const QString h = activeHorizon();
            if (h.isEmpty())
            {
              status(tr("先选择层位再保存版本"));
              return;
            }
            QVariantMap provenance;
            provenance.insert(QStringLiteral("saved_from"),
                              QStringLiteral("compose_page"));
            QString err;
            if (versions->startSaveVersion(h, provenance, &err)) status(tr("正在保存成果快照，可在任务中心查看进度或取消"));
            else
              status(err.isEmpty() ? tr("保存版本失败") : err);
            if (m_refreshPublishGate) m_refreshPublishGate();
          });

}
