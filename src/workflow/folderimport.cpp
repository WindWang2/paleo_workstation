// 层：功能
#include "folderimport.h"

#include "../io/dataimportservice.h"
#include "../catalog/datacatalog.h"
#include "../services/paleotaskservice.h"
#include "importledger.h"

#include <QDir>
#include <QFileInfo>
#include <QPointer>
#include <QFile>
#include "domain/projectclassifier.h"

namespace
{
// （#560）台账入账前的「同一工程」判定（importFolder/importFiles 的
// finish 共用）：session.projectDir 与当前 catalog 所在工程目录先比词法
// cleanPath（setProjectDir 可带预开 catalog，其目录字符串形态未必与
// session.projectDir 一致——不能因写法不同把正常批次漏记），不等再比
// canonicalFilePath（符号链接/大小写别名；catDir 侧 canonical 为空即不认）。
bool sameProjectSession(DataCatalog *cat, const ImportSession &s)
{
  if (!cat || s.projectDir.isEmpty())
    return false;
  const QString catDir =
      QDir::cleanPath(cat->catalogPath() + QStringLiteral("/../../.."));
  const QString sessionDir = QDir::cleanPath(s.projectDir);
  if (catDir == sessionDir)
    return true;
  const QString a = QFileInfo(catDir).canonicalFilePath();
  return !a.isEmpty() && a == QFileInfo(sessionDir).canonicalFilePath();
}
} // namespace

// 审计 02 M-8：一次异步导入作业。produce 在 worker 上只碰 session；finish
// 在 GUI 线程拿提交后的 session（行/结果已按提交结局改写）回壳。
struct FolderImportWorkflow::ImportJob
{
  QString title;
  std::function<void(ImportSession &, PaleoTask *)> produce;
  std::function<void(ImportSession &)> finish;
  std::function<void()> fail; // beginImport 不可用（服务未就绪等）
  int attempts = 0;
  // 单文件作业置位：用户取消即不提交（对齐 dataops 队列「取消的作业不提交」
  // 语义，session 析构删暂存）。文件夹作业保持「已入库的行保留」口径不变。
  bool discardOnCancel = false;
};

FolderImportWorkflow::FolderImportWorkflow(DataImportService *svc,
                                           PaleoTaskService *taskSvc,
                                           QObject *parent)
    : QObject(parent), m_svc(svc), m_taskSvc(taskSvc)
{
}

DataImportService *FolderImportWorkflow::importService() const
{
  return m_svc.data();
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
      // 未提交即丢弃：produce 期在 staging 上记成「已入库/待关联」的行一律
      // 改报失败——回调方（确认表/台账）不能看到「入库」而库里并没有。
      for (FolderRowResult &row : session->rows)
        if (row.outcome == FolderRowResult::Outcome::Imported ||
            row.outcome == FolderRowResult::Outcome::Unresolved)
        {
          row.outcome = FolderRowResult::Outcome::Failed;
          row.message = session->error;
        }
      session->fileResult.assetId.clear();
      job->finish(*session);
      pumpImports();
      return;
    }
    // 取消过的作业不重做（用户已叫停）：冲突按失败落行。
    const bool mayRetry =
        job->attempts < kMaxImportAttempts && !task->cancelRequested();
    // 单文件作业取消即不提交：produce 对取消无检查点（整文件原子完成），
    // 用户在任务页叫停后把成品悄悄入库违背取消承诺——session 析构删暂存，
    // 行/回调如实报「已取消」。文件夹作业不在此列（行级口径另行保留）。
    if (job->discardOnCancel && task->cancelRequested() && !session->committed)
    {
      // produce 本就失败（坏文件）时保留真实失败原因，只有「成品被叫停」
      // 才报取消。取舍：produce 期记入 events 的 ImportFailed 不补发——
      // 用户语境是「取消」而非「失败」，行结局经 finish 回调如实可达。
      if (!session->fileResult.assetId.isEmpty() || session->error.isEmpty())
        session->error = QStringLiteral("导入已取消");
      session->fileResult.assetId.clear();
      m_activeImport.reset();
      job->finish(*session);
      pumpImports();
      return;
    }
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

// （#560）importFolder/importFiles 的任务池路径共用脚手架。D1b+T2+
// 审计 02 M-8：整个批次（plan 扫描/哈希 + LAS 解析/层位装箱/SEG-Y 索引）
// 在 worker 上 produce 进 session 的 staging 副本；finished（GUI）里
// commitImport 一次事务入库，随后按原序发 layerDeclared/imported/
// importFailed，再 importActiveChanged(false)——抑制逐文件标签的标志在
// 信号之后复位，不会漏开逐文件标签。两口径（目录/文件清单）只差
// title/produce/台账目录，收口在此；后续加口径只改一处。
void FolderImportWorkflow::makeImportJob(const QString &title,
                                         const BatchProduce &produce,
                                         const QString &ledgerDir,
                                         const ImportDone &done)
{
  const QDateTime started = QDateTime::currentDateTime();
  auto job = std::make_shared<ImportJob>();
  job->title = title;
  // 进度口径（两口径逐字同型）：扫描段 total==0（跑马灯 + 文件名
  // detail），执行段 total=行数（reportBytes + 路径 detail）；false =
  // 协作取消。
  job->produce = [produce](ImportSession &s, PaleoTask *t) {
    produce(s, [t](int d, int total, const QString &p) {
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
  job->finish = [this, done, ledgerDir, started](ImportSession &s) {
    emit importActiveChanged(false);
    // 台账写进「当前打开」工程的 sidecar：produce 期间若已切换工程，当前
    // catalog 是另一个工程的——把别处的、整批作废的导入记进它的审计台账
    // 是串账（opt-01 P5）。此时只经回调如实报「工程已切换」，不入账。
    if (sameProjectSession(m_svc ? m_svc->catalog() : nullptr, s))
      recordLedger(ledgerDir, started, s.rows, s.error);
    else
      qInfo() << "FolderImportWorkflow: project changed during import, ledger not written for"
              << ledgerDir;
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

// （#560）preview 双胞胎（previewFolderAsync/previewFilesAsync）的骨架
// 收口：两口径只差 title/produce/同步 fallback。T2：任务池在场 → 扫描/
// 分类/哈希跑 worker（plan 经 COW 快照构建，catalog 不被 worker 触碰），
// 扫描进度回报任务页，GUI 零同步整目录扫描；无任务服务 → 同步旧路径
// （小环境/测试行为不变）。
void FolderImportWorkflow::previewAsync(
    const QString &title,
    const std::function<QVector<FolderPreviewRow>(
        ImportSession &, QString *,
        const std::function<bool(int, const QString &)> &)> &produce,
    const std::function<QVector<FolderPreviewRow>(QString *)> &syncPreview,
    const std::function<void(const QVector<FolderPreviewRow> &,
                             const QString &)> &done)
{
  if (!m_svc)
  {
    if (done)
      done({}, QStringLiteral("导入服务未就绪"));
    return;
  }
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
        title, [session, produce, outRows, outErr](PaleoTask *t) -> QString {
          *outRows = produce(*session, outErr.get(),
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
  const QVector<FolderPreviewRow> rows = syncPreview(&err);
  if (done)
    done(rows, err);
}

void FolderImportWorkflow::previewFolderAsync(
    const QString &dir,
    const std::function<void(const QVector<FolderPreviewRow> &,
                             const QString &)> &done)
{
  previewAsync(
      tr("扫描工区文件夹"),
      [dir](ImportSession &s, QString *err,
            const std::function<bool(int, const QString &)> &scanProgress) {
        return DataImportService::producePreview(s, dir, err, scanProgress);
      },
      [this, dir](QString *err) { return m_svc->previewFolder(dir, err); },
      done);
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
  // staging 副本；finished（GUI）里 commitImport 一次事务入库（脚手架
  // 见 makeImportJob）。
  if (m_taskSvc)
  {
    makeImportJob(
        tr("导入工区文件夹"),
        [dir, overrides, forceImportPaths](
            ImportSession &s,
            const std::function<bool(int, int, const QString &)> &progress) {
          DataImportService::produceFolder(s, dir, overrides, forceImportPaths, progress);
        },
        dir, done);
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

void FolderImportWorkflow::previewFilesAsync(
    const QStringList &paths,
    const std::function<void(const QVector<FolderPreviewRow> &,
                             const QString &)> &done)
{
  // 与 previewFolderAsync 同一任务池/进度口径，只换数据源为文件清单。
  previewAsync(
      tr("扫描所选文件"),
      [paths](ImportSession &s, QString *err,
              const std::function<bool(int, const QString &)> &scanProgress) {
        return DataImportService::producePreviewForFiles(s, paths, err, scanProgress);
      },
      [this, paths](QString *err) { return m_svc->previewFiles(paths, err); },
      done);
}

void FolderImportWorkflow::importFiles(const QStringList &paths,
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
  // 台账/溯源目录：首个文件所在目录（清单可跨目录——目录只是台账展示位）。
  const QString ledgerDir =
      paths.isEmpty() ? QString() : QFileInfo(paths.first()).absolutePath();
  // 任务池路径与 importFolder 同一脚手架（makeImportJob），只换数据源。
  if (m_taskSvc)
  {
    makeImportJob(
        tr("导入所选文件"),
        [paths, overrides, forceImportPaths](
            ImportSession &s,
            const std::function<bool(int, int, const QString &)> &progress) {
          DataImportService::produceFiles(s, paths, overrides, forceImportPaths, progress);
        },
        ledgerDir, done);
    return;
  }
  const QDateTime started = QDateTime::currentDateTime();
  emit importActiveChanged(true);
  QString importErr;
  const auto res = m_svc->importFiles(paths, &importErr, overrides, forceImportPaths);
  emit importActiveChanged(false);
  recordLedger(ledgerDir, started, res, importErr);
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
    job->discardOnCancel = true;
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
  // 找回刚入库的井口资产。导入登记的 sourceUri 是源文件绝对路径——先按它
  // 精确定位（工区里不同子目录常有同名井口表，如 a/wellheads.csv 与
  // b/wellheads.csv；旧实现只比文件名，第二行会打开第一份资产的预览）。
  // 多份同源（重复导入改过的同一文件）取最新登记的一份。无路径命中时
  // （如 dedup「字节已在库」命中别处的同内容资产）才退回文件名口径，且
  // 只在唯一候选时采用——有歧义宁可不开预览，也不开错资产。
  DataCatalog *cat = m_svc ? m_svc->catalog() : nullptr;
  if (!cat || rowPath.isEmpty())
    return {};
  const QFileInfo rowInfo(rowPath);
  const QString rowAbs = QDir::cleanPath(rowInfo.absoluteFilePath());
  const QString fileName = rowInfo.fileName();
  // 两遍：第一遍只做纯词法的绝对路径比较（不查盘），命中即返回；仅当无词法
  // 命中时第二遍才对候选做 canonicalFilePath（符号链接/大小写别名）——GUI 线程
  // 上不再为每个井口资产 stat 一次。
  const QVector<CatalogAsset> assets = cat->assets();
  QVector<QPair<QString, QString>> candidates; // (assetId, sourceUri)，新→旧
  QStringList byName;
  for (auto it = assets.crbegin(); it != assets.crend(); ++it)
  {
    if (it->type != QLatin1String("well_head"))
      continue;
    const CatalogVersion v = cat->currentVersion(it->id);
    if (!v.sourceUri.isEmpty())
    {
      if (QDir::cleanPath(QFileInfo(v.sourceUri).absoluteFilePath()) == rowAbs)
        return it->id; // 逆序遍历：最新登记者优先
      candidates.append({it->id, v.sourceUri});
    }
    if (v.fileName == fileName)
      byName << it->id;
  }
  const QString rowCanonical = rowInfo.canonicalFilePath();
  if (!rowCanonical.isEmpty())
    for (const auto &c : std::as_const(candidates))
      if (QFileInfo(c.second).canonicalFilePath() == rowCanonical)
        return c.first;
  return byName.size() == 1 ? byName.first() : QString();
}
