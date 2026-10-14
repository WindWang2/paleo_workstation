// 层：功能
#include "datagraphworkflow.h"
#include "../catalog/datacatalog.h"
#include "../services/jobrunner.h"
#include "../services/paleotaskservice.h"
#include <QCoreApplication>
#include <QDateTime>
#include <QJsonDocument>
#include <QSaveFile>
#include <algorithm>

struct DataGraphWorkflow::Job {
  std::shared_ptr<DataCatalog> snapshot;
  std::shared_ptr<const paleo::datagraph::Index> index;
  paleo::datagraph::Query query;
  paleo::datagraph::Presentation presentation;
  paleo::datagraph::Graph graph;
  QPointer<DataCatalog> owner;
  QString path, error;
  quint64 sequence = 0, generation = 0;
};
DataGraphWorkflow::DataGraphWorkflow(QObject *parent) : QObject(parent) {
  m_fallback = new PaleoTaskService(nullptr, this);
  m_runner = new paleo::jobs::JobRunner<Job>(this);
  m_runner->setParent(this);
  m_timer.setSingleShot(true);
  m_timer.setInterval(180);
  connect(&m_timer, &QTimer::timeout, this, &DataGraphWorkflow::launch);
  connect(m_runner, &paleo::jobs::JobRunnerBase::jobCompleted, this, [this] {
    if (m_pending)
      m_timer.start();
  });
}
DataGraphWorkflow::~DataGraphWorkflow() {
  m_timer.stop();
  m_runner->requestCancel();
  for (const auto &task : m_exports)
    if (task && task->running())
      task->requestCancel();
  // Worker callbacks capture values; publication callbacks use the runner's
  // QObject context.
  delete m_runner;
  m_runner = nullptr;
}
void DataGraphWorkflow::setContext(DataCatalog *catalog,
                                   PaleoTaskService *tasks) {
  if (m_catalog == catalog && m_tasks == tasks)
    return;
  cancel();
  for (const auto &task : m_exports)
    if (task && task->running())
      task->requestCancel();
  m_catalog = catalog;
  m_tasks = tasks;
  m_index.reset();
  m_cachePath.clear();
  m_graphCache.clear();
  emit ready({});
}
void DataGraphWorkflow::request(const paleo::datagraph::Query &query) {
  m_query = query;
  ++m_generation;
  m_pending = true;
  if (m_runner->busy())
    m_runner->requestCancel();
  emit statusChanged(tr("正在准备图谱查询"), true);
  m_timer.start();
}
void DataGraphWorkflow::setPresentation(
    const paleo::datagraph::Presentation &presentation) {
  if (m_presentation == presentation)
    return;
  m_presentation = presentation;
  m_index.reset();
  m_graphCache.clear();
  ++m_generation;
  if (m_runner->busy())
    m_runner->requestCancel();
}
void DataGraphWorkflow::cancel() {
  ++m_generation;
  m_pending = false;
  m_timer.stop();
  m_runner->requestCancel();
  emit statusChanged(tr("图谱任务已取消"), false);
}
void DataGraphWorkflow::launch() {
  if (!m_pending || m_runner->busy())
    return;
  m_pending = false;
  if (!m_catalog || !m_catalog->isOpen()) {
    emit ready({});
    emit statusChanged(tr("请先打开工区"), false);
    return;
  }
  if (m_catalog->catalogPath() != m_cachePath ||
      m_catalog->mutationSeq() != m_cacheSequence)
    m_graphCache.clear();
  for (int i = 0; i < m_graphCache.size(); ++i)
    if (m_graphCache[i].first == m_query) {
      // LRU：命中条目移到尾部，淘汰始终从最久未用的一端发生。
      const auto entry = m_graphCache.takeAt(i);
      m_graphCache.append(entry);
      emit ready(entry.second);
      emit statusChanged(tr("图谱就绪（复用当前目录缓存）"), false);
      return;
    }
  m_runner->setTaskService(m_tasks ? m_tasks.data() : m_fallback);
  auto job = std::make_shared<Job>();
  job->query = m_query;
  job->owner = m_catalog;
  job->path = m_catalog->catalogPath();
  job->presentation = m_presentation;
  job->sequence = m_catalog->mutationSeq();
  job->generation = m_generation;
  if (m_index && job->path == m_cachePath && job->sequence == m_cacheSequence)
    job->index = m_index;
  else
    job->snapshot =
        std::shared_ptr<DataCatalog>(m_catalog->createStagingCopy(QString()));
  using Runner = paleo::jobs::JobRunner<Job>;
  Runner::Callbacks callbacks;
  callbacks.compute = [](Job &job, const paleo::jobs::CancelFn &cancel,
                         const paleo::jobs::ProgressFn &progress) {
    if (!job.index) {
      if (!job.snapshot) {
        job.error = QCoreApplication::translate("DataGraphWorkflow",
                                                "无法取得资料目录快照");
        return false;
      }
      job.index = paleo::datagraph::buildIndex(*job.snapshot, job.presentation,
                                               cancel, progress);
    }
    if (cancel())
      return false;
    if (!job.index) {
      job.error =
          QCoreApplication::translate("DataGraphWorkflow", "图谱索引构建失败");
      return false;
    }
    job.graph =
        paleo::datagraph::query(*job.index, job.query, cancel, progress);
    return !cancel();
  };
  callbacks.commit = [this](Job &job, QString *error) {
    if (job.generation != m_generation || !job.owner || job.owner != m_catalog)
      return true;
    if (job.path != m_catalog->catalogPath() ||
        job.sequence != m_catalog->mutationSeq()) {
      m_pending = true;
      emit statusChanged(tr("资料已更新，正在重建图谱"), true);
      return true;
    }
    if (!job.error.isEmpty()) {
      *error = job.error;
      emit statusChanged(tr("图谱失败：%1").arg(job.error), false);
      return false;
    }
    m_index = job.index;
    m_cachePath = job.path;
    m_cacheSequence = job.sequence;
    m_graphCache.append({job.query, job.graph});
    if (m_graphCache.size() > 8)
      m_graphCache.removeFirst();
    emit ready(job.graph);
    emit statusChanged(tr("图谱就绪"), false);
    return true;
  };
  callbacks.onDropped = [this](Job &job, paleo::jobs::DropReason) {
    if (job.generation == m_generation && !m_pending)
      emit statusChanged(tr("图谱任务已取消"), false);
  };
  emit statusChanged(tr("正在构建资料图谱，可在任务中心查看或取消"), true);
  if (auto *task =
          m_runner->start(tr("资料图谱：关系查询与布局"), job, callbacks))
    task->setNavigationContext(
        {{"catalog_path", job->path}, {"graph_focus", job->query.focus}});
  else
    emit statusChanged(tr("无法启动图谱任务"), false);
}

void DataGraphWorkflow::exportGraph(const paleo::datagraph::Graph &graph,
                                    const QString &path) {
  if (path.isEmpty() || graph.nodes.isEmpty() || !m_catalog ||
      !m_catalog->isOpen())
    return;
  auto *service = m_tasks ? m_tasks.data() : m_fallback;
  const QString catalogPath = m_catalog->catalogPath();
  // 元数据与图内容同源：导出描述的是产出该 Graph 的查询，而不是工作流
  // 当前待算的新查询（180ms 合并窗口内两者可能不同）。
  const QString focus = graph.query.focus;
  const auto query = graph.query;
  const quint64 session = service->session();
  const QPointer<DataCatalog> owner = m_catalog;
  auto *task =
      service->start(tr("导出资料图谱与生成证据"), [graph, path, catalogPath,
                                                    query](PaleoTask *task) {
        task->reportStage(QStringLiteral("encode"), 5);
        const QStringList nodeKinds{"entity",   "asset", "version",
                                    "activity", "group", "missing", "evidence", "assertion", "interpretation", "feature"};
        const QStringList relations{"linked",   "has_version", "derived",
                                    "used",     "generated",   "edit_baseline",
                                    "rollback", "grouped", "supported_by", "contradicted_by", "constrained_by", "references", "informs", "materializes"};
        const QStringList modes{"overview", "relations", "lineage",
                                "entities", "impact"};
        QVariantList nodes, edges;
        for (const auto &n : graph.nodes) {
          if (task->cancelRequested())
            return QString();
          QVariantMap node{{"id", n.id},
                           {"title", n.title},
                           {"detail", n.detail},
                           {"entity_id", n.entityId},
                           {"asset_id", n.assetId},
                           {"version_id", n.versionId},
                           {"categories", n.categories},
                           {"stage", n.stage},
                           {"path", n.path},
                           {"source_uri", n.sourceUri},
                           {"sha256", n.sha256},
                           {"kind", nodeKinds.value(int(n.kind))},
                           {"current", n.current},
                           {"external", n.external},
                           {"stale", n.stale},
                           {"unresolved", n.unresolved},
                           {"cycle", n.cycle},
                           {"feature_uid", n.featureUid}, {"evidence_id", n.evidenceId},
                           {"assertion_id", n.assertionId}, {"interpretation_id", n.interpretationId},
                           {"horizon", n.horizon}, {"layer_id", n.layerId}, {"dependency_depth", n.dependencyDepth},
                           {"evidence", n.evidence}};
          nodes.append(node);
        }
        for (const auto &e : graph.edges) {
          if (task->cancelRequested())
            return QString();
          edges.append(
              QVariantMap{{"source", e.source},
                          {"target", e.target},
                          {"label", e.label},
                          {"relation", relations.value(int(e.relation))},
                          {"unresolved", e.unresolved}});
        }
        QVariantMap document{
            {"schema", QStringLiteral("paleo_data_graph_v1")},
            {"catalog_path", catalogPath},
            {"exported_at",
             QDateTime::currentDateTimeUtc().toString(Qt::ISODate)},
            {"scope", QStringLiteral("visible_subgraph")},
            {"query", QVariantMap{{"focus", query.focus},
                                  {"mode", modes.value(int(query.mode))},
                                  {"category", query.category},
                                  {"stage", query.stage},
                                  {"search", query.search},
                                  {"history", query.history},
                                  {"depth", query.depth}, {"horizon", query.horizon},
                                  {"evidence_type", query.evidenceType}, {"review_state", query.reviewState},
                                  {"evidence_offset", query.evidenceOffset}, {"evidence_limit", query.evidenceLimit},
                                  {"max_traversal_edges", query.maxTraversalEdges}}},
            {"nodes", nodes},
            {"edges", edges},
            {"total_assets", graph.totalAssets},
            {"total_versions", graph.totalVersions},
            {"total_entities", graph.totalEntities},
            {"hidden_nodes", graph.hidden},
            {"hidden_edges", graph.hiddenEdges},
            {"work_truncated", graph.workTruncated}, {"examined_edges", graph.examinedEdges},
            {"message", graph.message}};
        const auto bytes = QJsonDocument::fromVariant(document).toJson(
            QJsonDocument::Indented);
        if (task->cancelRequested())
          return QString();
        task->reportStage(QStringLiteral("encode"), 75);
        QSaveFile file(path);
        if (!file.open(QIODevice::WriteOnly))
          return file.errorString();
        if (file.write(bytes) != bytes.size())
          return file.errorString();
        if (task->cancelRequested()) {
          file.cancelWriting();
          return QString();
        }
        if (!file.commit())
          return file.errorString();
        task->reportStage(QStringLiteral("publish"), 100);
        return QString();
      });
  if (!task) {
    emit exportFinished(path, tr("无法启动导出任务"));
    return;
  }
  task->setNavigationContext(
      {{"catalog_path", catalogPath}, {"graph_focus", focus}});
  m_exports.erase(
      std::remove_if(m_exports.begin(), m_exports.end(),
                     [](const auto &t) { return !t || !t->running(); }),
      m_exports.end());
  m_exports.append(task);
  connect(task, &PaleoTask::finished, this,
          [this, task = QPointer<PaleoTask>(task),
           service = QPointer<PaleoTaskService>(service), session, owner,
           path] {
            // 回执只在工作流仍属于导出发起时的目录上下文时投递：
            // 新查询不吞导出回执，换工区（owner 已换/亡）才丢弃。
            if (!task || !service || session != service->session() ||
                !owner || owner != m_catalog)
              return;
            emit exportFinished(
                path, task->state() == PaleoTask::State::Succeeded ? QString()
                      : task->state() == PaleoTask::State::Cancelled
                          ? tr("导出已取消")
                          : task->errorText());
          });
}
