// 层：视图
// paleomainwindow_attach — 壳接线聚合胶水层：attachWorkflows 入口 + 静态辅助函数 + 跨页图层显隐辅助
//（子页面/域接线已解耦至 paleomainwindow_attach_<domain>.cpp，严格保证类定义与 API 零变更）。
#include "paleomainwindow.h"

#include "paleotheme.h"
#include "paleoicons.h"
#include "paleoribbon.h"

#include "../qgis/qgiscanvascontroller.h"
#include "../qgis/qgisprojectservice.h"
#include "../qgis/qgislayerservice.h"
#include "../qgis/qgislayerprofile.h"
#include "../linkage/selectioncontext.h"
#include "../linkage/seismicmaplink.h"
#include "layers/layertreepanel.h"
#include "welltops/welltopseditordialog.h"
#include "../workflow/workflows.h"
#include "../workflow/surfacegridding.h"
#include "../workflow/realizationworkflow.h"
#include "../metadata/paleoprojectstore.h"
#include "../metadata/layermanifest.h"
#include "../services/previewdoc.h"
#include "pages/pagepanels.h"
#include "pages/pageshared.h"
#include "pages/datalist.h"
#include "dialogs/griddingdialog.h"
#include "correlationpanel.h"
#include "correlation/petrophyspanel.h"
#include "io/lasdoc.h"
#include "datapreview/datapreviewtabs.h"
#include "wellcomposite/derivedsink.h"
#include "../catalog/datacatalog.h"
#include "../catalog/realizationset.h"
#include "decorations/paleodecorations.h"
#include "edittools/editingtoolbar.h"
#include "../services/paleotaskservice.h"
#include "ui/seismic3d/seismic3dviewpanel.h"
#include "services/seismictaskservice.h"

#include <qgsmapcanvas.h>
#include <qgsproject.h>
#include <qgsmaplayer.h>
#include <qgslayertree.h>
#include <qgslayertreeview.h>
#include <qgslayertreelayer.h>
#include <qgsmessagelog.h>
#include <qgslayoutitemmap.h>

#include <QDir>
#include <QFile>
#include <QFileInfo>
#include "notifications/paleonotify.h"
#include <QStackedLayout>
#include <QStatusBar>
#include <QTabWidget>
#include <QVBoxLayout>

#include <memory>

namespace
{
// ---------------------------------------------------------------------------
// 井综合派生登记（wave/deepen-perf D1，wellcomposite 分段）：
// 全局默认 WellCompositeDerivedSink——预览页 WellCompositePanel 构造即挂接，
// derivedDocumentReady 意图信号落成 catalog DERIVED 版本（受管路径 +
// sha256 + 父版本=源井数据 RAW，审计行进派生 XML「编辑审计」工作表）。
// io 序列化器/井斜时深解析器不在本 TU 绑定：ui→io include 白名单只放行
// lasdoc.h，函数由组装根（src/app/main.cpp）装配期注入；未注入时 sink
// 按错误路径如实回报（状态栏 + 消息日志），不静默回落。
// ---------------------------------------------------------------------------
// goal/gridding-surface-ops：层位散点网格化 + 栅格面运算（壳接线）
// 视图只发意图信号；这里串参数表 → 异步任务 → 受管派生 → manifest 声明。
// ---------------------------------------------------------------------------

// 层位资产 → 散点文本（读当前版本文件）。失败弹原因并返回 false；
// 空文件如实返回 true + 空文本（由勘察阶段报「散点为空」）。
bool readHorizonAssetText(PaleoMainWindow *win, DataCatalog *catalog,
                          const QString &projectDir, const QString &assetId, QByteArray *out)
{
  const CatalogAsset a = catalog->assetById(assetId);
  if (a.id.isEmpty())
  {
    PaleoNotify::warning(win, QObject::tr("网格化"), QObject::tr("资产不存在：%1").arg(assetId));
    return false;
  }
  if (a.type != QLatin1String("horizon"))
  {
    PaleoNotify::warning(win, QObject::tr("网格化"),
                         QObject::tr("「网格化」只适用于层位资产（%1 是 %2）").arg(a.displayName, a.type));
    return false;
  }
  const CatalogVersion v = catalog->currentVersion(assetId);
  const QString src = v.managed ? QDir(projectDir).absoluteFilePath(v.path) : v.path;
  QFile f(src);
  if (!f.open(QIODevice::ReadOnly))
  {
    PaleoNotify::warning(win, QObject::tr("网格化"),
                         QObject::tr("无法读取层位文件：%1").arg(src));
    return false;
  }
  *out = f.readAll();
  return true;
}

// 网格化结果/等厚栅格的 manifest 声明 + 实例化（rasterReady 排队回 GUI 后执行）。
void declareGriddedRaster(PaleoMainWindow *win, QgisLayerService *layerSvc,
                          const QString &layerId, const QString &source, const QString &title,
                          const QString &horizon)
{
  Q_UNUSED(win);
  LayerDeclaration decl;
  decl.layerId = layerId;
  decl.horizon = horizon;
  decl.type = QStringLiteral("raster");
  decl.source = source;
  decl.title = title;
  decl.group = QStringLiteral("00_Data"); // 与导入派生的层位栅格同组
  QString err;
  if (!layerSvc->declare(decl, &err))
  {
    QgsMessageLog::logMessage(QObject::tr("图层声明失败 %1：%2").arg(layerId, err),
                              QStringLiteral("Paleo"), Qgis::MessageLevel::Warning);
    return;
  }
  QgsMapLayer *layer = layerSvc->instantiate(layerId, &err);
  if (layer)
    QgsMessageLog::logMessage(QObject::tr("%1 已上图（%2）").arg(title, layerId),
                              QStringLiteral("Paleo"));
  else
    QgsMessageLog::logMessage(QObject::tr("图层实例化失败 %1：%2").arg(layerId, err),
                              QStringLiteral("Paleo"), Qgis::MessageLevel::Warning);
}

void runHorizonGridding(PaleoMainWindow *win, DataCatalog *catalog, const QString &projectDir,
                        PaleoTaskService *taskSvc, QgisLayerService *layerSvc,
                        const QString &constraintGpkg, const QString &assetId)
{
  QByteArray text;
  if (!readHorizonAssetText(win, catalog, projectDir, assetId, &text))
    return;
  const CatalogAsset a = catalog->assetById(assetId);

  // 参数表上下文：散点勘察下沉在 workflow（io 解析不许进视图层）。
  const SurfaceGriddingWorkflow::Inspect ctxIn =
      SurfaceGriddingWorkflow::inspectHorizonText(text, constraintGpkg);
  if (!ctxIn.ok)
  {
    PaleoNotify::warning(win, QObject::tr("网格化"),
                         QObject::tr("层位 %1：%2").arg(a.displayName, ctxIn.error));
    return;
  }
  PaleoGriddingDialog::RequestContext ctx;
  ctx.horizonName = a.displayName;
  ctx.minX = ctxIn.minX;
  ctx.maxX = ctxIn.maxX;
  ctx.minY = ctxIn.minY;
  ctx.maxY = ctxIn.maxY;
  ctx.hasHeaderCell = ctxIn.hasHeaderCell;
  ctx.headerCellSize = ctxIn.headerCellSize;
  ctx.hasConstraints = ctxIn.hasConstraints;
  PaleoGriddingDialog::Request req;
  if (!PaleoGriddingDialog::prompt(win, ctx, &req))
    return;

  auto *wf = new SurfaceGriddingWorkflow(layerSvc, win);
  wf->setCatalog(catalog, projectDir);
  wf->setBarrierSource(constraintGpkg);
  QObject::connect(wf, &SurfaceGriddingWorkflow::rasterReady, win,
                   [win, layerSvc](const QString &layerId, const QString &source,
                                   const QString &title, const QString &horizon)
                   { declareGriddedRaster(win, layerSvc, layerId, source, title, horizon); });
  QObject::connect(wf, &SurfaceGriddingWorkflow::griddingFailed, win,
                   [win](const QString &, const QString &error)
                   {
                     QgsMessageLog::logMessage(error, QStringLiteral("Paleo"),
                                               Qgis::MessageLevel::Warning);
                     win->statusBar()->showMessage(error, 10000);
                   });

  const QString horizon = a.displayName;
  const QByteArray textCopy = text;
  if (taskSvc)
  {
    taskSvc->start(QObject::tr("网格化 %1（最小曲率）").arg(horizon),
                   [wf, horizon, textCopy, req](PaleoTask *t) -> QString
                   {
                     SurfaceGriddingWorkflow::Options opt;
                     opt.cellSize = req.cellSize;
                     opt.tension = req.tension;
                     opt.maxSweeps = req.maxSweeps;
                     opt.useBarriers = req.useBarriers;
                     opt.runCrossValidation = req.runCrossValidation;
                     opt.cvPoints = req.cvPoints;
                     SurfaceGriddingWorkflow::Outcome o;
                     const QString err = wf->gridHorizonText(
                         horizon, textCopy, opt, [t] { return t->cancelRequested(); },
                         [t](const QString &stage, int percent)
                         { t->reportStage(stage, percent); },
                         &o);
                     if (err.isEmpty())
                     {
                       t->reportDetail(QObject::tr("网格 %1×%2，%3 遍%4，距数据最远 %5")
                                           .arg(o.cols)
                                           .arg(o.rows)
                                           .arg(o.sweeps)
                                           .arg(o.converged
                                                    ? QObject::tr("（已收敛）")
                                                    : QObject::tr("（达迭代上限未收敛）"))
                                           .arg(o.distToDataMax, 0, 'f', 1));
                       if (o.cvFolds > 0)
                         t->reportDetail(QObject::tr("留一法 CV RMS = %1（%2 折）")
                                             .arg(o.cvRms, 0, 'g', 4)
                                             .arg(o.cvFolds));
                     }
                     return err;
                    });
  }
  else
  {
    // 无任务服务（测试/退化环境）：同步跑，错误弹窗。
    SurfaceGriddingWorkflow::Options opt;
    opt.cellSize = req.cellSize;
    opt.tension = req.tension;
    opt.maxSweeps = req.maxSweeps;
    opt.useBarriers = req.useBarriers;
    opt.runCrossValidation = req.runCrossValidation;
    opt.cvPoints = req.cvPoints;
    SurfaceGriddingWorkflow::Outcome o;
    const QString err = wf->gridHorizonText(horizon, text, opt, nullptr, nullptr, &o);
    if (!err.isEmpty())
      PaleoNotify::warning(win, QObject::tr("网格化失败"), err);
  }
}

// 图层树「面运算（等厚/体积）」：声明栅格两两组合 → 等厚 + 体积报告。
void runSurfaceOps(PaleoMainWindow *win, QgisLayerService *layerSvc, DataCatalog *catalog,
                   const QString &projectDir)
{
  QVector<LayerDeclaration> decls;
  QString readErr;
  if (!layerSvc || !layerSvc->tryDeclared(&decls, &readErr))
  {
    PaleoNotify::warning(win, QObject::tr("面运算"), readErr);
    return;
  }
  QVector<QPair<QString, QString>> candidates; // (title, source)
  for (const LayerDeclaration &d : decls)
    if (d.type == QLatin1String("raster") && !d.source.isEmpty())
      candidates.append({d.title.isEmpty() ? d.layerId : d.title, d.source});
  PaleoGriddingDialog::IsopachSelection sel;
  if (!PaleoGriddingDialog::promptIsopach(win, candidates, &sel))
    return;

  auto *wf = new SurfaceGriddingWorkflow(layerSvc, win);
  wf->setCatalog(catalog, projectDir);
  QObject::connect(wf, &SurfaceGriddingWorkflow::rasterReady, win,
                   [win, layerSvc](const QString &layerId, const QString &source,
                                   const QString &title, const QString &horizon)
                   { declareGriddedRaster(win, layerSvc, layerId, source, title, horizon); });

  const QString label = QStringLiteral("%1–%2")
                            .arg(PaleoGriddingDialog::safeAssetLabel(
                                candidates.at(sel.topIndex).first),
                                 PaleoGriddingDialog::safeAssetLabel(
                                     candidates.at(sel.baseIndex).first));
  SurfaceGriddingWorkflow::VolumeReport rep;
  const QString err = wf->isopachBetweenRasters(
      candidates.at(sel.topIndex).second, candidates.at(sel.baseIndex).second, label, &rep,
      sel.writeManaged, nullptr);
  if (!err.isEmpty())
  {
    PaleoNotify::warning(win, QObject::tr("面运算失败"), err);
    return;
  }
  QVector<QPair<QString, QString>> metrics;
  metrics.append({QObject::tr("有效像元"), QString::number(rep.cells)});
  metrics.append({QObject::tr("空值像元"), QString::number(rep.nullCells)});
  metrics.append({QObject::tr("正厚度像元"), QString::number(rep.positiveCells)});
  metrics.append({QObject::tr("负厚度像元（顶低于底）"), QString::number(rep.negativeCells)});
  metrics.append({QObject::tr("面积"), QObject::tr("%1 m²").arg(rep.area, 0, 'g', 8)});
  metrics.append({QObject::tr("体积（z 单位·m²）"),
                  QObject::tr("%1（时间层位为 ms·m²，换算 m³ 需速度场）")
                      .arg(rep.volume, 0, 'g', 8)});
  metrics.append({QObject::tr("厚度范围"),
                  QObject::tr("%1 ~ %2").arg(rep.minThickness, 0, 'g', 6)
                                        .arg(rep.maxThickness, 0, 'g', 6)});
  metrics.append({QObject::tr("平均厚度"), QString::number(rep.meanThickness, 'g', 6)});
  PaleoGriddingDialog::showVolumeReport(
      win, QObject::tr("等厚 / 体积报告：%1").arg(label), metrics, rep.csv());
}

void attachWellCompositeDerived(PaleoMainWindow *win, DataCatalog *catalog)
{
  if (!win || !catalog)
    return;
  auto *sink = new WellComposite::WellCompositeDerivedSink(win);
  sink->bind(catalog, QString()); // 工程目录按 catalog 当前打开的工程解析（换工程自适应）
  WellComposite::WellCompositeDerivedSink::setDefault(sink);
  QObject::connect(sink, &WellComposite::WellCompositeDerivedSink::derivedRegistered, win,
                   [win](const QString &path, const QString &versionId) {
                     win->statusBar()->showMessage(
                         QObject::tr("派生版本已登记 catalog：%1（%2）").arg(path, versionId),
                         10000);
                   });
  QObject::connect(sink, &WellComposite::WellCompositeDerivedSink::derivedFailed, win,
                   [win](const QString &reason) {
                     QgsMessageLog::logMessage(reason, QStringLiteral("Paleo"),
                                               Qgis::MessageLevel::Warning);
                     win->statusBar()->showMessage(
                         QObject::tr("派生版本登记失败：%1").arg(reason), 10000);
                   });
}
} // namespace

void PaleoMainWindow::attachWorkflows(PredictionWorkflow *pred, ConstraintWorkflow *constraint,
                                      CompositionWorkflow *compose, ValidationWorkflow *validate,
                                      DataImportService *importSvc, SeismicMapLink *seismicLink,
                                      QgisProcessingService *procSvc, PaleoProjectStore *store,
                                      QgisEditingService *editSvc, QgisLayoutService *layoutSvc,
                                      PaleoTaskService *taskSvc)
{
  // 幂等守卫：本函数不是增量接线，而是整建——清空右栏页面栈重建、往
  // bottomTabs/状态栏加面板（correlationPanel、releasePanel、任务/属性表、
  // statusCatalogError…）、在 topBar 建按钮（processingButton、designerButton…）
  // 并往 importSvc/preview/pages 上叠信号连接。同一窗口二次执行会重复建
  // dock/按钮（各对象双份），且旧页面 deleteLater 后残留的信号捕获会悬空，
  // 后续用例段错误。测试套件会二次触达同一窗口（tst_ui 单跑用例的补调
  // 路径 + attachWorkflowsIsIdempotent 直证），因此首次完整接线后早退；
  // rightPanelHost 缺席的早退不算完成，不置位。
  if (m_workflowsAttached)
    return;
  auto *host = findChild<QWidget *>(QStringLiteral("rightPanelHost"));
  auto *stack = host ? static_cast<QStackedLayout *>(host->layout()) : nullptr;
  if (!stack)
    return;
  m_taskSvc = taskSvc; // D1b：导入任务池（nullptr 时保持同步旧路径）
  m_importSvc = importSvc; // 「从工区文件夹新建」直达入口
  if (importSvc)
    m_previewDoc = new PreviewDocService(importSvc, this); // 壳唯一数据门面（页属性/预览共用）
  if (taskSvc && !m_seismicTaskSvc)
  {
    m_seismicTaskSvc = std::make_unique<seismic::SeismicTaskService>(taskSvc, 256, this);
  }
  if (m_seismicTaskSvc && m_seismic3dPanel)
  {
    m_seismic3dPanel->setTaskService(m_seismicTaskSvc.get());
  }

  // Replace placeholders in page order (data, predict, constraint, compose, validate).
  while (stack->count() > 0)
  {
    QLayoutItem *item = stack->takeAt(0);
    if (item->widget())
      item->widget()->deleteLater();
    delete item;
  }

  // 数据管理页（ribbon 布局）：DataPage 本体进中央「数据列表」；实体数据
  // 视图段挂到右 dock 第 0 页「数据属性」；导入段藏起——导入命令在 ribbon
  // 「数据导入」组（镜像这些按钮，门控/原因仍写在按钮上）。
  auto *dataPage = new DataPage(m_dataListHost ? m_dataListHost : host);
  auto *dataProps = new QWidget(host);
  dataProps->setObjectName(QStringLiteral("dataPropertiesPage"));
  {
    auto *pl = new QVBoxLayout(dataProps);
    pl->setContentsMargins(PaleoTheme::tokens().spacingSm, PaleoTheme::tokens().spacingSm, PaleoTheme::tokens().spacingSm, PaleoTheme::tokens().spacingSm); // spacing.sm
    auto *previewDetails = new QWidget(dataProps);
    previewDetails->setObjectName(QStringLiteral("previewDetailsHost"));
    auto *detailsLayout = new QVBoxLayout(previewDetails);
    detailsLayout->setContentsMargins(0, 0, 0, 0);
    pl->addWidget(previewDetails);
    m_previewTabs->setDetailsHost(previewDetails);
    if (m_dataListHost)
    {
      m_dataListHost->layout()->addWidget(dataPage);
      if (QWidget *entity = dataPage->entityViewSection())
        pl->addWidget(entity, 1); // 重新挂父：刷新按段查找，不受影响
      if (auto *imports = dataPage->findChild<QWidget *>(QStringLiteral("dataImportSection")))
        imports->hide();
    }
    else
      pl->addWidget(dataPage);
  }
  auto *predictPage = new PredictPage(pred, m_layerSvc, host);
  auto *constraintPage = new ConstraintPage(constraint, host);
  auto *composePage = new ComposePage(compose, m_layerSvc, host);
  auto *validatePage = new ValidatePage(validate, host);
  stack->addWidget(dataProps);
  stack->addWidget(new QWidget(host)); // 地层对比使用中央 Web 页，无右侧属性页
  stack->addWidget(predictPage);
  stack->addWidget(constraintPage);
  stack->addWidget(composePage);
  stack->addWidget(validatePage);

  attachSections(seismicLink);
  attachWellSection(taskSvc, store); // 连井剖面 dock ← workflow + 编辑产物落库（store）
  attachWellCompositeDerived(this, m_previewDoc ? m_previewDoc->catalog() : nullptr);
  WellCorrelationPanel *corrPanel = nullptr;
  if (auto *bottomTabs = findChild<QTabWidget *>(QStringLiteral("bottomTabs")))
  {
    if (m_selection)
    {
      corrPanel = new WellCorrelationPanel(m_selection, bottomTabs);
      corrPanel->setObjectName(QStringLiteral("correlationPanel"));
      corrPanel->setTaskService(taskSvc); // B1：大 LAS 解析走 quiet 异步（nullptr 保持同步旧路径）
      // goal/ui-experience-polish：异步 LAS 解析失败此前全仓无消费者——井列
      // 静默不出现对用户不可见；接状态栏 + MessageLog（错误可见，E1 弹窗豁免）。
      connect(corrPanel, &WellCorrelationPanel::lasLoadError, this,
              [this](const QString &wellId, const QString &reason) {
                statusBar()->showMessage(
                    tr("测井 %1 曲线加载失败：%2").arg(wellId, reason), 6000);
                QgsMessageLog::logMessage(
                    tr("连井剖面：井 %1 LAS 加载失败 — %2").arg(wellId, reason),
                    QStringLiteral("Paleo"));
              });
      bottomTabs->addTab(corrPanel, tr("测井对比"));
      m_corrPanel = corrPanel;
    }
  }

  // goal/petrophysics-logs：测井计算面板（视图只发意图信号）+ 壳层编排：
  // 井集 = 连井面板当前井 → catalog 解析 LAS → PetroPhysTaskService 批任务
  // （进度/取消/产物落盘走写队列 + catalog DERIVED 登记）→ 结果曲线并回
  // 曲线集（mergeComputedCurves，上轨由用户勾选驱动）。
  if (taskSvc && !m_petroPhysSvc)
    m_petroPhysSvc = std::make_unique<paleo::petrophys::PetroPhysTaskService>(
        taskSvc, store, this);
  if (auto *bottomTabs = findChild<QTabWidget *>(QStringLiteral("bottomTabs")))
  {
    auto *petroPanel = new paleo::petrophys::PetroPhysPanel(bottomTabs);
    petroPanel->setObjectName(QStringLiteral("petrophysPanel"));
    bottomTabs->addTab(petroPanel, tr("测井计算"));
    if (m_petroPhysSvc)
    {
      connect(petroPanel, &paleo::petrophys::PetroPhysPanel::computeRequested, this,
              [this, petroPanel, corrPanel](
                  const paleo::petrophys::PetroPhysTaskService::BatchRequest &intent) {
                if (!corrPanel || corrPanel->wellCount() == 0)
                {
                  petroPanel->showResult(
                      false, tr("井集为空——先在数据页导入井（含 LAS）"));
                  return;
                }
                QStringList wellIds;
                for (int i = 0; i < corrPanel->wellCount(); ++i)
                  wellIds.append(corrPanel->wellAt(i));
                auto *catalog = m_previewDoc ? m_previewDoc->catalog() : nullptr;
                // projectDir = catalogPath 去固定后缀（derivedsink.cpp 同先例；
                // 形状异常 → 空 = 工程目录未解析，如实报错）
                const QString catSuffix =
                    QStringLiteral("/artifacts/metadata/catalog.json");
                const QString catPath = catalog ? catalog->catalogPath() : QString();
                const QString projDir =
                    catPath.endsWith(catSuffix)
                        ? catPath.left(catPath.size() - catSuffix.size())
                        : QString();
                QStringList missing;
                paleo::petrophys::PetroPhysTaskService::BatchRequest req = intent;
                if (catalog && !projDir.isEmpty())
                {
                  req.wells = paleo::petrophys::PetroPhysTaskService::resolveWellLas(
                      catalog, projDir, wellIds, &missing);
                }
                petroPanel->setWellScope(corrPanel->wellCount(), req.wells.size());
                if (req.wells.isEmpty())
                {
                  petroPanel->showResult(
                      false, tr("无可解析 LAS 的井：%1").arg(missing.join(u'；')));
                  return;
                }
                petroPanel->setBusy(true);
                m_petroPhysTask = m_petroPhysSvc->startBatch(
                    req, catalog, projDir + QStringLiteral("/artifacts/derived/petrophys"),
                    [this, petroPanel, corrPanel, req](
                        bool ok, const paleo::petrophys::PetroPhysTaskService::BatchResult &res) {
                      int merged = 0;
                      if (corrPanel)
                      {
                        for (const auto &w : res.wells)
                        {
                          if (!w.ok || w.values.isEmpty())
                            continue;
                          LasCurve depth;
                          depth.name = QStringLiteral("DEPT");
                          depth.values = w.depths;
                          LasCurve c;
                          c.name = req.outputMnemonic;
                          c.unit = req.outputUnit;
                          c.descr = req.outputDescr;
                          c.values = w.values;
                          corrPanel->mergeComputedCurves(w.wellId, {depth, c});
                          ++merged;
                        }
                      }
                      QString extra;
                      if (!res.error.isEmpty())
                        extra = tr("（%1）").arg(res.error);
                      petroPanel->showResult(
                          ok, tr("%1/%2 井完成，%3 条曲线并入曲线集%4")
                                  .arg(res.succeeded)
                                  .arg(res.wells.size())
                                  .arg(merged)
                                  .arg(extra));
                    });
                if (m_petroPhysTask)
                {
                  connect(m_petroPhysTask, &PaleoTask::changed, this,
                          [this, petroPanel]() {
                            if (PaleoTask *t = m_petroPhysTask.data())
                              petroPanel->updateProgress(t->percent(), t->stage());
                          });
                }
              });
      connect(petroPanel, &paleo::petrophys::PetroPhysPanel::cancelRequested, this,
              [this]() {
                if (m_petroPhysTask)
                  m_petroPhysTask->requestCancel();
              });
    }
    else
    {
      petroPanel->setEnabled(false); // 无任务服务（测试/小环境）：不可用如实降级
    }
  }

  // W4：按页拆接线——入口只排顺序；每页一段私有成员函数（本 TU）。
  attachDataPage(dataPage, corrPanel, importSvc, taskSvc);
  attachPredictPage(predictPage, pred);
  attachConstraintPage(constraintPage, constraint);
  attachComposePage(composePage, compose, layoutSvc);
  attachValidatePage(validatePage, validate, corrPanel, importSvc);

  // goal/gridding-surface-ops：层位右键「网格化…」→ 参数表 → 异步任务 →
  // 受管派生 + manifest 声明上图。视图只发意图信号（gridHorizonRequested）。
  if (dataPage && dataPage->listPanel() && m_previewDoc && m_previewDoc->catalog())
  {
    const QString projectDir =
        m_projectSvc ? QFileInfo(m_projectSvc->projectPath()).absolutePath() : QString();
    const DataCatalog *catalogConst = m_previewDoc->catalog();
    const QString gpkg = store ? store->gpkgPath() : QString();
    connect(dataPage->listPanel(), &DataListPanel::gridHorizonRequested, this,
            [this, catalogConst, projectDir, taskSvc, gpkg](const QString &assetId)
            {
              if (projectDir.isEmpty())
              {
                statusBar()->showMessage(tr("先打开工程再网格化（派生产物需要受管目录）"));
                return;
              }
              runHorizonGridding(this, const_cast<DataCatalog *>(catalogConst), projectDir,
                                 taskSvc, m_layerSvc, gpkg, assetId);
            });
    // 方向 47：集合树的成员/统计叶节点 → 同上图通道（图签文案同源）。
    connect(dataPage->listPanel(), &DataListPanel::realizationMemberRequested, this,
            [this](const QString &setId, int index) {
              showRealizationMember(setId, index);
            });
    connect(dataPage->listPanel(), &DataListPanel::realizationStatRequested, this,
            [this](const QString &setId, const QString &token) {
              showRealizationStat(setId, token);
            });
    // 方向 32：井分层右键「编辑分层…」→ 版本化编辑对话框（GUI 线程单事务）。
    connect(dataPage->listPanel(), &DataListPanel::topsEditRequested, this,
            [this, catalogConst, projectDir](const QString &assetId)
            {
              if (projectDir.isEmpty())
              {
                statusBar()->showMessage(tr("先打开工程再编辑分层（新版本需要受管目录）"));
                return;
              }
              WellTopsEditorDialog dlg(const_cast<DataCatalog *>(catalogConst), projectDir,
                                       assetId, this);
              dlg.exec();
            });
    // 图层树栅格「面运算（等厚/体积）…」。
    if (m_layerPanel)
      connect(m_layerPanel, &LayerTreePanel::surfaceOpsRequested, this,
              [this, catalogConst, projectDir](const QString &)
              {
                if (projectDir.isEmpty() || !m_layerSvc)
                {
                  statusBar()->showMessage(tr("面运算需要已打开的工程与图层声明"));
                  return;
                }
                runSurfaceOps(this, m_layerSvc, const_cast<DataCatalog *>(catalogConst),
                              projectDir);
              });
  }

  PaleoEditingToolbar *editTb =
      attachShellSurfaces(store, procSvc, editSvc, layoutSvc, taskSvc);

  buildRibbonPanels(dataPage, predictPage, constraintPage, composePage, validatePage, editTb,
                    corrPanel);

  // Re-sync visible page index with current tab.
  const int idx = paleo::pagesinternal::kPageIds.indexOf(m_currentPage);
  stack->setCurrentIndex(idx >= 0 ? idx : 0);

  if (store)
  {
    connect(store, &PaleoProjectStore::readOnlyChanged, this, &PaleoMainWindow::setProjectReadOnly);
    if (store->isReadOnly())
      setProjectReadOnly(true);
  }

  m_workflowsAttached = true;
}

// ---------------------------------------------------------------------------
// m2(C) 接缝：版面地图项钉页面档案主题（实现说明见 paleomainwindow.h）
// ---------------------------------------------------------------------------
bool PaleoMainWindow::revealDeclaredLayer(const QString &layerId, bool zoomTo)
{
  if (!m_layerSvc || layerId.isEmpty())
    return false;
  QString err;
  QgsMapLayer *layer = m_layerSvc->instantiate(layerId, &err);
  if (!layer)
  {
    QgsMessageLog::logMessage(tr("图层上图失败：%1").arg(err.isEmpty() ? layerId : err),
                              QStringLiteral("Paleo"), Qgis::MessageLevel::Warning);
    return false;
  }
  QgsProject *proj = m_projectSvc ? m_projectSvc->project() : nullptr;
  QgsLayerTree *tree = proj ? proj->layerTreeRoot() : nullptr;
  if (tree)
  {
    if (QgsLayerTreeLayer *node = tree->findLayer(layer->id()))
    {
      node->setItemVisibilityCheckedParentRecursive(true);
      if (auto *view = findChild<QgsLayerTreeView *>(QStringLiteral("layerTreeView")))
        view->setCurrentLayer(layer);
    }
  }
  if (zoomTo && m_canvasCtl)
  {
    if (QgsMapCanvas *canvas = m_canvasCtl->canvas())
      canvas->setCurrentLayer(layer);
    m_canvasCtl->zoomToLayer(layerId);
  }
  return true;
}

void PaleoMainWindow::showRealizationLayer(const QString &layerId,
                                           const QString &badgeTitle,
                                           const QString &badgeSub)
{
  if (!revealDeclaredLayer(layerId, false))
  {
    statusBar()->showMessage(tr("集合图层上图失败：%1").arg(layerId), 8000);
    return;
  }
  // 同集合互斥：成员面 realset.<id>.m* 与统计面 realset.<id>.stat.* 各按
  // 前缀成组——切成员只换成员、切统计只换统计，成员+统计可并置同读；
  // 差值面 realsetdiff.* 不互斥（对比视图可叠在成员/统计之上）。
  QString mutexPrefix;
  const int statPos = layerId.indexOf(QStringLiteral(".stat."));
  const int memberPos = layerId.lastIndexOf(QStringLiteral(".m"));
  if (statPos >= 0)
    mutexPrefix = layerId.left(statPos) + QStringLiteral(".stat.");
  else if (memberPos >= 0)
    mutexPrefix = layerId.left(memberPos) + QStringLiteral(".m");
  if (!mutexPrefix.isEmpty())
  {
    QgsProject *proj = m_projectSvc ? m_projectSvc->project() : nullptr;
    QgsLayerTree *tree = proj ? proj->layerTreeRoot() : nullptr;
    if (tree && m_layerSvc)
    {
      const QVector<LayerDeclaration> decls = m_layerSvc->declared();
      for (const LayerDeclaration &d : decls)
      {
        if (d.layerId == layerId || !d.layerId.startsWith(mutexPrefix))
          continue;
        if (QgsMapLayer *other = m_layerSvc->layer(d.layerId))
          if (auto *node = tree->findLayer(other->id()))
            node->setItemVisibilityChecked(false);
      }
    }
  }
  // 不确定性图签：图签只随集合面在画布期间存在；badgeTitle 空 = 关。
  if (m_decorMgr)
  {
    if (badgeTitle.isEmpty())
      m_decorMgr->clearUncertaintyBadge();
    else
      m_decorMgr->setUncertaintyBadge(badgeTitle, badgeSub);
  }
}

void PaleoMainWindow::showRealizationMember(const QString &setId, int index)
{
  QString subtitle;
  if (DataCatalog *cat = m_previewDoc ? m_previewDoc->catalog() : nullptr)
  {
    const paleo::realization::RealizationSet set =
        paleo::realization::setById(*cat, setId);
    subtitle = tr("%1 成员在场").arg(set.members.size());
    if (!set.missingIndices.isEmpty())
    {
      QStringList missing;
      for (const int idx : set.missingIndices)
        missing << QStringLiteral("#%1").arg(idx);
      subtitle += tr(" · 缺 %1").arg(missing.join(QStringLiteral(" ")));
    }
  }
  showRealizationLayer(RealizationWorkflow::memberLayerId(setId, index),
                       tr("集合成员 #%1").arg(index), subtitle);
}

void PaleoMainWindow::showRealizationStat(const QString &setId, const QString &token)
{
  QString subtitle;
  if (DataCatalog *cat = m_previewDoc ? m_previewDoc->catalog() : nullptr)
  {
    const auto stats = paleo::realization::statSurfaces(*cat, setId);
    for (const auto &s : stats)
      if (s.token == token)
        subtitle = tr("%1 成员参与").arg(s.memberCount);
  }
  showRealizationLayer(RealizationWorkflow::statLayerId(setId, token),
                       paleo::realization::statisticDisplayLabel(token), subtitle);
}

void PaleoMainWindow::pinLayoutTheme(QgsLayoutItemMap *mapItem, const QString &pageId)
{
  const QString theme = QgisLayerProfileService::pageThemeName(pageId);
  if (m_profileSvc)
  {
    m_profileSvc->setLayoutMapTheme(mapItem, theme);
    return;
  }
  mapItem->setFollowVisibilityPreset(true);
  mapItem->setFollowVisibilityPresetName(theme);
}
