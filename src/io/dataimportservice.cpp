// 层：数据
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
#include "lasparser.h"
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

// ---------------------------------------------------------------------------
// project_area 导入契约（plan §3）：
//   classify → parse metadata → resolve/create entity → managed RAW（复制边算
//   SHA-256，落盘只读）/ 外部链接（SEG-Y 一律外链）→ 显式 entity_asset_link。
//   层位在 8 个层序界面集合内 → 派生时间栅格（DERIVED，父版本=RAW）并登记
//   LayerManifest（图层清单只登记要画进 QGIS 的结果）。
//   身份顺序：已有 id → UWI → 规范化井名 → 别名；文件名不作身份，仅作回退。
// ---------------------------------------------------------------------------
namespace
{
  void setError(QString *error, const QString &text)
  {
    if (error)
      *error = text;
  }

  // 编图层序界面集合（plan §3/§5E）。名单经 AreaRules（本工区默认
  // C3/C6/D53/D61/D62/D63/D71/D72；第二工区经 project_area.json 换名单）。
  bool isKnownSequenceBoundary(const QString &stem)
  {
    return AreaRules::active().sequenceBoundaries.contains(stem.toUpper());
  }

  // 阶段 D 固定辅助规则：isFixedAuxiliaryPath（HZ28-6-1 命名文件）在
  // projectclassifier.cpp——T22 起只锁这一个文件；「参考资料」目录段的
  // 默认「参考」展示归确认表（isDefaultReferencePath），改动成 override。

  QString readFileOrEmpty(const QString &path)
  {
    QFile f(path);
    if (!f.open(QIODevice::ReadOnly))
      return QString();
    return QString::fromUtf8(f.readAll());
  }

  // 未决链接备注（§3 修订）：双候选记两个规范化井名（附实体 id 消歧）；
  // 零匹配记规范化的未匹配井名。
  QString candidatesNote(const DataCatalog *cat, const QStringList &candidateIds)
  {
    QStringList parts;
    for (const QString &id : candidateIds)
    {
      const CatalogEntity e = cat->entityById(id);
      const QString name = e.name.isEmpty() ? id : DataCatalog::normalizeWellName(e.name);
      parts.append(QStringLiteral("%1(%2)").arg(name, id));
    }
    return QStringLiteral("候选: ") + parts.join(QStringLiteral(", "));
  }

  QString unmatchedNameNote(const QStringList &triedNames)
  {
    QStringList norm;
    for (const QString &n : triedNames)
    {
      const QString nn = DataCatalog::normalizeWellName(n);
      if (!nn.isEmpty() && !norm.contains(nn))
        norm.append(nn);
    }
    return QStringLiteral("未匹配井名: ") + norm.join(QStringLiteral(", "));
  }

  // 已决 well_log 的主标记与 ordinal。只数同一实体上 role==well_log 且
  // !unresolved：没有则本条为主、ordinal 0；有成员但没有主则本条补主；
  // 已有主则本条非主。ordinal = 这些成员的 max(ordinal)+1。不改既有链接。
  void assignResolvedWellLogSlot(const DataCatalog *cat, EntityAssetLink &link)
  {
    bool any = false;
    bool hasPrimary = false;
    int maxOrdinal = 0;
    for (const EntityAssetLink &existing : cat->linksForEntity(link.entityId))
    {
      if (existing.role != QLatin1String("well_log") || existing.unresolved)
        continue;
      if (!any || existing.ordinal > maxOrdinal)
        maxOrdinal = existing.ordinal;
      any = true;
      if (existing.isPrimary)
        hasPrimary = true;
    }
    if (!any)
    {
      link.isPrimary = true;
      link.ordinal = 0;
      return;
    }
    link.isPrimary = !hasPrimary;
    link.ordinal = maxOrdinal + 1;
  }
} // namespace

DataImportService::DataImportService(PaleoProjectStore *store, QObject *parent)
  : QObject(parent)
  , m_store(store)
  , m_catalog(new DataCatalog(this))
{
  // T5 腐败恢复广播：catalog open() 走了 .bak 回退 → 转发壳侧（状态面向
  // 用户告警「catalog 已从备份恢复，主文件损坏」）。PreviewDocService 再转
  // 一道给 UI。
  connect(m_catalog, &DataCatalog::backupRecovered, this,
          [this](const QString &reason) { emit catalogRecoveredFromBackup(reason); });
}

DataImportService::~DataImportService()
{
  if (m_pdfProc)
  {
    m_pdfProc->kill();
    m_pdfProc->waitForFinished(2000);
  }
}

void DataImportService::setProjectDir(const QString &dir)
{
  // 工程切换时丢弃在途/排队转换——它们写向旧工程目录，且状态随之失效。
  if (m_pdfProc)
  {
    m_pdfProc->disconnect(this);
    m_pdfProc->kill();
    m_pdfProc->deleteLater();
    m_pdfProc = nullptr;
  }
  m_pdfQueue.clear();
  m_pdfCurrent.clear();
  m_pdfPending.clear();
  m_pdfErrors.clear();

  const bool projectChanged = QDir::cleanPath(m_projectDir) != QDir::cleanPath(dir);
  m_projectDir = dir;
  ++m_catalogEpoch; // 在途 session 的产出不得提交进新工程
  // wave/io-perf-cache：工程级缓存根统一接线（P4）。
  //  · LAS 解析缓存 + SHA 摘要表落 <project>/artifacts/index/（工程私有，
  //    随工程迁移；损坏自愈见 lascache/shacache）。
  //  · SEG-Y 道索引目录同源（openCached 调用方取 indexCacheDir()）。
  //  · vendor SgyIndexCache 的全局缓存目录顺手预建（D2.1 仓外根治）。
  if (!dir.trimmed().isEmpty())
  {
    LasCache::shared().setDiskRoot(dir + QStringLiteral("/artifacts/index/las"));
    ShaCache::shared().setDiskFile(dir + QStringLiteral("/artifacts/index/sha.json"));
    SegyIndexStore::ensureLegacyGlobalCacheDir();
  }
  m_catalogReady = false;
  QString err;
  m_catalogReady = m_catalog->open(dir, &err);
  // T20a：记错误面 + 发信号——catalog 已进入拒绝写入态，后续 mutator
  // 都如实失败。状态栏/消息区接线归 paleomainwindow。
  m_catalogOpenError = m_catalogReady ? QString() : err;
  if (!m_catalogReady)
  {
    qWarning("DataImportService: catalog open failed: %s", qPrintable(err));
    emit catalogOpenFailed(err);
  }
  // #155：上次进程崩溃残留的 artifacts/staging/<uuid> 无人回收。只在真正
  // 切到另一工程、且本实例拿到写锁（无别的进程在用该工程）时清扫——本进程
  // 的在途 session 都属于旧工程目录，同目录重设不扫。
  else if (projectChanged && !m_catalog->isLockedReadOnly())
    sweepStaleStaging(dir);
}

int DataImportService::sweepStaleStaging(const QString &projectDir)
{
  if (projectDir.trimmed().isEmpty())
    return 0;
  QDir staging(projectDir + QStringLiteral("/artifacts/staging"));
  if (!staging.exists())
    return 0;
  int removed = 0;
  const QStringList entries =
      staging.entryList(QDir::Dirs | QDir::Files | QDir::Hidden | QDir::NoDotAndDotDot);
  for (const QString &e : entries)
  {
    const QString path = staging.filePath(e);
    const bool ok = QFileInfo(path).isDir() ? QDir(path).removeRecursively()
                                            : QFile::remove(path);
    if (ok)
      ++removed;
    else
      qWarning("DataImportService: cannot remove stale staging %s", qPrintable(path));
  }
  if (removed > 0)
    qWarning("DataImportService: swept %d stale staging entr%s under %s", removed,
             removed == 1 ? "y" : "ies", qPrintable(staging.path()));
  QDir().rmdir(staging.path()); // 空了顺手收掉（非空失败无妨）
  return removed;
}

QString DataImportService::indexCacheDir() const
{
  return m_projectDir.isEmpty()
             ? QString()
             : m_projectDir + QStringLiteral("/artifacts/index/segy");
}

QString DataImportService::pyramidCacheDir() const
{
  // B3：栅格瓦片金字塔根（工程私有，随工程迁移；损坏/过版自愈走
  // RasterPyramidService 的 meta 身份闸）。
  return m_projectDir.isEmpty()
             ? QString()
             : m_projectDir + QStringLiteral("/artifacts/pyramids");
}

int DataImportService::ensureRasterPyramids(
    const QStringList &absPaths, QString *error,
    const std::function<bool(int, int, const QString &)> &progress)
{
  if (absPaths.isEmpty())
    return 0;
  const QString root = pyramidCacheDir();
  if (root.isEmpty())
  {
    if (error)
      *error = QStringLiteral("no project dir — pyramid cache root unavailable");
    return 0;
  }
  RasterPyramidService pyramids(root);
  int ok = 0;
  for (int i = 0; i < absPaths.size(); ++i)
  {
    const QString &path = absPaths.at(i);
    if (progress && !progress(i, absPaths.size(), path))
      break; // 协作取消：已 ensure 的保留
    QString err;
    RasterPyramidService::PyramidMeta meta;
    if (pyramids.ensure(path, &meta, &err,
                        RasterPyramidService::BuildStrategy::Lazy))
      ++ok;
    else
      qWarning("DataImportService: pyramid ensure failed for %s: %s",
               qPrintable(path), qPrintable(err));
  }
  if (progress)
    progress(absPaths.size(), absPaths.size(), QString());
  return ok;
}

// 单文件 GDAL 外部概览。C 回调桥（GDALProgressFunc：返回 0 = 用户终止）。
namespace
{
struct GdalOverviewCtx
{
  std::function<bool(double)> progress;
};

int gdalOverviewProgress(double pct, const char *, void *userData)
{
  auto *ctx = static_cast<GdalOverviewCtx *>(userData);
  if (!ctx || !ctx->progress)
    return 1;
  return ctx->progress(pct) ? 1 : 0;
}
} // namespace

bool DataImportService::buildRasterOverviews(
    const QString &absPath, QString *error,
    const std::function<bool(double)> &buildProgress)
{
  if (absPath.isEmpty())
  {
    if (error)
      *error = QStringLiteral("empty raster path");
    return false;
  }
  // 外部 .ovr 边车：受管 RAW 字节不动（SHA 留底与复验契约保持有效）。
  // GDAL 3.x 语义：概览落内/外由打开模式决定——UPDATE 打开写内部概览，
  // 只读打开自动落 <file>.ovr 边车（源字节零改动）。
  GDALDatasetH ds = GDALOpenEx(absPath.toUtf8().constData(),
                               GDAL_OF_RASTER, nullptr, nullptr, nullptr);
  if (!ds)
  {
    if (error)
      *error = QStringLiteral("cannot open raster: %1").arg(absPath);
    return false;
  }
  const int w = GDALGetRasterXSize(ds), h = GDALGetRasterYSize(ds);
  // 层级 2,4,8,… 至最小维 <512px（再细的概览对画布渲染无增益）；上限 8 级。
  QList<int> levels;
  for (int lv = 2; qMin(w, h) / lv >= 512 && levels.size() < 8; lv *= 2)
    levels.append(lv);
  if (levels.isEmpty())
  {
    GDALClose(ds);
    return true; // 小图无概览可建——成功语义（幂等）
  }
  GdalOverviewCtx ctx{buildProgress};
  const CPLErr err =
      GDALBuildOverviews(ds, "NEAREST", levels.size(), levels.constData(), 0,
                         nullptr, &gdalOverviewProgress, &ctx);
  GDALFlushCache(ds);
  GDALClose(ds);
  if (err != CE_None)
  {
    if (error)
      *error = QStringLiteral("GDALBuildOverviews failed: %1")
                   .arg(QString::fromUtf8(CPLGetLastErrorMsg()));
    return false;
  }
  return true;
}

int DataImportService::buildRasterOverviews(
    const QStringList &absPaths, QString *error,
    const std::function<bool(int, int, const QString &)> &progress)
{
  if (absPaths.isEmpty())
    return 0;
  int built = 0;
  for (int i = 0; i < absPaths.size(); ++i)
  {
    if (progress && !progress(i, absPaths.size(), absPaths.at(i)))
      break; // 协作取消
    QString err;
    if (buildRasterOverviews(absPaths.at(i), &err))
      ++built;
    else
      qWarning("DataImportService: overview build failed for %s: %s",
               qPrintable(absPaths.at(i)), qPrintable(err));
  }
  if (progress)
    progress(absPaths.size(), absPaths.size(), QString());
  if (error && built < absPaths.size())
    *error = QStringLiteral("%1/%2 built").arg(built).arg(absPaths.size());
  return built;
}

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

DataImportService::WellBind DataImportService::resolveWell(const DataCatalog *cat,
                                                           const QString &name)
{
  WellBind b;
  if (name.trimmed().isEmpty())
  {
    b.unresolved = true;
    return b;
  }
  const QStringList ids = cat->wellsMatchingName(name);
  if (ids.size() == 1)
  {
    b.entityId = ids.front();
    return b;
  }
  b.unresolved = true;
  b.candidates = ids; // 0 个或 2+ 个
  return b;
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

QString DataImportService::importProjectFile(const QString &sourcePath, QString *error)
{
  return importProjectFile(sourcePath, ImportOptions{}, error);
}

QString DataImportService::importProjectFile(const QString &sourcePath, const ImportOptions &options,
                                             QString *error)
{
  // 兼容签名：dedup 命中也返回（已存在的）资产 id——分辨结局用 importProjectFileEx。
  return importProjectFileEx(sourcePath, options, error).assetId;
}

DataImportService::ImportResult
DataImportService::importProjectFileEx(const QString &sourcePath, QString *error)
{
  return importProjectFileEx(sourcePath, ImportOptions{}, error);
}

// ===========================================================================
// 审计 02 M-8：produce-then-commit。
// ===========================================================================
ImportSession::~ImportSession()
{
  if (!committed)
    discardStaging();
}

bool ImportSession::ensureStagingRoot(QString *err)
{
  if (stagingRoot.isEmpty())
  {
    setError(err, QStringLiteral("no project directory"));
    return false;
  }
  if (QDir(stagingRoot).exists() || QDir().mkpath(stagingRoot))
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

void DataImportService::produceFile(ImportSession &s, const QString &sourcePath,
                                    const ImportOptions &options)
{
  // C 包三段式入口（docs/DATA_FABRIC_ADOPTION.md）：单文件也先立 1 项 plan——
  // 同一 buildIngestPlan（分类/身份/plan 期 dedup 标记全走），重复项的
  // duplicateOfVersionId 如实标上。显式单文件导入的语义是「仍导入」：这里不走
  // 执行器的 skip/幂等分支，同字节结局由内部 dedup 消化（AlreadyStored+补挂，
  // 与既有测试同口径）。plan 不适用时（服务未接线/文件不存在/目录源）原路进
  // importOneFile——错误与 importFailed 信号口径不变。
  s.hasFileResult = true;
  s.fileSourcePath = sourcePath;
  QString err;
  if (s.cat && s.catalogReady && !sourcePath.isEmpty() && QFileInfo(sourcePath).isFile())
  {
    const IngestPlan plan = planFor(s, sourcePath);
    if (!plan.items.isEmpty())
    {
      const PlannedItem &item = plan.items.constFirst();
      ImportOptions eff = options;
      if (eff.forceType.isEmpty())
        eff.forceType = item.type; // 生效类型取 plan 分类（与内部同口径）
      s.fileResult = importOneFile(s, item.path, eff, &err);
      s.error = err;
      return;
    }
  }
  s.fileResult = importOneFile(s, sourcePath, options, &err);
  s.error = err;
}

DataImportService::ImportResult
DataImportService::importProjectFileEx(const QString &sourcePath, const ImportOptions &options,
                                       QString *error)
{
  if (error)
    error->clear();
  const auto s = runInline(
      [&](ImportSession &sess) { produceFile(sess, sourcePath, options); });
  if (!s)
  {
    setError(error, offThreadError());
    return {};
  }
  setError(error, s->error);
  return s->fileResult;
}

// 单文件导入实体——plan 化前的 importProjectFileEx 主体：分类 → dedup →
// 受管 RAW 复制/外链 → 实体解析 → 关联，语义原样未动。
DataImportService::ImportResult
DataImportService::importOneFile(ImportSession &s, const QString &sourcePath,
                                 const ImportOptions &options, QString *error)
{
  QString internalError;
  if (!error)
    error = &internalError;
  else
    error->clear();

  // 审计 02 M-8：本函数在 produce 线程跑——只碰 session（staging 副本 +
  // 暂存根 + 事件表）。信号记成事件，owner 线程提交成功后按原序发射。
  DataCatalog *const cat = s.cat.get();
  ImportResult res;
  const auto fail = [&](const QString &msg) -> ImportResult {
    setError(error, msg);
    s.recordImportFailed(QString(), sourcePath, msg);
    res.outcome = ImportOutcome::Failed;
    res.message = msg;
    return res;
  };
  const auto done = [&](ImportOutcome outcome, const QString &assetId,
                        const QString &message = QString()) -> ImportResult {
    res.outcome = outcome;
    res.assetId = assetId;
    res.message = message;
    return res;
  };

  if (!s.wired)
    return fail(QStringLiteral("import service is not fully wired"));
  if (s.projectDir.isEmpty())
    return fail(QStringLiteral("project dir is not set"));
  // T20a：catalog 拒绝写入时提前如实失败——不等算完 SHA/复制完字节才撞墙。
  if (!s.catalogReady)
    return fail(QStringLiteral("catalog 拒绝写入：%1")
                    .arg(s.catalogOpenError.isEmpty()
                             ? QStringLiteral("catalog 打开失败")
                             : s.catalogOpenError));
  // 单写实例降级（T4）：锁在别的实例手里——受管字节还没开始复制就拒，
  // 不留「文件拷进 artifacts/ 而 catalog 拒登记」的孤儿。
  if (cat->isLockedReadOnly())
    return fail(QStringLiteral("工程目录被另一个实例锁定——本实例只读，导入被拒绝"));
  if (sourcePath.isEmpty() || !QFile::exists(sourcePath))
    return fail(QStringLiteral("找不到源文件: %1").arg(sourcePath));

  const QFileInfo fi(sourcePath);

  // §3 路径卫生：文件名本身必须是一段合法路径段——否则受管路径和 displayName
  // 都带病。坏文件名让这一行如实失败，不进 catalog。
  if (!DataCatalog::isSafePathSegment(fi.fileName()))
    return fail(QStringLiteral("文件名不是合法路径段: %1").arg(fi.fileName()));

  // 流式算一遍源文件 SHA-256：dedup 查询与外链入库留底共用这一趟。
  QString shaErr;
  const QString sourceSha = ShaCache::shared().sha256Hex(sourcePath, &shaErr); // D7.7
  if (sourceSha.isEmpty())
    return fail(shaErr.isEmpty() ? QStringLiteral("cannot hash %1").arg(sourcePath) : shaErr);

  // §3 dedup：同一 SHA-256 已在库 → 不新建资产/版本/主关联；只把现在恰好能
  // 匹配到一口井的未决关联补挂上（不建井、不并井）。versionBySha256 只认
  // 文件仍在且重哈希一致的版本——受管文件丢失/被改的旧条目不再冒充命中，
  // 重导据此走全新导入（issue #5）。
  const CatalogVersion existing =
      cat->versionBySha256(sourceSha);
  if (!existing.id.isEmpty())
  {
    const CatalogAsset existingAsset =
        cat->assetById(existing.assetId);
    if (existingAsset.type == QLatin1String("horizon"))
    {
      const QString horizon = QFileInfo(existing.fileName.isEmpty() ? sourcePath : existing.fileName)
                                  .completeBaseName().toUpper();
      if (isKnownSequenceBoundary(horizon))
      {
        CatalogVersion derived;
        const QVector<CatalogVersion> siblings =
            cat->versionsForAsset(existing.assetId);
        for (const CatalogVersion &v : siblings)
          if (v.stage.compare(QStringLiteral("DERIVED"), Qt::CaseInsensitive) == 0 &&
              v.parentVersionIds.contains(existing.id) &&
              QFileInfo::exists(cat->versionFilePath(v))) // 同批新增派生件在暂存根
          {
            derived = v;
            break;
          }
        if (derived.id.isEmpty())
        {
          QFile source(sourcePath);
          if (!source.open(QIODevice::ReadOnly))
            return fail(QStringLiteral("cannot read %1").arg(sourcePath));
          BinnedHorizon binned;
          if (!binHorizon(source.readAll(), &binned, error))
            return fail(error ? *error : QStringLiteral("horizon binning failed"));
          derived.id = cat->nextVersionId();
          derived.assetId = existing.assetId;
          derived.stage = QStringLiteral("DERIVED");
          derived.versionNumber =
              cat->currentVersion(existing.assetId).versionNumber + 1;
          derived.managed = true;
          derived.fileName = horizon + QStringLiteral(".tif");
          derived.path = QStringLiteral("artifacts/") + DataCatalog::managedPath(
              QStringLiteral("derived"), existing.assetId, derived.id, derived.fileName);
          derived.sourceUri = sourcePath;
          derived.parentVersionIds = QStringList{existing.id};
          derived.extra.insert(QStringLiteral("collisions"), binned.collisions);
          derived.extra.insert(QStringLiteral("rejected"), binned.rejected);
          derived.extra.insert(QStringLiteral("grid_rows"), binned.rows);
          derived.extra.insert(QStringLiteral("grid_cols"), binned.cols);
          derived.extra.insert(QStringLiteral("z_units"), QStringLiteral("ms"));
          derived.extra.insert(QStringLiteral("z_min"), binned.zMin);
          derived.extra.insert(QStringLiteral("z_max"), binned.zMax);
          derived.extra.insert(QStringLiteral("filled_cells"), binned.filledCells);
          if (DataCatalog::resolvedVersionPath(s.projectDir, derived).isEmpty())
            return fail(QStringLiteral("unsafe managed destination: %1").arg(derived.path));
          if (!s.ensureStagingRoot(error))
            return fail(*error);
          const QString dst = DataCatalog::resolvedVersionPath(s.stagingRoot, derived);
          if (dst.isEmpty())
            return fail(QStringLiteral("unsafe managed destination: %1").arg(derived.path));
          if (!QDir().mkpath(QFileInfo(dst).absolutePath()))
            return fail(QStringLiteral("cannot create directory %1")
                            .arg(QFileInfo(dst).absolutePath()));
          if (!writeHorizonGeoTiff(binned, dst, error))
            return fail(error ? *error : QStringLiteral("horizon raster write failed"));
          QFile::setPermissions(dst, QFileDevice::ReadOwner | QFileDevice::ReadUser |
                                     QFileDevice::ReadGroup | QFileDevice::ReadOther);
          if (!cat->addVersion(derived, error))
            return fail(error ? *error : QStringLiteral("catalog addVersion failed"));
        }
        LayerDeclaration decl;
        decl.layerId = QStringLiteral("horizon.%1").arg(horizon);
        decl.horizon = horizon;
        decl.type = QStringLiteral("raster");
        decl.source = DataCatalog::resolvedVersionPath(s.projectDir, derived); // 提交后的最终位置
        decl.group = QStringLiteral("00_Data");
        s.recordLayerDeclared(decl);
      }
    }
    QString aerr;
    // 补挂在 staging 副本上执行（attachLink 记进 journal，提交时重放）。
    const int attached = attachResolvableLinks(cat, existingAsset, sourcePath, &aerr);
    if (!aerr.isEmpty())
      qWarning("import dedup attach: %s", qPrintable(aerr));
    const QString msg =
        QStringLiteral("字节已在库 · %1")
            .arg(attached > 0 ? QStringLiteral("已补上关联")
                              : QStringLiteral("没有新的关联"));
    qInfo("import: %s — %s", qPrintable(msg), qPrintable(sourcePath));
    res.linkAttached = attached > 0;
    s.recordImported(existingAsset.type, existing.assetId, QString()); // 聚焦已有资产
    return done(ImportOutcome::AlreadyStored, existing.assetId, msg);
  }
  const QByteArray xmlContent =
      fi.suffix().compare(QStringLiteral("xml"), Qt::CaseInsensitive) == 0
          ? readFileOrEmpty(sourcePath).toUtf8()
          : QByteArray();
  ProjectClassification cls = classifyProjectImport(sourcePath, xmlContent);
  // 确认表「改类型」：forceType 覆盖分类器结果（其余分类字段保留）。
  if (!options.forceType.isEmpty())
    cls.type = options.forceType;
  const QString stem = fi.completeBaseName();

  const QString assetId = cat->nextAssetId();
  const QString versionId = cat->nextVersionId();

  CatalogAsset asset;
  asset.id = assetId;
  asset.type = cls.type;
  asset.format = cls.format;
  asset.displayName = fi.fileName();
  if (!cat->addAsset(asset, error))
    return fail(*error);

  CatalogVersion version;
  version.id = versionId;
  version.assetId = assetId;
  version.stage = QStringLiteral("RAW");
  version.versionNumber = 1;
  version.fileName = fi.fileName();
  version.sourceUri = fi.absoluteFilePath();

  // SEG-Y 一律外链（plan §3：966 MB 体不复制）。其余默认受管 RAW。
  const bool external = options.linkExternal || cls.type == QLatin1String("seismic");
  if (external)
  {
    version.managed = false;
    version.path = fi.absoluteFilePath();
    // §3：外链也留入库时 SHA-256（上面流式算好的同一趟），打开时照它校验。
    version.sha256 = sourceSha;
  }
  else
  {
    QString relPath, sha;
    if (!storeManagedRaw(s, sourcePath, assetId, versionId, &relPath, &sha, error))
      return fail(*error);
    version.path = relPath;
    version.sha256 = sha;
  }
  if (!cat->addVersion(version, error))
    return fail(*error);

  // ---- 实体解析与关联（角色沿用已有名字）----
  const QString auxRefRole = QStringLiteral("reference");
  QString manifestLayerId;
  // 阶段 D：HZ28-6-1 命名的井类内容 XML 固定作辅助参考——override 也不理
  // （T22 收窄：「参考资料」目录内其他文件的默认「参考」由确认表给，可改）。
  const bool fixedAux = isFixedAuxiliaryPath(sourcePath) &&
                        (cls.type == QLatin1String("well_head") ||
                         cls.type == QLatin1String("well_log"));

  if (cls.type == QLatin1String("well_head") && !fixedAux)
  {
    QFile f(sourcePath);
    if (!f.open(QIODevice::ReadOnly))
      return fail(QStringLiteral("cannot read %1").arg(sourcePath));
    const QVector<WellHeadRecord> rows = parseWellHeadText(f.readAll());
    if (rows.isEmpty())
      return fail(QStringLiteral("no well head rows in %1").arg(sourcePath));
    // §3：井口是建井来源，但同文件里同一规范化井名出现两行、或一行同时匹配
    // 两口已有井 → 该行标 unresolved（实体 id 留空、备注记名），不新建不合并；
    // 恰好匹配一口已有井时挂 well_head，不另建井。
    QHash<QString, int> normRowCount;
    for (const WellHeadRecord &r : rows)
      normRowCount[DataCatalog::normalizeWellName(r.name)] += 1;
    for (const WellHeadRecord &r : rows)
    {
      const QString norm = DataCatalog::normalizeWellName(r.name);
      EntityAssetLink link;
      link.entityType = QStringLiteral("well");
      link.assetId = assetId;
      link.role = QStringLiteral("well_head");
      if (normRowCount.value(norm) >= 2)
      {
        link.unresolved = true;
        link.note = QStringLiteral("井口重名: %1").arg(norm);
        if (!cat->addLink(link, error))
          return fail(*error);
        continue;
      }
      const QStringList matches =
          cat->wellsMatchingName(r.name);
      if (matches.size() >= 2)
      {
        link.unresolved = true;
        link.note = candidatesNote(cat, matches);
        if (!cat->addLink(link, error))
          return fail(*error);
        continue;
      }
      QString wid;
      if (matches.size() == 1)
      {
        wid = matches.front(); // 已有井：直接挂，不另建
      }
      else
      {
        wid = QStringLiteral("well-%1").arg(r.name);
        // id 被别的规范化名占用（罕见）→ 让位于序号 id。
        if (cat->hasEntity(wid))
          wid = cat->nextEntityId(QStringLiteral("well"));
        CatalogEntity w;
        w.id = wid;
        w.entityType = QStringLiteral("well");
        w.name = r.name;
        w.hasSurface = true;
        w.surfaceX = r.x;
        w.surfaceY = r.y;
        w.kb = r.kb;
        w.td = r.td;
        // 局部测网坐标：真投影参数出现前保持未变换（plan §3）。
        w.coordinateStatus = QStringLiteral("untransformed");
        if (!cat->addEntity(w, error))
          return fail(*error);
      }
      link.entityId = wid;
      link.isPrimary = true;
      if (!cat->addLink(link, error))
        return fail(*error);
    }
  }
  else if (cls.type == QLatin1String("well_log") && !fixedAux)
  {
    // LAS 先读 ~W 的 WELL；XML 测井/读不到时用文件名主名。
    // （D12：UWI 回退已随 uwi/aliases 字段剥离——井身份只走 name。）
    QString wellName;
    if (cls.format == QLatin1String("las"))
      LasParser::readWellInfo(sourcePath, wellName);
    QStringList tried{wellName};
    WellBind bind = resolveWell(cat, wellName);
    if (bind.unresolved && bind.candidates.isEmpty())
    {
      bind = resolveWell(cat, stem); // A1.Las → A1
      if (bind.unresolved && bind.candidates.isEmpty())
        tried.append(stem);
    }
    // §3 修订：未决也是一条链接——实体 id 留空，备注记候选或未匹配名。
    EntityAssetLink link;
    link.entityType = QStringLiteral("well");
    link.assetId = assetId;
    link.role = QStringLiteral("well_log");
    if (!bind.unresolved)
    {
      link.entityId = bind.entityId;
      assignResolvedWellLogSlot(cat, link);
    }
    else
    {
      link.unresolved = true;
      link.note = bind.candidates.size() >= 2
                      ? candidatesNote(cat, bind.candidates)
                      : unmatchedNameNote(tried);
    }
    if (!cat->addLink(link, error))
      return fail(*error);
  }
  else if (cls.type == QLatin1String("well_stratification") ||
           cls.type == QLatin1String("time_depth"))
  {
    // 井名来自文件内容（分层=井名列；时深=# Well 行），规则同测井。
    QStringList names;
    if (cls.type == QLatin1String("well_stratification"))
    {
      QFile f(sourcePath);
      if (!f.open(QIODevice::ReadOnly))
        return fail(QStringLiteral("cannot read %1").arg(sourcePath));
      const QVector<WellTopRecord> tops = parseWellTopsText(f.readAll());
      for (const WellTopRecord &t : tops)
        if (!names.contains(t.wellName))
          names.append(t.wellName);
    }
    else
    {
      QFile f(sourcePath);
      if (!f.open(QIODevice::ReadOnly))
        return fail(QStringLiteral("cannot read %1").arg(sourcePath));
      const TimeDepthTable td = parseTimeDepthText(f.readAll());
      names.append(td.wellName.isEmpty() ? stem : td.wellName);
    }
    if (names.isEmpty())
      return fail(QStringLiteral("no well names in %1").arg(sourcePath));

    const QString role = cls.type == QLatin1String("well_stratification")
                             ? QStringLiteral("tops")
                             : QStringLiteral("time_depth");
    // §3 修订：每个井名一条链接；未决链接实体 id 留空，备注记候选或未匹配名，
    // 不新建井、不合并、不再挂辅助实体。
    for (const QString &n : names)
    {
      QStringList tried{n};
      WellBind bind = resolveWell(cat, n);
      if (bind.unresolved && bind.candidates.isEmpty() && names.size() == 1)
      {
        bind = resolveWell(cat, stem); // 单井文件的文件名主名回退
        tried.append(stem);
      }
      EntityAssetLink link;
      link.entityType = QStringLiteral("well");
      link.assetId = assetId;
      link.role = role;
      if (!bind.unresolved)
      {
        link.entityId = bind.entityId;
        link.isPrimary = true;
      }
      else
      {
        link.unresolved = true;
        link.note = bind.candidates.size() >= 2
                        ? candidatesNote(cat, bind.candidates)
                        : unmatchedNameNote(tried);
      }
      if (!cat->addLink(link, error))
        return fail(*error);
    }
  }
  else if (cls.type == QLatin1String("well_deviation"))
  {
    // 井斜站表：井名来自文本 '# Well :' 行（XML 站表无井名——文件名主名），
    // 规则同时深：每井名一条 trajectory 链接；未决留空不建井，不猜。
    QStringList names;
    if (cls.format == QLatin1String("xml"))
    {
      QVector<WellComposite::XmlDeviationStation> parsed;
      QString perr;
      if (!WellComposite::parseDeviationSurvey(sourcePath, parsed, &perr))
        return fail(QStringLiteral("no deviation stations in %1 (%2)")
                        .arg(sourcePath, perr));
      names.append(stem);
    }
    else
    {
      QFile f(sourcePath);
      if (!f.open(QIODevice::ReadOnly))
        return fail(QStringLiteral("cannot read %1").arg(sourcePath));
      const DeviationTable dev = parseDeviationText(f.readAll());
      names.append(dev.wellName.isEmpty() ? stem : dev.wellName);
    }
    if (names.isEmpty())
      return fail(QStringLiteral("no well names in %1").arg(sourcePath));
    for (const QString &n : names)
    {
      QStringList tried{n};
      WellBind bind = resolveWell(cat, n);
      if (bind.unresolved && bind.candidates.isEmpty() && names.size() == 1)
      {
        bind = resolveWell(cat, stem); // 单井文件的文件名主名回退
        tried.append(stem);
      }
      EntityAssetLink link;
      link.entityType = QStringLiteral("well");
      link.assetId = assetId;
      link.role = QStringLiteral("trajectory");
      if (!bind.unresolved)
      {
        link.entityId = bind.entityId;
        link.isPrimary = true;
      }
      else
      {
        link.unresolved = true;
        link.note = bind.candidates.size() >= 2
                        ? candidatesNote(cat, bind.candidates)
                        : unmatchedNameNote(tried);
      }
      if (!cat->addLink(link, error))
        return fail(*error);
    }
  }
  else if (cls.type == QLatin1String("horizon"))
  {
    const bool known = isKnownSequenceBoundary(stem);
    const QString sbId = QStringLiteral("sb-%1").arg(stem.toUpper());
    if (!cat->hasEntity(sbId))
    {
      CatalogEntity sb;
      sb.id = sbId;
      sb.entityType = QStringLiteral("sequence_boundary");
      sb.name = stem.toUpper();
      if (!known)
        sb.extra.insert(QStringLiteral("pending"), true); // 未决层位，不进编图 chip
      if (!cat->addEntity(sb, error))
        return fail(*error);
    }
    EntityAssetLink link;
    link.entityType = QStringLiteral("sequence_boundary");
    link.entityId = sbId;
    link.assetId = assetId;
    link.role = QStringLiteral("horizon");
    link.isPrimary = true;
    link.unresolved = !known;
    if (!cat->addLink(link, error))
      return fail(*error);

    // 已知界面：装箱派生时间栅格（DERIVED，父版本=RAW）并登记图层清单。
    if (known)
    {
      QFile f(sourcePath);
      if (!f.open(QIODevice::ReadOnly))
        return fail(QStringLiteral("cannot read %1").arg(sourcePath));
      BinnedHorizon binned;
      if (!binHorizon(f.readAll(), &binned, error))
        return fail(*error);

      const QString derivedVersionId =
          cat->nextVersionId();
      const QString tifName = stem.toUpper() + QStringLiteral(".tif");
      const QString derivedRel = DataCatalog::managedPath(QStringLiteral("derived"), assetId,
                                                          derivedVersionId, tifName);
      if (derivedRel.isEmpty())
        return fail(QStringLiteral("派生文件名不是合法路径段: %1").arg(tifName));
      const QString relPath = QStringLiteral("artifacts/") + derivedRel;
      CatalogVersion pending;
      pending.managed = true;
      pending.path = relPath;
      // 最终位置（提交后）——图层声明/金字塔用它；字节先写进暂存根。
      const QString tifPath = DataCatalog::resolvedVersionPath(s.projectDir, pending);
      if (tifPath.isEmpty())
        return fail(QStringLiteral("unsafe managed destination: %1").arg(relPath));
      if (!s.ensureStagingRoot(error))
        return fail(*error);
      const QString stagedTif = DataCatalog::resolvedVersionPath(s.stagingRoot, pending);
      if (stagedTif.isEmpty())
        return fail(QStringLiteral("unsafe managed destination: %1").arg(relPath));
      if (!QDir().mkpath(QFileInfo(stagedTif).absolutePath()))
        return fail(QStringLiteral("cannot create directory %1")
                        .arg(QFileInfo(stagedTif).absolutePath()));
      if (!writeHorizonGeoTiff(binned, stagedTif, error))
        return fail(*error);
      QFile::setPermissions(stagedTif, QFileDevice::ReadOwner | QFileDevice::ReadUser |
                                           QFileDevice::ReadGroup | QFileDevice::ReadOther);
      // B3：派生时间栅格同批接线——Lazy ensure 瓦片金字塔（近零开销）。提交
      // 后在 owner 线程对最终路径执行（暂存路径不进金字塔身份表）。
      s.pyramidTargets.append(tifPath);

      CatalogVersion derived;
      derived.id = derivedVersionId;
      derived.assetId = assetId;
      derived.stage = QStringLiteral("DERIVED");
      derived.versionNumber = 2;
      derived.managed = true;
      derived.path = relPath;
      derived.sourceUri = fi.absoluteFilePath();
      derived.fileName = stem.toUpper() + QStringLiteral(".tif");
      derived.parentVersionIds = QStringList{versionId};
      derived.extra.insert(QStringLiteral("collisions"), binned.collisions);
      derived.extra.insert(QStringLiteral("rejected"), binned.rejected);
      derived.extra.insert(QStringLiteral("grid_rows"), binned.rows);
      derived.extra.insert(QStringLiteral("grid_cols"), binned.cols);
      derived.extra.insert(QStringLiteral("z_units"), QStringLiteral("ms"));
      derived.extra.insert(QStringLiteral("z_min"), binned.zMin);
      derived.extra.insert(QStringLiteral("z_max"), binned.zMax);
      derived.extra.insert(QStringLiteral("filled_cells"), binned.filledCells);
      if (!cat->addVersion(derived, error))
        return fail(*error);

      // 图层清单只登记要画的结果（§2）：北向上时间栅格 + 局部测网 CRS。
      LayerDeclaration decl;
      decl.layerId = QStringLiteral("horizon.%1").arg(stem.toUpper());
      decl.horizon = stem.toUpper();
      decl.type = QStringLiteral("raster");
      decl.source = tifPath;
      decl.group = QStringLiteral("00_Data");
      // declare 写 layer manifest（sqlite）——提交成功后由 owner 线程发射。
      s.recordLayerDeclared(decl);
      manifestLayerId = decl.layerId;
    }
  }
  else if (cls.type == QLatin1String("seismic"))
  {
    // 打开（索引式）冻结 survey 几何：角点、inline/crossline 范围、采样间隔、起始时间。
    SegyReader reader;
    QString serr;
    if (!reader.open(sourcePath, &serr))
      return fail(serr);
    const SegyGeometry g = reader.geometry();
    const QString surveyId = QStringLiteral("survey-%1").arg(stem);
    if (!cat->hasEntity(surveyId))
    {
      CatalogEntity s;
      s.id = surveyId;
      s.entityType = QStringLiteral("seismic_survey");
      s.name = stem;
      s.inlineMin = g.inlineMin;
      s.inlineMax = g.inlineMax;
      s.xlineMin = g.xlineMin;
      s.xlineMax = g.xlineMax;
      s.sampleIntervalUs = reader.sampleIntervalUs();
      s.startTimeMs = g.startTimeMs;
      for (int i = 0; i < 4; ++i)
        s.corners.append({g.cornerX[i], g.cornerY[i]});
      if (!cat->addEntity(s, error))
        return fail(*error);
    }
    EntityAssetLink link;
    link.entityType = QStringLiteral("seismic_survey");
    link.entityId = surveyId;
    link.assetId = assetId;
    link.role = QStringLiteral("seismic_volume");
    link.isPrimary = true;
    if (!cat->addLink(link, error))
      return fail(*error);
  }
  else
  {
    // document / image_reference / geojson / unknown / 参考资料 XML：辅助实体 + reference。
    const QString auxId =
        cat->nextEntityId(QStringLiteral("aux"));
    CatalogEntity aux;
    aux.id = auxId;
    aux.entityType = QStringLiteral("auxiliary");
    aux.name = stem;
    if (cls.type == QLatin1String("geojson"))
    {
      aux.extra.insert(QStringLiteral("georeferenced"), false); // 未配准，不生成地图图层
      // 相/亚相/微相名称收成图例字典（阶段 D）。
      QFile gf(sourcePath);
      if (gf.open(QIODevice::ReadOnly))
      {
        const QJsonDocument doc = QJsonDocument::fromJson(gf.readAll());
        const QJsonArray feats = doc.object().value(QStringLiteral("features")).toArray();
        QVariantMap legend;
        for (const QJsonValue &fv : feats)
        {
          const QJsonObject props = fv.toObject().value(QStringLiteral("properties")).toObject();
          for (auto it = props.begin(); it != props.end(); ++it)
          {
            if (!it.key().contains(QString::fromUtf8("相")))
              continue;
            QStringList vals = legend.value(it.key()).toStringList();
            const QString v = it.value().toString();
            if (!v.isEmpty() && !vals.contains(v))
              vals.append(v);
            legend.insert(it.key(), vals);
          }
        }
        aux.extra.insert(QStringLiteral("legend"), legend);
      }
    }
    if (!cat->addEntity(aux, error))
      return fail(*error);
    EntityAssetLink link;
    link.entityType = QStringLiteral("auxiliary");
    link.entityId = auxId;
    link.assetId = assetId;
    link.role = auxRefRole;
    link.isPrimary = true;
    if (!cat->addLink(link, error))
      return fail(*error);
  }

  // B3（wave/deepen-perf）：栅格资产（image_reference 的 tif/png/jpg）入库即
  // Lazy ensure 瓦片金字塔——近零开销（只铺目录/状态表），消费侧首次取瓦片
  // 才付构建成本。受管副本与外链源都适用：金字塔落工程缓存根，不动源文件。
  if (cls.type == QLatin1String("image_reference"))
  {
    const QString ext = fi.suffix().toLower();
    if (ext == QLatin1String("tif") || ext == QLatin1String("tiff") ||
        ext == QLatin1String("png") || ext == QLatin1String("jpg") ||
        ext == QLatin1String("jpeg"))
    {
      // 受管副本取提交后的最终位置；ensure 在 owner 线程提交成功后执行。
      const QString rasterAbs =
          external ? version.path
                   : DataCatalog::resolvedVersionPath(s.projectDir, version);
      if (!rasterAbs.isEmpty())
        s.pyramidTargets.append(rasterAbs);
    }
  }

  s.recordImported(cls.type, assetId, manifestLayerId);
  return done(ImportOutcome::Imported, assetId);
}

// ---------------------------------------------------------------------------
// §3/§4「导入工区文件夹」：递归枚举普通文件（分类依赖 井位/井分层/时深/层位
// 路径段），两阶段处理——全部 well_head 行先走（井建齐），其余文件再对已齐
// 的井集解析。单行失败只落行、不中断；逃出所选根目录的符号链接与非普通文
// 件标 Skipped。dedup（AlreadyStored）记 Imported，message 留「字节已在库」。
// ---------------------------------------------------------------------------
// 文件夹枚举/分类/shp 归组/身份匹配/去重/两阶段排序已并入 C 包 plan 构建器
// （io/ingestplan.cpp 的 buildIngestPlan）——previewFolder 与 importFolder
// 共用同一份 plan，确认表逐行看到的就是将要执行的决策。
// ---------------------------------------------------------------------------

QVector<FolderPreviewRow>
DataImportService::previewFolder(const QString &dirPath, QString *error)
{
  return previewFolder(dirPath, error, {});
}

QVector<FolderPreviewRow>
DataImportService::previewFolder(const QString &dirPath, QString *error,
                                 const std::function<bool(int, const QString &)> &scanProgress)
{
  if (error)
    error->clear();
  // 审计 02 M-8：预览也只读 session 的 staging 副本；后台预览走
  // beginImport + producePreview（FolderImportWorkflow::previewFolderAsync）。
  const std::shared_ptr<ImportSession> s = beginImport();
  if (!s)
  {
    setError(error, offThreadError());
    return {};
  }
  return producePreview(*s, dirPath, error, scanProgress);
}

QVector<FolderPreviewRow>
DataImportService::producePreview(ImportSession &sess, const QString &dirPath, QString *error,
                                  const std::function<bool(int, const QString &)> &scanProgress)
{
  QVector<FolderPreviewRow> rows;
  if (error)
    error->clear();
  if (dirPath.isEmpty() || !QFileInfo(dirPath).isDir())
  {
    setError(error, QStringLiteral("找不到目录: %1").arg(dirPath));
    return rows;
  }
  // C 包：预览即 plan。T2：经 COW 快照在当前线程构建（扫描/分类/族归组/
  // 身份匹配/去重/主建议）——GUI 直调等价旧路径，worker 调用（经
  // FolderImportWorkflow::previewFolderAsync）整段离 GUI。行序 = 执行序。
  const IngestPlan plan = planFor(sess, dirPath, scanProgress);
  if (plan.cancelled)
  {
    setError(error, QStringLiteral("已取消"));
    return rows; // 扫描段取消：不展示半份预览
  }
  if (plan.items.isEmpty() && plan.skipped.isEmpty())
  {
    setError(error, plan.issues.isEmpty()
                        ? QStringLiteral("目录里没有可导入的文件: %1").arg(dirPath)
                        : plan.issues.join(QStringLiteral("\n")));
    return rows;
  }
  // 确认表显示语义在预览期算好（domain/importrows.h）：行显示类型、可改
  // 与否、类型词表——视图纯渲染，不回查分类器谓词。
  const QStringList vocab = projectClassifierTypes(); // 「reference」伪类型含在内
  const auto fillDisplay = [&vocab](FolderPreviewRow &r) {
    // 行默认显示类型：HZ28-6-1 固定辅助 → 「参考」；「参考资料」目录内井类/
    // 未判内容默认「参考」（可改）；其余行显示分类器原类型。
    QString disp = r.classifiedType;
    if (isFixedAuxiliaryPath(r.path))
      disp = QStringLiteral("reference");
    else if (isDefaultReferencePath(r.path))
    {
      // document/image_reference/geojson/seismic/horizon 等显示真实类型——
      // 它们本来就走辅助实体，且类型名驱动预览分支（document → PDF 预览）。
      static const QSet<QString> kWellish = {QStringLiteral("well_head"),
                                             QStringLiteral("well_log"),
                                             QStringLiteral("unknown")};
      if (kWellish.contains(r.classifiedType))
        disp = QStringLiteral("reference");
    }
    if (disp.isEmpty())
      disp = QStringLiteral("unknown");
    r.displayType = disp;
    r.typeVocab = vocab;
    if (!r.classifiedType.isEmpty() && !vocab.contains(r.classifiedType))
      r.typeVocab.append(r.classifiedType); // 词表外类型（未来扩展）保住可选
    r.typeEditable = !r.skipped && !isFixedAuxiliaryPath(r.path);
  };
  for (const PlannedItem &item : plan.items)
  {
    FolderPreviewRow r;
    r.path = item.path;
    r.classifiedType = item.type;
    r.decision = item.decision; // plan 期决策（重复→跳过等）随行进确认表
    r.sizeBytes = item.size;    // T2 大小估算（族项 = 主件字节）
    fillDisplay(r);
    rows.append(r);
  }
  for (const PlannedItem &s : plan.skipped)
  {
    FolderPreviewRow r;
    r.path = s.path;
    r.classifiedType = s.type;
    r.skipped = true;
    r.skipReason = s.note;
    r.sizeBytes = s.size; // 枚举期跳过行也带大小（用户看得见它占了多少）
    fillDisplay(r);
    rows.append(r);
  }
  return rows;
}

QVector<FolderRowResult>
DataImportService::importFolder(const QString &dirPath, QString *error)
{
  return importFolder(dirPath, error, QMap<QString, QString>{}, {}, {});
}

QVector<FolderRowResult>
DataImportService::importFolder(
    const QString &dirPath, QString *error,
    const QMap<QString, QString> &typeOverrides,
    const std::function<bool(int, int, const QString &)> &progress)
{
  return importFolder(dirPath, error, typeOverrides, QStringList{}, progress);
}

QVector<FolderRowResult>
DataImportService::importFolder(
    const QString &dirPath, QString *error,
    const QMap<QString, QString> &typeOverrides,
    const QStringList &forceImportPaths,
    const std::function<bool(int, int, const QString &)> &progress)
{
  if (error)
    error->clear(); // 成功路径不写 error——先清掉调用方复用的旧值
  const auto s = runInline([&](ImportSession &sess) {
    produceFolder(sess, dirPath, typeOverrides, forceImportPaths, progress);
  });
  if (!s)
  {
    setError(error, offThreadError());
    return {};
  }
  setError(error, s->error);
  return s->rows;
}

void DataImportService::produceFolder(
    ImportSession &sess, const QString &dirPath, const QMap<QString, QString> &typeOverrides,
    const QStringList &forceImportPaths,
    const std::function<bool(int, int, const QString &)> &progress)
{
  sess.rows.clear();
  sess.error.clear();
  const auto finish = [&sess](QVector<FolderRowResult> rows, const QString &err) {
    sess.rows = std::move(rows);
    sess.error = err;
  };
  QVector<FolderRowResult> rows;
  if (!sess.wired)
    return finish(rows, QStringLiteral("import service is not fully wired"));
  if (sess.projectDir.isEmpty())
    return finish(rows, QStringLiteral("project dir is not set"));
  QString errText;
  QString *const error = &errText;

  if (dirPath.isEmpty() || !QFileInfo(dirPath).isDir())
    return finish(rows, QStringLiteral("找不到目录: %1").arg(dirPath));

  // C 包三段式：T2 起 plan 经 COW 快照在当前线程构建（扫描/分类/shp 归组/
  // 身份匹配/去重/主建议不再 marshal 回 GUI）。扫描期进度复用 progress 回
  // 调——total=0 标记扫描段（调用方区分跑马灯/行进度）。阶段 1 全部
  // well_head（井建齐）、阶段 2 其余——序在 builder 里已排好。
  IngestPlan plan;
  if (progress)
  {
    const auto scanCb = [&progress](int seen, const QString &p) {
      return progress(seen, 0, p); // total=0：plan 扫描段
    };
    plan = planFor(sess, dirPath, scanCb);
  }
  else
    plan = planFor(sess, dirPath);
  if (plan.cancelled)
  {
    // 扫描段取消：零行执行零行入库（执行段取消另有「保留已处理行」语义）。
    return finish(rows, QStringLiteral("已取消"));
  }
  if (plan.items.isEmpty() && plan.skipped.isEmpty())
  {
    return finish(rows, plan.issues.isEmpty()
                            ? QStringLiteral("目录里没有可导入的文件: %1").arg(dirPath)
                            : plan.issues.join(QStringLiteral("\n")));
  }

  // 确认表「改类型」：合法 override 并进 item.type（生效类型）；非法 override
  // 忽略——不落进资产类型。D5：阶段归属按生效类型算——改成 well_head 的行
  // 回阶段 1，井建得够早排前的阶段 2 行仍能挂上。
  bool retype = false;
  for (PlannedItem &item : plan.items)
  {
    const QString o = typeOverrides.value(item.path);
    if (isClassifierType(o) && o != item.type)
    {
      item.type = o;
      retype = true;
    }
  }
  if (retype)
    orderIngestPlanItems(plan.items);

  // 「仍导入」改判（T2）：只翻 plan 期 sha 重复行（decision==skip 且带
  // duplicateOfVersionId）；枚举期跳过行（软链逃逸等）不在用户改判面。
  if (!forceImportPaths.isEmpty())
    for (PlannedItem &item : plan.items)
      if (forceImportPaths.contains(item.path) && item.decision == QLatin1String("skip") &&
          !item.duplicateOfVersionId.isEmpty())
      {
        item.decision = QStringLiteral("as_new_version");
        item.note =
            (item.note.isEmpty() ? QString() : item.note + QStringLiteral("；")) +
            QStringLiteral("用户改判：仍导入");
      }

  // 执行面：executeIngestPlan 逐项 produceItem（写 staging 副本）+ progress
  // 协作取消；入库在 owner 线程 commitImport 一次事务落盘。幂等——
  // decision==skip 或 (path,sha) 已注册的项不再登记；重跑同目录每行
  // Skipped、目录零增量。
  rows = executeIngestPlan(plan, sess, progress, error);
  finish(std::move(rows), errText);
}

// ---------------------------------------------------------------------------
// C 包执行面：单条 plan 项 → 行结果（executeIngestPlan 逐项调它；确认表
// 「重试」经 folderRowFor 合成的单项也走这里）。FolderRowResult::
// classifiedType 记生效类型（override 已并进 item.type），与确认表显示一致。
// ---------------------------------------------------------------------------
FolderRowResult
DataImportService::executePlannedItem(const PlannedItem &item, QString *error)
{
  if (error)
    error->clear();
  FolderRowResult row;
  QString perr;
  const auto s = runInline([&](ImportSession &sess) {
    row = produceItem(sess, item, &perr);
    sess.rows = {row};
    sess.error = perr;
  });
  if (!s)
  {
    setError(error, offThreadError());
    row = FolderRowResult{};
    row.path = item.path;
    row.classifiedType = item.type;
    row.outcome = FolderRowResult::Outcome::Failed;
    row.message = offThreadError();
    return row;
  }
  // 提交失败会把行改写成 Failed（failSession）。
  row = s->rows.value(0, row);
  setError(error, s->error);
  return row;
}

FolderRowResult
DataImportService::produceItem(ImportSession &s, const PlannedItem &item, QString *error)
{
  if (error)
    error->clear();
  DataCatalog *const cat = s.cat.get();
  FolderRowResult row;
  row.path = item.path;
  row.classifiedType = item.type;

  // T33 符号链接 TOCTOU 终验：真正读字节的是 importOneFile 里的 open/hash/
  // copy——入它之前再核一次：路径仍解析到枚举时判定的同一 canonical、仍是
  // 普通文件；被改指向的链接/被换掉的文件如实跳过。
  if (!item.canonicalPath.isEmpty())
  {
    const QFileInfo now(item.path);
    if (!now.isFile() || now.canonicalFilePath() != item.canonicalPath)
    {
      row.outcome = FolderRowResult::Outcome::Skipped;
      row.message = QStringLiteral(
          "文件在导入前已变化（符号链接改指向或不再是普通文件），已跳过");
      return row;
    }
  }

  if (item.decision == QLatin1String("skip"))
  {
    row.outcome = FolderRowResult::Outcome::Skipped;
    row.message = !item.duplicateOfVersionId.isEmpty()
                      ? QStringLiteral("重复→跳过")
                      : (item.note.isEmpty() ? QStringLiteral("已跳过")
                                             : item.note);
    // shp 族补齐：重复跳过的族顺带把缺的成员拷进已注册版本目录（幂等修复）。
    if (!item.members.isEmpty() && !item.duplicateOfVersionId.isEmpty())
      copyBundleMembersIntoVersion(s, item, item.duplicateOfVersionId, &row.message);
    return row;
  }

  // 幂等：同一（源路径, sha256）已注册 → 不再导入。as_new_version 有意绕过
  // ——显式「仍导入」的同字节结局交内部 dedup（AlreadyStored + 补挂）。
  // 路径按字符串口径比对（不解析符号链接）：link 路径≠目标路径，mirror 文件
  // 仍走 dedup 结局行，与旧行为一致。
  if (!item.sha256.isEmpty() && item.decision != QLatin1String("as_new_version"))
  {
    const CatalogVersion hit =
        cat->versionBySha256(item.sha256);
    if (!hit.id.isEmpty() &&
        QDir::cleanPath(hit.sourceUri) == QDir::cleanPath(item.path))
    {
      row.outcome = FolderRowResult::Outcome::Skipped;
      row.message = QStringLiteral("已在库，幂等跳过");
      if (!item.members.isEmpty())
        copyBundleMembersIntoVersion(s, item, hit.id, &row.message);
      return row;
    }
  }

  QString ferr;
  ImportOptions opts;
  opts.forceType = item.type; // 恒下传——与分类器同值时是无害同值
  const ImportResult res = importOneFile(s, item.path, opts, &ferr);
  row.message = res.message.isEmpty() ? ferr : res.message;
  if (res.outcome == ImportOutcome::Failed || res.assetId.isEmpty())
  {
    row.outcome = FolderRowResult::Outcome::Failed;
    if (row.message.isEmpty())
      row.message = QStringLiteral("导入失败");
    return row;
  }

  // 确认表口径（autoplan-dx）：未决=资产已存但实体 id 全空（没有任何已决
  // 关联——被同批新主关联降级的旧关联实体 id 仍非空，不算未决）；入库=写
  // 成了主关联；dedup 命中也记 Imported（message 已是「字节已在库」）。
  struct ResolveSummary
  {
    QStringList names;
    QStringList notes;
    int resolved = 0;
  };
  ResolveSummary summary;
  for (const EntityAssetLink &l : cat->linksForAsset(res.assetId))
  {
    if (l.unresolved)
    {
      if (!l.note.isEmpty() && !summary.notes.contains(l.note))
        summary.notes.append(l.note);
      continue;
    }
    ++summary.resolved;
    const CatalogEntity e = cat->entityById(l.entityId);
    const QString n = e.name.isEmpty() ? l.entityId : e.name;
    if (!n.isEmpty() && !summary.names.contains(n))
      summary.names.append(n);
  }
  row.entityName = summary.names.join(QStringLiteral(", "));
  row.outcome = res.outcome == ImportOutcome::Imported && summary.resolved == 0
                    ? FolderRowResult::Outcome::Unresolved
                    : FolderRowResult::Outcome::Imported;
  if (!summary.notes.isEmpty())
    row.message = row.message.isEmpty()
                      ? summary.notes.join(QStringLiteral("；"))
                      : row.message + QStringLiteral("；") +
                            summary.notes.join(QStringLiteral("；"));

  // shp 族：主件入库成功后把其余成员拷进同一受管 RAW 版本目录（外链版本的
  // 成员本就在源目录相邻，copyBundleMembersIntoVersion 自己不拷）。
  if (!item.members.isEmpty() && res.outcome == ImportOutcome::Imported)
  {
    CatalogVersion raw;
    for (const CatalogVersion &v : cat->versionsForAsset(res.assetId))
      if (v.stage == QLatin1String("RAW") && v.managed)
      {
        raw = v;
        break;
      }
    copyBundleMembersIntoVersion(s, item, raw.id, &row.message);
  }
  return row;
}

// shp 族成员落位：把 members 里除主件外的文件拷进指定受管版本目录。
// 幂等——已存在同名成员跳过（受管文件只读，不覆盖）；失败成员名附进
// *messageOut。非受管版本（外链）不拷：成员文件本就在源目录与主件相邻。
void DataImportService::copyBundleMembersIntoVersion(ImportSession &s, const PlannedItem &item,
                                                     const QString &versionId,
                                                     QString *messageOut)
{
  if (item.members.size() < 2 || versionId.isEmpty())
    return;
  const CatalogVersion v = s.cat->versionById(versionId);
  if (v.id.isEmpty() || !v.managed)
    return;
  // 本 session 新建的版本落在暂存根（随提交一起 rename）；已入库版本是
  // 幂等补齐缺失成员（只增不改，与旧行为一致）。
  const QString abs = s.cat->versionFilePath(v);
  if (abs.isEmpty())
    return;
  const QDir dir = QFileInfo(abs).absoluteDir();
  QStringList failed;
  for (const QString &member : item.members)
  {
    if (QDir::cleanPath(member) == QDir::cleanPath(item.path))
      continue; // 主件已是版本文件本体
    const QString dst = dir.filePath(QFileInfo(member).fileName());
    if (QFileInfo::exists(dst))
      continue; // 幂等：已在位不重拷
    if (!QFile::copy(member, dst))
      failed.append(QFileInfo(member).fileName());
    else
      QFile::setPermissions(dst, QFileDevice::ReadOwner | QFileDevice::ReadUser |
                                 QFileDevice::ReadGroup | QFileDevice::ReadOther);
  }
  if (!failed.isEmpty() && messageOut)
    *messageOut += (messageOut->isEmpty() ? QString() : QStringLiteral("；")) +
                   QStringLiteral("族成员复制失败: ") +
                   failed.join(QStringLiteral(", "));
}

// 「重试」/兼容入口合成 1 项：决策恒 as_new_version——显式单文件导入的语义
// 是「仍导入」，同字节结局由内部 dedup 消化（AlreadyStored + 补挂），
// registered-check 对它不生效。
FolderRowResult
DataImportService::folderRowFor(const QString &path, const QString &classifiedType,
                                const QString &effectiveType)
{
  Q_UNUSED(classifiedType); // 生效类型已含 override 口径，分类器原值不再用
  PlannedItem item;
  item.path = path;
  item.type = effectiveType;
  item.decision = QStringLiteral("as_new_version");
  return executePlannedItem(item, nullptr);
}

FolderRowResult
DataImportService::importFolderRow(const QString &sourcePath, const QString &forceType,
                                   QString *error)
{
  if (error)
    error->clear();
  // 与 plan 枚举同一分类口径：.xml 要看内容判定。
  const QByteArray xml =
      QFileInfo(sourcePath).suffix().compare(QLatin1String("xml"), Qt::CaseInsensitive) == 0
          ? readFileOrEmpty(sourcePath).toUtf8()
          : QByteArray();
  const QString classified = classifyProjectImport(sourcePath, xml).type;
  const QString eff = isClassifierType(forceType) ? forceType : classified;
  const FolderRowResult row = folderRowFor(sourcePath, classified, eff);
  if (row.outcome == FolderRowResult::Outcome::Failed)
    setError(error, row.message); // 成功路径不写 error（同 importFolder）
  return row;
}

// ---------------------------------------------------------------------------
// §3 dedup 补挂：同一 SHA-256 再导入时不新建资产/版本；只把「现在恰好匹配
// 一口井」的未决链接挂上去。名称来源与原导入各分支一致——SHA-256 相同 ⇒
// 解析出的井名与顺序一致，未决链接按 links() 序与之一一配对。不建井、不并井。
// ---------------------------------------------------------------------------
int DataImportService::attachResolvableLinks(DataCatalog *cat, const CatalogAsset &asset,
                                             const QString &sourcePath, QString *error)
{
  if (asset.id.isEmpty())
    return 0;
  const QString stem = QFileInfo(sourcePath).completeBaseName();

  // 每条（同资产、well 型）链接按创建序对应一组按序尝试的井名。
  QVector<QStringList> namesPerLink;
  if (asset.type == QLatin1String("well_log"))
  {
    QString wellName;
    if (asset.format == QLatin1String("las"))
      LasParser::readWellInfo(sourcePath, wellName);
    namesPerLink.append({wellName, stem}); // ~W WELL → 文件名主名（D12：UWI 层已删）
  }
  else if (asset.type == QLatin1String("well_stratification"))
  {
    QFile f(sourcePath);
    if (!f.open(QIODevice::ReadOnly))
    {
      setError(error, QStringLiteral("cannot read %1").arg(sourcePath));
      return 0;
    }
    QStringList names;
    for (const WellTopRecord &t : parseWellTopsText(f.readAll()))
      if (!names.contains(t.wellName))
        names.append(t.wellName);
    for (const QString &n : names)
      namesPerLink.append(names.size() == 1 ? QStringList{n, stem} : QStringList{n});
  }
  else if (asset.type == QLatin1String("time_depth"))
  {
    QFile f(sourcePath);
    if (!f.open(QIODevice::ReadOnly))
    {
      setError(error, QStringLiteral("cannot read %1").arg(sourcePath));
      return 0;
    }
    const TimeDepthTable td = parseTimeDepthText(f.readAll());
    namesPerLink.append({td.wellName.isEmpty() ? stem : td.wellName, stem});
  }
  else if (asset.type == QLatin1String("well_head"))
  {
    QFile f(sourcePath);
    if (!f.open(QIODevice::ReadOnly))
    {
      setError(error, QStringLiteral("cannot read %1").arg(sourcePath));
      return 0;
    }
    const QVector<WellHeadRecord> rows = parseWellHeadText(f.readAll());
    QHash<QString, int> normRowCount;
    for (const WellHeadRecord &r : rows)
      normRowCount[DataCatalog::normalizeWellName(r.name)] += 1;
    for (const WellHeadRecord &r : rows)
      // 文件内规范化重名的行在原导入就是「井口重名」未决——仍歧义，不补挂。
      namesPerLink.append(normRowCount.value(DataCatalog::normalizeWellName(r.name)) >= 2
                              ? QStringList{}
                              : QStringList{r.name});
  }
  else
    return 0; // seismic/horizon/auxiliary：实体在入库时已确定，dedup 不补井关联。

  const QVector<EntityAssetLink> links = cat->links(); // 快照（attachLink 不增删）
  // T33：一次 dedup 可能连挂多条链接——并入批次（嵌套在 importFolder 的
  // 外层批次里也安全，深度计数）。
  DataCatalog::BatchSave batch(cat);
  int nameIdx = 0, attached = 0;
  for (int i = 0; i < links.size() && nameIdx < namesPerLink.size(); ++i)
  {
    const EntityAssetLink &l = links.at(i);
    if (l.assetId != asset.id || l.entityType != QLatin1String("well"))
      continue;
    const QStringList &tried = namesPerLink.at(nameIdx++);
    if (!l.unresolved)
      continue;
    QString target;
    for (const QString &n : tried)
    {
      const QStringList ids = cat->wellsMatchingName(n);
      if (ids.size() == 1)
      {
        target = ids.front();
        break;
      }
      if (!ids.isEmpty())
        break; // 2+ 候选仍不决——与导入同一判据（不试下一个名字）
    }
    if (target.isEmpty())
      continue;
    // 该 (well, role) 已有已决主关联 → 没有新的关联可补。
    bool hasPrimary = false;
    for (const EntityAssetLink &o : cat->linksForEntity(target))
      if (o.role == l.role && o.isPrimary && !o.unresolved)
        hasPrimary = true;
    if (hasPrimary)
      continue;
    QString aerr;
    if (!cat->attachLink(i, target, &aerr))
    {
      setError(error, aerr);
      continue;
    }
    ++attached;
  }
  // 批次结算：落盘失败如实透给调用方（dedup 路径只记 qWarning，不中断）。
  QString berr;
  if (!batch.flush(&berr))
  {
    // #169：结算失败时批内改动已回滚，不能再报「已补上关联」。
    setError(error, berr.isEmpty() ? QStringLiteral("catalog 落盘失败") : berr);
    return 0;
  }
  return attached;
}

QString DataImportService::importFile(const QString &kind, const QString &sourcePath, QString *error)
{
  // 旧签名：kind 只透传给信号（由 importProjectFile 内部再次发射真实类型）。
  Q_UNUSED(kind);
  return importProjectFile(sourcePath, ImportOptions{}, error);
}

QString DataImportService::absolutePath(const QString &assetId) const
{
  return absolutePathForVersion(m_catalog->currentVersion(assetId));
}

QString DataImportService::absolutePathForVersion(const CatalogVersion &v) const
{
  if (v.path.isEmpty())
    return QString();
  if (!v.managed)
    return v.path;
  return DataCatalog::resolvedVersionPath(m_projectDir, v);
}

// ---------------------------------------------------------------------------
// 文档 PDF 预览：doc/docx/ppt/pptx 经 LibreOffice headless 转 PDF，落受管
// DERIVED 版本（parent=RAW），预览标签页用 QtPdf 渲染。原件仍是规范来源；
// 转换失败/无 soffice 如实 Failed，UI 降级为「用系统程序打开」。
// ---------------------------------------------------------------------------

void DataImportService::resolveDocumentConverter()
{
  if (m_converterResolved)
    return;
  m_converter = QStandardPaths::findExecutable(QStringLiteral("soffice"));
  if (m_converter.isEmpty())
    m_converter = QStandardPaths::findExecutable(QStringLiteral("libreoffice"));
  m_converterResolved = true;
}

void DataImportService::setDocumentConverterProgram(const QString &program)
{
  m_converter = program;
  m_converterResolved = true;
}

DataImportService::DocPdfState
DataImportService::documentPdfState(const QString &assetId) const
{
  for (const CatalogVersion &v : m_catalog->versionsForAsset(assetId))
    if (v.stage == QLatin1String("DERIVED") &&
        v.fileName.endsWith(QLatin1String(".pdf"), Qt::CaseInsensitive))
      return DocPdfState::Ready;
  if (m_pdfPending.contains(assetId))
    return DocPdfState::Pending;
  if (m_pdfErrors.contains(assetId))
    return DocPdfState::Failed;
  return DocPdfState::None;
}

QString DataImportService::documentPdfPath(const QString &assetId) const
{
  for (const CatalogVersion &v : m_catalog->versionsForAsset(assetId))
    if (v.stage == QLatin1String("DERIVED") &&
        v.fileName.endsWith(QLatin1String(".pdf"), Qt::CaseInsensitive))
      return absolutePathForVersion(v);
  return QString();
}

QString DataImportService::documentPdfError(const QString &assetId) const
{
  return m_pdfErrors.value(assetId);
}

void DataImportService::ensureDocumentPdf(const QString &assetId)
{
  if (documentPdfState(assetId) != DocPdfState::None)
    return;

  const auto failNow = [this, &assetId](const QString &msg) {
    m_pdfErrors.insert(assetId, msg);
    emit documentPdfFailed(assetId, msg);
  };

  resolveDocumentConverter();
  if (m_converter.isEmpty())
    return failNow(tr("找不到 LibreOffice（soffice）——无法生成 PDF 预览"));

  QString rawAbs, rawVersionId;
  CatalogVersion rawVersion;
  for (const CatalogVersion &v : m_catalog->versionsForAsset(assetId))
    if (v.stage == QLatin1String("RAW"))
    {
      rawAbs = absolutePathForVersion(v);
      rawVersionId = v.id;
      rawVersion = v;
      break;
    }
  if (rawAbs.isEmpty() || !QFile::exists(rawAbs))
    return failNow(tr("原始文件缺失，无法转换"));

  // T8 staleness 补漏：外链 RAW 带指纹时转换前复验——soffice 读的是当前
  // 字节，源被改后转出的 DERIVED 与入库指纹无血缘；失配如实失败 + 下游
  // DERIVED 标过时（与剖面解码路径同一纪律，previewdoc.cpp 的两处之外的
  // 第三个失配入口）。
  if (!rawVersion.managed && !rawVersion.sha256.isEmpty())
  {
    QString verr;
    if (!m_catalog->verifyExternalVersionSha(rawVersion, &verr))
    {
      QString markErr;
      if (!m_catalog->markDownstreamStale(rawVersion.id,
                                          QStringLiteral("上游外链版本 sha 校验失败"),
                                          &markErr))
        qWarning() << "markDownstreamStale:" << markErr;
      return failNow(verr);
    }
  }

  m_pdfPending.insert(assetId);
  m_pdfQueue.append(assetId);
  startNextDocumentPdf();
}

void DataImportService::startNextDocumentPdf()
{
  if (m_pdfProc || m_pdfQueue.isEmpty())
    return;

  m_pdfCurrent = m_pdfQueue.takeFirst();
  m_pdfCurrentVersionId = m_catalog->nextVersionId();

  QString rawAbs;
  for (const CatalogVersion &v : m_catalog->versionsForAsset(m_pdfCurrent))
    if (v.stage == QLatin1String("RAW"))
    {
      rawAbs = absolutePathForVersion(v);
      m_pdfRawVersionId = v.id;
      break;
    }

  const QString relDir = QStringLiteral("artifacts/derived/%1/%2")
                             .arg(m_pdfCurrent, m_pdfCurrentVersionId);
  const QString outDir = QDir(m_projectDir).absoluteFilePath(relDir);
  if (!QDir().mkpath(outDir))
    return finishDocumentPdf(-1);
  m_pdfOutFile = outDir + QLatin1Char('/') +
                 QFileInfo(rawAbs).completeBaseName() + QStringLiteral(".pdf");

  // 独立 UserInstallation：避开 LibreOffice 单实例 profile 锁，且 URL 合规。
  const QString profileDir = QDir::temp().filePath(
      QStringLiteral("paleo-lo-profile-%1").arg(QCoreApplication::applicationPid()));
  const QString profile = QStringLiteral("-env:UserInstallation=") +
                          QUrl::fromLocalFile(profileDir).toString();

  m_pdfProc = new QProcess(this);
  connect(m_pdfProc, qOverload<int, QProcess::ExitStatus>(&QProcess::finished),
          this, [this](int code, QProcess::ExitStatus status) {
            finishDocumentPdf(status == QProcess::NormalExit ? code : -1);
          });
  m_pdfProc->start(m_converter,
                   {QStringLiteral("--headless"), QStringLiteral("--norestore"),
                    profile, QStringLiteral("--convert-to"), QStringLiteral("pdf"),
                    QStringLiteral("--outdir"), outDir, rawAbs});
}

void DataImportService::finishDocumentPdf(int exitCode)
{
  const QString assetId = m_pdfCurrent;
  QString err;
  bool ok = false;

  if (exitCode == 0 && QFile::exists(m_pdfOutFile))
  {
    QFile f(m_pdfOutFile);
    if (f.open(QIODevice::ReadOnly))
    {
      QCryptographicHash hash(QCryptographicHash::Sha256);
      hash.addData(&f);
      f.close();
      QFile::setPermissions(m_pdfOutFile, QFileDevice::ReadOwner |
                                              QFileDevice::ReadUser |
                                              QFileDevice::ReadGroup |
                                              QFileDevice::ReadOther);

      CatalogVersion d;
      d.id = m_pdfCurrentVersionId;
      d.assetId = assetId;
      d.stage = QStringLiteral("DERIVED");
      d.versionNumber = m_catalog->currentVersion(assetId).versionNumber + 1;
      d.managed = true;
      d.path = QStringLiteral("artifacts/derived/%1/%2/%3")
                   .arg(assetId, m_pdfCurrentVersionId, QFileInfo(m_pdfOutFile).fileName());
      d.sourceUri = m_converter;
      d.sha256 = QString::fromLatin1(hash.result().toHex());
      d.fileName = QFileInfo(m_pdfOutFile).fileName();
      d.parentVersionIds = QStringList{m_pdfRawVersionId};
      d.extra.insert(QStringLiteral("generator"), QStringLiteral("libreoffice"));
      ok = m_catalog->addVersion(d, &err);
    }
    else
      err = f.errorString();
  }
  else
    err = m_pdfProc ? QString::fromLocal8Bit(m_pdfProc->readAllStandardError()).trimmed()
                    : tr("无法创建输出目录");
  if (err.isEmpty() && !ok)
    err = tr("soffice 退出码 %1，未产出 PDF").arg(exitCode);

  if (!ok && QFile::exists(m_pdfOutFile))
    QFile::remove(m_pdfOutFile);

  if (m_pdfProc)
  {
    m_pdfProc->deleteLater();
    m_pdfProc = nullptr;
  }
  m_pdfCurrent.clear();
  m_pdfPending.remove(assetId);

  if (ok)
    emit documentPdfReady(assetId);
  else
  {
    m_pdfErrors.insert(assetId, err);
    emit documentPdfFailed(assetId, err);
  }
  startNextDocumentPdf();
}

QStringList DataImportService::assets(const QString &type) const
{
  QStringList out;
  for (const CatalogAsset &a : m_catalog->assets())
    if (type.isEmpty() || a.type == type)
      out.append(a.id);
  return out;
}

QString DataImportService::assetSource(const QString &assetId) const
{
  return absolutePath(assetId);
}

// ---------------------------------------------------------------------------
// wave4/runtime-resilience：外链源重定位（TODOS P3「重新定位文件」恢复路径）。
// 「找不到源文件」死胡同的出口——但出口不是换内容：流式重算候选文件 SHA-256，
// 与该版本入库时留底一致才接受。版本记录不可变：不改写旧记录，而是追加一条
// 同内容、指向新路径的外链 RAW 版本（extra.relocatedFrom 留血统），
// currentVersion（取最高 versionNumber）从此解析到新路径；dedup 的
// versionBySha256 只认文件仍在且重哈希一致的版本——死路径旧记录自动出局，
// 重导/预览两条链路都不需要特判。不一致 → 拒解，catalog 一字不动（不静默
// 换源）。新路径落在工程目录内也仍按 external 记（不升级为 managed——工程
// 目录内容物是 catalog 自己的产物，混入外部文件会破坏受管面语义）。
// ---------------------------------------------------------------------------
QString DataImportService::relocateVersionSource(const QString &versionId,
                                                 const QString &newPath, QString *error)
{
  QString internalError;
  if (!error)
    error = &internalError;
  else
    error->clear();
  const auto fail = [&](const QString &msg) -> QString {
    setError(error, msg);
    return QString();
  };

  // 审计 02 M-8：活 catalog 只在 owner 线程读写——跨线程调用如实拒绝。
  if (QThread::currentThread() != m_catalog->thread())
    return fail(offThreadError());
  if (m_projectDir.isEmpty())
    return fail(QStringLiteral("project dir is not set"));
  if (!m_catalogReady)
    return fail(QStringLiteral("catalog 拒绝写入：%1")
                    .arg(m_catalogOpenError.isEmpty()
                             ? QStringLiteral("catalog 打开失败")
                             : m_catalogOpenError));
  const CatalogVersion v = m_catalog->versionById(versionId);
  if (v.id.isEmpty())
    return fail(QStringLiteral("版本不存在: %1").arg(versionId));
  if (v.managed)
    return fail(QStringLiteral("受管版本不支持重定位（受管文件属于工程目录，丢失应重导）: %1")
                    .arg(v.path));
  if (v.sha256.isEmpty())
    return fail(QStringLiteral("该版本入库时未留 SHA-256，无法核验新文件内容"));
  if (newPath.isEmpty() || !QFile::exists(newPath))
    return fail(QStringLiteral("找不到源文件: %1").arg(newPath));
  const QFileInfo fi(newPath);
  if (!fi.isFile())
    return fail(QStringLiteral("不是普通文件: %1").arg(newPath));
  if (!DataCatalog::isSafePathSegment(fi.fileName()))
    return fail(QStringLiteral("文件名不是合法路径段: %1").arg(fi.fileName()));
  const QString abs = fi.absoluteFilePath();
  if (abs == v.path)
    return versionId; // 幂等：同一文件已在原位恢复，不动 catalog

  // 流式 SHA-256 复验（与导入/外链校验同一面）。
  QString herr;
  const QString sha = ShaCache::shared().sha256Hex(abs, &herr); // D7.7
  if (sha.isEmpty())
    return fail(herr.isEmpty() ? QStringLiteral("cannot hash %1").arg(abs) : herr);
  if (sha.compare(v.sha256, Qt::CaseInsensitive) != 0)
    return fail(QStringLiteral("文件内容与原版本不符（SHA-256 不一致）——已拒绝重定位"));

  CatalogVersion relocated;
  relocated.id = m_catalog->nextVersionId();
  relocated.assetId = v.assetId;
  relocated.stage = v.stage.isEmpty() ? QStringLiteral("RAW") : v.stage;
  relocated.versionNumber =
      m_catalog->currentVersion(v.assetId).versionNumber + 1;
  relocated.managed = false;
  relocated.path = abs;
  relocated.sourceUri = abs;
  relocated.sha256 = v.sha256; // 已验证一致——沿用留底值
  relocated.fileName = fi.fileName();
  relocated.extra = v.extra;   // 外链夹带的图例等元数据随内容一起搬
  relocated.extra.insert(QStringLiteral("relocatedFrom"), v.id);
  if (!m_catalog->addVersion(relocated, error))
    return fail(error->isEmpty() ? QStringLiteral("catalog addVersion failed") : *error);
  qInfo("relocate: %s -> %s (asset %s, from version %s)", qPrintable(v.path),
        qPrintable(abs), qPrintable(v.assetId), qPrintable(v.id));
  return relocated.id;
}
