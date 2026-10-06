// 层：视图
// paleomainwindow_attach_predict — 智能预测页接线（W4 拆分段）
#include "paleomainwindow.h"

#include "../qgis/qgiscanvascontroller.h"
#include "../qgis/qgisprojectservice.h"
#include "../qgis/qgislayerservice.h"
#include "../services/paleotaskservice.h"
#include "../services/pythonenv.h"
#include "../workflow/workflows.h"
#include "../workflow/mamcltool.h"
#include "pages/pagepanels.h"

#include <qgsmapcanvas.h>
#include <qgsproject.h>
#include <qgsmaplayer.h>
#include <qgslayertree.h>
#include <qgslayertreelayer.h>
#include <qgsmessagelog.h>

#include <QLabel>
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
