// 层：视图
// paleomainwindow_attach — W4 壳瘦身：attachWorkflows 按页拆私有成员函数
//（attachDataPage/attachPredictPage/attachConstraintPage/attachComposePage/
// attachValidatePage + attachShellSurfaces），attachMapping 按段拆（发布门
// /导出接线/版本状态机）。仍是 PaleoMainWindow 成员函数，定义在本 TU；
// 入口函数只排顺序 + 幂等守卫。
#include "paleomainwindow.h"

#include "paleotheme.h"
#include "paleoicons.h"
#include "paleoribbon.h"

#include "../qgis/qgiscanvascontroller.h"
#include "../qgis/qgisprojectservice.h"
#include "../qgis/qgislayerservice.h"
#include "../services/toolavailability.h"
#include "../linkage/selectioncontext.h"
#include "layers/layertreepanel.h"
#include "../workflow/depthconversionworkflow.h"
#include "../workflow/propertymodelworkflow.h"
#include "../workflow/faultinterpretationcontroller.h"
#include "propertymodel/propertymodelpanel.h"
#include "faciesmapping/faciesmappingpanel.h"
#include "../workflow/workflows.h"
#include "../domain/arearules.h"
#include "../domain/projectclassifier.h"
#include "../linkage/seismicmaplink.h"
#include "../linkage/threewaylocator.h"
#include "../qgis/qgisprocessingservice.h"
#include "../metadata/paleoprojectstore.h"
#include "../metadata/layermanifest.h"
#include "../metadata/mapversionstore.h"
#include "../services/projectdata.h"
#include "../services/previewdoc.h"
#include "../workflow/folderimport.h"
#include "../workflow/registration.h"
#include "../workflow/mappingworkflow.h"
#include "../workflow/mapexport.h"
#include "../workflow/mapversioncontroller.h"
#include "locator/paleolocatorfilters.h"
#include "releasepanel.h"
#include "taskpanel.h"
#include "attributetablepanel.h"
#include "pages/pagepanels.h"
#include "pages/pageshared.h" // kPageIds
#include "pages/dataops/dataopsimportqueue.h" // B2：导入队列生产 runner（wave/deepen-perf）
#include "pages/datalist.h" // B2：listPanel()->importQueuePanel() 需完整类型
#include "constraintdrawcontroller.h"
#include "typedconstraintdrawcontroller.h" // m2(B)：物源线/展布线/控制点类型化捕获

#include <QDateTime>
#include <QDir>
#include <QFileInfo>

#include <memory>
#include "dialogs/folderconfirm.h"
#include "dialogs/griddingdialog.h" // goal/gridding-surface-ops：网格化/面运算参数表
#include "../workflow/surfacegridding.h"
#include "layers/layertreepanel.h"
#include "correlationpanel.h"
#include "correlation/petrophyspanel.h" // goal/petrophysics-logs：测井计算面板（意图信号）
#include "io/lasdoc.h"                  // LasCurve 值类型（ui io 白名单头）
#include "datapreview/datapreviewtabs.h"
#include "wellcomposite/derivedsink.h" // deepen-perf D1：井综合派生登记 sink
#include "../catalog/datacatalog.h"
#include "layoutdesignershell.h"
#include "edittools/editingtoolbar.h"
#include "layout/layoutexportactions.h"     // m2(C)：导出版面钉 compose 主题后走同一 PDF 出口
#include "../qgis/qgislayoutservice.h"
#include "../qgis/qgislayerprofile.h"       // m1 页面档案：setLayoutMapTheme/pageThemeName
#include "../qgis/qgiseditingservice.h"
#include "../services/paleotaskservice.h"
#include "../services/pythonenv.h"
#include "../workflow/mamcltool.h"
#include "ui/seismicsection/seismicsectiondockwidget.h"
#include "ui/seismic3d/seismic3dviewpanel.h"
#include "services/seismictaskservice.h"
#include "domain/seismic/sgyvolume.h"

#include <qgsmapcanvas.h>
#include <qgsmaptool.h>
#include <qgsproject.h>
#include <qgsmaplayer.h>
#include <qgslayertree.h>
#include <qgslayertreeview.h>
#include <qgsmessagelog.h>
#include <qgslocatorwidget.h>
#include <qgslocator.h>
#include <qgsadvanceddigitizingdockwidget.h>
#include <qgsvectorlayer.h>
#include <qgslayertreelayer.h>
#include <qgslayout.h>
#include <qgslayoutitemmap.h>
#include <qgsprintlayout.h>
#include <qgsapplication.h>
#include <qgsprocessingalgorithm.h>
#include <qgsprocessingregistry.h>

#include <QCoreApplication>
#include <QDir>
#include <QDoubleSpinBox>
#include <QFileDialog>
#include <QFileInfo>
#include <QGuiApplication>
#include <QLabel>
#include <QLineEdit>
#include <QMap>
#include <QMenu>
#include <QMessageBox>
#include <QPushButton>
#include <QShortcut>
#include <QToolButton>
#include <QSettings>
#include <QStandardPaths>
#include <QStatusBar>
#include <QStackedLayout>
#include <QTabWidget>
#include <QTextEdit>
#include <QPointer>
#include <QVBoxLayout>
#include <QComboBox>
#include <QDialog>
#include <QDialogButtonBox>

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
    QMessageBox::warning(win, QObject::tr("网格化"), QObject::tr("资产不存在：%1").arg(assetId));
    return false;
  }
  if (a.type != QLatin1String("horizon"))
  {
    QMessageBox::warning(win, QObject::tr("网格化"),
                         QObject::tr("「网格化」只适用于层位资产（%1 是 %2）").arg(a.displayName, a.type));
    return false;
  }
  const CatalogVersion v = catalog->currentVersion(assetId);
  const QString src = v.managed ? QDir(projectDir).absoluteFilePath(v.path) : v.path;
  QFile f(src);
  if (!f.open(QIODevice::ReadOnly))
  {
    QMessageBox::warning(win, QObject::tr("网格化"),
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
    QMessageBox::warning(win, QObject::tr("网格化"),
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
      QMessageBox::warning(win, QObject::tr("网格化失败"), err);
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
    QMessageBox::warning(win, QObject::tr("面运算"), readErr);
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
    QMessageBox::warning(win, QObject::tr("面运算失败"), err);
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

// goal/time-depth-velocity：层树「转换为深度域…」意图信号 → 建模/换算/登记
// 全在 DepthConversionWorkflow（功能层）；壳只解析声明与反馈状态。
void PaleoMainWindow::attachDepthConversion(DepthConversionWorkflow *depth)
{
  m_depthWf = depth;
  if (!depth || !m_layerPanel)
    return;
  connect(m_layerPanel, &LayerTreePanel::depthConversionRequested, this,
          [this, depth](const QString &layerId) {
    const auto status = [this](const QString &text, bool warn) {
      QgsMessageLog::logMessage(text, QStringLiteral("Paleo"),
                                warn ? Qgis::MessageLevel::Warning : Qgis::MessageLevel::Info);
      if (statusBar())
        statusBar()->showMessage(text, 8000);
    };
    const QString horizon = layerId.mid(QStringLiteral("horizon.").size());
    if (!m_layerSvc)
    {
      status(tr("层服务不可用——深度域转换未接线"), true);
      return;
    }
    QString timeRaster;
    for (const LayerDeclaration &d : m_layerSvc->declared())
      if (d.layerId == layerId)
        timeRaster = d.source;
    if (timeRaster.isEmpty())
    {
      status(tr("找不到层位 %1 的时间域栅格声明").arg(horizon), true);
      return;
    }
    // 模型优先复用最新存档；缺则从 catalog 井控制数据先建层间平均模型。
    QString modelPath = depth->latestModelPath();
    if (modelPath.isEmpty())
    {
      const VelocityModelBuildRequest req = depth->requestFromCatalog();
      if (req.topsFilePaths.isEmpty() && req.tdFilePaths.isEmpty())
      {
        status(tr("没有可用的井分层/校验炮数据——先导入再转换"), true);
        return;
      }
      QString err;
      modelPath = depth->buildAndStoreModel(req, &err);
      if (modelPath.isEmpty())
      {
        status(tr("速度模型建立失败：%1").arg(err), true);
        return;
      }
      status(tr("已建立并存档速度模型（层间平均）"), false);
    }
    QString convertedId, err;
    if (!depth->convertRasterToDepth(horizon, timeRaster, modelPath, &convertedId, &err))
    {
      status(tr("深度域转换失败：%1").arg(err), true);
      return;
    }
    status(tr("深度域转换完成：%1 → %2").arg(horizon, convertedId), false);
  });
}


// ---- goal/facies-automapping：证据合成 + QA 报告面板 --------------------
// 同 attachPropertyModel 形态：面板只发意图，链路在 FaciesMappingWorkflow
//（约束装配/优势相/相界/合成/QA/登记全在功能层）。幂等：dock 已建则只
// 更新 workflow 指针。
void PaleoMainWindow::attachFaciesMapping(FaciesMappingWorkflow *wf)
{
  if (wf)
    m_faciesMappingWf = wf;
  if (m_faciesMappingDock)
    return;

  m_faciesMappingPanel = new FaciesMappingPanel(this);
  m_faciesMappingDock = new QDockWidget(tr("相图合成"), this);
  m_faciesMappingDock->setObjectName(QStringLiteral("faciesMappingDock"));
  m_faciesMappingDock->setWidget(m_faciesMappingPanel);
  addDockWidget(Qt::RightDockWidgetArea, m_faciesMappingDock);
  if (m_rightDock)
    tabifyDockWidget(m_rightDock, m_faciesMappingDock);
  m_faciesMappingDock->hide();

  connect(m_faciesMappingPanel, &FaciesMappingPanel::cancelRequested, this, [this]() {
    m_faciesMappingRunner.requestCancel();
    if (m_faciesMappingTask)
      m_faciesMappingTask->requestCancel();
  });

  // QA 行点击 → 画布定位（问题逐条可定位；无画布时状态栏如实说明）。
  connect(m_faciesMappingPanel, &FaciesMappingPanel::issueSelected, this,
          [this](const QString &regionId, double x, double y) {
            if (m_canvasCtl)
            {
              m_canvasCtl->zoomToPoint(x, y);
              if (statusBar())
                statusBar()->showMessage(tr("已定位到问题单元 %1").arg(regionId), 5000);
            }
            else if (statusBar())
            {
              statusBar()->showMessage(
                  tr("画布不可用——问题单元 %1 位于 (%2, %3)").arg(regionId).arg(x).arg(y),
                  8000);
            }
          });

  connect(m_faciesMappingPanel, &FaciesMappingPanel::generateRequested, this,
          [this](const QString &horizon, double wellWeight, double factorWeight,
                 double predictionWeight, double assignThreshold, double minRegionArea,
                 double minIslandArea, double wellCoverageRadius) {
            if (!m_faciesMappingWf || m_faciesMappingRunning || !m_faciesMappingPanel)
              return;
            if (horizon.isEmpty())
            {
              m_faciesMappingPanel->showResult(false, tr("需要层位"));
              return;
            }

            FaciesMappingWorkflow::DraftFaciesRequest request;
            request.horizon = horizon;
            request.wellWeight = wellWeight;
            request.factorWeight = factorWeight;
            request.predictionWeight = predictionWeight;
            request.assignThreshold = assignThreshold;
            request.minRegionArea = minRegionArea;
            request.minIslandArea = minIslandArea;
            request.wellCoverageRadius = wellCoverageRadius;
            QString err;
            if (!m_faciesMappingWf->assembleConstraintInputs(&request, &err))
            {
              m_faciesMappingPanel->showResult(false, err);
              if (statusBar())
                statusBar()->showMessage(tr("草稿相图失败：%1").arg(err), 8000);
              return;
            }

            m_faciesMappingRunning = true;
            m_faciesMappingPanel->setBusy(true);
            if (m_taskSvc)
            {
              // JobRunner 三段式：compute 在任务池 worker，commit 回 owner 线程
              //（catalog 登记线程亲和由框架断言）。
              m_faciesMappingRunner.setTaskService(m_taskSvc);
              m_faciesMappingJob.reset();
              m_faciesMappingTask = m_faciesMappingWf->startJob(
                  m_faciesMappingRunner, request, m_faciesMappingPanel, &m_faciesMappingJob);
              if (m_faciesMappingTask)
                connect(m_faciesMappingTask.data(), &PaleoTask::finished, this,
                        [this] { finishFaciesMappingRun(); });
              else
                m_faciesMappingRunning = false;
              return;
            }

            // 无任务池（未接线壳/旧测试）：同步直跑，不泵事件。
            auto computed = m_faciesMappingWf->runCompute(
                request, [this](double fraction, const QString &stage) {
                  if (m_faciesMappingPanel)
                    m_faciesMappingPanel->updateProgress(
                        static_cast<int>(fraction * 100.0), stage);
                  return true;
                });
            if (!computed.ok)
            {
              m_faciesMappingRunning = false;
              m_faciesMappingPanel->showResult(false, computed.error);
              return;
            }
            if (!m_faciesMappingWf->commitComputed(request, &computed))
            {
              m_faciesMappingRunning = false;
              m_faciesMappingPanel->showResult(false, computed.error);
              return;
            }
            m_faciesMappingJob = std::make_shared<FaciesMappingWorkflow::DraftFaciesJob>();
            m_faciesMappingJob->computed = std::move(computed);
            finishFaciesMappingRun();
          });
}

void PaleoMainWindow::finishFaciesMappingRun()
{
  m_faciesMappingRunning = false;
  const bool cancelled =
      m_faciesMappingTask && m_faciesMappingTask->state() == PaleoTask::State::Cancelled;
  m_faciesMappingTask = nullptr;
  if (!m_faciesMappingPanel)
    return;

  // 三段式 commit 段成功时结果已在 job 里回填；同步兜底路径由调用方先行
  // commit 再进这里（同 finishPropertyModelRun 的判据口径）。
  const FaciesMappingWorkflow::DraftFaciesComputed *computed =
      m_faciesMappingJob ? &m_faciesMappingJob->computed : nullptr;
  if (cancelled)
  {
    m_faciesMappingPanel->showResult(false, tr("已取消"));
    return;
  }
  if (!computed || !computed->ok)
  {
    const QString why = computed ? computed->error : tr("无计算结果");
    m_faciesMappingPanel->showResult(false, why);
    if (statusBar())
      statusBar()->showMessage(tr("草稿相图失败：%1").arg(why), 8000);
    return;
  }

  // QA 报告行喂表：工作流给的 issue 结构 → 面板行（视图不读文件、不判几何）。
  QList<FaciesMappingPanel::QaRow> rows;
  rows.reserve(static_cast<int>(computed->qaIssues.size()));
  for (const paleo::faciesmapping::FaciesQaIssue &issue : computed->qaIssues)
  {
    FaciesMappingPanel::QaRow row;
    row.type = QString::fromLatin1(paleo::faciesmapping::faciesQaIssueName(issue.type));
    QStringList ids;
    for (const std::string &id : issue.regionIds)
      ids << QString::fromStdString(id);
    row.regionIds = ids.join(QLatin1Char(';'));
    QStringList related;
    for (const std::string &id : issue.relatedIds)
      related << QString::fromStdString(id);
    row.related = related.join(QLatin1Char(';'));
    row.metric = issue.metric;
    row.x = issue.location.x;
    row.y = issue.location.y;
    rows.append(row);
  }
  m_faciesMappingPanel->setQaRows(rows);
  const int regionCount = computed->extra.value(QStringLiteral("region_count")).toInt();
  const int issues = static_cast<int>(computed->qaIssues.size());
  m_faciesMappingPanel->showResult(
      true, tr("草稿相图完成：%1 个单元 · %2 条 QA 问题").arg(regionCount).arg(issues));
  if (statusBar())
    statusBar()->showMessage(
        tr("草稿相图完成：%1 个单元，QA 问题 %2 条").arg(regionCount).arg(issues), 8000);
}

void PaleoMainWindow::attachPropertyModel(PropertyModelWorkflow *wf,
                                           paleo::fault::FaultInterpretationController *faults)
{
  m_propModelWf = wf;
  m_propModelFaults = faults;
  if (m_propModelDock)
    return;

  m_propModelPanel = new PropertyModelPanel(this);
  m_propModelDock = new QDockWidget(tr("属性建模"), this);
  m_propModelDock->setObjectName(QStringLiteral("propertyModelDock"));
  m_propModelDock->setWidget(m_propModelPanel);
  addDockWidget(Qt::RightDockWidgetArea, m_propModelDock);
  if (m_rightDock)
    tabifyDockWidget(m_rightDock, m_propModelDock);
  m_propModelDock->hide();

  connect(m_propModelPanel, &PropertyModelPanel::cancelRequested, this, [this]() {
    m_propModelCancel = true;
    // 方向20：取消经框架传播（会同时置本代 cancel 标志与任务自身的取消位）。
    // 同步兜底路径没有任务，靠 m_propModelCancel 那个布尔的原逻辑仍有效。
    m_propModelRunner.requestCancel();
    if (m_propModelTask)
      m_propModelTask->requestCancel();
  });
  connect(m_propModelPanel, &PropertyModelPanel::alphaChanged, this, [this](double alpha) {
    if (m_seismicSectionDock && m_seismicSectionDock->canvas())
      m_seismicSectionDock->canvas()->setZoneOverlayAlpha(alpha);
  });
  connect(m_propModelPanel, &PropertyModelPanel::buildRequested, this,
          [this](const QString &top, const QString &bot, const QString &curve, int nLayers,
                 int aggregator, double idwPower, double overlayAlpha) {
            if (m_propModelDock)
            {
              m_propModelDock->show();
              m_propModelDock->raise();
            }
            if (!m_propModelWf || m_propModelRunning || !m_propModelPanel)
              return;

            QString err;
            PropertyModelRequest req = m_propModelWf->requestFromCatalog(
                top, bot, curve, nLayers, static_cast<paleo::stratgrid::Aggregator>(aggregator),
                idwPower, &err);
            if (!err.isEmpty())
            {
              m_propModelPanel->showResult(false, err);
              if (statusBar())
                statusBar()->showMessage(tr("属性建模失败：%1").arg(err), 8000);
              return;
            }
            if (m_propModelFaults)
            {
              const auto segs =
                  PropertyModelWorkflow::segmentsFromFaultSet(m_propModelFaults->faultSet());
              req.faults.insert(req.faults.end(), segs.begin(), segs.end());
            }

            m_propModelRunning = true;
            m_propModelCancel = false;
            m_propModelJob.reset(); // 新一代：别让 UI 段读到上一代的登记结果
            m_propModelPanel->setBusy(true);

            if (m_taskSvc)
            {
              // #85 + 方向20：重计算段（格架/粗化/IDW）跑任务池 worker，不再占
              // GUI 线程。catalog 登记（#106 owner-thread 写守卫）与切片/叠置
              // 留在 commit 段——框架把它排在 owner 线程执行并在入口断言线程
              // 亲和，#80 的纪律由此变成机制。忙则拒绝由框架 busy() 门控承担
              // （原 m_propModelRunning 布尔的等价物）。
              m_propModelRunner.setTaskService(m_taskSvc);
              m_propModelTask = m_propModelWf->startJob(m_propModelRunner, req, overlayAlpha,
                                                        m_propModelPanel, &m_propModelJob);
              if (m_propModelTask)
                connect(m_propModelTask.data(), &PaleoTask::finished, this,
                        [this, overlayAlpha] { finishPropertyModelRun(overlayAlpha); });
              else
                m_propModelRunning = false;
              return;
            }

            // 无任务池（未接线壳/旧测试）：同步直跑，不泵事件。
            m_propModelComputed = m_propModelWf->runCompute(
                req, [this](double fraction, const QString &stage) {
                  if (m_propModelPanel)
                    m_propModelPanel->updateProgress(static_cast<int>(fraction * 100.0), stage);
                  return !m_propModelCancel;
                });
            finishPropertyModelRun(overlayAlpha);
          });
}

void PaleoMainWindow::finishPropertyModelRun(double overlayAlpha)
{
  m_propModelRunning = false;
  const bool cancelled =
      m_propModelTask && m_propModelTask->state() == PaleoTask::State::Cancelled;
  m_propModelTask = nullptr;
  if (!m_propModelWf || !m_propModelPanel)
    return;

  // finished 回包/同步兜底都在 GUI 线程。方向20 起 catalog 登记由 JobRunner 的
  // commit 段完成（且已断言 owner 线程），本函数只负责把结果呈到 UI——不要再
  // 无条件调 commitComputed，那会二次登记同一份 staging。
  //
  // 判据用 job->registered（commit 段成功时置位），不拿 runner.busy() 判：commit
  // 段在发 finished 之前就 clearTask() 了，busy() 会误判成「没登记过」。
  // 同步兜底路径（无任务池）压根不建 job，故仍需直连登记。
  PropertyModelOutput out;
  if (m_propModelJob)
  {
    m_propModelComputed = m_propModelJob->computed; // 共享同一份，commit 已回填
  }
  else if (!cancelled && m_propModelComputed.ok)
  {
    m_propModelWf->commitComputed(&m_propModelComputed); // 无池兜底：同步直连
  }
  out = m_propModelComputed.out;
  if (cancelled)
  {
    out.ok = false;
    out.error = tr("已取消");
  }

  if (!out.ok)
  {
    const QString why = out.error.isEmpty() ? tr("属性建模失败") : out.error;
    m_propModelPanel->showResult(false, why);
    if (statusBar())
      statusBar()->showMessage(tr("属性建模失败：%1").arg(why), 8000);
    return;
  }

  const QString fileName = QFileInfo(out.path).fileName();
  m_propModelPanel->showResult(
      true, tr("已充填 %1 个单元 · %2").arg(out.filledCells).arg(fileName));
  if (statusBar())
    statusBar()->showMessage(
        tr("属性建模完成：%1（%2 个单元）").arg(fileName).arg(out.filledCells), 8000);

  const int ni = out.volume.grid.ni;
  const int nj = out.volume.grid.nj;
  const int nk = out.volume.grid.nk;
  if (ni <= 0 || nj <= 0 || nk <= 0)
    return;

  seismic::Seismic3DViewPanel *panel3d = m_seismic3dPanel;
  if (!panel3d && m_seismic3dDock)
    panel3d = qobject_cast<seismic::Seismic3DViewPanel *>(m_seismic3dDock->widget());
  seismic::Seismic3DViewportWidget *vp = panel3d ? panel3d->viewport() : nullptr;
  seismic::SeismicSectionCanvas *canvas =
      m_seismicSectionDock ? m_seismicSectionDock->canvas() : nullptr;

  seismic::PropertyBrickAxes axes;
  axes.iMin = 0;
  axes.iMax = ni - 1;
  axes.jMin = 0;
  axes.jMax = nj - 1;
  axes.kMin = 0;
  axes.kMax = nk - 1;

  struct SlicePlan
  {
    int axis;
    int index;
    seismic::SeismicSliceSlot slot;
    seismic::SgySliceType type;
  };
  const SlicePlan plans[3] = {
      {0, ni / 2, seismic::SeismicSliceSlot::Crossline, seismic::SgySliceType::Xline},
      {1, nj / 2, seismic::SeismicSliceSlot::Inline, seismic::SgySliceType::Inline},
      {2, nk / 2, seismic::SeismicSliceSlot::Time, seismic::SgySliceType::Time},
  };
  seismic::SgySliceImage baked[3];
  bool bakedOk[3] = {false, false, false};
  for (int n = 0; n < 3; ++n)
  {
    QString sliceErr;
    const auto slice =
        PropertyModelWorkflow::gridSlice(out.volume, plans[n].axis, plans[n].index, &sliceErr);
    if (!sliceErr.isEmpty() || slice.width <= 0 || slice.height <= 0 || slice.values.empty())
      continue;
    seismic::SgySliceImage img;
    img.width = slice.width;
    img.height = slice.height;
    img.valueMin = slice.valueMin;
    img.valueMax = slice.valueMax;
    img.values = slice.values;
    baked[n] = std::move(img);
    bakedOk[n] = true;
    if (vp)
      vp->updatePropertySlice(plans[n].slot, plans[n].type, plans[n].index, axes, baked[n]);
  }
  if (!canvas || !canvas->hasData())
    return;
  const bool timeLike = canvas->orientation() == seismic::SectionOrientation::TimeSlice;
  const int prefer[3] = {timeLike ? 2 : 1, timeLike ? 1 : 0, timeLike ? 0 : 2};
  for (int n : prefer)
  {
    if (!bakedOk[n])
      continue;
    if (canvas->traceCount() == baked[n].width && canvas->sampleCount() == baked[n].height)
    {
      canvas->setZoneOverlay(baked[n]);
      canvas->setZoneOverlayAlpha(overlayAlpha);
      break;
    }
  }
}

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
    pl->setContentsMargins(8, 8, 8, 8); // spacing.sm
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
  stack->addWidget(predictPage);
  stack->addWidget(constraintPage);
  stack->addWidget(composePage);
  stack->addWidget(validatePage);

  attachSections(seismicLink);
  attachWellSection(taskSvc); // 连井剖面 dock ← workflow（m_seismicTaskSvc 已在上方就位）
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

  m_workflowsAttached = true; // 走到末尾才算接线完成（幂等守卫置位）
}
// ---------------------------------------------------------------------------
// 数据页接线：门面绑定 / catalog 联动 / 预览分栏 / 导入意图（W4 拆分段）
// ---------------------------------------------------------------------------
void PaleoMainWindow::attachDataPage(DataPage *dataPage,
                                     WellCorrelationPanel *corrPanel,
                                     DataImportService *importSvc,
                                     PaleoTaskService *taskSvc)
{
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
              syncSeismicVolumeToDocks();
            });
    dataPage->refreshAssetTable();
    syncSeismicVolumeToDocks();
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
    const auto setImportsEnabled = [this](bool enabled) {
      for (const char *name : {"importWells", "importSeismic", "importBoundary",
                               "importFolder"})
        if (auto *btn = findChild<QPushButton *>(QLatin1String(name)))
        {
          btn->setEnabled(enabled);
          if (!enabled)
            btn->setToolTip(tr("数据目录打开失败 — 导入暂不可用（见状态栏）"));
          else
            btn->setToolTip(QString());
        }
    };
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
                syncSeismicVolumeToDocks();
              });
    DataPreviewTabs *preview = m_previewTabs;
    if (preview)
    {
      preview->setDocService(m_previewDoc);
      preview->setTaskService(taskSvc); // D1：剖面索引/解码异步化（nullptr 时保持同步）
      if (m_projectSvc)
        preview->setProject(m_projectSvc->project());
      // D11 临时配准：GeoJSON 标签手工仿射 → DERIVED + 水印图层。
      connect(preview, &DataPreviewTabs::provisionalRegistrationRequested, this,
              [this, importSvc](const QString &assetId, const QVariantMap &params) {
                applyProvisionalRegistration(importSvc, assetId, params);
              });
      connect(dataPage, &DataPage::assetActivated, preview, &DataPreviewTabs::openAsset);
      connect(dataPage, &DataPage::assetActivated, dataPage, &DataPage::selectAsset);
      connect(dataPage, &DataPage::assetWellActivated, preview, &DataPreviewTabs::openAssetForWell);
      connect(dataPage, &DataPage::seismicLineActivated, preview,
              [preview](const QString &aid, const QString &mode) {
                preview->openSeismicLine(aid, mode, 0, 0.0);
              });
      connect(dataPage, &DataPage::surveyAreaActivated, preview, &DataPreviewTabs::openSurveyArea);
      connect(preview, &DataPreviewTabs::requestShowOnMainCanvas, this, [this]() {
        if (m_workspaceStack)
          m_workspaceStack->setCurrentIndex(0);
      });
      if (auto *inner = preview->findChild<QTabWidget *>(QStringLiteral("dataPreviewTabs")))
      {
        connect(inner, &QTabWidget::currentChanged, this, [preview, dataPage](int idx) {
          if (idx >= 0 && preview && dataPage)
          {
            const QString aid = preview->assetIdAt(idx);
            if (!aid.isEmpty())
              dataPage->selectAsset(aid);
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
                [this](const QString &wellEntityId) {
                  m_selection->setSelection({wellEntityId}, QStringLiteral("datapreview"));
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
    connect(dataPage, &DataPage::importRequested, this,
            [this, importSvc, preview](const QString &kind) {
              if (kind == QLatin1String("folder"))
              {
                runFolderImport(importSvc);
                return;
              }
              const QString filter = (kind == QLatin1String("seismic"))
                                         ? tr("地震数据文件 (*.sgy *.segy);;All files (*)")
                                         : ((kind == QLatin1String("well_log"))
                                                ? tr("测井与柱状图文件 (*.las *.xml *.txt *.csv);;All files (*)")
                                                : tr("数据文件 (*.las *.xml *.csv *.dat *.gpkg *.shp *.tif *.img);;All files (*)"));
              const QString path = QFileDialog::getOpenFileName(
                  this, tr("导入 %1").arg(kind), QString(), filter);
              if (path.isEmpty())
                return;
              // T22：单文件导入同样展示 CRS 契约句（确认一步，含识别类型）。
              {
                QDialog confirmDlg(this);
                confirmDlg.setObjectName(QStringLiteral("singleImportDialog"));
                confirmDlg.setWindowTitle(tr("导入数据"));
                auto *cl = new QVBoxLayout(&confirmDlg);
                const QString recogType = classifyProjectImport(path).type;
                auto *fileLabel = new QLabel(
                    tr("文件：%1\n识别类型：%2").arg(path, PaleoFolderConfirm::folderTypeLabel(recogType)),
                    &confirmDlg);
                fileLabel->setWordWrap(true);
                cl->addWidget(fileLabel);
                auto *crsNote = new QLabel(PaleoFolderConfirm::engineeringCrsSentence(), &confirmDlg);
                crsNote->setObjectName(QStringLiteral("singleImportCrsNote"));
                crsNote->setWordWrap(true);
                PaleoTheme::applyThemedStyleSheet(
                    crsNote, [] { return PaleoTheme::mutedCaptionStyleSheet(); });
                cl->addWidget(crsNote);
                auto *bb = new QDialogButtonBox(&confirmDlg);
                bb->addButton(tr("导入"), QDialogButtonBox::AcceptRole);
                bb->addButton(tr("取消"), QDialogButtonBox::RejectRole);
                QObject::connect(bb, &QDialogButtonBox::accepted, &confirmDlg,
                                 &QDialog::accept);
                QObject::connect(bb, &QDialogButtonBox::rejected, &confirmDlg,
                                 &QDialog::reject);
                cl->addWidget(bb);
                if (confirmDlg.exec() != QDialog::Accepted)
                  return;
              }
              // D1b：LAS 解析/SEG-Y 索引等大文件在任务池跑——无任务服务时保持
              // 同步旧路径。imported 信号照常排队回 GUI（预览标签在终态后开）。
              // D1b：LAS 解析/SEG-Y 索引等大文件在任务池跑（编排在
              // FolderImportWorkflow）；imported 信号照常排队回 GUI。
              if (auto *wf = folderImportWorkflow())
                wf->importFile(kind, path, [](const QString &assetId,
                                              const QString &err) {
                  if (assetId.isEmpty())
                    QgsMessageLog::logMessage(
                        QObject::tr("导入失败：%1").arg(err),
                        QStringLiteral("Paleo"), Qgis::Critical);
                });
            });
  }
    // Imported assets feed the bottom-dock correlation panel and the central
    // preview tabs (§4: the preview tab opens only after the import is
    // confirmed; 地震预览不再走底栏面板，预览壳直接按资产渲染测线控件)。
    DataPreviewTabs *previewForImport = m_previewTabs;
    connect(m_previewDoc, &PreviewDocService::assetImported, this,
            [this, corrPanel, previewForImport](const QString &kind, const QString &assetId, const QString &) {
              // 文件夹导入期间不逐文件开标签——确认后只开井口标签（§4）。
              if (previewForImport && !m_folderImportActive)
                previewForImport->openAsset(assetId);
              if (corrPanel && kind == QLatin1String("well_log"))
              {
                QList<QPair<QString, QString>> wells;
                for (const QString &id : m_previewDoc->assetIds(QStringLiteral("well_log")))
                  wells.append({id, m_previewDoc->assetSource(id)});
                corrPanel->setWells(wells);
                // LAS imports pull the GR curve into the column when present.
                const QString src = m_previewDoc->assetSource(assetId);
                if (src.endsWith(QLatin1String(".las"), Qt::CaseInsensitive))
                  corrPanel->loadWellLas(assetId,
                                         QDir(m_projectSvc ? QFileInfo(m_projectSvc->projectPath()).absolutePath()
                                                           : QString()).absoluteFilePath(src),
                                         QStringLiteral("GR"));
              }
            });
}

// ---------------------------------------------------------------------------
// 预测页接线（W4 拆分段）
// ---------------------------------------------------------------------------
void PaleoMainWindow::attachPredictPage(PredictPage *predictPage,
                                        PredictionWorkflow *pred)
{
  if (pred && predictPage)
  {
    // 工程打开后 appcontext 已把 onnx 模型根重设到 <工程>/models——算法
    // 列表随之刷新（未装模型 → 页面如实降级文案）。
    connect(m_projectSvc, &QgisProjectService::projectOpened, this,
            [this, pred, predictPage] {
              predictPage->setAlgorithms(pred->availableAlgorithms());
            });
    // ---- m2(A): 预测运行任务化 + 历史结果显示接线 ----
    // 任务池在场 → runPrediction 跑 worker 线程（模仿导入任务化样例：结果经
    // PaleoTask 终态回 GUI；changed→进度、finished→解忙+失败文案）；取消是
    // 协作式的——预测运算本体无法中断，取消后以实际完成状态如实呈现。
    // 无任务服务时保持同步旧路径。
    connect(predictPage, &PredictPage::runRequested, this,
            [this, pred, predictPage](const QString &horizon, const QString &algId,
                                      const QVariantMap &params) {
              auto *status = predictPage->findChild<QLabel *>(QStringLiteral("statusLabel"));
              const auto logFail = [status](const QString &msg) {
                if (status)
                  status->setText(msg);
                QgsMessageLog::logMessage(msg, QStringLiteral("Paleo"), Qgis::Critical);
              };
              if (!m_taskSvc)
              {
                QString err;
                pred->runPrediction(horizon, algId, params, &err);
                return;
              }
              predictPage->setRunBusy(true);
              auto outErr = std::make_shared<QString>();
              PaleoTask *task = m_taskSvc->start(
                  tr("预测 %1 · %2").arg(horizon, algId),
                  [pred, horizon, algId, params, outErr](PaleoTask *) -> QString {
                    QString err;
                    if (pred->runPrediction(horizon, algId, params, &err))
                      return QString();
                    return err.isEmpty() ? tr("预测失败") : err;
                  });
              QObject::connect(task, &PaleoTask::changed, predictPage,
                               [predictPage, task] {
                                 const int pct = task->percent();
                                 if (pct >= 0) // 无进度回调的算法不伪造进度
                                   predictPage->updateProgress(pct);
                               });
              QObject::connect(task, &PaleoTask::finished, predictPage,
                               [predictPage, task, outErr, status, logFail] {
                                 predictPage->setRunBusy(false);
                                 if (task->state() == PaleoTask::State::Cancelled)
                                 {
                                   const QString msg = tr(
                                       "已请求取消——预测运算无法中断，结果以实际完成为准");
                                   if (status)
                                     status->setText(msg);
                                   QgsMessageLog::logMessage(msg, QStringLiteral("Paleo"),
                                                             Qgis::MessageLevel::Warning);
                                 }
                                 else if (task->state() == PaleoTask::State::Failed)
                                 {
                                   logFail(tr("预测失败：%1").arg(*outErr));
                                 }
                               });
            });
    // 取消按钮 → 协作式取消请求（worker 自查 cancelRequested）。
    connect(predictPage, &PredictPage::runCancelRequested, this, [this]( ) {
      if (!m_taskSvc || m_taskSvc->tasks().isEmpty())
        return;
      // 最近一个「预测 ·」任务是本页发起的运行（页内同一时刻至多一个）。
      for (int i = m_taskSvc->tasks().size() - 1; i >= 0; --i)
      {
        PaleoTask *t = m_taskSvc->tasks().at(i);
        if (t->running() && t->title().startsWith(tr("预测")))
        {
          t->requestCancel();
          return;
        }
      }
    });
    // 历史结果「显示」：instantiate → 图层树勾选 → zoomToLayer（§37 按需实例化）。
    connect(predictPage, &PredictPage::showResultRequested, this,
            [this](const QString &layerId) {
              if (!m_layerSvc)
                return;
              QString err;
              QgsMapLayer *layer = m_layerSvc->instantiate(layerId, &err);
              if (!layer)
              {
                QgsMessageLog::logMessage(tr("在地图上显示失败：%1").arg(err),
                                          QStringLiteral("Paleo"), Qgis::Critical);
                return;
              }
              if (m_canvasCtl)
                m_canvasCtl->zoomToLayer(layerId);
              QgsProject *proj = m_projectSvc ? m_projectSvc->project() : nullptr;
              QgsLayerTree *treeRoot = proj ? proj->layerTreeRoot() : nullptr;
              if (QgsLayerTreeLayer *node =
                      treeRoot ? treeRoot->findLayer(layer->id()) : nullptr)
                node->setItemVisibilityChecked(true); // 显示意图（可能已在）
            });
    // ---- m2(A) end ----
  }

  // MAMCL 外部工具：视图只发意图；解包/venv/依赖/启动编排在 MamclTool
  // （功能层）+ PythonEnvService（数据层）。与 pred 是否在场无关，独立接线。
  if (predictPage)
  {
    auto *status = predictPage->findChild<QLabel *>(QStringLiteral("statusLabel"));
    const QString envRoot =
        QStandardPaths::writableLocation(QStandardPaths::AppDataLocation) +
        QStringLiteral("/external/mamcl");
    auto *pyenv = new PythonEnvService(envRoot, this);
    auto *mamcl = new MamclTool(pyenv, this);
    connect(predictPage, &PredictPage::mamclLaunchRequested, mamcl, &MamclTool::open);
    connect(mamcl, &MamclTool::busyChanged, predictPage, &PredictPage::setMamclBusy);
    connect(mamcl, &MamclTool::statusMessage, predictPage,
            [status](const QString &msg) {
              if (status)
                status->setText(msg);
            });
    connect(mamcl, &MamclTool::launchFinished, predictPage,
            [status](bool ok, const QString &msg) {
              if (status)
                status->setText(msg);
              if (!ok)
                QgsMessageLog::logMessage(msg, QStringLiteral("Paleo"), Qgis::Critical);
            });
    // pip/解包逐行输出落消息日志（状态条只承载阶段文案，不刷屏）。
    connect(pyenv, &PythonEnvService::outputLine, this, [](const QString &line) {
      QgsMessageLog::logMessage(line, QStringLiteral("MAMCL"), Qgis::Info);
    });
  }
}

// ---------------------------------------------------------------------------
// 约束页接线：绘制捕获 + IDW 插值（W4 拆分段）
// ---------------------------------------------------------------------------
void PaleoMainWindow::attachConstraintPage(ConstraintPage *constraintPage,
                                           ConstraintWorkflow *constraint)
{
  if (constraint && constraintPage)
  {
    // 约束页端到端: 绘制请求 → 画布上的捕获工具 → workflow 提交 (§42).
    if (m_canvasCtl)
    {
      auto *drawCtl = new ConstraintDrawController(m_canvasCtl, constraint, this);
      connect(constraintPage, &ConstraintPage::drawConstraintRequested, drawCtl,
              [drawCtl](const QString &horizon, const QString &shape, int faciesCode) {
                drawCtl->startCapture(horizon, shape, faciesCode);
              });
      connect(drawCtl, &ConstraintDrawController::captureFailed, this,
              [](const QString &err) {
                QgsMessageLog::logMessage(err, QStringLiteral("Paleo"), Qgis::MessageLevel::Warning);
              });
    }
    connect(constraintPage, &ConstraintPage::runIdwRequested, this,
            [this, constraint, constraintPage](const QString &horizon) {
              auto *status = constraintPage->findChild<QLabel *>(QStringLiteral("statusLabel"));
              const auto fail = [status](const QString &msg) {
                if (status)
                  status->setText(msg);
                QgsMessageLog::logMessage(msg, QStringLiteral("Paleo"), Qgis::MessageLevel::Warning);
              };
              if (m_factorTask && m_factorTask->running())
              {
                fail(tr("已有单因素计算在进行"));
                return;
              }

              QString field = QStringLiteral("z");
              if (auto *edit = constraintPage->findChild<QLineEdit *>(QStringLiteral("idwField")))
              {
                const QString typed = edit->text().trimmed();
                if (!typed.isEmpty())
                  field = typed;
              }
              double cellSize = 1.0;
              if (auto *spin = constraintPage->findChild<QDoubleSpinBox *>(QStringLiteral("idwCellSize")))
                cellSize = spin->value();

              QString pointsId;
              if (!m_layerSvc)
              {
                fail(tr("图层服务未就绪"));
                return;
              }
              QVector<LayerDeclaration> decls;
              QString readErr;
              if (!m_layerSvc->tryDeclared(&decls, &readErr))
              {
                fail(readErr.isEmpty() ? tr("无法读取图层清单") : readErr);
                return;
              }
              QString agnostic;
              for (const LayerDeclaration &d : decls)
              {
                if (d.type.compare(QStringLiteral("vector"), Qt::CaseInsensitive) != 0)
                  continue;
                if (!d.layerId.startsWith(QStringLiteral("wells")))
                  continue;
                if (d.horizon == horizon)
                {
                  pointsId = d.layerId;
                  break;
                }
                if (agnostic.isEmpty() && d.horizon.isEmpty())
                  agnostic = d.layerId;
              }
              if (pointsId.isEmpty())
                pointsId = agnostic;
              if (pointsId.isEmpty())
              {
                fail(tr("层位 %1 没有井点图层").arg(horizon));
                return;
              }

              QString err;
              if (!constraint->runConstraintIDW(horizon, pointsId, field, cellSize, &err))
                fail(err.isEmpty() ? tr("约束插值失败") : err);
            });

    // ---- m2(B): 单因素图页（deliverable 2）接线——三入口的类型化捕获。----
    // 状态列刷新：页面监听 layerDeclared/factorGenerated（含重开工程重读）。
    if (m_layerSvc)
      constraintPage->bindLayerService(m_layerSvc);
    // 生成链：本地方向走任务池（准备/发布留在 catalog 所属线程）。
    // 其它方法仍同步调用 generateFactor。
    connect(constraintPage, &ConstraintPage::generateFactorRequested, this,
            [this, constraint, constraintPage](const QString &factorId, const QString &horizon,
                                               const QVariantMap &params) {
              auto *status = constraintPage->findChild<QLabel *>(QStringLiteral("statusLabel"));
              const auto fail = [status](const QString &msg) {
                if (status)
                  status->setText(msg);
                QgsMessageLog::logMessage(msg, QStringLiteral("Paleo"), Qgis::MessageLevel::Warning);
              };
              if (m_factorTask && m_factorTask->running())
              {
                fail(tr("已有单因素计算在进行"));
                return;
              }
              const QString methodId = params.value(QStringLiteral("method")).toString();
              const bool taskPool = methodId == QLatin1String("local_direction_idw")
                                 || methodId == QLatin1String("surfer_idw");
              if (!taskPool || !m_taskSvc)
              {
                QString err;
                if (!constraint->generateFactor(horizon, factorId, params, &err))
                  fail(err.isEmpty() ? tr("单因素生成失败") : err);
                return;
              }
              auto job = std::make_shared<ConstraintWorkflow::LocalDirectionJob>();
              QString prepErr;
              if (!constraint->prepareLocalDirectionJob(horizon, factorId, params, job.get(), &prepErr))
              {
                fail(prepErr.isEmpty() ? tr("单因素生成失败") : prepErr);
                return;
              }
              constraintPage->setRunBusy(true);
              constraintPage->noteRunStage(tr("正在准备"), 5);
              auto lastReport = std::make_shared<qint64>(0);
              PaleoTask *task = m_taskSvc->start(
                  tr("单因素 %1 · %2").arg(horizon, factorId),
                  [constraint, job, lastReport](PaleoTask *running) -> QString {
                    const bool ok = constraint->computeLocalDirectionJob(
                        job.get(),
                        [running] { return running->cancelRequested(); },
                        [running, lastReport](double percent) {
                          const qint64 now = QDateTime::currentMSecsSinceEpoch();
                          if (percent < 100.0 && now - *lastReport < 50)
                            return;
                          *lastReport = now;
                          const int pct = qBound(0, qRound(percent), 100);
                          QString stage = QStringLiteral("interpolate");
                          if (pct < 10)
                            stage = QStringLiteral("prepare");
                          else if (pct < 25)
                            stage = QStringLiteral("geometry");
                          else if (pct >= 80)
                            stage = QStringLiteral("encode");
                          running->reportStage(stage, pct);
                        });
                    if (running->cancelRequested())
                      return QStringLiteral("已取消");
                    if (!ok)
                      return job->error.isEmpty() ? QObject::tr("本地方向插值失败") : job->error;
                    return QString();
                  },
                  QString(), PaleoTask::Priority::High, true);
              m_factorTask = task;
              connect(task, &PaleoTask::changed, constraintPage, [this, constraintPage, task] {
                if (!constraintPage || task->stagePercent() < 0)
                  return;
                const QString stage = task->stage();
                QString label = tr("正在插值");
                if (stage == QLatin1String("prepare"))
                  label = tr("正在准备");
                else if (stage == QLatin1String("geometry"))
                  label = tr("正在读取几何");
                else if (stage == QLatin1String("encode"))
                  label = tr("正在编码");
                constraintPage->noteRunStage(label, task->stagePercent());
              });
              connect(task, &PaleoTask::finished, this,
                      [this, constraint, constraintPage, job, fail, task] {
                        const auto clearTask = [this, task] {
                          if (m_factorTask == task)
                            m_factorTask.clear();
                        };
                        if (!constraintPage)
                        {
                          clearTask();
                          return;
                        }
                        if (task->state() == PaleoTask::State::Cancelled)
                        {
                          if (job->outputPath.contains(QStringLiteral("paleo-sf-")))
                            QDir(QFileInfo(job->outputPath).absolutePath()).removeRecursively();
                          constraintPage->setRunBusy(false);
                          if (auto *status =
                                  constraintPage->findChild<QLabel *>(QStringLiteral("statusLabel")))
                            status->setText(tr("已取消"));
                          clearTask();
                          return;
                        }
                        if (task->state() != PaleoTask::State::Succeeded)
                        {
                          constraintPage->setRunBusy(false);
                          fail(task->errorText().isEmpty() ? tr("单因素生成失败") : task->errorText());
                          clearTask();
                          return;
                        }
                        // 发布是临界区：不再接受取消，避免写到一半丢掉声明。
                        constraintPage->noteRunStage(tr("正在保存"), 95);
                        QString pubErr;
                        const bool published = constraint->publishLocalDirectionJob(*job, &pubErr);
                        constraintPage->setRunBusy(false);
                        if (!published)
                          fail(pubErr.isEmpty() ? tr("单因素生成失败") : pubErr);
                        else
                          constraintPage->noteRunStage(tr("正在保存"), 100);
                        clearTask();
                      });
            });
    connect(constraintPage, &ConstraintPage::runCancelRequested, this, [this] {
      if (m_factorTask && m_factorTask->running())
        m_factorTask->requestCancel();
    });
    // 等值线（§12 GIS LineString）：horizon 从 factor layerId 前缀取（"factor.<h>.<fid>"）。
    connect(constraint, &ConstraintWorkflow::contoursGenerated, this,
            [this](const QString &, const QString &, const QString &layerId) {
              revealDeclaredLayer(layerId, true);
            });
    connect(constraint, &ConstraintWorkflow::cartographicWorkGenerated, this,
            [this](const QString &, const QString &, const QString &layerId) {
              revealDeclaredLayer(layerId, true);
            });
    connect(constraint, &ConstraintWorkflow::interpretiveContoursGenerated, this,
            [this](const QString &, const QString &, const QString &layerId) {
              revealDeclaredLayer(layerId, true);
            });
    // 等值线 / 解释性等值线：GDAL 提线和制图核在任务线程，图层打开与登记留在界面线程。
    connect(constraintPage, &ConstraintPage::interpretiveContourRequested, this,
            [this, constraint, constraintPage](const QString &factorLayerId,
                                               const QVector<double> &levels) {
              const QString horizon = factorLayerId.startsWith(QStringLiteral("factor."))
                                         ? factorLayerId.mid(QStringLiteral("factor.").size())
                                              .section(QLatin1Char('.'), 0, 0)
                                         : QString();
              auto *status = constraintPage->findChild<QLabel *>(QStringLiteral("statusLabel"));
              const auto fail = [status](const QString &msg) {
                if (status)
                  status->setText(msg);
                QgsMessageLog::logMessage(msg, QStringLiteral("Paleo"), Qgis::MessageLevel::Warning);
              };
              if (!m_taskSvc)
              {
                constraintPage->setRunBusy(true);
                QString err;
                const bool ok =
                    constraint->generateInterpretiveContours(horizon, factorLayerId, levels, &err);
                constraintPage->setRunBusy(false);
                if (!ok)
                  fail(err.isEmpty() ? tr("解释性等值线生成失败") : err);
                return;
              }
              if (m_factorTask && m_factorTask->running())
              {
                fail(tr("已有单因素计算在进行"));
                return;
              }
              auto job = std::make_shared<ConstraintWorkflow::InterpretiveContourJob>();
              QString prepErr;
              if (!constraint->prepareInterpretiveContourJob(horizon, factorLayerId, levels, true, job.get(),
                                                             &prepErr))
              {
                fail(prepErr.isEmpty() ? tr("解释性等值线生成失败") : prepErr);
                return;
              }
              constraintPage->setRunBusy(true);
              constraintPage->noteRunStage(tr("正在准备"), 5);
              PaleoTask *task = m_taskSvc->start(
                  tr("解释性等值线 %1").arg(horizon),
                  [constraint, job](PaleoTask *running) -> QString {
                    const bool ok = constraint->computeInterpretiveContourJob(
                        job.get(), [running] { return running->cancelRequested(); });
                    if (running->cancelRequested())
                      return QStringLiteral("已取消");
                    if (!ok)
                      return job->error.isEmpty() ? QObject::tr("解释性等值线生成失败") : job->error;
                    return QString();
                  },
                  QString(), PaleoTask::Priority::High, true);
              m_factorTask = task;
              connect(task, &PaleoTask::finished, this,
                      [this, constraint, constraintPage, job, fail, task] {
                        const auto clearTask = [this, task] {
                          if (m_factorTask == task)
                            m_factorTask.clear();
                        };
                        const auto dropTemp = [](const QString &path) {
                          if (path.contains(QStringLiteral("paleo-sf-")))
                            QDir(QFileInfo(path).absolutePath()).removeRecursively();
                        };
                        if (!constraintPage)
                        {
                          dropTemp(job->workPath);
                          clearTask();
                          return;
                        }
                        if (task->state() == PaleoTask::State::Cancelled)
                        {
                          dropTemp(job->workPath);
                          dropTemp(job->contourPath);
                          constraintPage->setRunBusy(false);
                          if (auto *statusLabel =
                                  constraintPage->findChild<QLabel *>(QStringLiteral("statusLabel")))
                            statusLabel->setText(tr("已取消"));
                          clearTask();
                          return;
                        }
                        if (task->state() != PaleoTask::State::Succeeded)
                        {
                          dropTemp(job->workPath);
                          constraintPage->setRunBusy(false);
                          fail(task->errorText().isEmpty() ? tr("解释性等值线生成失败")
                                                           : task->errorText());
                          clearTask();
                          return;
                        }
                        // 发布是临界区：任务已结束，不再 requestCancel。
                        constraintPage->noteRunStage(tr("正在保存"), 95);
                        QString pubErr;
                        const bool published = constraint->publishInterpretiveContourJob(*job, &pubErr);
                        constraintPage->setRunBusy(false);
                        if (!published)
                          fail(pubErr.isEmpty() ? tr("解释性等值线生成失败") : pubErr);
                        else
                          constraintPage->noteRunStage(tr("正在保存"), 100);
                        clearTask();
                      });
            });
    connect(constraintPage, &ConstraintPage::contourRequested, this,
            [this, constraint, constraintPage](const QString &factorLayerId, double interval) {
              const QString horizon = factorLayerId.startsWith(QStringLiteral("factor."))
                                         ? factorLayerId.mid(QStringLiteral("factor.").size())
                                              .section(QLatin1Char('.'), 0, 0)
                                         : QString();
              auto *status = constraintPage->findChild<QLabel *>(QStringLiteral("statusLabel"));
              const auto fail = [status](const QString &msg) {
                if (status)
                  status->setText(msg);
                QgsMessageLog::logMessage(msg, QStringLiteral("Paleo"), Qgis::MessageLevel::Warning);
              };
              if (!m_taskSvc)
              {
                constraintPage->setRunBusy(true);
                QString err;
                const bool ok = constraint->generateContours(horizon, factorLayerId, interval, &err);
                constraintPage->setRunBusy(false);
                if (!ok)
                  fail(err.isEmpty() ? tr("等值线生成失败") : err);
                return;
              }
              if (m_factorTask && m_factorTask->running())
              {
                fail(tr("已有单因素计算在进行"));
                return;
              }
              auto job = std::make_shared<ConstraintWorkflow::AnalysisContourJob>();
              QString prepErr;
              if (!constraint->prepareAnalysisContourJob(horizon, factorLayerId, interval, {}, false,
                                                         job.get(), &prepErr))
              {
                fail(prepErr.isEmpty() ? tr("等值线生成失败") : prepErr);
                return;
              }
              constraintPage->setRunBusy(true);
              constraintPage->noteRunStage(tr("正在生成等值线"), 10);
              PaleoTask *task = m_taskSvc->start(
                  tr("等值线 %1").arg(horizon),
                  [constraint, job](PaleoTask *running) -> QString {
                    const bool ok = constraint->computeAnalysisContourJob(
                        job.get(), [running] { return running->cancelRequested(); });
                    if (running->cancelRequested())
                      return QStringLiteral("已取消");
                    if (!ok)
                      return job->error.isEmpty() ? QObject::tr("等值线生成失败") : job->error;
                    return QString();
                  },
                  QString(), PaleoTask::Priority::High, true);
              m_factorTask = task;
              connect(task, &PaleoTask::finished, this,
                      [this, constraint, constraintPage, job, fail, task] {
                        const auto clearTask = [this, task] {
                          if (m_factorTask == task)
                            m_factorTask.clear();
                        };
                        const auto dropTemp = [](const QString &path) {
                          if (path.contains(QStringLiteral("paleo-sf-")))
                            QDir(QFileInfo(path).absolutePath()).removeRecursively();
                        };
                        if (!constraintPage)
                        {
                          dropTemp(job->outputPath);
                          clearTask();
                          return;
                        }
                        if (task->state() == PaleoTask::State::Cancelled)
                        {
                          dropTemp(job->outputPath);
                          constraintPage->setRunBusy(false);
                          if (auto *statusLabel =
                                  constraintPage->findChild<QLabel *>(QStringLiteral("statusLabel")))
                            statusLabel->setText(tr("已取消"));
                          clearTask();
                          return;
                        }
                        if (task->state() != PaleoTask::State::Succeeded)
                        {
                          dropTemp(job->outputPath);
                          constraintPage->setRunBusy(false);
                          fail(task->errorText().isEmpty() ? tr("等值线生成失败") : task->errorText());
                          clearTask();
                          return;
                        }
                        // 发布是临界区：任务已结束，不再 requestCancel。
                        constraintPage->noteRunStage(tr("正在保存"), 95);
                        QString pubErr;
                        const bool published = constraint->publishAnalysisContourJob(*job, &pubErr);
                        constraintPage->setRunBusy(false);
                        if (!published)
                          fail(pubErr.isEmpty() ? tr("等值线生成失败") : pubErr);
                        else
                          constraintPage->noteRunStage(tr("正在保存"), 100);
                        clearTask();
                      });
            });
    // 互斥上图：visible=true → instantiate + 图层树勾选该层，04_SingleFactor 组
    // 其它已实例化层取消勾选；false 只取消该层（业务上同时只看一张单因素图）。
    connect(constraintPage, &ConstraintPage::factorVisibilityRequested, this,
            [this](const QString &layerId, bool visible) {
              if (!m_layerSvc || layerId.isEmpty())
                return;
              QgsProject *proj = m_projectSvc ? m_projectSvc->project() : nullptr;
              QgsLayerTree *treeRoot = proj ? proj->layerTreeRoot() : nullptr;
              if (!treeRoot)
                return;
              const auto setNodeChecked = [this, treeRoot](const QString &id, bool checked) {
                QgsMapLayer *layer = m_layerSvc->layer(id); // 只拨已实例化层
                if (!layer)
                  return;
                if (QgsLayerTreeLayer *node = treeRoot->findLayer(layer->id()))
                  node->setItemVisibilityChecked(checked);
              };
              if (visible)
              {
                QString err;
                QgsMapLayer *layer = m_layerSvc->instantiate(layerId, &err);
                if (!layer)
                {
                  QgsMessageLog::logMessage(
                      tr("单因素上图失败：%1").arg(err.isEmpty() ? layerId : err),
                      QStringLiteral("Paleo"), Qgis::MessageLevel::Warning);
                  return;
                }
                // 组内互斥：04_SingleFactor（含 Contours 子组，按前缀）先全下。
                QVector<LayerDeclaration> decls;
                if (m_layerSvc->tryDeclared(&decls))
                {
                  for (const LayerDeclaration &d : decls)
                  {
                    if (d.layerId == layerId)
                      continue;
                    if (d.group == QLatin1String("04_SingleFactor") ||
                        d.group.startsWith(QLatin1String("04_SingleFactor/")))
                      setNodeChecked(d.layerId, false);
                  }
                }
                setNodeChecked(layerId, true);
              }
              else
              {
                setNodeChecked(layerId, false);
              }
            });
    // 类型化约束线五入口（五种 Semantic 各一按钮）：类型化捕获工具
    //（type 列落地质类型词表）。CAD dock 复用编辑条那只（方向23 收敛四处懒建）。
    if (m_canvasCtl)
    {
      auto *typedCtl = new TypedConstraintDrawController(m_canvasCtl, constraint, this);
      if (auto *dock = findChild<QgsAdvancedDigitizingDockWidget *>(
               QStringLiteral("paleo-editing-cad-dock")))
        typedCtl->shareCadDock(dock);
      connect(constraintPage, &ConstraintPage::drawTypedConstraintRequested, typedCtl,
              [typedCtl](const QString &horizon, const QString &shape,
                         const QString &constraintType, int faciesCode) {
                typedCtl->startCapture(horizon, shape, constraintType, faciesCode);
              });
      connect(typedCtl, &TypedConstraintDrawController::captureFailed, this,
              [](const QString &err) {
                QgsMessageLog::logMessage(err, QStringLiteral("Paleo"), Qgis::MessageLevel::Warning);
              });
    }
    // ---- 方向23：已绘约束线编辑面 ----
    // 删除：store 落盘删除 + 已实例化的 constraints.<horizon> 图层重载。
    connect(constraintPage, &ConstraintPage::constraintDeleteRequested, this,
            [this, constraint, constraintPage](const QString &horizon, const QString &id) {
              QString err;
              if (!constraint->removeConstraint(id, &err))
              {
                QgsMessageLog::logMessage(err.isEmpty() ? tr("删除约束失败") : err,
                                          QStringLiteral("Paleo"), Qgis::MessageLevel::Warning);
                return;
              }
              if (m_layerSvc)
              {
                const QString layerId = QStringLiteral("constraints.%1").arg(horizon);
                if (auto *layer = m_layerSvc->instantiate(layerId))
                {
                  if (auto *vl = qobject_cast<QgsVectorLayer *>(layer))
                  {
                    vl->reload();
                    vl->triggerRepaint();
                  }
                }
              }
              constraintPage->refreshConstraintList();
            });
    // 语义切换：走 updateConstraintLine 同一持久化通道（type 列 + params_json）。
    connect(constraintPage, &ConstraintPage::constraintSemanticChangeRequested, this,
            [constraint](const QString &, const QString &id, const QString &semantic) {
              QString err;
              if (!constraint->switchConstraintSemantic(id, semantic, &err))
                QgsMessageLog::logMessage(err.isEmpty() ? tr("切换约束语义失败") : err,
                                          QStringLiteral("Paleo"), Qgis::MessageLevel::Warning);
            });
    // 顶点编辑：约束表是可写 GPKG（非派生只读），直接进编辑会话 + 顶点工具，
    // 撤销/重做走编辑条的原生 undo 栈；提交后约束页重读列表。
    connect(constraintPage, &ConstraintPage::editConstraintVerticesRequested, this,
            [this, constraint, constraintPage](const QString &horizon) {
              if (!m_layerSvc || !m_canvasCtl)
                return;
              const QString layerId = QStringLiteral("constraints.%1").arg(horizon);
              QgsMapLayer *layer = m_layerSvc->instantiate(layerId);
              auto *vl = qobject_cast<QgsVectorLayer *>(layer);
              if (!vl)
              {
                QgsMessageLog::logMessage(tr("约束图层不可用：%1").arg(layerId),
                                          QStringLiteral("Paleo"), Qgis::MessageLevel::Warning);
                return;
              }
              auto *editTb = findChild<PaleoEditingToolbar *>(QStringLiteral("editingToolbar"));
              if (editTb)
              {
                editTb->refreshFromProject();
                editTb->setCurrentLayer(vl);
                editTb->actionVertexEdit()->trigger();
                connect(editTb, &PaleoEditingToolbar::editingStopped, constraintPage,
                        &ConstraintPage::refreshConstraintList, Qt::UniqueConnection);
                if (editTb->isEditing() && editTb->currentLayer() == vl)
                  return;
              }
              m_canvasCtl->canvas()->setCurrentLayer(vl);
            });
    // ---- m2(B) end ----
  }
}

// ---------------------------------------------------------------------------
// 编图页接线：因子融合 + 相面多边形化（W4 拆分段）
// ---------------------------------------------------------------------------
void PaleoMainWindow::attachComposePage(ComposePage *composePage,
                                        CompositionWorkflow *compose,
                                        QgisLayoutService *layoutSvc)
{
  if (compose && composePage)
  {
    connect(composePage, &ComposePage::fuseRequested, this,
            [this, compose, composePage](const QStringList &factorIds) {
              QString err;
              const QString horizon = m_selection ? m_selection->activeHorizon() : QString();
              if (compose->fuseFactors(horizon, factorIds, &err))
                composePage->refreshFactors();
            });
    connect(composePage, &ComposePage::polygonizeRequested, this,
            [this, compose, composePage](const QString &rasterId, double minArea, double simplify) {
              QString err;
              const QString horizon = m_selection ? m_selection->activeHorizon() : QString();
              QVariantMap params;
              params.insert(QStringLiteral("MIN_AREA"), minArea);
              params.insert(QStringLiteral("SIMPLIFY"), simplify);
              if (compose->deriveFaciesPolygons(horizon, rasterId, params, &err))
                composePage->refreshFactors();
              else if (!err.isEmpty())
                QgsMessageLog::logMessage(err, QStringLiteral("Paleo"), Qgis::MessageLevel::Warning);
            });

    // ---- m2(C): 矢量化成功 → 自动进入相界编辑态（z3 PaleoVertexTool）------
    // 拿 facies.<horizon> 矢量层 instantiate → 编辑条选层 + 触发顶点工具
    // （QgsVertexTool 是 app-only，顶点编辑走 PaleoVertexTool；snapping 在
    // 画布控制器侧已开，拓扑编辑由编辑条「拓扑」开关驱动——默认关）。派生
    // gpkg 按 T26 纪律只读——进编辑前先铺
    // 可编辑工作副本（prepareFaciesForEditing）。编辑条缺席（无画布环境）
    // → 降级为选中层 + 状态文案提示手动进入编辑。
    connect(compose, &CompositionWorkflow::compositionDone, this,
            [this](const QString &, const QString &layerId) {
              revealDeclaredLayer(layerId, true);
            });
    connect(compose, &CompositionWorkflow::faciesPolygonsReady, this,
            [this, compose, composePage](const QString &h, const QString &layerId) {
              Q_UNUSED(h);
              composePage->setFaciesEditTarget(layerId);
              if (!m_layerSvc)
                return;
              QString target = layerId;
              QString prepErr;
              const QString prepared = compose->prepareFaciesForEditing(layerId, &prepErr);
              if (prepared.isEmpty())
                QgsMessageLog::logMessage(
                    tr("相界工作副本铺设失败：%1").arg(prepErr), QStringLiteral("Paleo"),
                    Qgis::MessageLevel::Warning);
              else
                target = prepared;
              revealDeclaredLayer(target, true);
              QgsMapLayer *l = m_layerSvc->instantiate(target);
              auto *vl = qobject_cast<QgsVectorLayer *>(l);
              auto *status = composePage->findChild<QLabel *>(QStringLiteral("statusLabel"));
              auto *editTb = findChild<PaleoEditingToolbar *>(QStringLiteral("editingToolbar"));
              if (vl && editTb && m_canvasCtl)
              {
                editTb->refreshFromProject(); // 新层入列（instantiate 已挂工程）
                editTb->setCurrentLayer(vl);
                editTb->actionVertexEdit()->trigger(); // 自动开编辑（走编辑服务纪律）+ 装顶点工具
                if (editTb->isEditing() && editTb->currentLayer() == vl)
                {
                  if (status)
                    status->setText(tr("相界就绪，已进入编辑：%1（顶点工具 — 选中要素改属性）")
                                        .arg(layerId));
                  return;
                }
              }
              // 降级：选中层 + 文案提示手动进入编辑（如实报告，不假装在编辑）。
              if (vl && m_canvasCtl)
                m_canvasCtl->canvas()->setCurrentLayer(vl);
              if (status)
                status->setText(tr("相界就绪：%1 — 请在「要素编辑」组手动进入编辑")
                                    .arg(layerId));
            });

    // ---- m2(C): 相属性保存 → edit buffer 回写 ------------------------------
    connect(composePage, &ComposePage::faciesAttributesSaveRequested, this,
            [this, compose, composePage](const QString &layerId, const QVariantMap &attrs) {
              if (!compose)
                return;
              QString err;
              if (compose->saveFaciesAttributes(layerId, attrs, &err))
              {
                if (auto *status = composePage->findChild<QLabel *>(QStringLiteral("statusLabel")))
                  status->setText(tr("相属性已写入编辑缓冲：%1（随「保存编辑」提交）")
                                      .arg(layerId));
              }
              else
              {
                if (auto *status = composePage->findChild<QLabel *>(QStringLiteral("statusLabel")))
                  status->setText(err.isEmpty() ? tr("相属性保存失败") : err);
                if (!err.isEmpty())
                  QgsMessageLog::logMessage(err, QStringLiteral("Paleo"),
                                            Qgis::MessageLevel::Warning);
              }
            });

    // ---- m2(C): 参考图勾选 → instantiate + 图层树节点勾选/取消 ------------
    connect(composePage, &ComposePage::referenceVisibilityRequested, this,
            [this](const QString &layerId, bool visible) {
              if (!m_layerSvc || layerId.isEmpty())
                return;
              QgsMapLayer *l = visible ? m_layerSvc->instantiate(layerId)
                                       : m_layerSvc->layer(layerId);
              if (!l)
                return;
              QgsProject *proj = m_projectSvc ? m_projectSvc->project() : nullptr;
              QgsLayerTree *root = proj ? proj->layerTreeRoot() : nullptr;
              if (!root)
                return;
              for (QgsLayerTreeLayer *node : root->findLayers())
                if (node->layer() == l)
                  node->setItemVisibilityChecked(visible);
            });

    // ---- m2(C): 在布局设计器中打开（layoutdesignershell 公共入口）---------
    // 打开（或新建）本层位的版面：优先复用导出同名布局 "<h>_map"；新空布局
    // 没有地图项时主题钉定自然跳过（设计器里由模板/手工补地图项）。
    connect(composePage, &ComposePage::layoutDesignerRequested, this,
            [this, layoutSvc]() {
              if (!layoutSvc)
              {
                QgsMessageLog::logMessage(tr("布局服务未接入 — 无法打开图件设计器"),
                                          QStringLiteral("Paleo"), Qgis::MessageLevel::Warning);
                return;
              }
              if (!m_projectSvc || m_projectSvc->projectPath().isEmpty())
              {
                QgsMessageLog::logMessage(tr("无打开工程 — 无法打开图件设计器"),
                                          QStringLiteral("Paleo"), Qgis::MessageLevel::Warning);
                return;
              }
              const QString h = m_selection ? m_selection->activeHorizon() : QString();
              const QString name = h.isEmpty() ? tr("编图布局")
                                               : QStringLiteral("%1_map").arg(h);
              QString err;
              QgsLayout *layout = layoutSvc->layout(name);
              if (!layout)
                layout = layoutSvc->createLayout(name, &err);
              if (!layout)
              {
                QgsMessageLog::logMessage(
                    err.isEmpty() ? tr("创建布局失败：%1").arg(name) : err,
                    QStringLiteral("Paleo"), Qgis::MessageLevel::Critical);
                return;
              }
              // 版面地图项钉本页主题（m1 setLayoutMapTheme 接缝）。
              if (QgsLayoutItemMap *mapItem =
                      qobject_cast<QgsLayoutItemMap *>(layout->itemById(QStringLiteral("map"))))
                pinLayoutTheme(mapItem, QStringLiteral("compose"));
              auto *shell = new PaleoLayoutDesignerShell(layout, this);
              shell->setTaskService(m_taskSvc); // #85：导出走任务池 worker
              shell->setAttribute(Qt::WA_DeleteOnClose);
              shell->setModal(false);
              shell->show();
            });
    // ---- m2(C) end ----
  }
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
// 壳面接线：locator / 保存 / 底栏面板 / 处理算法 / 编辑工具 / 图件设计
// （W4 拆分段）——返回逻辑宿主编辑条供 buildRibbonPanels 镜像进 ribbon。
// ---------------------------------------------------------------------------
PaleoEditingToolbar *PaleoMainWindow::attachShellSurfaces(
    PaleoProjectStore *store, QgisProcessingService *procSvc,
    QgisEditingService *editSvc, QgisLayoutService *layoutSvc,
    PaleoTaskService *taskSvc)
{
  m_editSvc = editSvc; // closeEvent 的保存/放弃编辑走服务（busy 挂账随终态清）
  // W2 长任务可见性：任务进场/终态时重估底栏露出（本体在主 TU，
  // syncBottomDockForTasks——露出/恢复都走程序化显隐，不动用户意愿）。
  if (taskSvc)
    connect(taskSvc, &PaleoTaskService::taskAdded, this, [this](PaleoTask *task) {
      if (task)
        connect(task, &PaleoTask::finished, this,
                [this] { syncBottomDockForTasks(); });
      syncBottomDockForTasks();
    });

  // ---- ribbon 右侧组的搜索槽 + 快速访问栏的保存 ----
  if (auto *topBar = findChild<QWidget *>(QStringLiteral("locatorSlot")))
  {
    if (m_layerSvc && m_selection && m_canvasCtl)
    {
      auto *locatorWidget = new QgsLocatorWidget(topBar);
      locatorWidget->setObjectName(QStringLiteral("paleoLocator"));
      locatorWidget->setMapCanvas(m_canvasCtl->canvas());
      locatorWidget->setPlaceholderText(tr("搜索井位/层位  Ctrl+K"));
      locatorWidget->setMinimumWidth(220);

      // Wells filter: resolve the declared "wells" layer lazily (instantiate
      // on demand — the search must not force materialization at open time).
      WellLocatorFilter::WellLayerProvider wellProvider = [this]() {
        QPair<QgsVectorLayer *, QString> out{nullptr, QString()};
        QgsMapLayer *l = m_layerSvc->layer(QStringLiteral("wells"));
        if (!l)
          l = m_layerSvc->instantiate(QStringLiteral("wells"));
        auto *vl = qobject_cast<QgsVectorLayer *>(l);
        if (!vl)
          return out;
        const int nameIdx = vl->fields().lookupField(QStringLiteral("name"));
        out.first = vl;
        out.second = nameIdx >= 0 ? QStringLiteral("name")
                                  : (vl->fields().isEmpty() ? QString()
                                                            : vl->fields().at(0).name());
        return out;
      };
      locatorWidget->locator()->registerFilter(
          new WellLocatorFilter(wellProvider, m_canvasCtl->canvas()));

      // Horizons filter: manifest horizon set → activate + materialize.
      HorizonLocatorFilter::HorizonListProvider horizonProvider = [this]() {
        QStringList hs;
        QVector<LayerDeclaration> declared;
        QString manifestErr;
        if (!m_layerSvc || !m_layerSvc->tryDeclared(&declared, &manifestErr))
        {
          QgsMessageLog::logMessage(tr("层位搜索：图层清单读取失败：%1")
                                        .arg(manifestErr),
                                    QStringLiteral("Paleo"), Qgis::MessageLevel::Warning);
          return hs;
        }
        for (const LayerDeclaration &d : declared)
          if (!d.horizon.isEmpty() && !hs.contains(d.horizon))
            hs << d.horizon;
        hs.sort();
        return hs;
      };
      HorizonLocatorFilter::ActivateFn activate = [this](const QString &h) {
        // 与 HorizonChipBar 同一拦截口径：编辑中切层位会在 releaseHorizon
        // 里静默回滚丢编辑成果（历史上还绕过 busy 释放）。定位器入口必须
        // 同样拒绝并说明原因，而不是开一条丢数据的旁路。
        QString editingName;
        if (m_layerSvc && m_layerSvc->isEditingAnyLayer(&editingName))
        {
          QgsMessageLog::logMessage(
              tr("正在编辑「%1」——先保存或放弃编辑，再切换层位（定位器切换已拒绝）")
                  .arg(editingName),
              QStringLiteral("Paleo"), Qgis::MessageLevel::Warning);
          return;
        }
        if (m_selection)
          m_selection->setActiveHorizon(h);
        if (m_layerSvc)
          m_layerSvc->setActiveHorizon(h);
      };
      locatorWidget->locator()->registerFilter(
          new HorizonLocatorFilter(horizonProvider, activate));

      // Note: no IssueLocatorFilter registration — the validation workflow
      // keeps no queryable issue store, so the filter could only ever sit on
      // a permanently-empty provider. Issue navigation lives on the
      // validation page's issueTable → ThreeWayLocator path instead.

      topBar->layout()->addWidget(locatorWidget);
      auto *focus = new QShortcut(QKeySequence(QStringLiteral("Ctrl+K")), this);
      connect(focus, &QShortcut::activated, locatorWidget,
              [locatorWidget] { locatorWidget->search(QString()); });
    }

    if (store && m_projectSvc)
    {
      // 保存 = 快速访问栏第一颗（Office 惯例）+「文件」菜单；Ctrl+S 挂在
      // action 上（菜单里可见快捷键），不再单独立 QShortcut。
      auto *saveAct = new QAction(PaleoIcons::qgisTheme(QStringLiteral("mActionFileSave.svg")),
                                  tr("保存工程"), this);
      saveAct->setObjectName(QStringLiteral("saveProjectAction"));
      saveAct->setShortcut(QKeySequence::Save);
      saveAct->setToolTip(tr("保存工程（Ctrl+S）"));
      // §41.2 ordering through the write queue: gpkg commit (no-op until edit
      // buffers report dirty state) then the atomic .qgz write.
      auto saveFn = [this, store]() {
        if (!m_projectSvc || m_projectSvc->projectPath().isEmpty())
        {
          QgsMessageLog::logMessage(tr("无打开工程 — 无法保存"),
                                  QStringLiteral("Paleo"), Qgis::MessageLevel::Warning);
          return;
        }
        const auto res = store->saveAll(
            [] { return PaleoProjectStore::WriteResult{true, QString()}; },
            [this] {
              saveCanvasExtent(); // display state travels inside the .qgz
              const bool ok = m_projectSvc->writeProject();
              return PaleoProjectStore::WriteResult{
                  ok, ok ? QString() : m_projectSvc->lastErrors().join(QLatin1Char(';'))};
            });
        // W3 保存反馈：成功落状态栏（与打开工程失败的 §38 弹框契约对齐——
        // 失败是阻断级，弹框如实给原因），同时照记日志。
        QgsMessageLog::logMessage(
            res.ok ? tr("工程已保存") : tr("保存失败：%1").arg(res.error),
            QStringLiteral("Paleo"),
            res.ok ? Qgis::MessageLevel::Info : Qgis::MessageLevel::Critical);
        if (res.ok)
        {
          if (statusBar())
            statusBar()->showMessage(tr("工程已保存：%1").arg(m_projectSvc->projectPath()), 5000);
          updateWindowTitle();
        }
        else if (QGuiApplication::platformName() != QLatin1String("offscreen"))
          QMessageBox::critical(this, tr("保存工程失败"), res.error);
      };
      connect(saveAct, &QAction::triggered, this, saveFn);
      if (SARibbonQuickAccessBar *qab = ribbonBar()->quickAccessBar())
      {
        qab->addAction(saveAct);
        if (auto *saveBtn = qobject_cast<QToolButton *>(qab->widgetForAction(saveAct)))
        {
          saveBtn->setObjectName(QStringLiteral("saveButton"));
          saveBtn->setAccessibleName(tr("保存工程"));
        }
      }
      if (auto *fileMenu = findChild<QMenu *>(QStringLiteral("fileMenu")))
        for (QAction *a : fileMenu->actions())
          if (a->objectName() == QLatin1String("fileMenuSaveAnchor"))
          {
            fileMenu->insertAction(a, saveAct);
            fileMenu->insertSeparator(saveAct);
            break;
          }
    }
  }

  // Release management tab in the bottom dock.
  if (store)
    if (auto *bottomTabs = findChild<QTabWidget *>(QStringLiteral("bottomTabs")))
    {
      auto *releasePanel = new ReleasePanel(bottomTabs);
      releasePanel->setObjectName(QStringLiteral("releasePanel"));
      releasePanel->setProviders(
          [store]() { return store->metaDbPath(); },
          [this]() {
            QVector<LayerDeclaration> declared;
            QString manifestErr;
            if (m_layerSvc && !m_layerSvc->tryDeclared(&declared, &manifestErr))
              QgsMessageLog::logMessage(tr("发布面板：图层清单读取失败：%1")
                                          .arg(manifestErr),
                                      QStringLiteral("Paleo"), Qgis::MessageLevel::Warning);
            return declared;
          });
      connect(releasePanel, &ReleasePanel::statusMessage, this,
              [](const QString &msg) {
                QgsMessageLog::logMessage(msg, QStringLiteral("Paleo"), Qgis::MessageLevel::Info);
              });
      if (m_projectSvc)
        connect(m_projectSvc, &QgisProjectService::projectOpened, releasePanel,
                &ReleasePanel::refresh);
      bottomTabs->addTab(releasePanel, tr("发布"));
    }

  // Task panel replaces the placeholder in the 任务 tab (index 1); attribute
  // table joins as its own tab, fed by the instantiated-layer set.
  if (store)
    if (auto *bottomTabs = findChild<QTabWidget *>(QStringLiteral("bottomTabs")))
    {
      const int idx = bottomTabs->indexOf(
          bottomTabs->findChild<QTextEdit *>(QStringLiteral("tasksPlaceholder")));
      auto *taskPanel = new TaskPanel(store, taskSvc, bottomTabs);
      if (idx >= 0)
      {
        QWidget *old = bottomTabs->widget(idx);
        bottomTabs->removeTab(idx);
        delete old;
        bottomTabs->insertTab(idx, taskPanel, tr("任务"));
      }
      else
        bottomTabs->addTab(taskPanel, tr("任务"));

      if (m_layerSvc && m_canvasCtl)
      {
        auto *attrPanel = new AttributeTablePanel(
            m_canvasCtl->canvas(),
            [this](const QString &layerId) -> QgsVectorLayer * {
              QgsMapLayer *l = m_layerSvc->layer(layerId);
              if (!l)
                l = m_layerSvc->instantiate(layerId);
              return qobject_cast<QgsVectorLayer *>(l);
            },
            bottomTabs);
        attrPanel->setObjectName(QStringLiteral("attributeTablePanel"));
        auto refreshIds = [this, attrPanel] {
          QVector<LayerDeclaration> declared;
          QString manifestErr;
          if (!m_layerSvc->tryDeclared(&declared, &manifestErr))
          {
            QgsMessageLog::logMessage(tr("属性表面板：图层清单读取失败：%1")
                                          .arg(manifestErr),
                                      QStringLiteral("Paleo"), Qgis::MessageLevel::Warning);
            return; // keep the existing layer list instead of blanking it
          }
          QStringList ids;
          for (const LayerDeclaration &d : declared)
            ids << d.layerId;
          attrPanel->setLayerIds(ids);
        };
        refreshIds();
        connect(m_layerSvc, &QgisLayerService::layerInstantiated, this,
                [refreshIds](const QString &) { refreshIds(); });
        connect(m_layerSvc, &QgisLayerService::horizonReleased, this,
                [this, attrPanel, refreshIds](const QString &) {
                  refreshIds();
                  const QString cur = attrPanel->currentLayerId();
                  if (!cur.isEmpty() && (!m_layerSvc->isInstantiated(cur) || !m_layerSvc->layer(cur)))
                    attrPanel->showLayer(cur);
                });
        if (m_projectSvc)
          connect(m_projectSvc, &QgisProjectService::projectOpened, this,
                  [refreshIds](const QString &) { refreshIds(); });
        bottomTabs->addTab(attrPanel, tr("属性表"));
      }
    }

  // Processing entry point in the ribbon's right group (global tool, like the
  // QGIS Processing Toolbox): paleo:* algorithms first-class, the full
  // registry grouped under per-provider submenus. Each item opens the native
  // QGIS algorithm dialog (non-blocking, offscreen-safe).
  if (procSvc)
  {
    if (SARibbonButtonGroupWidget *topBar = ribbonBar()->rightButtonGroup())
    {
      auto *btn = new QToolButton(topBar);
      btn->setObjectName(QStringLiteral("processingButton"));
      btn->setText(tr("处理算法"));
      btn->setAccessibleName(tr("处理算法选择"));
      btn->setIcon(PaleoIcons::qgisTheme(QStringLiteral("processingAlgorithm.svg")));
      btn->setToolButtonStyle(Qt::ToolButtonTextBesideIcon);
      btn->setPopupMode(QToolButton::InstantPopup);
      auto *menu = new QMenu(btn);

      // W6：菜单显示算法的 displayName（人读名），机器 id 只留 tooltip——
      // 此前 addAction(id) 直接把 "paleo:paleo_constraint_idw" 甩给用户。
      const auto displayNameFor = [](const QString &id) {
        const QgsProcessingRegistry *reg = QgsApplication::processingRegistry();
        const QgsProcessingAlgorithm *alg = reg ? reg->algorithmById(id) : nullptr;
        return alg ? alg->displayName() : id;
      };
      const auto addAlgorithmAction = [this, procSvc, &displayNameFor](QMenu *m,
                                                                       const QString &id) {
        QAction *a = m->addAction(displayNameFor(id), this,
                                  [this, procSvc, id]() {
                                    QString err;
                                    if (!procSvc->showAlgorithmDialog(id, QVariantMap(), this, &err))
                                      QgsMessageLog::logMessage(err, QStringLiteral("Paleo"),
                                                                Qgis::MessageLevel::Warning);
                                  });
        a->setToolTip(id);
      };

      for (const QString &id : procSvc->paleoAlgorithmIds())
        addAlgorithmAction(menu, id);

      QMap<QString, QMenu *> providerMenus;
      for (const QString &id : procSvc->algorithmIds())
      {
        if (id.startsWith(QStringLiteral("paleo:")))
          continue;
        const QString provider = id.section(QLatin1Char(':'), 0, 0);
        QMenu *&sub = providerMenus[provider];
        if (!sub)
          sub = menu->addMenu(provider);
        addAlgorithmAction(sub, id);
      }

      btn->setMenu(menu);
      // 插在「面板」钮之前：[搜索][处理算法][面板][Web 服务]。
      QAction *before = nullptr;
      if (auto *panelsBtn = topBar->findChild<QToolButton *>(QStringLiteral("panelsMenuButton")))
        for (QAction *a : topBar->actions())
          if (topBar->widgetForAction(a) == panelsBtn)
            before = a;
      topBar->insertWidget(before, btn);
    }
  }

  // 编辑工具 (wave/edit-tools): digitizing toolset — add/reshape/move/delete +
  // vertex editing routed through the editing service; undo/redo follows the
  // selected layer. Ribbon 形态：PaleoEditingToolbar 只当逻辑宿主（工具
  // 生命周期、图层下拉、门控/原因），本体不上屏；它的 QAction 进三个编图页
  // 的「要素编辑」组（buildRibbonPanels），撤销/重做进快速访问栏。页作用域
  // = 只有编图链三页的页签里有这组命令。
  PaleoEditingToolbar *editTb = nullptr;
  if (m_canvasCtl)
  {
    editTb = new PaleoEditingToolbar(m_canvasCtl->canvas(), this);
    editTb->setObjectName(QStringLiteral("editingToolbar"));
    if (editSvc)
      editTb->setEditingService(editSvc);
    if (m_projectSvc)
      editTb->setProject(m_projectSvc->project());
    editTb->refreshFromProject();
    if (m_projectSvc)
    {
      connect(m_projectSvc, &QgisProjectService::projectOpened, editTb,
              [editTb](const QString &) { editTb->refreshFromProject(); });
      if (QgsProject *proj = m_projectSvc->project())
      {
        connect(proj, &QgsProject::layersAdded, editTb,
                [editTb](const QList<QgsMapLayer *> &) { editTb->refreshFromProject(); });
        connect(proj, &QgsProject::layersRemoved, editTb,
                [editTb](const QStringList &) { editTb->refreshFromProject(); });
      }
    }
    // The selected tree layer, canvas target and ribbon target form one context.
    QgsMapCanvas *canvas = m_canvasCtl->canvas();
    connect(canvas, &QgsMapCanvas::mapToolSet, this, [this, canvas](QgsMapTool *tool, QgsMapTool *) {
      if (!tool || !tool->property("paleo-action").isValid() || !canvas->currentLayer() ||
          !m_projectSvc || !m_projectSvc->project() || !m_projectSvc->project()->layerTreeRoot())
        return;
      auto *node = m_projectSvc->project()->layerTreeRoot()->findLayer(canvas->currentLayer()->id());
      if (node && !node->isVisible())
      {
        node->setItemVisibilityCheckedParentRecursive(true);
        statusBar()->showMessage(tr("已显示操作图层：%1").arg(canvas->currentLayer()->name()), 5000);
      }
    });
    auto *tree = findChild<QgsLayerTreeView *>(QStringLiteral("layerTreeView"));
    if (tree)
    {
      connect(tree, &QgsLayerTreeView::currentLayerChanged, editTb,
              [editTb, canvas, tree](QgsMapLayer *layer) {
        auto *vector = qobject_cast<QgsVectorLayer *>(layer);
        editTb->setCurrentLayer(vector);
        if (!editTb->isEditing() && !vector)
          canvas->setCurrentLayer(layer); // a raster is a valid browsing target
        // A refused switch restores the tree as well as the ribbon.
        const QSignalBlocker block(tree);
        tree->setCurrentLayer(canvas->currentLayer());
      });
      connect(editTb, &PaleoEditingToolbar::stateChanged, tree, [canvas, tree] {
        const QSignalBlocker block(tree);
        tree->setCurrentLayer(canvas->currentLayer());
      });
    }
    connect(editTb, &PaleoEditingToolbar::editRefused, this, [this](const QString &reason) {
      statusBar()->showMessage(reason, 8000);
    });
    editTb->hide(); // 逻辑宿主，不进布局
    if (SARibbonQuickAccessBar *qab = ribbonBar()->quickAccessBar())
    {
      qab->addAction(editTb->actionUndo());
      qab->addAction(editTb->actionRedo());
    }
  }

  // 图件设计 entry (wave/layout-designer): create a print layout via the
  // layout service and open the designer shell dialog non-modally. 入口在
  // 「智能编图 › 图件输出」组（buildRibbonPanels 按 objectName 取这颗动作）。
  if (layoutSvc)
    {
      auto *designerAct = new QAction(
          PaleoIcons::qgisTheme(QStringLiteral("mActionNewLayout.svg")), tr("图件设计"), this);
      designerAct->setObjectName(QStringLiteral("ribbonDesignerAction"));
      designerAct->setToolTip(tr("新建布局并打开图件设计器"));
      connect(designerAct, &QAction::triggered, this, [this, layoutSvc] {
        if (!m_projectSvc || m_projectSvc->projectPath().isEmpty())
        {
          QgsMessageLog::logMessage(tr("无打开工程 — 无法创建布局"),
                                  QStringLiteral("Paleo"), Qgis::MessageLevel::Warning);
          return;
        }
        static int s_layoutSeq = 0;
        QString err;
        QgsLayout *layout = layoutSvc->createLayout(
            tr("布局 %1").arg(++s_layoutSeq), &err);
        if (!layout)
        {
          QgsMessageLog::logMessage(tr("创建布局失败：%1").arg(err),
                                  QStringLiteral("Paleo"), Qgis::MessageLevel::Critical);
          return;
        }
        auto *shell = new PaleoLayoutDesignerShell(layout, this);
        shell->setTaskService(m_taskSvc); // #85：导出走任务池 worker
        shell->setAttribute(Qt::WA_DeleteOnClose);
        shell->setModal(false);
        shell->show();
      });
    }
  return editTb;
}

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

  // 工程打开时恢复发布门状态（版本行的 PDF 资产 + 残差覆盖重算）。
  if (m_projectSvc)
    connect(m_projectSvc, &QgisProjectService::projectOpened, this,
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
  const auto refreshPublishGate = [this, versionStore, projectData, catalog]() {
    auto *page = findChild<ComposePage *>();
    if (!page)
      return;
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
            const auto choice = QMessageBox::question(
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
                QMessageBox::Ok | QMessageBox::Cancel, QMessageBox::Cancel);
            if (choice != QMessageBox::Ok)
              return;
            QString err;
            const QString dir = versions->publish(h, summary, &err);
            if (!dir.isEmpty())
            {
              status(tr("已发布：%1 v%2 → %3").arg(h).arg(v.version).arg(dir));
              composePage->setVersionState(v.version, true);
            }
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
            if (!mapping->runThicknessChain(h, &err))
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
          [this, versionStore, projectData, catalog, activeHorizon, status]() {
            const QString h = activeHorizon();
            if (h.isEmpty() || !m_layerSvc)
            {
              status(tr("先在顶部 chip 选择层位再导出"));
              return;
            }
            const QString projectDir = m_projectSvc
                                           ? QFileInfo(m_projectSvc->projectPath()).absolutePath()
                                           : QDir::temp().absolutePath();
            const QString target = QDir(projectDir).filePath(
                QStringLiteral("%1_map.pdf").arg(h));
            // ---- m2(C): 版面地图项钉本页主题（paleo.page.compose）---------
            // exportHorizonMapPdf 内建布局不外露地图项，这里走同一
            // buildHorizonMapLayout + PaleoLayoutExportActions（300dpi PDF，
            // 与既有导出参数逐字一致），在导出前把布局里 id="map" 的地图项
            // 钉到 compose 页主题（m1 setLayoutMapTheme 接缝；服务缺席时
            // pinLayoutTheme 直写原生 follow-visibility 预设）。
            const auto exportPinned = [this](const QString &horizon, const QString &outPath,
                                             QString *errOut) -> QString {
              QgsPrintLayout *layout = buildHorizonMapLayout(m_layerSvc, m_projectSvc,
                                                             horizon, errOut);
              if (!layout)
                return QString();
              if (QgsLayoutItemMap *mapItem = qobject_cast<QgsLayoutItemMap *>(
                      layout->itemById(QStringLiteral("map"))))
                pinLayoutTheme(mapItem, QStringLiteral("compose"));
              PaleoLayoutExportActions exports;
              const auto outcome =
                  exports.exportLayout(layout, outPath, PaleoLayoutExportActions::Format::Pdf,
                                       300.0, PaleoLayoutExportActions::PageRange());
              delete layout;
              if (!outcome.ok)
              {
                if (errOut)
                  *errOut = outcome.error;
                return QString();
              }
              return outcome.files.value(0);
            };
            QString pdf;
            while (true) // 失败 → 重试 / 取消（§215：导出失败要写原因）
            {
              QString err;
              pdf = exportPinned(h, target, &err);
              if (!pdf.isEmpty())
                break;
              QgsMessageLog::logMessage(err, QStringLiteral("Paleo"), Qgis::MessageLevel::Warning);
              const auto choice = QMessageBox::warning(
                  this, tr("导出失败"),
                  tr("%1\n\n导出目标：%2").arg(err, target),
                  QMessageBox::Retry | QMessageBox::Cancel, QMessageBox::Retry);
              if (choice != QMessageBox::Retry)
              {
                status(err);
                return;
              }
            }

            // 阶段E：登记 catalog OUTPUT 受管资产 —— 登记失败的导出不算完成
            // （发布门要求的是「已登记的 PDF」，不是「写出过文件」）。
            QString sha, managedPath, regErr;
            const QString assetId =
                registerMapPdfAsset(catalog, projectDir, pdf, &sha, &managedPath, &regErr);
            if (assetId.isEmpty())
            {
              status(regErr.isEmpty() ? tr("PDF 资产登记失败") : regErr);
              QgsMessageLog::logMessage(regErr, QStringLiteral("Paleo"),
                                        Qgis::MessageLevel::Warning);
              return;
            }
            if (versionStore)
            {
              QString productErr;
              const QString recorded = managedPath.isEmpty() ? pdf : managedPath;
              if (!versionStore->recordLayoutProduct(h, recorded, assetId, sha, &productErr))
                QgsMessageLog::logMessage(productErr, QStringLiteral("Paleo"),
                                          Qgis::MessageLevel::Warning);
            }
            status(tr("层位图已导出：%1").arg(pdf));
            QMessageBox::information(this, tr("导出成功"),
                                     tr("已导出层位图：\n%1\n\nSHA-256：%2")
                                         .arg(pdf, sha));
            if (m_refreshPublishGate) m_refreshPublishGate();
          });

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
            const MapVersion v = versions->saveVersion(h, provenance, &err);
            if (v.version > 0)
            {
              status(tr("已保存版本：%1 v%2").arg(h).arg(v.version));
              composePage->setVersionState(v.version, false);
            }
            else
              status(err.isEmpty() ? tr("保存版本失败") : err);
            if (m_refreshPublishGate) m_refreshPublishGate();
          });

}
