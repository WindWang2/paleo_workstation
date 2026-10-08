// 层：视图
// paleomainwindow_attach_constraint — 约束与单因素页接线（W4 拆分段）
#include "paleomainwindow.h"

#include "../qgis/qgiscanvascontroller.h"
#include "../qgis/qgisprojectservice.h"
#include "../qgis/qgislayerservice.h"
#include "../metadata/layermanifest.h"
#include "../workflow/workflows.h"
#include "../services/paleotaskservice.h"
#include "../services/previewdoc.h"
#include "../catalog/datacatalog.h"
#include "../catalog/realizationset.h"
#include "../workflow/realizationworkflow.h"
#include "realization/realizationpanel.h"
#include "pages/pagepanels.h"
#include "constraintdrawcontroller.h"
#include "typedconstraintdrawcontroller.h"
#include "edittools/editingtoolbar.h"

#include <qgsmapcanvas.h>
#include <qgsproject.h>
#include <qgsmaplayer.h>
#include <qgsvectorlayer.h>
#include <qgslayertree.h>
#include <qgslayertreelayer.h>
#include <qgsmessagelog.h>
#include <qgsadvanceddigitizingdockwidget.h>

#include <QDateTime>
#include <QDir>
#include <QDoubleSpinBox>
#include <QFileInfo>
#include <QLabel>
#include <QLineEdit>
#include <QStatusBar>
#include <QUndoStack>
#include <memory>

// ---------------------------------------------------------------------------
// 约束页接线：绘制捕获 + IDW 插值（W4 拆分段）
// ---------------------------------------------------------------------------
void PaleoMainWindow::attachConstraintPage(ConstraintPage *constraintPage,
                                           ConstraintWorkflow *constraint)
{
  if (constraint && constraintPage)
  {
    connect(constraint, &ConstraintWorkflow::wellAttributeTableRequested, this, &PaleoMainWindow::showAttributeTable);
    connect(constraintPage, &ConstraintPage::maintainWellFactorsRequested, this, [this, constraint](const QString &horizon) {
      QString error;
      if (!constraint->maintainWellFactors(horizon, &error))
        statusBar()->showMessage(error);
    });
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
    connect(constraintPage, &ConstraintPage::extractWellFactorsRequested, this,
            [constraint](const QString &factorId, const QString &horizon, const QVariantMap &params) {
              QString error;
              constraint->extractWellFactors(horizon, factorId, params, &error);
            });
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
                                 || methodId == QLatin1String("local_direction_kriging")
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
              [typedCtl, constraintPage](const QString &horizon, const QString &shape,
                         const QString &constraintType, int faciesCode) {
                typedCtl->startCapture(horizon, shape, constraintType, faciesCode, constraintPage->newConstraintLineParams(constraintType));
              });
      connect(typedCtl, &TypedConstraintDrawController::captureFailed, this,
              [](const QString &err) {
                QgsMessageLog::logMessage(err, QStringLiteral("Paleo"), Qgis::MessageLevel::Warning);
              });
    }
    connect(constraintPage, &ConstraintPage::constraintSelectionChanged, this,
            [this](const QString &horizon, const QStringList &ids) {
      if (!m_layerSvc)
        return;
      auto *layer = qobject_cast<QgsVectorLayer *>(m_layerSvc->instantiate(QStringLiteral("constraints.%1").arg(horizon)));
      if (!layer)
        return;
      QgsFeatureIds selection;
      QgsFeature feature;
      auto features = layer->getFeatures();
      while (features.nextFeature(feature))
        if (ids.contains(feature.attribute(QStringLiteral("id")).toString()))
          selection.insert(feature.id());
      layer->selectByIds(selection);
    });
    const auto beginConstraintEdit = [this, constraintPage](const QString &horizon) -> QgsVectorLayer * {
      if (!m_layerSvc)
        return nullptr;
      auto *layer = qobject_cast<QgsVectorLayer *>(m_layerSvc->instantiate(QStringLiteral("constraints.%1").arg(horizon)));
      auto *toolbar = findChild<PaleoEditingToolbar *>(QStringLiteral("editingToolbar"));
      if (!layer || !toolbar)
        return nullptr;
      toolbar->refreshFromProject();
      toolbar->setCurrentLayer(layer);
      if (toolbar->currentLayer() != layer || !toolbar->startEditing())
        return nullptr;
      connect(layer->undoStack(), &QUndoStack::indexChanged, constraintPage,
              &ConstraintPage::refreshConstraintList, Qt::UniqueConnection);
      connect(toolbar, &PaleoEditingToolbar::editingStopped, constraintPage,
              &ConstraintPage::refreshConstraintList, Qt::UniqueConnection);
      return layer;
    };
    connect(constraintPage, &ConstraintPage::constraintParametersRequested, this,
            [constraint, constraintPage, beginConstraintEdit](const QString &horizon, const QStringList &ids, const QVariantMap &patch) {
      QString error;
      const bool ok = beginConstraintEdit(horizon) && constraint->updateConstraintLines(ids, patch, &error);
      if (!ok)
        if (auto *status = constraintPage->findChild<QLabel *>(QStringLiteral("statusLabel")))
          status->setText(error.isEmpty() ? tr("约束编辑不可用，请检查编辑会话与资产状态") : error);
    });
    // ---- 方向23：已绘约束线编辑面 ----
    // 删除：store 落盘删除 + 已实例化的 constraints.<horizon> 图层重载。
    connect(constraintPage, &ConstraintPage::constraintDeleteRequested, this,
            [this, constraint, constraintPage, beginConstraintEdit](const QString &horizon, const QString &id) {
              if (!beginConstraintEdit(horizon))
                return;
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
                    if (!vl->isEditable())
                      vl->reload();
                    vl->triggerRepaint();
                  }
                }
              }
              constraintPage->refreshConstraintList();
            });
    // 语义切换：走 updateConstraintLine 同一持久化通道（type 列 + params_json）。
    connect(constraintPage, &ConstraintPage::constraintSemanticChangeRequested, this,
            [constraint, beginConstraintEdit](const QString &horizon, const QString &id, const QString &semantic) {
              if (!beginConstraintEdit(horizon))
                return;
              QString err;
              if (!constraint->switchConstraintSemantic(id, semantic, &err))
                QgsMessageLog::logMessage(err.isEmpty() ? tr("切换约束语义失败") : err,
                                          QStringLiteral("Paleo"), Qgis::MessageLevel::Warning);
            });
    // 顶点编辑：约束表是可写 GPKG（非派生只读），直接进编辑会话 + 顶点工具，
    // 撤销/重做走编辑条的原生 undo 栈；提交后约束页重读列表。
    connect(constraintPage, &ConstraintPage::editConstraintVerticesRequested, this,
            [this, beginConstraintEdit, constraintPage](const QString &horizon) {
      auto *layer = beginConstraintEdit(horizon);
      auto *toolbar = findChild<PaleoEditingToolbar *>(QStringLiteral("editingToolbar"));
      if (!layer || !toolbar)
        return;
      if (layer->selectedFeatureCount() == 0)
        layer->selectAll();
      toolbar->actionVertexEdit()->trigger();
      connect(toolbar, &PaleoEditingToolbar::editingStopped, constraintPage,
              &ConstraintPage::refreshConstraintList, Qt::UniqueConnection);
    });
    // ---- m2(B) end ----
  }

  // ---- 方向 47：realization 集合面板接线（独立于约束 workflow——集合是
  // SGS 副产物，查看/派生/差值不依赖约束捕获通道）----------------------------
  if (constraintPage)
  {
    auto *rsPanel = constraintPage->findChild<RealizationPanel *>(
        QStringLiteral("realizationPanel"));
    if (rsPanel)
    {
      if (!m_realizationWf)
      {
        m_realizationWf = new RealizationWorkflow(this);
        m_realizationWf->setObjectName(QStringLiteral("realizationWorkflow"));
      }
      // catalog 换绑：工程打开后 catalog/projectDir 才就绪；接线时已在场则
      // 立即换绑。bind 幂等（成员寻址按调用时刻状态查询）。
      const auto rebindRealization = [this, rsPanel] {
        DataCatalog *cat = m_previewDoc ? m_previewDoc->catalog() : nullptr;
        const QString projectDir =
            m_projectSvc ? QFileInfo(m_projectSvc->projectPath()).absolutePath() : QString();
        if (m_realizationWf)
        {
          m_realizationWf->bind(cat, projectDir, m_layerSvc);
          m_realizationWf->setTaskService(m_taskSvc);
        }
        rsPanel->bindCatalog(cat);
      };
      if (m_projectSvc)
        connect(m_projectSvc, &QgisProjectService::projectOpened, this,
                rebindRealization);
      rebindRealization();

      connect(m_realizationWf, &RealizationWorkflow::busyChanged, rsPanel,
              &RealizationPanel::setBusy);

      connect(rsPanel, &RealizationPanel::statusMessage, this,
              [this](const QString &text) { statusBar()->showMessage(text, 8000); });
      connect(rsPanel, &RealizationPanel::memberShowRequested, this,
              [this](const QString &setId, int index) {
                showRealizationMember(setId, index);
              });
      connect(rsPanel, &RealizationPanel::statShowRequested, this,
              [this](const QString &setId, const QString &token) {
                showRealizationStat(setId, token);
              });
      // 派生/差值走异步三段式（#227：取数/计算/写盘在任务池，主线程只贴结果；面板置 busy 防连点重跑）。
      connect(rsPanel, &RealizationPanel::deriveStatsRequested, this,
              [this, rsPanel](const QString &setId) {
                if (!m_realizationWf)
                  return;
                rsPanel->setBusy(true);
                QString err;
                PaleoTask *task = m_realizationWf->deriveAllStatisticsAsync(setId, &err);
                if (!task)
                {
                  if (!m_realizationWf->deriveAllStatistics(setId, &err))
                    statusBar()->showMessage(
                        err.isEmpty() ? tr("统计派生失败") : err, 8000);
                  rsPanel->setBusy(false);
                }
              });
      connect(rsPanel, &RealizationPanel::diffRequested, this,
              [this, rsPanel](const QString &setIdA, const QString &setIdB) {
                if (!m_realizationWf)
                  return;
                rsPanel->setBusy(true);
                QString err;
                PaleoTask *task = m_realizationWf->differenceOfMeansAsync(setIdA, setIdB, &err);
                if (!task)
                {
                  if (!m_realizationWf->differenceOfMeans(setIdA, setIdB, &err))
                    statusBar()->showMessage(
                        err.isEmpty() ? tr("集合差值失败") : err, 8000);
                  rsPanel->setBusy(false);
                }
              });
      // 派生/差值成功 → 直接上图反馈（统计亮均值面，图签口径词同源）。
      connect(m_realizationWf, &RealizationWorkflow::realizationStatsDerived, this,
              [this](const QString &setId, const QStringList &tokens) {
                if (tokens.contains(paleo::realization::kStatMean))
                  showRealizationStat(setId, paleo::realization::kStatMean);
              });
      connect(m_realizationWf, &RealizationWorkflow::realizationDiffReady, this,
              [this](const QString &, const QString &, const QString &layerId) {
                showRealizationLayer(
                    layerId,
                    paleo::realization::statisticDisplayLabel(
                        paleo::realization::kStatMeanDiff),
                    QString());
              });
    }
  }
}
