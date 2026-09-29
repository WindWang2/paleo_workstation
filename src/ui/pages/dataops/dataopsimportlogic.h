// 层：视图
// ui/pages/dataops/dataopsimportlogic — D8 导入流增强的纯逻辑面：
// 导入预设（常用目录/类型映射）、重复检测（SHA/同名）、目录预估、
// 失败重试队列（状态机）、导入摘要报告。
// 重活（解析/入库）不在此——队列条目持 std::function 执行钩子，由装配方
// 注入（生产 = 壳接 FolderImportWorkflow；测试 = 直写 catalog 的假钩子）。
#pragma once

#include <QCryptographicHash>
#include <QDir>
#include <QDirIterator>
#include <QFile>
#include <QFileInfo>
#include <QHash>
#include <QSettings>
#include <QString>
#include <QStringList>
#include <QVariantMap>
#include <QVector>

#include "../../../catalog/datacatalog.h"
#include "dataopsmodel.h"

namespace paleo::dataops
{

// ---- D8.3 导入预设 -----------------------------------------------------------
// QSettings dataops/importPresets：name → { dirs: [...], typeMap: {ext: type} }
struct ImportPreset
{
  QString name;
  QStringList dirs;
  QHash<QString, QString> typeMap; // 扩展名（小写）→ 资产类型

  QVariantMap toVariant() const
  {
    QVariantMap m;
    m.insert(QStringLiteral("dirs"), dirs);
    QVariantMap tm;
    for (auto it = typeMap.constBegin(); it != typeMap.constEnd(); ++it)
      tm.insert(it.key(), it.value());
    m.insert(QStringLiteral("typeMap"), tm);
    return m;
  }
  static ImportPreset fromVariant(const QString &n, const QVariantMap &m)
  {
    ImportPreset p;
    p.name = n;
    p.dirs = m.value(QStringLiteral("dirs")).toStringList();
    const QVariantMap tm = m.value(QStringLiteral("typeMap")).toMap();
    for (auto it = tm.constBegin(); it != tm.constEnd(); ++it)
      p.typeMap.insert(it.key(), it.value().toString());
    return p;
  }
};

inline QList<ImportPreset> importPresets()
{
  QList<ImportPreset> out;
  QSettings s(QStringLiteral("paleo"), QStringLiteral("paleo"));
  const QVariantMap all = s.value(QStringLiteral("dataops/importPresets")).toMap();
  for (auto it = all.constBegin(); it != all.constEnd(); ++it)
    out.append(ImportPreset::fromVariant(it.key(), it.value().toMap()));
  return out;
}

inline bool saveImportPreset(const ImportPreset &p)
{
  const QString n = p.name.trimmed();
  if (n.isEmpty() || n.size() > 40)
    return false;
  QSettings s(QStringLiteral("paleo"), QStringLiteral("paleo"));
  QVariantMap all = s.value(QStringLiteral("dataops/importPresets")).toMap();
  all.insert(n, p.toVariant());
  s.setValue(QStringLiteral("dataops/importPresets"), all);
  return true;
}

inline bool deleteImportPreset(const QString &name)
{
  QSettings s(QStringLiteral("paleo"), QStringLiteral("paleo"));
  QVariantMap all = s.value(QStringLiteral("dataops/importPresets")).toMap();
  if (!all.contains(name))
    return false;
  all.remove(name);
  s.setValue(QStringLiteral("dataops/importPresets"), all);
  return true;
}

// ---- D8.4 重复检测 -----------------------------------------------------------
struct DuplicateHit
{
  QString incomingPath;
  QString existingAssetId;
  QString existingPath;
  QString reason; // "sha" | "name"
};

// 就地判重：外链/受管入库前按 SHA-256（全库 versionBySha256）与同名
//（displayName == 新文件 basename）双口径。文件读不动 → 只做同名口径。
inline QVector<DuplicateHit> detectDuplicates(DataCatalog *cat,
                                              const QStringList &incomingPaths)
{
  QVector<DuplicateHit> hits;
  if (!cat)
    return hits;
  // 同名索引一次建好（O(n) 而非每文件全扫）。
  QHash<QString, QString> byName;
  for (const CatalogAsset &a : cat->assets())
    byName.insert(a.displayName.toLower(), a.id);
  for (const QString &p : incomingPaths)
  {
    const QFileInfo fi(p);
    const QString base = fi.fileName();
    const auto it = byName.constFind(base.toLower());
    if (it != byName.constEnd())
    {
      DuplicateHit h;
      h.incomingPath = p;
      h.existingAssetId = it.value();
      const CatalogAsset a = cat->assetById(it.value());
      const CatalogVersion v = cat->currentVersion(a.id);
      h.existingPath = v.path;
      h.reason = QStringLiteral("name");
      hits.append(h);
      continue;
    }
    QString err;
    const QString sha = DataCatalog::sha256FileHex(p, &err);
    if (!sha.isEmpty())
    {
      const CatalogVersion v = cat->versionBySha256(sha);
      if (!v.id.isEmpty())
      {
        DuplicateHit h;
        h.incomingPath = p;
        h.existingAssetId = v.assetId;
        h.existingPath = v.path;
        h.reason = QStringLiteral("sha");
        hits.append(h);
      }
    }
  }
  return hits;
}

// 处置选项（D8.4）：跳过 / 重命名（加后缀导入）/ 覆盖→派生版。
enum class DuplicateResolution { Skip, Rename, Derive };
inline QString duplicateResolutionKey(DuplicateResolution r)
{
  switch (r)
  {
    case DuplicateResolution::Skip: return QStringLiteral("skip");
    case DuplicateResolution::Rename: return QStringLiteral("rename");
    case DuplicateResolution::Derive: return QStringLiteral("derive");
  }
  return QStringLiteral("skip");
}

// ---- D8.6 目录预估 -----------------------------------------------------------
struct ImportEstimate
{
  int fileCount = 0;
  qint64 totalBytes = 0;
  QStringList byExtension;           // "las:12, sgY:3"（小写 ext:count）
  int largeFileCount = 0;            // >100MB
  bool canceled = false;             // 迭代上限触发（防 100 万文件目录）
  static constexpr int kMaxScan = 50000;
};

inline ImportEstimate estimateDirectory(const QString &dir)
{
  ImportEstimate est;
  QDirIterator it(dir, QDir::Files, QDirIterator::Subdirectories);
  QHash<QString, int> extCount;
  while (it.hasNext())
  {
    if (est.fileCount >= ImportEstimate::kMaxScan)
    {
      est.canceled = true;
      break;
    }
    it.next();
    const QFileInfo fi = it.fileInfo();
    ++est.fileCount;
    est.totalBytes += fi.size();
    if (fi.size() > qint64(100) * 1024 * 1024)
      ++est.largeFileCount;
    const QString ext = fi.suffix().toLower();
    if (!ext.isEmpty())
      extCount[ext] += 1;
  }
  QStringList parts;
  for (auto e = extCount.constBegin(); e != extCount.constEnd(); ++e)
    parts << QStringLiteral("%1:%2").arg(e.key()).arg(e.value());
  parts.sort();
  est.byExtension = parts;
  return est;
}

// 分批确认（D8.6）：大目录按批切分（每批 ≤ batch 个文件）。
inline QStringList batchPaths(const QStringList &paths, int batch)
{
  if (batch <= 0 || paths.size() <= batch)
    return paths;
  return paths.mid(0, batch);
}

// ---- D8.1/D8.2 导入队列条目 + 重试队列状态机 ---------------------------------
enum class ImportItemState
{
  Queued,      // 排队中
  Running,     // 导入中（进度 0-100）
  Done,        // 成功
  Failed,      // 失败（可重试）
  Skipped,     // 用户跳过 / 重复处置为跳过
  Canceled,    // 单项取消
  RetryWait,   // 自动重试等待（指数退避 N 次内）
};

struct ImportQueueItem
{
  QString path;
  QString type;                // 预填类型（预设映射给出）
  ImportItemState state = ImportItemState::Queued;
  int progressPercent = 0;     // 0-100（D8.1 逐文件进度）
  QString error;               // 失败原因
  int retryCount = 0;          // 已重试次数
  int autoRetriesLeft = 2;     // D8.2 自动重试（默认 2 次）
  QString resultingAssetId;    // 成功后回填

  static QString stateText(ImportItemState s)
  {
    switch (s)
    {
      case ImportItemState::Queued: return QStringLiteral("排队");
      case ImportItemState::Running: return QStringLiteral("导入中");
      case ImportItemState::Done: return QStringLiteral("完成");
      case ImportItemState::Failed: return QStringLiteral("失败");
      case ImportItemState::Skipped: return QStringLiteral("跳过");
      case ImportItemState::Canceled: return QStringLiteral("已取消");
      case ImportItemState::RetryWait: return QStringLiteral("等待重试");
    }
    return QString();
  }
};

// 重试队列状态机：失败条目自动重试（RetryWait→Running），手动重试/跳过/
// 取消均可。canAutoRetry = retriesLeft > 0。
class ImportRetryQueue
{
public:
  // onFail 回调：返回 true = 接受自动重试（重新 Running）；false = 转硬失败。
  // 调用方（面板）持有真正的执行器；本机只管状态迁移。
  void enqueue(ImportQueueItem item) { m_items.append(item); }
  QVector<ImportQueueItem> items() const { return m_items; }
  ImportQueueItem *at(int i) { return i >= 0 && i < m_items.size() ? &m_items[i] : nullptr; }
  int indexOfPath(const QString &p) const
  {
    for (int i = 0; i < m_items.size(); ++i)
      if (m_items.at(i).path == p)
        return i;
    return -1;
  }

  // D8.1 单项取消：Queued/RetryWait → Canceled（Running 的取消由执行器
  // 协作处理，状态在此落 Canceled）。
  bool cancelItem(int i)
  {
    ImportQueueItem *it = at(i);
    if (!it || it->state == ImportItemState::Done ||
        it->state == ImportItemState::Canceled)
      return false;
    it->state = ImportItemState::Canceled;
    it->progressPercent = 0;
    return true;
  }

  // D8.2 手动重试：Failed/Canceled/Skipped → Queued。
  bool retryItem(int i)
  {
    ImportQueueItem *it = at(i);
    if (!it || (it->state != ImportItemState::Failed &&
                it->state != ImportItemState::Canceled &&
                it->state != ImportItemState::Skipped))
      return false;
    it->state = ImportItemState::Queued;
    it->error.clear();
    it->progressPercent = 0;
    return true;
  }

  // 执行器回调面：开始/进度/成功/失败。
  void markRunning(int i) { if (ImportQueueItem *it = at(i)) { it->state = ImportItemState::Running; it->progressPercent = 0; } }
  void markProgress(int i, int pct) { if (ImportQueueItem *it = at(i)) it->progressPercent = qBound(0, pct, 100); }
  void markDone(int i, const QString &assetId)
  {
    if (ImportQueueItem *it = at(i))
    {
      it->state = ImportItemState::Done;
      it->progressPercent = 100;
      it->resultingAssetId = assetId;
    }
  }
  // 失败落账：自动重试额度未尽 → RetryWait（重试次数 +1）；额度尽 → Failed。
  void markFailed(int i, const QString &error)
  {
    ImportQueueItem *it = at(i);
    if (!it)
      return;
    it->error = error;
    it->retryCount += 1;
    if (it->autoRetriesLeft > 0)
    {
      it->autoRetriesLeft -= 1;
      it->state = ImportItemState::RetryWait;
    }
    else
      it->state = ImportItemState::Failed;
  }
  // 驱动一拍：RetryWait → Queued（自动重试触发；由面板定时器调用）。
  int promoteRetryWaiters()
  {
    int n = 0;
    for (ImportQueueItem &it : m_items)
      if (it.state == ImportItemState::RetryWait)
      {
        it.state = ImportItemState::Queued;
        ++n;
      }
    return n;
  }

  // D8.5 摘要报告（可复制）。
  QString summaryText() const
  {
    int done = 0, failed = 0, skipped = 0, canceled = 0;
    for (const ImportQueueItem &it : m_items)
    {
      switch (it.state)
      {
        case ImportItemState::Done: ++done; break;
        case ImportItemState::Failed: ++failed; break;
        case ImportItemState::Skipped: ++skipped; break;
        case ImportItemState::Canceled: ++canceled; break;
        default: break;
      }
    }
    QString out = tr("导入摘要：共 %1 项 — 成功 %2、失败 %3、跳过 %4、取消 %5")
                      .arg(m_items.size()).arg(done).arg(failed).arg(skipped).arg(canceled);
    for (const ImportQueueItem &it : m_items)
      if (it.state == ImportItemState::Failed && !it.error.isEmpty())
        out += QStringLiteral("\n· 失败：%1（%2）").arg(it.path, it.error);
    return out;
  }

  bool hasPending() const
  {
    for (const ImportQueueItem &it : m_items)
      if (it.state == ImportItemState::Queued || it.state == ImportItemState::Running ||
          it.state == ImportItemState::RetryWait)
        return true;
    return false;
  }
  void clearFinished()
  {
    for (int i = m_items.size() - 1; i >= 0; --i)
      if (m_items.at(i).state == ImportItemState::Done ||
          m_items.at(i).state == ImportItemState::Canceled ||
          m_items.at(i).state == ImportItemState::Skipped)
        m_items.removeAt(i);
  }

private:
  QVector<ImportQueueItem> m_items;

  static QString tr(const char *s) { return QObject::tr(s); }
};

} // namespace paleo::dataops
