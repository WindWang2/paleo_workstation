// 层：视图
// paleomainwindow_attach_constraint — 约束与单因素页接线（W4 拆分段）
#include "paleomainwindow.h"
#include "../linkage/unitselectionbridge.h"
#include "../linkage/selectioncontext.h"

#include "../qgis/qgiscanvascontroller.h"
#include "../qgis/qgisprojectservice.h"
#include "../qgis/qgislayerservice.h"
#include "../qgis/qgislayerprofile.h"
#include "../qgis/qgisstyleservice.h"
#include "../qgis/layervocabulary.h"
#include "../metadata/layermanifest.h"
#include "../workflow/workflows.h"
#include "../workflow/constraintjobs.h" // #558：job 族完整定义（原内嵌 workflows.h）
#include "../workflow/mappingworkbench.h"
#include "pages/mappingworkbenchpage.h"
#include "../services/paleotaskservice.h"
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
#include <QFileDialog>
#include <QLabel>
#include <QVariant>
#include <QLineEdit>
#include <QStatusBar>
#include <QUndoStack>
#include <functional>
#include <memory>

namespace {
struct AnalysisRun
{
  paleo::strat::AnalysisBinding bind;
  QVariantMap params;
};
AnalysisRun makeAnalysisRun(SelectionContext *selection, const QString &pageHorizon,
                            const QVariantMap &params)
{
  // pageHorizon 只喂界面编辑 legacy（constraints.<top>）；analysisKey 只认 UnitSelection。
  AnalysisRun r;
  r.bind = paleo::strat::bindAnalysis(selection, pageHorizon);
  r.params = r.bind.enrich(params);
  r.params.insert(QStringLiteral("constraintHorizon"), r.bind.constraintHorizon);
  return r;
}

void runConstraintPageIdw(ConstraintWorkflow *constraint,
                          ConstraintPage *constraintPage,
                          SelectionContext *selection,
                          QgisLayerService *layerSvc,
                          PaleoTask *factorTask,
                          const QString &pageHorizon)
{
  auto *status = constraintPage->findChild<QLabel *>(QStringLiteral("statusLabel"));
  const auto fail = [status](const QString &msg) {
    if (status)
      status->setText(msg);
    QgsMessageLog::logMessage(msg, QStringLiteral("Paleo"), Qgis::MessageLevel::Warning);
  };
  if (constraint->factorBusy() || (factorTask && factorTask->running())) {
    fail(PaleoMainWindow::tr("已有单因素计算在进行"));
    return;
  }
  // 兼容 IDW 也只认 UnitSelection；pageHorizon/combo 不可驱动分析键。
  const auto bind = paleo::strat::bindAnalysis(selection, pageHorizon);
  if (!bind.hasUnit || bind.analysisKey.isEmpty())
  {
    fail(PaleoMainWindow::tr("分析须先在顶部层段选择器选定子层段；层位下拉不可驱动分析"));
    return;
  }
  const QString editHorizon =
      !bind.constraintHorizon.isEmpty() ? bind.constraintHorizon : bind.analysisKey;

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
  if (!layerSvc)
  {
    fail(PaleoMainWindow::tr("图层服务未就绪"));
    return;
  }
  QVector<LayerDeclaration> decls;
  QString readErr;
  if (!layerSvc->tryDeclared(&decls, &readErr))
  {
    fail(readErr.isEmpty() ? PaleoMainWindow::tr("无法读取图层清单") : readErr);
    return;
  }
  QString agnostic;
  for (const LayerDeclaration &d : decls)
  {
    if (d.type.compare(QStringLiteral("vector"), Qt::CaseInsensitive) != 0)
      continue;
    if (!d.layerId.startsWith(QStringLiteral("wells")))
      continue;
    if (d.horizon == editHorizon || d.horizon == bind.analysisKey)
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
    fail(PaleoMainWindow::tr("层位 %1 没有井点图层").arg(editHorizon));
    return;
  }

  QString err;
  // 成果键用 analysisKey（稳定 SubIntervalId）；井点匹配可用父单元上界面。
  if (!constraint->runConstraintIDW(bind.analysisKey, pointsId, field, cellSize, &err))
    fail(err.isEmpty() ? PaleoMainWindow::tr("约束插值失败") : err);
}

void wireConstraintUnitRefresh(ConstraintPage *constraintPage, SelectionContext *selection)
{
  if (!selection)
    return;
  constraintPage->setProperty("paleo.selection",
                              QVariant::fromValue(static_cast<QObject *>(selection)));
  // 同父单元切 S1–S4 时 combo 文本不变，须靠 activeUnitChanged 刷新分析态。
  QObject::connect(selection, &SelectionContext::activeUnitChanged, constraintPage,
                   [constraintPage](const paleo::strat::UnitSelection &) {
                     constraintPage->refreshAnalysisUnit();
                   });
}

} // namespace


// ---------------------------------------------------------------------------
// 约束页接线：绘制捕获 + IDW 插值（W4 拆分段）
// ---------------------------------------------------------------------------
void PaleoMainWindow::attachConstraintDrawController(ConstraintPage *constraintPage,
                                                    ConstraintWorkflow *constraint)
{
  if (!m_canvasCtl)
    return;
  auto *drawCtl = new ConstraintDrawController(m_canvasCtl, constraint, this);
  connect(constraintPage, &ConstraintPage::drawConstraintRequested, drawCtl,
          [drawCtl](const QString &horizon, const QString &shape, int faciesCode) {
            drawCtl->startCapture(horizon, shape, faciesCode);
          });
  connect(drawCtl, &ConstraintDrawController::captureFailed, this,
          [](const QString &err) {
            QgsMessageLog::logMessage(err, QStringLiteral("Paleo"), Qgis::MessageLevel::Warning);
          });
  connect(drawCtl, &ConstraintDrawController::captureFinished, this,
          [this](const QString &horizon, const QString &) { showDrawnConstraintLines(horizon); });
}

void PaleoMainWindow::showDrawnConstraintLines(const QString &horizon)
{
  if (horizon.isEmpty() || !m_layerSvc)
    return;
  const QString layerId = QStringLiteral("constraints.%1").arg(horizon);
  if (!revealDeclaredLayer(layerId, false))
    return;
  if (m_workbench)
    m_workbench->styleLayer(layerId);
  else if (auto *vector = qobject_cast<QgsVectorLayer *>(m_layerSvc->layer(layerId)))
    QgisStyleService::applyConstraintLayerStyle(vector);
  const QString lineViewId =
      QStringLiteral("%1%2.line").arg(PaleoLayerVocabulary::kConstraintGeometryViewPrefix, horizon);
  if (!m_layerSvc->declaration(lineViewId).layerId.isEmpty())
    revealDeclaredLayer(lineViewId, false);
  if (auto *vector = qobject_cast<QgsVectorLayer *>(m_layerSvc->layer(layerId)))
    vector->triggerRepaint();
  if (m_canvasCtl)
  {
    if (QgsMapCanvas *canvas = m_canvasCtl->canvas())
    {
      if (QgsMapLayer *layer = m_layerSvc->layer(layerId))
        canvas->setCurrentLayer(layer);
      canvas->refresh();
    }
  }
  if (m_profileSvc)
    m_profileSvc->captureCurrentAsTheme(QgisLayerProfileService::pageThemeName(currentPage()));
}

void PaleoMainWindow::attachConstraintMapSelection(ConstraintPage *constraintPage)
{
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
  connect(constraintPage, &ConstraintPage::constraintLocateRequested, this,
          [this](const QString &horizon, const QString &id) {
    if (!m_layerSvc || !m_canvasCtl || !m_canvasCtl->canvas())
      return;
    auto *layer = qobject_cast<QgsVectorLayer *>(m_layerSvc->instantiate(QStringLiteral("constraints.%1").arg(horizon)));
    if (!layer)
      return;
    QgsFeatureIds selection;
    QgsFeature feature;
    auto features = layer->getFeatures();
    while (features.nextFeature(feature))
      if (feature.attribute(QStringLiteral("id")).toString() == id)
        selection.insert(feature.id());
    if (selection.isEmpty())
      return;
    layer->selectByIds(selection);
    m_canvasCtl->canvas()->zoomToSelected(layer);
    m_canvasCtl->canvas()->refresh();
  });
}

void PaleoMainWindow::attachConstraintPage(ConstraintPage *constraintPage,
                                           ConstraintWorkflow *constraint)
{
  if (constraint && constraintPage)
  {
    // 分析就绪读 UnitSelection；页内 horizonCombo 仅 legacy，不可驱动分析。
    wireConstraintUnitRefresh(constraintPage, m_selection);
    connect(constraint, &ConstraintWorkflow::wellAttributeTableRequested, this, &PaleoMainWindow::showAttributeTable);
    connect(constraintPage, &ConstraintPage::maintainWellFactorsRequested, this,
            [this, constraint, constraintPage](const QString &horizon) {
              QString error;
              if (!constraint->maintainWellFactors(horizon, &error))
                statusBar()->showMessage(error);
            });
    // 约束页端到端: 绘制请求 → 画布上的捕获工具 → workflow 提交 (§42).
    attachConstraintDrawController(constraintPage, constraint);
    connect(constraintPage, &ConstraintPage::runIdwRequested, this,
            [this, constraint, constraintPage](const QString &pageHorizon) {
              runConstraintPageIdw(constraint, constraintPage, m_selection, m_layerSvc,
                                   m_factorTask, pageHorizon);
            });

    // ---- m2(B): 单因素图页（deliverable 2）接线——三入口的类型化捕获。----
    // 状态列刷新：页面监听 layerDeclared/factorGenerated（含重开工程重读）。
    if (m_layerSvc)
      constraintPage->bindLayerService(m_layerSvc);
    connect(constraintPage, &ConstraintPage::taskCenterRequested, this,
            &PaleoMainWindow::openTaskCenter);
    connect(constraintPage, &ConstraintPage::constraintImportRequested, this,
            [this, constraintPage](const QString &horizon, const QString &role) {
              if (!m_workbench) {
                statusBar()->showMessage(tr("约束导入服务未就绪"));
                return;
              }
              const auto path = QFileDialog::getOpenFileName(
                  this, role == QLatin1String("barrier") ? tr("导入间隔线") : tr("导入方向线"), {},
                  tr("矢量文件 (*.gpkg *.geojson *.json *.shp)"));
              if (path.isEmpty())
                return;
              QString error;
              if (!m_workbench->startConstraintImport(horizon, path, role, &error))
                statusBar()->showMessage(error, 15000);
              constraintPage->refreshConstraintList();
              constraintPage->refreshFactorResults();
            });
    connect(constraintPage, &ConstraintPage::factorResultRequested, this,
            [this](const QString &versionId, const QString &action) {
              if (!m_workbench)
                return;
              if (action == QLatin1String("graph")) {
                const auto version =
                    m_workbench->versionForLayer(QStringLiteral("product.") + versionId);
                if (!version.id.isEmpty())
                  openDataGraph(version.assetId, version.id);
                return;
              }
              QString error;
              const auto id = m_workbench->layerForVersion(versionId, &error);
              if (id.isEmpty()) {
                statusBar()->showMessage(error, 15000);
                return;
              }
              const auto declaration = m_workbench->declaration(id);
              if (action == QLatin1String("compose")) {
                showPage(QStringLiteral("compose"));
                for (auto *page : findChildren<MappingWorkbenchPage *>())
                  if (page->mode() == QLatin1String("compose")) {
                    page->setHorizon(declaration.horizon);
                    page->refresh();
                    page->selectEvidenceSource(id);
                    page->showMessage(tr("已将此版本设为编图证据来源。请选择目标编辑图件及要素，填写说明后添加证据。"));
                  }
              } else {
                for (auto *page : findChildren<MappingWorkbenchPage *>()) {
                  if (page->mode() == QLatin1String("constraint")) {
                    page->setHorizon(declaration.horizon);
                    page->refresh();
                    page->selectLayer(id);
                    emit page->commandRequested(QStringLiteral("show"),
                                                {{"layer", id}, {"horizon", declaration.horizon}});
                    break;
                  }
                }
              }
            });
    connect(constraintPage, &ConstraintPage::extractWellFactorsRequested, this,
            [this, constraint, constraintPage](const QString &factorId, const QString &horizon,
                                         const QVariantMap &params) {
              QString error;
              const auto run = makeAnalysisRun(m_selection, horizon, params);
              if (!run.bind.hasUnit || run.bind.analysisKey.isEmpty())
              {
                if (auto *status = constraintPage->findChild<QLabel *>(QStringLiteral("statusLabel")))
                  status->setText(tr("分析须先在顶部层段选择器选定子层段；层位下拉不可驱动分析"));
                return;
              }
              if (!constraint->startWellFactorExtraction(run.bind.constraintHorizon, factorId, run.params, &error))
                if (auto *status = constraintPage->findChild<QLabel *>(QStringLiteral("statusLabel")))
                  status->setText(error);
            });
    connect(constraint, &ConstraintWorkflow::factorJobStateChanged,
            constraintPage, &ConstraintPage::setRunBusy);
    connect(constraint, &ConstraintWorkflow::factorJobProgress, constraintPage,
            [constraintPage](int percent, const QString &stage) {
              constraintPage->noteRunStage(stage == "hash"      ? tr("校验成果")
                                           : stage == "publish" ? tr("保存成果")
                                           : stage == "parse" ? tr("读取资料")
                                                                : tr("计算"),
                                           percent);
            });
    connect(constraint, &ConstraintWorkflow::factorJobFailed, constraintPage,
            [constraintPage](const QString &error) {
              constraintPage->setProperty("paleo.page.runoutcome", error);
              if (auto *status = constraintPage->findChild<QLabel *>(
                      QStringLiteral("statusLabel")))
                status->setText(error);
            });
    connect(constraint, &ConstraintWorkflow::factorJobCancelled, constraintPage, [constraintPage] {
      const auto message = tr("任务已取消或输入已变更；已保存的成果仍可引用。");
      constraintPage->setProperty("paleo.page.runoutcome", message);
      if (auto *status = constraintPage->findChild<QLabel *>(QStringLiteral("statusLabel")))
        status->setText(message);
    });
    connect(
        constraintPage, &ConstraintPage::generateFactorRequested, this,
        [this, constraint, constraintPage](const QString &factorId,
                                           const QString &horizon,
                                           const QVariantMap &params) {
          QString error;
          const auto run = makeAnalysisRun(m_selection, horizon, params);
          if (!run.bind.hasUnit || run.bind.analysisKey.isEmpty())
          {
            if (auto *status = constraintPage->findChild<QLabel *>(QStringLiteral("statusLabel")))
              status->setText(tr("分析须先在顶部层段选择器选定子层段；层位下拉不可驱动分析"));
            return;
          }
          bool ready = !(m_factorTask && m_factorTask->running()) && !constraint->factorBusy()
                       && constraint->saveConstraintEdits(run.bind.constraintHorizon, &error);
          // Freeze editable constraints as a managed input version before any surface calculation.
          if (ready && m_workbench && !constraint->loadConstraints(run.bind.constraintHorizon).isEmpty())
            ready = !m_workbench->snapshotConstraints(run.bind.constraintHorizon, {}, &error).isEmpty();
          if (!ready || !constraint->generateFactor(run.bind.analysisKey, factorId, run.params, &error)) {
            if (auto *status = constraintPage->findChild<QLabel *>(
                    QStringLiteral("statusLabel")))
              status->setText(error.isEmpty()
                                  ? tr("已有单因素任务，请在任务中心查看进度")
                                  : error);
          }
        });
    connect(constraintPage, &ConstraintPage::runCancelRequested, this,
            [this, constraint] {
              constraint->cancelFactorTask();
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
    // #481 P3-6：解释性/分析等值线两个 handler 的任务化三段式骨架原本逐字
    // 重复（~85 行×2，dropTemp 守卫口径曾各自漂移）——提取共享 runner：
    // 可变项（任务名/失败文案/首阶段进度/compute/publish/清理路径）经参数
    // 注入；忙检与 prepare 参数各异，留在各 handler。状态机、守卫与清理
    // 顺序原样搬移（行为守恒）。
    const auto runContourTaskJob =
        [this, constraint, constraintPage](
            const QString &title, const QString &failText,
            const QString &stageText, int stagePercent,
            const QStringList &dropPaths, const QString &dropOnCancelExtra,
            const std::function<QString(PaleoTask *)> &compute,
            const std::function<bool(QString *)> &publish,
            const std::function<void(const QString &)> &fail) {
          constraintPage->setRunBusy(true);
          constraintPage->noteRunStage(stageText, stagePercent);
          PaleoTask *task = m_taskSvc->start(title, compute, QString(),
                                             PaleoTask::Priority::High, true);
          m_factorTask = task;
          connect(task, &PaleoTask::finished, this,
                  [this, constraintPage, failText, dropPaths, dropOnCancelExtra,
                   publish, fail, task] {
                    const auto clearTask = [this, task] {
                      if (m_factorTask == task)
                        m_factorTask.clear();
                    };
                    // #481 P2-1：删除语义收进 workflow——按目录名前缀判定
                    //（paleo-sf- 开头才删），路径任意位置含该串的无关目录
                    //（用户目录名撞串）不再被误删。
                    const auto dropTemp = [](const QString &path) {
                      PaleoWorkflowDiscardFactorTempTree(path);
                    };
                    if (!constraintPage)
                    {
                      for (const QString &p : dropPaths)
                        dropTemp(p);
                      clearTask();
                      return;
                    }
                    if (task->state() == PaleoTask::State::Cancelled)
                    {
                      for (const QString &p : dropPaths)
                        dropTemp(p);
                      if (!dropOnCancelExtra.isEmpty())
                        dropTemp(dropOnCancelExtra);
                      constraintPage->setRunBusy(false);
                      if (auto *statusLabel =
                              constraintPage->findChild<QLabel *>(QStringLiteral("statusLabel")))
                        statusLabel->setText(tr("已取消"));
                      clearTask();
                      return;
                    }
                    if (task->state() != PaleoTask::State::Succeeded)
                    {
                      for (const QString &p : dropPaths)
                        dropTemp(p);
                      constraintPage->setRunBusy(false);
                      fail(task->errorText().isEmpty() ? failText : task->errorText());
                      clearTask();
                      return;
                    }
                    // 发布是临界区：任务已结束，不再 requestCancel。
                    constraintPage->noteRunStage(tr("正在保存"), 95);
                    QString pubErr;
                    const bool published = publish(&pubErr);
                    constraintPage->setRunBusy(false);
                    if (!published)
                      fail(pubErr.isEmpty() ? failText : pubErr);
                    else
                      constraintPage->noteRunStage(tr("正在保存"), 100);
                    clearTask();
                  });
        };
    connect(constraintPage, &ConstraintPage::interpretiveContourRequested, this,
            [this, constraint, constraintPage,
             runContourTaskJob](const QString &factorLayerId, const QVector<double> &levels) {
              const QString horizon = factorLayerId.startsWith(QStringLiteral("factor."))
                                         ? factorLayerId.mid(QStringLiteral("factor.").size())
                                              .section(QLatin1Char('.'), 0, 0)
                                         : QString();
              auto *status = constraintPage->findChild<QLabel *>(QStringLiteral("statusLabel"));
              auto *hint = constraintPage->findChild<QLabel *>(QStringLiteral("factorContourHint"));
              const auto fail = [status, hint](const QString &msg) {
                if (status)
                  status->setText(msg);
                if (hint)
                  hint->setText(msg);
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
              if (constraint->factorBusy() ||
                  (m_factorTask && m_factorTask->running())) {
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
              runContourTaskJob(
                  tr("解释性等值线 %1").arg(horizon),
                  tr("解释性等值线生成失败"),
                  tr("正在准备"), 5,
                  {job->workPath}, job->contourPath,
                  [job](PaleoTask *running) -> QString {
                    const bool ok =
                        ConstraintWorkflow::computeInterpretiveContourJob(
                            job.get(),
                            [running] { return running->cancelRequested(); });
                    if (running->cancelRequested())
                      return QStringLiteral("已取消");
                    if (!ok)
                      return job->error.isEmpty() ? QObject::tr("解释性等值线生成失败") : job->error;
                    return QString();
                  },
                  [constraint, job](QString *err) {
                    return constraint->publishInterpretiveContourJob(*job, err);
                  },
                  fail);
            });
    connect(constraintPage, &ConstraintPage::contourRequested, this,
            [this, constraint, constraintPage,
             runContourTaskJob](const QString &factorLayerId, double interval,
                               const QString &contourMethod) {
              const QString horizon = factorLayerId.startsWith(QStringLiteral("factor."))
                                         ? factorLayerId.mid(QStringLiteral("factor.").size())
                                              .section(QLatin1Char('.'), 0, 0)
                                         : QString();
              auto *status = constraintPage->findChild<QLabel *>(QStringLiteral("statusLabel"));
              auto *hint = constraintPage->findChild<QLabel *>(QStringLiteral("factorContourHint"));
              const auto fail = [status, hint](const QString &msg) {
                if (status)
                  status->setText(msg);
                if (hint)
                  hint->setText(msg);
                QgsMessageLog::logMessage(msg, QStringLiteral("Paleo"), Qgis::MessageLevel::Warning);
              };
              if (!m_taskSvc)
              {
                constraintPage->setRunBusy(true);
                QString err;
                const bool ok = constraint->generateContours(horizon, factorLayerId, interval, &err,
                                                             contourMethod);
                constraintPage->setRunBusy(false);
                if (!ok)
                  fail(err.isEmpty() ? tr("等值线生成失败") : err);
                return;
              }
              if (constraint->factorBusy() ||
                  (m_factorTask && m_factorTask->running())) {
                fail(tr("已有单因素计算在进行"));
                return;
              }
              auto job = std::make_shared<ConstraintWorkflow::AnalysisContourJob>();
              QString prepErr;
              if (!constraint->prepareAnalysisContourJob(horizon, factorLayerId, interval, {}, false,
                                                         job.get(), &prepErr, contourMethod))
              {
                fail(prepErr.isEmpty() ? tr("等值线生成失败") : prepErr);
                return;
              }
              runContourTaskJob(
                  tr("等值线 %1").arg(horizon),
                  tr("等值线生成失败"),
                  tr("正在生成等值线"), 10,
                  {job->outputPath}, QString(),
                  [job](PaleoTask *running) -> QString {
                    const bool ok =
                        ConstraintWorkflow::computeAnalysisContourJob(
                            job.get(),
                            [running] { return running->cancelRequested(); });
                    if (running->cancelRequested())
                      return QStringLiteral("已取消");
                    if (!ok)
                      return job->error.isEmpty() ? QObject::tr("等值线生成失败") : job->error;
                    return QString();
                  },
                  [constraint, job](QString *err) {
                    return constraint->publishAnalysisContourJob(*job, err);
                  },
                  fail);
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
                // 显示走 parent-recursive：互斥下勾把组三态拨成部分/全关后，
                // 普通勾选子层不恢复祖先组——图「上了」却看不见（V3/V8 同款）。
                if (QgsLayerTreeLayer *node = treeRoot->findLayer(layer->id()))
                  node->setItemVisibilityCheckedParentRecursive(true);
              }
              else
              {
                setNodeChecked(layerId, false);
              }
              // 回写页主题：切页/换层位会按 page:* 主题回放树态；程序化拨
              // 勾选后不回写，用户切走再回来即被旧主题静默回滚（预测页
              // 显示结果同款场景已回写——此处对齐，消除不对称）。
              if (m_profileSvc)
                m_profileSvc->captureCurrentAsTheme(
                    QgisLayerProfileService::pageThemeName(currentPage()));
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
      connect(typedCtl, &TypedConstraintDrawController::captureFinished, this,
              [this](const QString &horizon, const QString &, const QString &) {
                showDrawnConstraintLines(horizon);
              });
    }
    attachConstraintMapSelection(constraintPage);
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
    // 修改已有约束时由编辑工具栏接管原生撤销会话，拒绝时带回原因。
    constraint->setDrawSessionStarter(
        [self = QPointer<QObject>(this), page = QPointer<ConstraintPage>(constraintPage), beginConstraintEdit, this](
            const QString &horizon, QString *error) {
          if (!self || !page)
            return false;
          QString refused;
          QMetaObject::Connection refusal;
          if (auto *toolbar = findChild<PaleoEditingToolbar *>(QStringLiteral("editingToolbar")))
            refusal = connect(toolbar, &PaleoEditingToolbar::editRefused, self,
                              [&refused](const QString &reason) { refused = reason; });
          const bool ok = beginConstraintEdit(horizon) != nullptr;
          disconnect(refusal);
          if (!ok && error)
            *error = refused.isEmpty() ? tr("编辑工具栏未能开启约束编辑会话") : refused;
          return ok;
        });
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
              {
                if (auto *status = constraintPage->findChild<QLabel *>(QStringLiteral("statusLabel")))
                  status->setText(tr("约束编辑不可用，请检查编辑会话与资产状态"));
                return;
              }
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
            [this, constraint, constraintPage, beginConstraintEdit](const QString &horizon, const QString &id, const QString &semantic) {
              if (!beginConstraintEdit(horizon))
              {
                if (auto *status = constraintPage->findChild<QLabel *>(QStringLiteral("statusLabel")))
                  status->setText(tr("约束编辑不可用，请检查编辑会话与资产状态"));
                return;
              }
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
      {
        if (auto *status = constraintPage->findChild<QLabel *>(QStringLiteral("statusLabel")))
          status->setText(tr("约束编辑不可用，请检查编辑会话与资产状态"));
        return;
      }
      if (layer->selectedFeatureCount() == 0)
        layer->selectAll();
      toolbar->actionVertexEdit()->trigger();
      connect(toolbar, &PaleoEditingToolbar::editingStopped, constraintPage,
              &ConstraintPage::refreshConstraintList, Qt::UniqueConnection);
    });
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
      // 异步 commit 失败（staging/复制/登记）经 errorOccurred 上状态栏，
      // 不再静默吞掉（此前该信号从未 emit、从未接线）。
      connect(m_realizationWf, &RealizationWorkflow::errorOccurred, this,
              [this](const QString &text) { statusBar()->showMessage(text, 8000); });

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
                if (!task) // L-13：busy 只提示不回退同步重跑（GUI 冻结+与在飞任务同算→同 token 双版本）
                {
                  const bool busy = m_realizationWf->isBusy();
                  if (busy || !m_realizationWf->deriveAllStatistics(setId, &err))
                    statusBar()->showMessage(busy ? tr("正在进行计算，请稍候") : err.isEmpty() ? tr("统计派生失败") : err, 8000);
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
                if (!task) // L-13：同上，busy 只提示不回退同步重跑
                {
                  const bool busy = m_realizationWf->isBusy();
                  if (busy || !m_realizationWf->differenceOfMeans(setIdA, setIdB, &err))
                    statusBar()->showMessage(busy ? tr("正在进行计算，请稍候") : err.isEmpty() ? tr("集合差值失败") : err, 8000);
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
