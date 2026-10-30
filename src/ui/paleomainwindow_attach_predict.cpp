// 层：视图
// paleomainwindow_attach_predict — 智能预测页接线（W4 拆分段）
#include "paleomainwindow.h"

#include "../qgis/qgiscanvascontroller.h"
#include "../qgis/qgisprojectservice.h"
#include "../qgis/qgislayerservice.h"
#include "../services/paleotaskservice.h"
#include "../workflow/workflows.h"
#include "pages/pagepanels.h"
#include "../linkage/unitselectionbridge.h"
#include "../linkage/selectioncontext.h"
#include "../services/subintervalfactors.h"

#include <qgsmapcanvas.h>
#include <qgsproject.h>
#include <qgsmaplayer.h>
#include <qgslayertree.h>
#include <qgslayertreelayer.h>
#include <qgsmessagelog.h>

#include <QLabel>
#include <QVariant>
#include <QStatusBar>
#include <QDir>
#include <QStandardPaths>
#include <memory>

// ---------------------------------------------------------------------------
// 预测页接线（W4 拆分段）
// ---------------------------------------------------------------------------
void PaleoMainWindow::attachPredictPage(PredictPage *predictPage,
                                        PredictionWorkflow *pred)
{
  if (pred && predictPage)
  {
    // 分析就绪读 UnitSelection；页内 horizonCombo 仅 legacy，不可驱动分析。
    if (m_selection)
    {
      predictPage->setProperty("paleo.selection",
                               QVariant::fromValue(static_cast<QObject *>(m_selection)));
      connect(m_selection, &SelectionContext::activeUnitChanged, predictPage,
              [predictPage](const paleo::strat::UnitSelection &) {
                predictPage->updateReadiness();
                predictPage->refreshHistory();
              });
    }
    // 工程打开后 appcontext 已把 onnx 模型根重设到 <工程>/models——算法
    // 列表随之刷新（未装模型 → 页面如实降级文案）。projectSvc 为空的装配
    // （测试壳传 nullptr）不连，否则 Qt 只打「Cannot connect (null)」告警、
    // 连接静默失效。
    if (m_projectSvc)
      connect(m_projectSvc, &QgisProjectService::projectOpened, this,
              [this, pred, predictPage] {
                predictPage->setAlgorithms(pred->availableAlgorithms());
              });
    // ---- m2(A): 预测运行任务化 + 历史结果显示接线 ----
    // #277 三段式（仿约束页）：prepare（catalog stage 线程闸，GUI 线程）→
    // worker 只跑 compute（Processing / ONNX 推理 + 栅格落盘，不碰 catalog）
    // → finished 回 GUI 线程 publish（DERIVED 登记 + 图层声明 +
    // predictionDone）。失败原因经任务 errorText 如实上屏（此前 outErr 从不
    // 写入，界面恒为空的「预测失败：」）；取消是协作式的——预测运算本体无法
    // 中断，取消后不登记/不声明、清理 staging，状态栏如实呈现「未发布」。
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
              // 层段分析：只认 UnitSelection；页内 horizon 字符串不可驱动分析键。
              Q_UNUSED(horizon);
              const auto bind = paleo::strat::bindAnalysis(m_selection);
              if (!bind.hasUnit || bind.analysisKey.isEmpty())
              {
                logFail(tr("分析须先在顶部层段选择器选定子层段；层位下拉不可驱动分析"));
                return;
              }
              const QVariantMap runParams = bind.enrich(params);
              const QString dataHorizon =
                  !bind.constraintHorizon.isEmpty() ? bind.constraintHorizon : bind.analysisKey;
              if (m_taskSvc)
              {
                for (PaleoTask *t : m_taskSvc->tasks())
                {
                  if (t && t->running() && t->title().startsWith(tr("预测")))
                  {
                    logFail(tr("已有预测任务正在运行，请等待完成或取消后再运行"));
                    return;
                  }
                }
              }
              if (!m_taskSvc)
              {
                QString err;
                if (!pred->runPrediction(dataHorizon, algId, runParams, &err))
                  logFail(tr("预测失败：%1")
                              .arg(err.isEmpty() ? tr("未知原因") : err));
                return;
              }
              auto job = std::make_shared<PredictionWorkflow::PredictionJob>();
              QString prepErr;
              if (!pred->preparePredictionJob(dataHorizon, algId, runParams, job.get(), &prepErr))
              {
                logFail(tr("预测失败：%1")
                            .arg(prepErr.isEmpty() ? tr("参数准备失败") : prepErr));
                return;
              }
              predictPage->setRunBusy(true);
              PaleoTask *task = m_taskSvc->start(
                  tr("预测 %1 · %2").arg(bind.hasUnit ? bind.unit.displayName : dataHorizon, algId),
                  [pred, job](PaleoTask *t) -> QString {
                    job->isCancelled = [t]() -> bool { return t ? t->cancelRequested() : false; };
                    if (pred->computePredictionJob(job.get()))
                      return QString();
                    return job->error.isEmpty() ? QObject::tr("预测失败") : job->error;
                  });
              QObject::connect(task, &PaleoTask::changed, predictPage,
                               [predictPage, task] {
                                 const int pct = task->percent();
                                 if (pct >= 0) // 无进度回调的算法不伪造进度
                                   predictPage->updateProgress(pct);
                               });
              QObject::connect(task, &PaleoTask::finished, predictPage,
                               [this, predictPage, pred, job, status, logFail, task] {
                                 predictPage->setRunBusy(false);
                                 if (task->state() == PaleoTask::State::Cancelled)
                                 {
                                   // 取消是协作式的：运算本体无法中断、已跑完，
                                   // 但成果不登记/不声明；staging 栅格如已落盘
                                   // 即清掉（约束页同口径），文案如实说「未发布」
                                   //（旧文案「以实际完成为准」与不发布矛盾）。
                                   // #481 P2-1：删除语义收进 workflow——按目录名
                                   // == staging versionId 判定，不再无守卫
                                   // removeRecursively（空路径会删当前目录）。
                                   PaleoWorkflowDiscardPredictionStagingDir(
                                       job->staging.versionId, job->outputPath);
                                   const QString msg = tr(
                                       "预测已取消——运算无法中断，计算结果未发布");
                                   if (status)
                                     status->setText(msg);
                                   QgsMessageLog::logMessage(msg, QStringLiteral("Paleo"),
                                                             Qgis::MessageLevel::Warning);
                                   return;
                                 }
                                 if (task->state() != PaleoTask::State::Succeeded)
                                 {
                                   // #277：失败原因如实传（task->errorText() =
                                   // compute 写入的 job.error）。
                                   const QString reason =
                                       task->errorText().isEmpty() ? tr("未知原因")
                                                                   : task->errorText();
                                   logFail(tr("预测失败：%1").arg(reason));
                                   return;
                                 }
                                 // 发布是临界区：回到 GUI 线程做 catalog 登记 +
                                 // 图层声明（worker 做会被 catalog 线程闸拒绝）。
                                 QString pubErr;
                                 if (!pred->publishPredictionJob(*job, &pubErr))
                                 {
                                   logFail(tr("预测失败：%1")
                                               .arg(pubErr.isEmpty() ? tr("成果发布失败")
                                                                     : pubErr));
                                   return;
                                 }
                                 bindSeismicSliceAfterPublish(pred, job->params, job->layerId, job->outputPath, status);
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

  attachPredictPageTools(predictPage, pred);
}
