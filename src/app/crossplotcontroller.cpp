// 层：组装根
#include "crossplotcontroller.h"
#include "appcontext.h"
#include "io/dataimportservice.h"
#include "linkage/selectioncontext.h"
#include "qgis/crossplotmaplink.h"
#include "qgis/qgiscanvascontroller.h"
#include "qgis/qgislayerservice.h"
#include "qgis/qgisprojectservice.h"
#include "services/faciestraining.h"
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
            // 换工程：训练态整体重置（setCatalog→clear 不清训练集，这里显式
            // 记陈旧），质量区与摘要由 setDimensions/refreshTrainingState 清。
            m_trainedMethod = -1;
            m_selection = {}; // 旧样本选区行号作废（与 load/project 对齐）
            m_panel->setModelTrained(false);
            refreshTrainingState();
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
  connect(m_panel, &CrossplotPanel::assignLabelRequested, this,
          [this](const QString &className) {
            // 选区行号来源 = select() 落定的当前 Selection.indices；无选区
            // （未套索/已切轴/未读样本）时只提示，不发空标注。
            if (m_selection.indices.isEmpty()) {
              m_panel->setMessage(tr("先在图上框选样本"));
              return;
            }
            m_workflow->assignTrainingLabel(m_selection.indices, className);
          });
  connect(m_panel, &CrossplotPanel::clearTrainingRequested, this,
          [this] { m_workflow->clearTraining(); });
  connect(m_panel, &CrossplotPanel::trainRequested, this, [this] {
    ClassificationOptions o = m_panel->options();
    o.selection.clear(); // 训练用全学习集；selection 只服务约束分类
    m_workflow->train(o);
  });
  connect(m_panel, &CrossplotPanel::methodChanged, this, [this] {
    // 方法切换：训练态按新方法重算（validate 下限与方法是绑定的）；已训练态
    // 按「模型方法 == 当前选择」刷新（LDA 模型不喂 QDA/kNN 推理）。
    const int current = int(m_panel->options().method);
    const bool matches = m_trainedMethod >= 0 && m_trainedMethod == current;
    m_panel->setModelTrained(matches);
    // 质量区：切回已训练方法 → 回填该次训练的 CV 质量；否则清空（旧方法的
    // 混淆矩阵不挂到新方法上）。m_trainedMethod == -1（陈旧/已清）时一切
    // 方法都不回填——报告与模型已失配。
    m_panel->setTrainingQuality(matches ? m_lastReport : QVariantMap{});
    refreshTrainingState();
  });
  connect(m_panel, &CrossplotPanel::paramsChanged, this,
          [this] { refreshTrainingState(); });
  connect(m_workflow, &FaciesClassifyWorkflow::trainingChanged, this, [this] {
    // 标注变更 / 清空 = 模型陈旧或作废（与 workflow classify 门禁同语义：
    // trainingDirty 即要求重训）。零标注初始态也经此落下禁用原因。
    m_trainedMethod = -1;
    m_panel->setModelTrained(false);
    m_panel->setTrainingQuality({}); // 陈旧/作废模型的旧混淆矩阵不得残留
    refreshTrainingState();
  });
  connect(m_workflow, &FaciesClassifyWorkflow::trainingReady, this,
          [this](const QVariantMap &report) {
            m_trainedMethod = report.value("supervisedMethod").toInt();
            m_lastReport = report; // 方法来回切换时质量区回填的缓存
            m_panel->setTrainingQuality(report);
            m_panel->setModelTrained(true);
            m_panel->setMessage(
                tr("训练完成：%1 折交叉验证；可在同方法下推理并写回工程。")
                    .arg(report.value("folds").toInt()));
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
            // 伴生层上图决策：productReady 只 instantiate 主分类图
            // （p.layerId）；置信度 / 低置信掩膜两件已在 write() 内 declare
            // 进图层树（02_Prediction 组），交由用户按需手动开启——不替用户
            // 决定默认可见层，避免一次写回铺开三图层。
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
  refreshTrainingState(); // 首启训练态：禁用原因由控制器单一出处下发
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
void CrossplotController::refreshTrainingState() {
  // 摘要区始终与 workflow 对齐（无样本/零标注 → 「未标注」）。
  m_panel->setTrainingSummary(m_workflow->trainingSummary());
  ClassificationOptions o = m_panel->options();
  o.selection.clear(); // 校验只看训练集与样本，selection 不参与
  const auto samples = m_workflow->samples();
  if (!samples) {
    m_panel->setTrainingState(false, tr("请先读取至少两个通道"));
    return;
  }
  if (!isSupervisedClassifier(o.method)) {
    m_panel->setTrainingState(
        false, tr("请选择监督分类方法（LDA / QDA / kNN）再训练"));
    return;
  }
  // 零标注首启：labels 为空（换样本已清）时给可操作提示，不让validate 的
  // 「行数不一致」误导用户以为数据坏了。
  if (m_workflow->trainingSet().labels.empty()) {
    m_panel->setTrainingState(
        false, tr("尚无标注样本——先用套索选区并赋予类名"));
    return;
  }
  const auto precondition = FaciesTrainingService::validateTrainingSet(
      *samples, m_workflow->trainingSet(), o);
  if (precondition.ok)
    // 放行也带 tooltip：失衡/小类 warnings 摘要（按钮可用，提示不阻断）。
    m_panel->setTrainingState(
        true,
        precondition.warnings.isEmpty()
            ? QString()
            : tr("可以训练（注意：%1）")
                  .arg(precondition.warnings.join(tr("；"))));
  else
    m_panel->setTrainingState(false, precondition.error);
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
  m_selection = {};     // 旧样本的选区行号全部作废
  m_trainedMethod = -1; // workflow->clear 不清训练集，控制器显式记陈旧
  m_panel->setModelTrained(false);
  const auto generation = ++m_generation;
  auto result = std::make_shared<SampleResult>();
  m_panel->setClassified(false);
  m_panel->setDimensions({});
  m_panel->setFrame({});
  m_panel->setSelection({}, {});
  // 面板展示重置之后才刷训练态：setDimensions 会清摘要区，refreshTrainingState
  // 若先跑，「未标注」摘要会被随后的 setDimensions 抹掉（轮 3 Medium-1 同款
  // 顺序问题，起始段一并修）。
  refreshTrainingState(); // 样本已空：训练按钮禁用 + 摘要回落「未标注」
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
    // 先推面板维度、后落 workflow 样本：setSamples → clearTraining →
    // trainingChanged → refreshTrainingState 下发「未标注」摘要，若被随后
    // 的 setDimensions（清摘要区）盖掉就丢了（轮 3 Medium-1）。轮 1
    // Medium-2 的根因修（setSamples 内部先落样本再 clearTraining）不受
    // 对调影响——回调里 samples 已就绪。
    m_panel->setDimensions(samples->names);
    m_workflow->setSamples(samples);
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
  m_selection = {}; // 换轴即重投影，旧选区高亮与行号一并作废
  m_map->clear();
}
void CrossplotController::select(const QVector<QPointF> &vertices) {
  const auto samples = m_workflow->samples();
  if (!samples)
    return;
  const auto selection = CrossplotSamples::select(*samples, m_frame, vertices);
  m_selection = selection; // assignLabelRequested 的行号来源
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
