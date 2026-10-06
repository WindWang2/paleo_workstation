// 层：数据
// 方向57：dataimportservice.cpp 只留生命周期（构造/析构/工程切换）与各入口
// wrapper。格式族执行面在 dataimport_<族>.cpp；会话/登记基座在
// dataimport_session.cpp；公共 API 见 dataimportservice.h（零改动）。
#include "dataimportservice.h"

#include "../catalog/datacatalog.h"
#include "../metadata/paleoprojectstore.h"
#include "rasterpyramid.h"
#include "lascache.h"
#include "segyindexstore.h"
#include "shacache.h"

#include <QDir>
#include <QFile>
#include <QFileInfo>
#include <QProcess>
#include <QThread>

// ---------------------------------------------------------------------------
// project_area 导入契约（plan §3）：
//   classify → parse metadata → resolve/create entity → managed RAW（复制边算
//   SHA-256，落盘只读）/ 外部链接（SEG-Y 一律外链）→ 显式 entity_asset_link。
//   层位在 8 个层序界面集合内 → 派生时间栅格（DERIVED，父版本=RAW）并登记
//   LayerManifest（图层清单只登记要画进 QGIS 的结果）。
//   身份顺序：已有 id → UWI → 规范化井名 → 别名；文件名不作身份，仅作回退。
// ---------------------------------------------------------------------------

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
  // 方向57 阻塞解除：析构不再同步等待转换进程至多 2s（主线程事件循环冻结）。
  // 语义保留：kill() 照旧强杀；QProcess 交 deleteLater 异步回收（与
  // setProjectDir 的工程切换清理同一模式）。2s 等待上限的取消是本方向显式
  // 授权的行为变更——进程终止由 OS 异步完成，资源回收不阻塞析构。
  if (m_pdfProc)
  {
    m_pdfProc->disconnect(this);
    m_pdfProc->kill();
    m_pdfProc->deleteLater();
    m_pdfProc = nullptr;
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
