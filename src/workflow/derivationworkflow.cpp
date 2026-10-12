// 层：功能
#include "derivationworkflow.h"
#include "../catalog/datacatalog.h"
#include "../services/jobrunner.h"
#include <QCoreApplication>
struct DerivationWorkflow::Job {
  std::shared_ptr<DataCatalog> snapshot;
  QPointer<DataCatalog> owner;
  QString path, error;
  quint64 generation, sequence;
  paleo::derivation::Query query;
  paleo::derivation::Graph graph;
};
DerivationWorkflow::DerivationWorkflow(QObject *parent) : QObject(parent) {
  m_fallback = new PaleoTaskService(nullptr, this);
  m_runner = new paleo::jobs::JobRunner<Job>(this);
  m_runner->setParent(this);
  m_timer.setSingleShot(true);
  m_timer.setInterval(180);
  connect(&m_timer, &QTimer::timeout, this, &DerivationWorkflow::launch);
  connect(m_runner, &paleo::jobs::JobRunnerBase::jobCompleted, this, [this] {
    if (m_pending)
      m_timer.start();
  });
}
DerivationWorkflow::~DerivationWorkflow() {
  cancel();
  delete m_runner;
}
void DerivationWorkflow::cancel() {
  ++m_generation;
  m_pending = false;
  m_timer.stop();
  m_runner->requestCancel();
}
void DerivationWorkflow::request(DataCatalog *catalog, PaleoTaskService *tasks,
                                 const paleo::derivation::Query &query) {
  if (catalog && catalog == m_catalog && tasks == m_tasks && query == m_query &&
      catalog->catalogPath() == m_requestedPath &&
      catalog->mutationSeq() == m_requestedSequence &&
      (m_pending || m_runner->busy()))
    return;
  m_requestedPath = catalog ? catalog->catalogPath() : QString();
  m_requestedSequence = catalog ? catalog->mutationSeq() : 0;
  ++m_generation;
  m_catalog = catalog;
  m_tasks = tasks;
  m_query = query;
  m_pending = true;
  m_runner->requestCancel();
  m_timer.start();
}
void DerivationWorkflow::launch() {
  if (!m_pending || m_runner->busy())
    return;
  m_pending = false;
  if (!m_catalog || !m_catalog->isOpen()) {
    emit ready({});
    return;
  }
  auto job = std::make_shared<Job>();
  job->owner = m_catalog;
  job->path = m_catalog->catalogPath();
  job->generation = m_generation;
  job->sequence = m_catalog->mutationSeq();
  job->query = m_query;
  job->snapshot =
      std::shared_ptr<DataCatalog>(m_catalog->createStagingCopy({}));
  m_runner->setTaskService(m_tasks ? m_tasks.data() : m_fallback);
  using Runner = paleo::jobs::JobRunner<Job>;
  Runner::Callbacks cb;
  cb.compute = [](Job &j, const paleo::jobs::CancelFn &cancel,
                  const paleo::jobs::ProgressFn &progress) {
    if (!j.snapshot) {
      j.error = QCoreApplication::translate("DerivationWorkflow",
                                            "无法取得资料目录快照");
      return false;
    }
    progress(5, QStringLiteral("build"));
    j.graph =
        paleo::derivation::Service::build(j.snapshot.get(), j.query, cancel);
    progress(100, QStringLiteral("build"));
    return !cancel();
  };
  cb.commit = [this](Job &j, QString *error) {
    if (j.generation != m_generation || !j.owner || j.owner != m_catalog)
      return true;
    if (j.path != m_catalog->catalogPath() ||
        j.sequence != m_catalog->mutationSeq()) {
      m_pending = true;
      return true;
    }
    if (!j.error.isEmpty()) {
      *error = j.error;
      paleo::derivation::Graph failed;
      failed.message = j.error;
      emit ready(failed);
      return false;
    }
    emit ready(j.graph);
    return true;
  };
  cb.onDropped = [this](Job &j, paleo::jobs::DropReason) {
    if (j.generation != m_generation || m_pending)
      return;
    paleo::derivation::Graph cancelled;
    cancelled.available = true;
    cancelled.message = tr("血缘查询已取消；调整筛选或重新选择资料可再次查询");
    emit ready(cancelled);
  };
  if (auto *task =
          m_runner->start(tr("数据属性：成果血缘查询"), job, cb, {}, true))
    task->setNavigationContext(
        {{"catalog_path", job->path},
         {"graph_focus", !m_query.versionId.isEmpty()
                             ? QStringLiteral("version:") + m_query.versionId
                         : !m_query.assetId.isEmpty()
                             ? QStringLiteral("asset:") + m_query.assetId
                             : QStringLiteral("entity:") + m_query.entityId}});
  else {
    paleo::derivation::Graph failed;
    failed.available = true;
    failed.message = tr("无法启动血缘查询");
    emit ready(failed);
  }
}
