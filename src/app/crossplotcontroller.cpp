// 层：组装根
#include "crossplotcontroller.h"
#include "appcontext.h"
#include "io/dataimportservice.h"
#include "linkage/selectioncontext.h"
#include "qgis/crossplotmaplink.h"
#include "qgis/qgiscanvascontroller.h"
#include "qgis/qgislayerservice.h"
#include "qgis/qgisprojectservice.h"
#include "ui/crossplot/crossplotpanel.h"
#include "ui/paleomainwindow.h"
#include "workflow/faciesclassify.h"
#include <QAction>
#include <QFileInfo>
#include <QTabWidget>
#include <QTimer>
#include <qgslayertreeview.h>
namespace paleo::crossplot {
CrossplotController::CrossplotController(AppContext *ctx,
                                         PaleoMainWindow *window)
    : QObject(window), m_context(ctx) {
  auto *tabs = window->findChild<QTabWidget *>(QStringLiteral("bottomTabs"));
  m_panel = new CrossplotPanel(tabs);
  if (tabs)
    tabs->addTab(m_panel, tr("交会相分类"));
  m_workflow = new FaciesClassifyWorkflow(ctx->taskSvc(), ctx->store(),
                                          ctx->layerSvc(), this);
  m_map = new CrossplotMapLink(ctx->canvasCtl()->canvas(), this);
  auto *action = new QAction(tr("交会相分类"), window);
  action->setObjectName(QStringLiteral("openCrossplot"));
  action->setShortcut(QKeySequence(QStringLiteral("Ctrl+Alt+X")));
  window->addAction(action);
  connect(action, &QAction::triggered, this, [window, tabs, this] {
    if (auto *dock = window->findChild<PaleoDockWidget *>(
            QStringLiteral("bottomDock"))) {
      dock->setUserWantsVisible(true);
      dock->setProgrammaticVisible(true);
      dock->raise();
    }
    if (tabs)
      tabs->setCurrentWidget(m_panel);
    refreshSources();
  });
  if (tabs)
    connect(tabs, &QTabWidget::currentChanged, this, [this, tabs] {
      if (tabs->currentWidget() == m_panel)
        refreshSources();
    });
  auto *catalog = ctx->importSvc()->catalog();
  connect(catalog, &DataCatalog::changed, this, [this] {
    if (m_refreshPending)
      return;
    m_refreshPending = true;
    QTimer::singleShot(150, this, [this] {
      m_refreshPending = false;
      if (m_panel->isVisible())
        refreshSources();
    });
  });
  connect(ctx->projectSvc(), &QgisProjectService::projectOpened, this,
          [this, catalog](const QString &path) {
            if (m_loadTask)
              m_loadTask->requestCancel();
            ++m_generation;
            m_loadTask = nullptr;
            m_panel->setBusy(false);
            m_workflow->setCatalog(catalog, QFileInfo(path).absolutePath());
            m_frame = {};
            m_panel->setFrame(m_frame);
            m_panel->setDimensions({});
            m_panel->setClassified(false);
            m_map->clear();
            refreshSources();
          });
  connect(m_panel, &CrossplotPanel::samplesRequested, this,
          &CrossplotController::load);
  connect(m_panel, &CrossplotPanel::axesRequested, this,
          &CrossplotController::project);
  connect(m_panel, &CrossplotPanel::lassoRequested, this,
          &CrossplotController::select);
  connect(m_panel, &CrossplotPanel::pointRequested, this,
          &CrossplotController::locate);
  connect(m_panel, &CrossplotPanel::classifyRequested, m_workflow,
          &FaciesClassifyWorkflow::classify);
  connect(m_panel, &CrossplotPanel::cancelRequested, this, [this] {
    if (m_loadTask)
      m_loadTask->requestCancel();
    m_workflow->cancel();
  });
  connect(m_panel, &CrossplotPanel::writeRequested, this, [this] {
    m_workflow->write(m_context->selection()->activeHorizon());
  });
  connect(m_workflow, &FaciesClassifyWorkflow::busyChanged, m_panel,
          &CrossplotPanel::setBusy);
  connect(m_workflow, &FaciesClassifyWorkflow::progressChanged, m_panel,
          &CrossplotPanel::setProgress);
  connect(m_workflow, &FaciesClassifyWorkflow::failed, m_panel,
          &CrossplotPanel::setMessage);
  connect(m_workflow, &FaciesClassifyWorkflow::classificationReady, this,
          [this](const Classification &r) {
            m_panel->setClassified(true, r.counts);
            project(m_panel->axes());
            m_panel->setMessage(
                tr("分类完成；类别统计与置信度已计算。可写回工程。"));
          });
  connect(m_workflow, &FaciesClassifyWorkflow::productReady, this,
          [this](const FaciesProduct &p) {
            m_panel->setMessage(
                tr("分类成果已登记：%1。栅格可在智能编图中作为先验相图矢量化。")
                    .arg(p.path));
            if (!p.layerId.isEmpty()) {
              QString error;
              m_context->layerSvc()->instantiate(p.layerId, &error);
              if (!error.isEmpty())
                m_panel->setMessage(error);
            }
          });
  if (!ctx->projectSvc()->projectPath().isEmpty())
    m_workflow->setCatalog(
        catalog, QFileInfo(ctx->projectSvc()->projectPath()).absolutePath());
  refreshSources();
}
void CrossplotController::refreshSources() {
  const auto path = m_context->projectSvc()->projectPath();
  m_sources = CrossplotSources::inventory(m_context->importSvc()->catalog(),
                                          QFileInfo(path).absolutePath(),
                                          m_context->layerSvc()->declared());
  QVector<SourceChoice> choices;
  for (const auto &s : m_sources)
    choices << s.choice;
  m_panel->setSources(choices);
}
void CrossplotController::load(const QStringList &ids) {
  QVector<SourceSpec> specs;
  for (const auto &id : ids) {
    auto it =
        std::find_if(m_sources.begin(), m_sources.end(),
                     [&](const SourceSpec &s) { return s.choice.id == id; });
    if (it != m_sources.end())
      specs << *it;
  }
  if (specs.size() < 2) {
    m_panel->setMessage(
        tr("请选择至少两个通道；井曲线来自同一口井，SATR 配时间层位。"));
    return;
  }
  if (m_loadTask)
    m_loadTask->requestCancel();
  m_workflow->clear();
  m_map->clear();
  const auto generation = ++m_generation;
  auto result = std::make_shared<SampleResult>();
  m_panel->setClassified(false);
  m_panel->setDimensions({});
  m_panel->setFrame({});
  m_panel->setSelection({}, {});
  m_panel->setBusy(true);
  auto *task = m_context->taskSvc()->start(
      tr("交会样本抽取"), [specs, result](PaleoTask *t) {
        cluster::Control ctl{
            [t] { return t->cancelRequested(); },
            [t](double p) { t->reportBytes(qint64(p * 1000), 1000); }};
        *result = CrossplotSources::load(specs, ctl);
        return result->cancelled ? QString() : result->error;
      });
  m_loadTask = task;
  connect(task, &PaleoTask::changed, this, [this, task, generation] {
    if (generation == m_generation)
      m_panel->setProgress(task->percent());
  });
  connect(task, &PaleoTask::finished, this, [this, task, result, generation] {
    if (generation != m_generation)
      return;
    m_loadTask = nullptr;
    m_panel->setBusy(false);
    if (!result->ok || task->state() != PaleoTask::State::Succeeded) {
      m_panel->setMessage(
          task->state() == PaleoTask::State::Cancelled
              ? tr("抽样已取消")
              : (result->error.isEmpty() ? task->errorText() : result->error));
      return;
    }
    auto samples = std::make_shared<SampleSet>(std::move(result->samples));
    m_workflow->setSamples(samples);
    m_panel->setDimensions(samples->names);
    project(m_panel->axes());
    m_panel->setMessage(
        tr("有效样本 %1，联合缺失剔除 %2；类别是未解释的簇编号。")
            .arg(samples->rows())
            .arg(samples->rejected));
  });
}
void CrossplotController::project(const Axes &a) {
  const auto samples = m_workflow->samples();
  if (!samples)
    return;
  m_frame = CrossplotSamples::project(*samples, a,
                                      m_workflow->classification().labels);
  m_panel->setFrame(m_frame);
  m_panel->setSelection({}, samples->names);
  m_map->clear();
}
void CrossplotController::select(const QVector<QPointF> &vertices) {
  const auto samples = m_workflow->samples();
  if (!samples)
    return;
  const auto selection = CrossplotSamples::select(*samples, m_frame, vertices);
  m_panel->setSelection(selection, samples->names);
  m_context->selection()->setSelection(selection.wellIds,
                                       QStringLiteral("crossplot"));
  QString error;
  if (!m_map->highlight(*samples, selection.indices, &error) &&
      !error.isEmpty())
    m_panel->setMessage(error);
  if (!samples->sourceLayerIds.isEmpty())
    if (auto *tree = parent()->findChild<QgsLayerTreeView *>(
            QStringLiteral("layerTreeView")))
      tree->setCurrentLayer(
          m_context->layerSvc()->instantiate(samples->sourceLayerIds.first()));
}
void CrossplotController::locate(QPointF point) {
  const auto samples = m_workflow->samples();
  if (!samples)
    return;
  const int index = CrossplotSamples::nearest(m_frame, point, .02);
  if (index < 0)
    return;
  QString error;
  if (!m_map->locate(*samples, index, &error)) {
    m_panel->setMessage(error);
    return;
  }
  const auto &loc = samples->locations[index];
  if (!loc.wellId.isEmpty())
    m_context->selection()->setSelection({loc.wellId},
                                         QStringLiteral("crossplot"));
  m_panel->setMessage(
      loc.wellId.isEmpty()
          ? tr("已定位像元 %1").arg(loc.pixel)
          : tr("已定位 %1，深度 %2").arg(loc.wellId).arg(loc.depth));
}
} // namespace paleo::crossplot
