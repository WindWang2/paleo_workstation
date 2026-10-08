// 层：视图
// paleomainwindow_attach_models — 属性建模/相图/时深转换接线（W4 拆分段）
#include "paleomainwindow.h"

#include "../qgis/qgiscanvascontroller.h"
#include "../qgis/qgislayerservice.h"
#include "layers/layertreepanel.h"
#include "../workflow/depthconversionworkflow.h"
#include "../workflow/propertymodelworkflow.h"
#include "../workflow/faultinterpretationcontroller.h"
#include "propertymodel/propertymodelpanel.h"
#include "faciesmapping/faciesmappingpanel.h"
#include "../services/paleotaskservice.h"
#include "ui/seismicsection/seismicsectiondockwidget.h"
#include "ui/seismic3d/seismic3dviewpanel.h"
#include "services/seismictaskservice.h"
#include "domain/seismic/sgyvolume.h"

#include <qgsmessagelog.h>

#include <QDockWidget>
#include <QFileInfo>
#include <QPointer>
#include <QStatusBar>
#include <memory>

// ---------------------------------------------------------------------------
// goal/time-depth-velocity：层树「转换为深度域…」意图信号 → 建模/换算/登记
// 全在 DepthConversionWorkflow（功能层）；壳只解析声明与反馈状态。
// ---------------------------------------------------------------------------
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

// ---------------------------------------------------------------------------
// goal/facies-automapping：证据合成 + QA 报告面板
// 同 attachPropertyModel 形态：面板只发意图，链路在 FaciesMappingWorkflow
//（约束装配/优势相/相界/合成/QA/登记全在功能层）。幂等：dock 已建则只
// 更新 workflow 指针。
// ---------------------------------------------------------------------------
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
              //（catalog 登记线程亲和由框架断言）。进度接 PaleoTask::changed 读
              // stagePercent()/stage()——不再把面板指针传进 worker（#163 形态）。
              m_faciesMappingRunner.setTaskService(m_taskSvc);
              m_faciesMappingJob.reset();
              m_faciesMappingTask = m_faciesMappingWf->startJob(
                  m_faciesMappingRunner, request, &m_faciesMappingJob);
              if (m_faciesMappingTask)
              {
                connect(m_faciesMappingTask.data(), &PaleoTask::changed, this,
                        [this] {
                          if (PaleoTask *t = m_faciesMappingTask.data())
                            if (m_faciesMappingPanel)
                              m_faciesMappingPanel->updateProgress(t->stagePercent(),
                                                                   t->stage());
                        });
                connect(m_faciesMappingTask.data(), &PaleoTask::finished, this,
                        [this] { finishFaciesMappingRun(); });
              }
              else
                m_faciesMappingRunning = false;
              return;
            }

            // 无任务池（未接线壳/旧测试）：同步直跑，不泵事件。
            auto computed = FaciesMappingWorkflow::runCompute(
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
              // V2：断距提取——cut.extra 带 throw_z 且盘侧已知 → 断块错位；
              // 其余段照旧只进竖帘（口径由 provenance 如实标注）。
              const auto extracted = PropertyModelWorkflow::throwSegmentsFromFaultSet(
                  m_propModelFaults->faultSet());
              // 竖帘全集（含断距段几何——错位后跨断层仍不连通）
              const auto segs =
                  PropertyModelWorkflow::segmentsFromFaultSet(m_propModelFaults->faultSet());
              req.faults.insert(req.faults.end(), segs.begin(), segs.end());
              req.faultThrows = extracted.throws;
            }

            // V2：方法/SGS/对象/相带参数（面板纯值 getter → 请求翻译）。
            const int method = m_propModelPanel->method();
            req.method = method == 1 ? PropertyMethod::Sgs : PropertyMethod::Idw;
            if (req.method == PropertyMethod::Sgs)
            {
              req.variogram.type = static_cast<paleo::geostat::VariogramModelType>(
                  m_propModelPanel->variogramType());
              req.variogram.nugget = m_propModelPanel->nugget();
              req.variogram.sill = m_propModelPanel->sill();
              req.variogram.range = m_propModelPanel->rangeMeters();
              req.variogram.azimuthDeg = m_propModelPanel->azimuthDeg();
              req.variogram.anisotropyRatio = m_propModelPanel->anisotropyRatio();
              req.variogram.verticalRangeRatio = m_propModelPanel->verticalRangeRatio();
              req.sgsRealizations = m_propModelPanel->realizations();
              req.sgsSeed = m_propModelPanel->seed();
              req.sgsMaxPoints = 16;
            }
            if (m_propModelPanel->objectEnabled())
            {
              paleo::stratgrid::ObjectSpec spec;
              spec.type = m_propModelPanel->objectType() == 1
                              ? paleo::stratgrid::ObjectType::PointBar
                              : paleo::stratgrid::ObjectType::Channel;
              spec.azimuthDeg = m_propModelPanel->objectAzimuthDeg();
              spec.length = m_propModelPanel->objectLengthMeters();
              spec.width = m_propModelPanel->objectWidthMeters();
              spec.thickness = m_propModelPanel->objectThicknessMeters();
              spec.curvature = m_propModelPanel->objectCurvatureMeters();
              spec.value = m_propModelPanel->objectValue();
              spec.count = m_propModelPanel->objectCount();
              req.objectSpecs = {spec};
              req.objectSeed = m_propModelPanel->objectSeed();
            }
            if (m_propModelPanel->faciesEnabled())
            {
              QString faciesErr;
              if (!m_propModelWf->collectFaciesPolygons(&req, &faciesErr))
              {
                // 没有相带资产不是建模失败：降级全域单一域并如实标注。
                m_propModelPanel->setCaliberNote(
                    tr("相带约束已请求但不可用（%1）——本次全域单一参数域").arg(faciesErr));
              }
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
                                                        &m_propModelJob);
              if (!m_propModelTask)
              {
                m_propModelRunning = false;
                m_propModelPanel->setBusy(false);
                return;
              }
              // #163：进度经任务对象回主线程（任务属服务，服务排空前不析构），
              // 由主窗口转给面板——worker 不再持有面板裸指针。
              const QPointer<PaleoTask> task = m_propModelTask;
              const QPointer<PropertyModelPanel> panel = m_propModelPanel;
              connect(m_propModelTask.data(), &PaleoTask::changed, this, [task, panel] {
                if (task && panel && task->running())
                  panel->updateProgress(qMax(0, task->stagePercent()), task->stage());
              });
              // #159：收尾接 jobCompleted（commit/drop 执行完之后发），而不是
              // PaleoTask::finished——UI 段必须读到登记结果与登记失败。
              // 框架忙则拒绝，同一时刻只有一代，单发连接即可。
              connect(&m_propModelRunner, &paleo::jobs::JobRunnerBase::jobCompleted, this,
                      [this, overlayAlpha](quint64, bool) { finishPropertyModelRun(overlayAlpha); },
                      Qt::SingleShotConnection);
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

  // 异步路径由 JobRunnerBase::jobCompleted 调到这里：commit（成功或失败）/
  // drop 都已执行完（#159）。catalog 登记由 JobRunner 的 commit 段完成（已断言
  // owner 线程），本函数只负责把结果呈到 UI——不要再调 commitComputed，那会
  // 二次登记同一份 staging。登记失败时 commitComputed 已把 computed.out.ok
  // 置假并回填错误串，下面按失败如实上 UI。
  // 同步兜底路径（无任务池）不建 job，仍需直连登记（多实现逐版本）。
  // job 是 shared_ptr 共享体——异步路径直接引用，不整表深拷贝（R=64 大网格
  // 时每项含完整 PropertyVolume + blob）。
  PropertyModelOutput out;
  const PropertyModelWorkflow::PropertyModelComputedList *computedPtr = &m_propModelComputed;
  if (m_propModelJob)
  {
    computedPtr = &m_propModelJob->computed; // commit 已回填
    if (!cancelled && !computedPtr->empty() && computedPtr->front().out.ok &&
        !m_propModelJob->registered)
    {
      // 防御：未登记不报成功（改写共享体会污染 commit 段回填，改走本地标记）
      out.ok = false;
      out.error = tr("属性体登记失败");
    }
  }
  else if (!cancelled && !m_propModelComputed.empty() && m_propModelComputed.front().ok)
  {
    m_propModelWf->commitAll(&m_propModelComputed); // 无池兜底：同步直连
  }
  if (out.error.isEmpty())
  {
    if (cancelled)
    {
      out.ok = false;
      out.error = tr("已取消");
    }
    else if (computedPtr->empty())
    {
      out.ok = false;
      out.error = tr("属性建模失败");
    }
    else
    {
      out = computedPtr->front().out; // 呈现首实现；实现数见 out.realizationCount
    }
  }
  else
  {
    out.ok = false;
  }
  if (out.ok && !computedPtr->empty())
  {
    // 诚实口径标签：竖直近似井数 / 断层竖帘与错位 / 相带 / 种子（首实现 extra）。
    m_propModelPanel->setCaliberNote(
        computedPtr->front().extra.value(QStringLiteral("caliber")).toString());
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
  const QString realizationNote =
      out.realizationCount > 1 ? tr(" · %1 个实现").arg(out.realizationCount) : QString();
  m_propModelPanel->showResult(
      true, tr("已充填 %1 个单元 · %2%3").arg(out.filledCells).arg(fileName).arg(realizationNote));
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

// goal/attr-volume — 属性体 3D 预览 → 视口喂入：预览（三中位面 + 堆叠层）
// 已由 dock 的静默预览任务在服务线程取数烘焙（SATV 可达 GB 级——主线程
// 同步读文件会冻 UI），此处仅贴纹理（几何走属性砖块 IJK：i=xline、
// j=inline、k=采样——finishPropertyModelRun 同一挂点）。ok=false（含
// 登记消息）状态栏如实报因。
void PaleoMainWindow::showAttributeVolumeIn3D(
    const seismic::SeismicTaskService::AttributeVolumePreview &preview, bool ok,
    const QString &message)
{
  if (!ok || !preview.ok)
  {
    const QString why =
        !message.isEmpty() ? message
                           : (!preview.error.isEmpty()
                                  ? preview.error
                                  : QStringLiteral("属性体预览不可用"));
    if (statusBar())
      statusBar()->showMessage(tr("属性体 3D 显示失败：%1").arg(why), 8000);
    return;
  }
  seismic::Seismic3DViewPanel *panel3d = m_seismic3dPanel;
  if (!panel3d && m_seismic3dDock)
    panel3d = qobject_cast<seismic::Seismic3DViewPanel *>(m_seismic3dDock->widget());
  seismic::Seismic3DViewportWidget *vp = panel3d ? panel3d->viewport() : nullptr;
  if (!vp)
  {
    if (statusBar())
      statusBar()->showMessage(tr("属性体已产出（%1×%2×%3）——三维视口不可用")
                                   .arg(preview.nIl)
                                   .arg(preview.nXl)
                                   .arg(preview.nS), 8000);
    return;
  }
  seismic::PropertyBrickAxes axes;
  axes.iMin = 0;
  axes.iMax = preview.nXl - 1;
  axes.jMin = 0;
  axes.jMax = preview.nIl - 1;
  axes.kMin = 0;
  axes.kMax = preview.nS - 1;
  vp->updatePropertySlice(seismic::SeismicSliceSlot::Crossline,
                          seismic::SgySliceType::Xline, preview.xlineIdx, axes,
                          preview.xlineSlice);
  vp->updatePropertySlice(seismic::SeismicSliceSlot::Inline,
                          seismic::SgySliceType::Inline, preview.inlineIdx, axes,
                          preview.inlineSlice);
  vp->updatePropertySlice(seismic::SeismicSliceSlot::Time,
                          seismic::SgySliceType::Time, preview.sampleIdx, axes,
                          preview.timeSlice);
  for (int k = 0; k < preview.stackLayerCount; ++k)
    vp->updatePropertyStackLayer(k, preview.stackKIndexes[k], axes,
                                 preview.stackLayers[std::size_t(k)]);
  if (statusBar())
    statusBar()->showMessage(
        tr("属性体已入 3D 视口（%1×%2×%3，堆叠 %4 层——栈模式开关查看）%5")
            .arg(preview.nIl)
            .arg(preview.nXl)
            .arg(preview.nS)
            .arg(preview.stackLayerCount)
            .arg(message.isEmpty() ? QString()
                                   : QStringLiteral("；") + message), 8000);
}
