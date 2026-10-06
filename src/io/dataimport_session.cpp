// 层：数据
// 方向57：从 dataimportservice.cpp 按格式族析出。公共 API（dataimportservice.h）
// 零改动；跨族共享辅助（setError/readFileOrEmpty/FamilyContext）经
// dataimport_internal.h；importOneFile 分支体族函数亦声明于该头。
#include "dataimportservice.h"

#include "lascache.h"
#include "rasterpyramid.h"
#include "segyindexstore.h"
#include "shacache.h"

#include "../catalog/datacatalog.h"
#include "../metadata/layermanifest.h"
#include "../metadata/paleoprojectstore.h"
#include "../domain/arearules.h"
#include "horizonbinner.h"
#include "ingestplan.h"
#include "welllogread.h" // 方向44：井名提取分派
#include "../domain/projectclassifier.h"
#include "segyreader.h"
#include "wellcompositexml.h"
#include "wellfileparsers.h"

#include <QCryptographicHash>
#include <QJsonArray>
#include <QJsonDocument>
#include <QJsonObject>
#include <QCoreApplication>
#include <QDir>
#include <QDirIterator>
#include <QFile>
#include <QFileInfo>
#include <QProcess>
#include <QThread>
#include <QUuid>

#include <cpl_conv.h>
#include <cpl_error.h>
#include <gdal.h>
#include <QStandardPaths>
#include <QUrl>

#include "../metadata/atomicfile.h"

#include <algorithm>
#include <cstdio>
#include "dataimport_internal.h"

using paleo::dataimport_detail::setError;
using paleo::dataimport_detail::readFileOrEmpty;

// ---- 会话/登记基座：plan 构建、井身份解析、受管 RAW 暂存、ImportSession
// 记账、produce-then-commit 的提交/失败面（方向57 自 dataimportservice.cpp
// 整函数搬迁）。


IngestPlan DataImportService::planFor(
    ImportSession &s, const QString &root,
    const std::function<bool(int filesSeen, const QString &path)> &scanProgress)
{
  // T2 + 审计 02 M-8：plan 对 session 的 staging 副本构建（owner 线程在
  // beginImport 里拷出）——扫描/分类/哈希/身份匹配/去重复核全在当前线程，
  // 活 catalog 零接触，不再有任何跨线程 marshal。
  IngestScanProgress progress;
  if (scanProgress)
    progress = [scanProgress](int seen, const QString &path) {
      return scanProgress(seen, path);
    };
  return buildIngestPlan(root, LiveCatalogSource(s.cat.get()), progress);
}


bool DataImportService::storeManagedRaw(ImportSession &s, const QString &sourcePath,
                                        const QString &assetId, const QString &versionId,
                                        QString *relPathOut, QString *shaOut, QString *error)
{
  QFile src(sourcePath);
  if (!src.open(QIODevice::ReadOnly))
  {
    setError(error, QStringLiteral("cannot read source %1").arg(sourcePath));
    return false;
  }
  const QString fileName = QFileInfo(sourcePath).fileName();
  // §3：文件名必须是一段合法路径段（import 入口已拦，这里再兜底）。
  const QString relDir =
      DataCatalog::managedPath(QStringLiteral("raw"), assetId, versionId, fileName);
  if (relDir.isEmpty())
  {
    setError(error, QStringLiteral("文件名不是合法路径段: %1").arg(fileName));
    return false;
  }
  const QString relPath = QStringLiteral("artifacts/") + relDir;
  CatalogVersion pending;
  pending.managed = true;
  pending.path = relPath;
  // 审计 02 M-8：落 session 私有暂存根（与工程目录同构），owner 线程提交时
  // 原子 rename 到 <project>/<relPath>。最终位置先按工程根校验一次安全性。
  if (DataCatalog::resolvedVersionPath(s.projectDir, pending).isEmpty())
  {
    setError(error, QStringLiteral("unsafe managed destination: %1").arg(relPath));
    return false;
  }
  if (!s.ensureStagingRoot(error))
    return false;
  const QString dst = DataCatalog::resolvedVersionPath(s.stagingRoot, pending);
  if (dst.isEmpty())
  {
    setError(error, QStringLiteral("unsafe managed destination: %1").arg(relPath));
    return false;
  }
  const QDir dir = QFileInfo(dst).absoluteDir();
  if (!dir.exists() && !dir.mkpath(QStringLiteral(".")))
  {
    setError(error, QStringLiteral("cannot create directory %1").arg(dir.absolutePath()));
    return false;
  }
  if (DataCatalog::resolvedVersionPath(s.stagingRoot, pending).isEmpty())
  {
    setError(error, QStringLiteral("unsafe managed destination: %1").arg(relPath));
    return false;
  }

  // 边复制边算 SHA-256；partial + rename 原子落位；成功后置只读。
  QCryptographicHash hash(QCryptographicHash::Sha256);
  const QString partial = dst + QStringLiteral(".partial");
  QFile out(partial);
  if (!out.open(QIODevice::WriteOnly | QIODevice::Truncate))
  {
    setError(error, QStringLiteral("cannot write %1").arg(partial));
    return false;
  }
  // 64KB chunks: a 1MB stack buffer overflows the default Windows thread
  // stack when the import runs on the QTest main thread.
  char buf[64 << 10];
  qint64 n = 0;
  while ((n = src.read(buf, sizeof(buf))) > 0)
  {
    hash.addData(QByteArrayView(buf, static_cast<qsizetype>(n)));
    if (out.write(buf, n) != n)
    {
      out.close();
      QFile::remove(partial);
      setError(error, QStringLiteral("short write to %1").arg(partial));
      return false;
    }
  }
  if (n < 0)
  {
    out.close();
    QFile::remove(partial);
    setError(error, QStringLiteral("read error on %1").arg(sourcePath));
    return false;
  }
  out.flush();
  out.close();
  if (!paleoReplaceFile(partial, dst))
  {
    QFile::remove(partial);
    setError(error, QStringLiteral("cannot place %1").arg(dst));
    return false;
  }
  QFile::setPermissions(dst, QFileDevice::ReadOwner | QFileDevice::ReadUser |
                                 QFileDevice::ReadGroup | QFileDevice::ReadOther);
  if (relPathOut)
    *relPathOut = relPath;
  if (shaOut)
    *shaOut = QString::fromLatin1(hash.result().toHex());
  return true;
}


bool ImportSession::ensureStagingRoot(QString *err)
{
  if (stagingRoot.isEmpty())
  {
    setError(err, QStringLiteral("no project directory"));
    return false;
  }
  // 并发 produce 会同时建 artifacts/staging/<uuid>：Windows 上 QDir::mkpath 在
  // 中间目录（artifacts/staging）被别的线程抢先建好时误报失败（Qt
  // createDirectoryWithParents 对已存在的父目录继续向上递归到盘符 → false）；
  // owner 线程 discardStaging 顺手 rmdir 空父目录也可能与 worker 建目录交错。
  // 重试：下一轮父目录已在，直接建叶子即成。
  for (int attempt = 0; attempt < 5; ++attempt)
  {
    if (QDir(stagingRoot).exists() || QDir().mkpath(stagingRoot))
      return true;
  }
  if (QDir(stagingRoot).exists())
    return true;
  setError(err, QStringLiteral("cannot create directory %1").arg(stagingRoot));
  return false;
}

void ImportSession::discardStaging()
{
  if (stagingRoot.isEmpty() || !QDir(stagingRoot).exists())
    return;
  // 受管字节落盘即只读——先放开写权限再删。
  QDirIterator it(stagingRoot, QDir::Files | QDir::Hidden | QDir::System,
                  QDirIterator::Subdirectories);
  while (it.hasNext())
  {
    const QString f = it.next();
    QFile::setPermissions(f, QFile::permissions(f) | QFileDevice::WriteOwner);
  }
  QDir(stagingRoot).removeRecursively();
  // 空的 staging 父目录顺手收掉（其余 session 的目录在则 rmdir 失败，无害）。
  QDir().rmdir(QFileInfo(stagingRoot).absolutePath());
}

void ImportSession::recordImported(const QString &kind, const QString &assetId,
                                   const QString &layerId)
{
  ImportEvent e;
  e.kind = ImportEvent::Kind::Imported;
  e.assetKind = kind;
  e.assetId = assetId;
  e.layerId = layerId;
  events.append(e);
}

void ImportSession::recordImportFailed(const QString &kind, const QString &path,
                                       const QString &err)
{
  ImportEvent e;
  e.kind = ImportEvent::Kind::ImportFailed;
  e.assetKind = kind;
  e.path = path;
  e.error = err;
  events.append(e);
}

void ImportSession::recordLayerDeclared(const LayerDeclaration &decl)
{
  ImportEvent e;
  e.kind = ImportEvent::Kind::LayerDeclared;
  e.decl = decl;
  events.append(e);
}

QString DataImportService::offThreadError()
{
  return QStringLiteral(
      "导入入口只能在 catalog 所属线程调用（后台导入请用 beginImport/produce*/commitImport）");
}

std::shared_ptr<ImportSession> DataImportService::beginImport()
{
  if (QThread::currentThread() != thread() || QThread::currentThread() != m_catalog->thread())
  {
    qWarning("DataImportService::beginImport called off the owner thread");
    return nullptr;
  }
  auto s = std::make_shared<ImportSession>();
  s->projectDir = m_projectDir;
  s->wired = m_store != nullptr;
  s->catalogReady = m_catalogReady;
  s->catalogOpenError = m_catalogOpenError;
  s->baseSeq = m_catalog->mutationSeq();
  s->epoch = m_catalogEpoch;
  if (!m_projectDir.isEmpty())
    s->stagingRoot = QDir(m_projectDir).absoluteFilePath(
        QStringLiteral("artifacts/staging/") +
        QUuid::createUuid().toString(QUuid::WithoutBraces));
  s->cat = m_catalog->createStagingCopy(s->stagingRoot);
  return s;
}

std::shared_ptr<ImportSession>
DataImportService::runInline(const std::function<void(ImportSession &)> &produce)
{
  std::shared_ptr<ImportSession> s = beginImport();
  if (!s)
    return nullptr;
  produce(*s);
  QString cerr;
  commitImport(*s, &cerr, /*allowConflict=*/false);
  return s;
}

bool DataImportService::moveStagedFiles(ImportSession &s, QStringList *moved,
                                        QString *error) const
{
  if (s.stagingRoot.isEmpty() || !QDir(s.stagingRoot).exists())
    return true;
  const QDir root(s.stagingRoot);
  const QDir project(s.projectDir);
  QDirIterator it(s.stagingRoot, QDir::Files | QDir::Hidden | QDir::System,
                  QDirIterator::Subdirectories);
  QStringList files;
  while (it.hasNext())
    files.append(it.next());
  std::sort(files.begin(), files.end());
  for (const QString &f : std::as_const(files))
  {
    const QString rel = root.relativeFilePath(f);
    if (!rel.startsWith(QLatin1String("artifacts/")) ||
        rel.split(QLatin1Char('/')).contains(QStringLiteral("..")))
    {
      setError(error, QStringLiteral("暂存文件路径非法: %1").arg(rel));
      return false;
    }
    if (f.endsWith(QLatin1String(".partial")))
      continue; // 半截产物（produce 失败分支已删；兜底不提交）
    const QString dst = project.absoluteFilePath(rel);
    if (QFileInfo::exists(dst))
    {
      setError(error, QStringLiteral("受管目标已存在，拒绝覆盖: %1").arg(rel));
      return false;
    }
    if (!QDir().mkpath(QFileInfo(dst).absolutePath()))
    {
      setError(error, QStringLiteral("cannot create directory %1")
                          .arg(QFileInfo(dst).absolutePath()));
      return false;
    }
    if (!QFile::rename(f, dst))
    {
      setError(error, QStringLiteral("cannot place %1").arg(dst));
      return false;
    }
    moved->append(dst);
  }
  return true;
}

static void removeMovedFiles(const QStringList &moved)
{
  for (const QString &f : moved)
  {
    QFile::setPermissions(f, QFile::permissions(f) | QFileDevice::WriteOwner);
    QFile::remove(f);
    // 受管版本目录（raw/<ast>/<ver> 与 derived/…）空了顺手收掉。
    QDir d = QFileInfo(f).absoluteDir();
    for (int i = 0; i < 3 && d.isEmpty(); ++i)
    {
      const QString name = d.dirName();
      if (!d.cdUp() || !d.rmdir(name))
        break;
    }
  }
}

void DataImportService::emitSessionEvents(const ImportSession &s, bool committed)
{
  for (const ImportEvent &e : s.events)
  {
    switch (e.kind)
    {
    case ImportEvent::Kind::LayerDeclared:
      if (committed)
        emit layerDeclared(e.decl);
      break;
    case ImportEvent::Kind::Imported:
      if (committed)
        emit imported(e.assetKind, e.assetId, e.layerId);
      break;
    case ImportEvent::Kind::ImportFailed:
      emit importFailed(e.assetKind, e.path, e.error);
      break;
    }
  }
}

void DataImportService::failSession(ImportSession &s, const QString &message)
{
  // 整批未入库：produce 期「成功」的行/文件结局改为 Failed（错误如实带上），
  // produce 期原本的失败事件照常发出，再为被回滚的条目补发 importFailed。
  s.error = message;
  emitSessionEvents(s, /*committed=*/false);
  for (FolderRowResult &r : s.rows)
  {
    if (r.outcome != FolderRowResult::Outcome::Imported &&
        r.outcome != FolderRowResult::Outcome::Unresolved)
      continue;
    r.outcome = FolderRowResult::Outcome::Failed;
    r.message = message;
    r.entityName.clear();
    emit importFailed(r.classifiedType, r.path, message);
  }
  if (s.hasFileResult && s.fileResult.outcome != ImportOutcome::Failed)
  {
    s.fileResult.outcome = ImportOutcome::Failed;
    s.fileResult.assetId.clear();
    s.fileResult.linkAttached = false;
    s.fileResult.message = message;
    emit importFailed(QString(), s.fileSourcePath, message);
  }
  s.discardStaging();
}

DataImportService::CommitStatus
DataImportService::commitImport(ImportSession &s, QString *error, bool allowConflict)
{
  if (error)
    error->clear();
  if (QThread::currentThread() != thread() || QThread::currentThread() != m_catalog->thread())
  {
    setError(error, offThreadError());
    return CommitStatus::Failed; // 不动 session：owner 线程仍可提交
  }
  if (s.committed)
    return CommitStatus::Committed;
  if (s.epoch != m_catalogEpoch || s.projectDir != m_projectDir)
  {
    const QString msg = QStringLiteral("工程已切换，导入结果已丢弃");
    failSession(s, msg);
    setError(error, msg);
    return CommitStatus::Failed;
  }
  const QVector<CatalogOp> ops = s.cat ? s.cat->journal() : QVector<CatalogOp>{};
  // 单文件 produce 失败时 importOneFile 可能已在 staging 副本上记了半截 journal
  // （如 addAsset 之后受管复制/建暂存目录失败）——整份作废，绝不把没有版本、
  // 没发 imported 的半截资产提交入库。
  if (s.hasFileResult && s.fileResult.outcome == ImportOutcome::Failed && !ops.isEmpty())
  {
    const QString msg = s.error.isEmpty() ? QStringLiteral("导入失败，结果已丢弃") : s.error;
    failSession(s, msg);
    setError(error, msg);
    return CommitStatus::Failed;
  }
  if (m_catalog->mutationSeq() != s.baseSeq && !ops.isEmpty())
  {
    // produce 期间 owner 侧另有写入：staging 副本的决策（id 分配/dedup/井
    // 解析）基于旧快照，不能盲目重放。
    const QString msg = QStringLiteral("导入期间工程数据已被修改，请重试导入");
    if (allowConflict)
    {
      setError(error, msg);
      return CommitStatus::Conflict;
    }
    failSession(s, msg);
    setError(error, msg);
    return CommitStatus::Failed;
  }

  QStringList moved;
  QString merr;
  if (!moveStagedFiles(s, &moved, &merr))
  {
    removeMovedFiles(moved);
    const QString msg = QStringLiteral("导入提交失败：%1").arg(merr);
    failSession(s, msg);
    setError(error, msg);
    return CommitStatus::Failed;
  }
  QString aerr;
  if (!m_catalog->applyJournal(ops, &aerr))
  {
    removeMovedFiles(moved);
    const QString msg = QStringLiteral("导入提交失败：%1").arg(aerr);
    failSession(s, msg);
    setError(error, msg);
    return CommitStatus::Failed;
  }
  s.committed = true;
  s.discardStaging();
  emitSessionEvents(s, /*committed=*/true);
  // B3：派生/栅格资产 Lazy ensure 金字塔（近零开销）——最终路径已落位。
  if (!s.pyramidTargets.isEmpty())
  {
    RasterPyramidService pyramids(pyramidCacheDir());
    for (const QString &p : std::as_const(s.pyramidTargets))
    {
      if (!QFileInfo::exists(p))
        continue;
      QString perr;
      if (!pyramids.ensure(p, nullptr, &perr))
        qWarning("DataImportService: pyramid ensure failed for %s: %s", qPrintable(p),
                 qPrintable(perr));
    }
  }
  if (!s.error.isEmpty())
    setError(error, s.error);
  return CommitStatus::Committed;
}
