// 层：功能
#include "datagraphworkflow.h"
#include "../catalog/datacatalog.h"
#include "../services/jobrunner.h"
#include "../services/paleotaskservice.h"
#include <QCoreApplication>

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
  m_timer.setSingleShot(true); m_timer.setInterval(180);
  connect(&m_timer, &QTimer::timeout, this, &DataGraphWorkflow::launch);
  connect(m_runner, &paleo::jobs::JobRunnerBase::jobCompleted, this, [this] {
    if (m_pending) m_timer.start();
  });
}
DataGraphWorkflow::~DataGraphWorkflow() {
  m_timer.stop(); m_runner->requestCancel();
  // Worker callbacks capture values; publication callbacks use the runner's QObject context.
  delete m_runner; m_runner = nullptr;
}
void DataGraphWorkflow::setContext(DataCatalog *catalog, PaleoTaskService *tasks) {
  if (m_catalog == catalog && m_tasks == tasks) return;
  cancel(); m_catalog = catalog; m_tasks = tasks; m_index.reset(); m_cachePath.clear();
  emit ready({});
}
void DataGraphWorkflow::request(const paleo::datagraph::Query &query) {
  m_query = query; ++m_generation; m_pending = true;
  if (m_runner->busy()) m_runner->requestCancel();
  emit statusChanged(tr("正在准备图谱查询"), true);
  m_timer.start();
}
void DataGraphWorkflow::setPresentation(const paleo::datagraph::Presentation &presentation) {
  if (m_presentation == presentation) return;
  m_presentation = presentation; m_index.reset(); ++m_generation;
  if (m_runner->busy()) m_runner->requestCancel();
}
void DataGraphWorkflow::cancel() {
  ++m_generation; m_pending = false; m_timer.stop(); m_runner->requestCancel();
  emit statusChanged(tr("图谱任务已取消"), false);
}
void DataGraphWorkflow::launch() {
  if (!m_pending || m_runner->busy()) return;
  m_pending = false;
  if (!m_catalog || !m_catalog->isOpen()) { emit ready({}); emit statusChanged(tr("请先打开工区"), false); return; }
  m_runner->setTaskService(m_tasks ? m_tasks.data() : m_fallback);
  auto job = std::make_shared<Job>();
  job->query = m_query; job->owner = m_catalog; job->path = m_catalog->catalogPath();
  job->presentation = m_presentation;
  job->sequence = m_catalog->mutationSeq(); job->generation = m_generation;
  if (m_index && job->path == m_cachePath && job->sequence == m_cacheSequence) job->index = m_index;
  else job->snapshot = std::shared_ptr<DataCatalog>(m_catalog->createStagingCopy(QString()));
  using Runner = paleo::jobs::JobRunner<Job>;
  Runner::Callbacks callbacks;
  callbacks.compute = [](Job &job, const paleo::jobs::CancelFn &cancel, const paleo::jobs::ProgressFn &progress) {
    if (!job.index) {
      if (!job.snapshot) { job.error = QCoreApplication::translate("DataGraphWorkflow", "无法取得资料目录快照"); return false; }
      job.index = paleo::datagraph::buildIndex(*job.snapshot, job.presentation, cancel, progress);
    }
    if (cancel()) return false;
    if (!job.index) { job.error = QCoreApplication::translate("DataGraphWorkflow", "图谱索引构建失败"); return false; }
    job.graph = paleo::datagraph::query(*job.index, job.query, cancel, progress);
    return !cancel();
  };
  callbacks.commit = [this](Job &job, QString *error) {
    if (job.generation != m_generation || !job.owner || job.owner != m_catalog) return true;
    if (job.path != m_catalog->catalogPath() || job.sequence != m_catalog->mutationSeq()) {
      m_pending = true; emit statusChanged(tr("资料已更新，正在重建图谱"), true); return true;
    }
    if (!job.error.isEmpty()) { *error = job.error; emit statusChanged(tr("图谱失败：%1").arg(job.error), false); return false; }
    m_index = job.index; m_cachePath = job.path; m_cacheSequence = job.sequence;
    emit ready(job.graph); emit statusChanged(tr("图谱就绪"), false); return true;
  };
  callbacks.onDropped = [this](Job &job, paleo::jobs::DropReason) {
    if (job.generation == m_generation && !m_pending) emit statusChanged(tr("图谱任务已取消"), false);
  };
  emit statusChanged(tr("正在构建资料图谱，可在任务中心查看或取消"), true);
  if (!m_runner->start(tr("资料图谱：关系查询与布局"), job, callbacks)) emit statusChanged(tr("无法启动图谱任务"), false);
}
