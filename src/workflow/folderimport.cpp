// 层：功能
#include "folderimport.h"

#include "../io/dataimportservice.h"
#include "../catalog/datacatalog.h"
#include "../services/paleotaskservice.h"
#include "importledger.h"

#include <QFileInfo>
#include <QPointer>
#include <QFile>
#include "domain/projectclassifier.h"

// 审计 02 M-8：一次异步导入作业。produce 在 worker 上只碰 session；finish
// 在 GUI 线程拿提交后的 session（行/结果已按提交结局改写）回壳。
struct FolderImportWorkflow::ImportJob
{
  QString title;
  std::function<void(ImportSession &, PaleoTask *)> produce;
  std::function<void(ImportSession &)> finish;
  std::function<void()> fail; // beginImport 不可用（服务未就绪等）
  int attempts = 0;
};

FolderImportWorkflow::FolderImportWorkflow(DataImportService *svc,
                                           PaleoTaskService *taskSvc,
                                           QObject *parent)
    : QObject(parent), m_svc(svc), m_taskSvc(taskSvc)
{
}

int FolderImportWorkflow::pendingImportJobs() const
{
  return static_cast<int>(m_importQueue.size()) + (m_activeImport ? 1 : 0);
}

QString FolderImportWorkflow::classifyFile(const QString &path) const
{
  QFile file(path);
  const QByteArray prefix = file.open(QIODevice::ReadOnly) ? file.read(65536) : QByteArray();
  return classifyProjectImport(path, prefix).type;
}

void FolderImportWorkflow::enqueueImport(const std::shared_ptr<ImportJob> &job)
{
  m_importQueue.append(job);
  pumpImports();
}

void FolderImportWorkflow::pumpImports()
{
  if (m_activeImport || m_importQueue.isEmpty())
    return;
  m_activeImport = m_importQueue.takeFirst();
  runImportJob(m_activeImport);
}

void FolderImportWorkflow::runImportJob(const std::shared_ptr<ImportJob> &job)
{
  ++job->attempts;
  std::shared_ptr<ImportSession> session = m_svc ? m_svc->beginImport() : nullptr;
  if (!session || !m_taskSvc)
  {
    m_activeImport.reset();
    if (job->fail)
      job->fail();
    pumpImports();
    return;
  }
  PaleoTask *task = m_taskSvc->start(
      job->title, [session, produce = job->produce](PaleoTask *t) -> QString {
        produce(*session, t); // worker：只写 session，活 catalog 零接触
        return session->error;
      });
  QPointer<DataImportService> svc = m_svc;
  connect(task, &PaleoTask::finished, this, [this, job, session, task, svc] {
    if (!svc)
    {
      m_activeImport.reset();
      session->error = QStringLiteral("导入服务已关闭，导入结果已丢弃");
      job->finish(*session);
      pumpImports();
      return;
    }
    // 取消过的作业不重做（用户已叫停）：冲突按失败落行。
    const bool mayRetry =
        job->attempts < kMaxImportAttempts && !task->cancelRequested();
    QString cerr;
    const DataImportService::CommitStatus st =
        svc->commitImport(*session, &cerr, /*allowConflict=*/mayRetry);
    if (st == DataImportService::CommitStatus::Conflict)
    {
      qInfo("FolderImportWorkflow: catalog changed during import, re-producing "
            "(attempt %d/%d)",
            job->attempts + 1, kMaxImportAttempts);
      runImportJob(job); // 新 session、新基线；旧 session 析构即删暂存
      return;
    }
    m_activeImport.reset();
    job->finish(*session);
    pumpImports();
  });
}

QVector<FolderPreviewRow>
FolderImportWorkflow::previewFolder(const QString &dir, QString *error)
{
  if (!m_svc)
  {
    if (error)
      *error = QStringLiteral("导入服务未就绪");
    return {};
  }
  return m_svc->previewFolder(dir, error);
}

void FolderImportWorkflow::previewFolderAsync(
    const QString &dir,
    const std::function<void(const QVector<FolderPreviewRow> &,
                             const QString &)> &done)
{
  if (!m_svc)
  {
    if (done)
      done({}, QStringLiteral("导入服务未就绪"));
    return;
  }
  // T2：任务池在场 → 扫描/分类/哈希跑 worker（plan 经 COW 快照构建，
  // catalog 不被 worker 触碰），扫描进度回报任务页；GUI 零同步整目录扫描。
  // 无任务服务 → 同步旧路径（小环境/测试行为不变）。
  if (m_taskSvc)
  {
    // 审计 02 M-8：预览也只读 session 的 staging 副本（GUI 上拷出）。
    std::shared_ptr<ImportSession> session = m_svc->beginImport();
    if (!session)
    {
      if (done)
        done({}, DataImportService::offThreadError());
      return;
    }
    emit previewActiveChanged(true);
    auto outRows = std::make_shared<QVector<FolderPreviewRow>>();
    auto outErr = std::make_shared<QString>();
    PaleoTask *task = m_taskSvc->start(
        tr("扫描工区文件夹"),
        [session, dir, outRows, outErr](PaleoTask *t) -> QString {
          *outRows = DataImportService::producePreview(
              *session, dir, outErr.get(),
              [t](int seen, const QString &path) {
                t->reportDetail(tr("扫描 %1").arg(QFileInfo(path).fileName()));
                return !t->cancelRequested();
              });
          return outErr->isEmpty() ? QString() : *outErr;
        });
    connect(task, &PaleoTask::finished, this,
            [this, done, outRows, outErr] {
              emit previewActiveChanged(false);
              if (done)
                done(*outRows, *outErr);
            });
    return;
  }
  QString err;
  const QVector<FolderPreviewRow> rows = m_svc->previewFolder(dir, &err);
  if (done)
    done(rows, err);
}

void FolderImportWorkflow::recordLedger(const QString &dir, const QDateTime &started,
                                        const QVector<FolderRowResult> &rows,
                                        const QString &importErr)
{
  DataCatalog *cat = m_svc ? m_svc->catalog() : nullptr;
  if (!cat || !cat->isOpen())
    return;
  using namespace paleo::imports;
  ImportLedger ledger;
  ledger.load(cat);
  LedgerBatch batch;
  batch.id = QStringLiteral("batch-%1").arg(started.toMSecsSinceEpoch());
  batch.dir = dir;
  batch.startedAt = started;
  batch.finishedAt = QDateTime::currentDateTime();
  batch.error = importErr;
  for (const FolderRowResult &r : rows)
  {
    LedgerRow lr;
    lr.path = r.path;
    lr.type = r.classifiedType;
    lr.entity = r.entityName;
    lr.message = r.message;
    switch (r.outcome)
    {
    case FolderRowResult::Outcome::Imported:
      lr.outcome = QStringLiteral("imported");
      ++batch.imported;
      break;
    case FolderRowResult::Outcome::Unresolved:
      lr.outcome = QStringLiteral("unresolved");
      ++batch.unresolved;
      break;
    case FolderRowResult::Outcome::Failed:
      lr.outcome = QStringLiteral("failed");
      ++batch.failed;
      break;
    case FolderRowResult::Outcome::Skipped:
      lr.outcome = QStringLiteral("skipped");
      ++batch.skipped;
      break;
    }
    batch.rows.append(lr);
  }
  QString lerr;
  if (!ledger.record(batch, &lerr))
    qWarning() << "import ledger record failed:" << lerr;
}

FolderRowResult
FolderImportWorkflow::importFolderRow(const QString &path, const QString &forceType,
                                      QString *error)
{
  if (!m_svc)
  {
    if (error)
      *error = QStringLiteral("导入服务未就绪");
    return {};
  }
  emit importActiveChanged(true);
  const FolderRowResult res = m_svc->importFolderRow(path, forceType, error);
  emit importActiveChanged(false);
  return res;
}

void FolderImportWorkflow::importFolder(const QString &dir,
                                        const QMap<QString, QString> &overrides,
                                        const ImportDone &done)
{
  importFolder(dir, overrides, QStringList{}, done);
}

void FolderImportWorkflow::importFolder(const QString &dir,
                                        const QMap<QString, QString> &overrides,
                                        const QStringList &forceImportPaths,
                                        const ImportDone &done)
{
  if (!m_svc)
  {
    if (done)
      done({}, QStringLiteral("导入服务未就绪"));
    return;
  }
  // D1b+T2+审计 02 M-8：任务池在场 → 整个文件夹导入（plan 扫描/哈希 +
  // LAS 解析/层位装箱/SEG-Y 索引）在 worker 上 produce 进 session 的
  // staging 副本；finished（GUI）里 commitImport 一次事务入库，随后按原序
  // 发 layerDeclared/imported/importFailed，再 importActiveChanged(false)
  // ——抑制逐文件标签的标志在信号之后复位，不会漏开逐文件标签。进度口径：
  // 扫描段 total==0（跑马灯 + 文件名 detail），执行段 total=行数。
  if (m_taskSvc)
  {
    const QDateTime started = QDateTime::currentDateTime();
    auto job = std::make_shared<ImportJob>();
    job->title = tr("导入工区文件夹");
    job->produce = [dir, overrides, forceImportPaths](ImportSession &s, PaleoTask *t) {
          DataImportService::produceFolder(
              s, dir, overrides, forceImportPaths,
              [t](int d, int total, const QString &p) {
                if (total > 0)
                {
                  t->reportBytes(d, total); // 执行段：行进度
                  t->reportDetail(p);
                }
                else
                {
                  t->reportDetail(tr("扫描 %1").arg(QFileInfo(p).fileName()));
                }
                return !t->cancelRequested();
              });
        };
    job->finish = [this, done, dir, started](ImportSession &s) {
      emit importActiveChanged(false);
      recordLedger(dir, started, s.rows, s.error);
      if (done)
        done(s.rows, s.error);
    };
    job->fail = [this, done] {
      emit importActiveChanged(false);
      if (done)
        done({}, DataImportService::offThreadError());
    };
    emit importActiveChanged(true);
    enqueueImport(job);
    return;
  }
  const QDateTime started = QDateTime::currentDateTime();
  emit importActiveChanged(true);
  QString importErr;
  const auto res = m_svc->importFolder(dir, &importErr, overrides, forceImportPaths);
  emit importActiveChanged(false);
  recordLedger(dir, started, res, importErr);
  if (done)
    done(res, importErr);
}

void FolderImportWorkflow::importFile(
    const QString &kind, const QString &path,
    std::function<void(const QString &assetId, const QString &error)> done)
{
  if (!m_svc)
  {
    if (done)
      done(QString(), QStringLiteral("导入服务未就绪"));
    return;
  }
  // D1b：LAS 解析/SEG-Y 索引等大文件在任务池 produce——无任务服务时保持
  // 同步旧路径。imported 信号在 GUI 提交后发（预览标签在终态后开）。
  if (m_taskSvc)
  {
    auto job = std::make_shared<ImportJob>();
    job->title = tr("导入 %1").arg(kind);
    job->produce = [path, kind](ImportSession &s, PaleoTask *) {
      DataImportService::ImportOptions options;
      if (isClassifierType(kind))
        options.forceType = kind;
      DataImportService::produceFile(s, path, options);
      if (!s.fileResult.assetId.isEmpty())
        s.error.clear(); // 任务态口径同旧：拿到资产 id 即成功
    };
    job->finish = [done](ImportSession &s) {
      if (done)
        done(s.fileResult.assetId, s.fileResult.assetId.isEmpty() ? s.error : QString());
    };
    job->fail = [done] {
      if (done)
        done(QString(), DataImportService::offThreadError());
    };
    enqueueImport(job);
    return;
  }
  QString err;
  DataImportService::ImportOptions options;
  if (isClassifierType(kind))
    options.forceType = kind;
  const QString assetId = m_svc->importProjectFile(path, options, &err);
  if (done)
    done(assetId, err);
}

QString FolderImportWorkflow::importedWellHeadAsset(const QString &rowPath) const
{
  // 找回刚入库的井口资产：按文件名在 catalog 里定位。
  DataCatalog *cat = m_svc ? m_svc->catalog() : nullptr;
  if (!cat)
    return {};
  const QString fileName = QFileInfo(rowPath).fileName();
  for (const auto &a : cat->assets())
    if (a.type == QLatin1String("well_head") &&
        cat->currentVersion(a.id).fileName == fileName)
      return a.id;
  return {};
}
