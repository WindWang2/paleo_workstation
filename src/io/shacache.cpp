// 层：数据
#include "shacache.h"

#include "pathcanon.h"

#include <QCryptographicHash>
#include <QDateTime>
#include <QDir>
#include <QFile>
#include <QFileInfo>
#include <QJsonDocument>
#include <QJsonObject>
#include <QSaveFile>

#include <algorithm>
#include <sys/stat.h>

namespace
{
  // 流式 SHA-256（64KB 块——Windows 主线程 1MB 栈缓冲教训，见 datacatalog）。
  bool hashFile(const QString &path, QString *shaOut, QString *error)
  {
    QFile f(path);
    if (!f.open(QIODevice::ReadOnly))
    {
      if (error)
        *error = QStringLiteral("cannot read %1").arg(path);
      return false;
    }
    QCryptographicHash hash(QCryptographicHash::Sha256);
    char buf[64 << 10];
    qint64 n = 0;
    while ((n = f.read(buf, sizeof(buf))) > 0)
      hash.addData(QByteArrayView(buf, static_cast<qsizetype>(n)));
    if (n < 0)
    {
      if (error)
        *error = QStringLiteral("read error on %1").arg(path);
      return false;
    }
    *shaOut = QString::fromLatin1(hash.result().toHex());
    return true;
  }

  // #561 inode 采样：segyindexstore::identityOf 同款平台口径——POSIX stat
  // 取 st_ino；Windows 的 stat 恒填 0（size+mtime 已够），失败也回 0（弱化为
  // 默认口径，不阻断哈希）。
  quint64 inodeOf(const QString &canonicalPath)
  {
    struct stat st;
    if (::stat(QFile::encodeName(canonicalPath).constData(), &st) != 0)
      return 0;
    return static_cast<quint64>(st.st_ino);
  }
} // namespace

ShaCache &ShaCache::shared()
{
  static ShaCache inst; // 首用构造；进程退出时析构（无跨线程注册问题）
  return inst;
}

void ShaCache::setDiskFile(const QString &path, bool lazy)
{
  QMutexLocker lock(&m_mutex);
  ++m_diskGeneration;
  m_diskFile = path;
  m_disk.clear();
  m_diskAge.clear();
  m_diskDirtyCount = 0;
  m_lastDiskSaveMs = 0; // A-01：换代后首个 miss 立即落盘（保留「算一次即持久」的会话起点语义）
  m_diskPending = lazy && !path.isEmpty();
  if (!lazy && !path.isEmpty())
    loadDiskLocked();
}

void ShaCache::setMemoryLimit(int entries)
{
  QMutexLocker lock(&m_mutex);
  m_memLimit = qMax(16, entries);
}

void ShaCache::setDiskSaveThrottle(qint64 intervalMs, int dirtyEntries)
{
  QMutexLocker lock(&m_mutex);
  m_diskSaveIntervalMs = qMax<qint64>(0, intervalMs);
  m_diskDirtyThreshold = qMax(1, dirtyEntries);
}

void ShaCache::flushDisk()
{
  QMutexLocker lock(&m_mutex);
  flushDiskLocked();
}

void ShaCache::flushDiskLocked()
{
  if (m_diskDirtyCount <= 0 || m_diskFile.isEmpty())
    return;
  while (m_disk.size() > m_diskLimit)
  {
    QString oldest;
    qint64 best = std::numeric_limits<qint64>::max();
    for (auto it = m_diskAge.constBegin(); it != m_diskAge.constEnd(); ++it)
      if (it.value() < best)
      {
        best = it.value();
        oldest = it.key();
      }
    if (oldest.isEmpty())
      break;
    m_diskAge.remove(oldest);
    m_disk.remove(oldest);
  }
  saveDiskLocked();
  m_diskDirtyCount = 0;
  m_lastDiskSaveMs = QDateTime::currentMSecsSinceEpoch();
}

void ShaCache::maybeSaveDiskLocked()
{
  if (m_diskDirtyCount <= 0 || m_diskFile.isEmpty())
    return;
  const qint64 now = QDateTime::currentMSecsSinceEpoch();
  if (m_diskDirtyCount < m_diskDirtyThreshold &&
      m_lastDiskSaveMs > 0 && now - m_lastDiskSaveMs < m_diskSaveIntervalMs)
    return;
  flushDiskLocked(); // 调用方已持锁（sha256Hex 临界区内）
}

QString ShaCache::sha256Hex(const QString &path, QString *error, Fingerprint fp)
{
  const QFileInfo info(path);
  if (!info.exists())
  {
    if (error)
      *error = QStringLiteral("no such file: %1").arg(path);
    return QString();
  }
  const QString canon = PathCanon::canonicalize(path);
  const qint64 mtime = info.lastModified().toMSecsSinceEpoch();
  const qint64 size = info.size();
  const bool withInode = fp == Fingerprint::MtimeSizeInode;
  const quint64 ino = withInode ? inodeOf(canon) : quint64(0);
  const QString fpKey = withInode
      ? QStringLiteral("%1|%2|%3|%4").arg(canon).arg(mtime).arg(size).arg(ino)
      : QStringLiteral("%1|%2|%3").arg(canon).arg(mtime).arg(size);

  quint64 diskGen = 0;
  {
    QMutexLocker lock(&m_mutex);
    if (m_diskPending) { m_diskPending = false; loadDiskLocked(); }
    const auto it = m_mem.constFind(fpKey);
    if (it != m_mem.constEnd())
    {
      ++m_counts.hits;
      return it.value();
    }
    // 磁盘层：键是 canonical path，值自带 mtime|size[|inode]——失配即弃。
    const auto dit = m_disk.constFind(canon);
    if (dit != m_disk.constEnd())
    {
      const QStringList parts = dit.value().split(QLatin1Char('|'));
      bool diskHit = false;
      if ((parts.size() == 3 || parts.size() == 4) &&
          parts.at(0) == QString::number(mtime) &&
          parts.at(1) == QString::number(size) && !parts.constLast().isEmpty())
      {
        // 默认口径只判 mtime+size（4 段条目里的 inode 是强口径的附加证据，
        // 不反向收紧默认语义）；强口径要求 4 段且 inode 一致——3 段旧条目
        // 证不了 inode，按失配重算（覆写自愈成 4 段）。
        diskHit = withInode
            ? (parts.size() == 4 && parts.at(2) == QString::number(ino))
            : true;
      }
      if (diskHit)
      {
        const QString &sha = parts.constLast();
        m_mem.insert(fpKey, sha);
        m_diskAge.insert(canon, QDateTime::currentMSecsSinceEpoch());
        ++m_counts.diskHits;
        ++m_counts.hits;
        return sha;
      }
      // 指纹失配：文件变了（或强口径遇旧条目）——清掉旧条目，下面重算。
      m_disk.remove(canon);
      m_diskAge.remove(canon);
    }
    diskGen = m_diskGeneration;
  }

  QString sha;
  QString herr;
  if (!hashFile(path, &sha, &herr))
  {
    if (error)
      *error = herr;
    QMutexLocker lock(&m_mutex);
    ++m_counts.misses;
    return QString();
  }

  QMutexLocker lock(&m_mutex);
  ++m_counts.misses;
  m_mem.insert(fpKey, sha);
  // setDiskFile 换代后不得把这次哈希写入新 sha.json（内存指纹仍保留）。
  if (diskGen == m_diskGeneration && !m_diskFile.isEmpty())
  {
    m_disk.insert(canon, withInode
        ? QStringLiteral("%1|%2|%3|%4").arg(mtime).arg(size).arg(ino).arg(sha)
        : QStringLiteral("%1|%2|%3").arg(mtime).arg(size).arg(sha));
    m_diskAge.insert(canon, QDateTime::currentMSecsSinceEpoch());
    // A-01：节流落盘——此前每次 miss 都在持锁临界区内全表序列化 + 同步写
    // 盘（表 3 万条时万级文件导入 ≈ 数十 GB 写放大，且磁盘 IO 期间阻塞并发
    // 哈希查询）；收缩扫描（O(表大小)）一并挪到落盘时刻。
    ++m_diskDirtyCount;
    maybeSaveDiskLocked();
  }
  // 内存层收缩：按指纹代内任意序截断（哈希表本身无 LRU 语义，超限丢旧即可
  // ——哈希重算成本有限，不值得为它建完整 LRU）。
  while (m_mem.size() > m_memLimit)
    m_mem.erase(m_mem.begin());
  return sha;
}

void ShaCache::invalidate(const QString &path)
{
  QMutexLocker lock(&m_mutex);
  if (m_diskPending) { m_diskPending = false; if (!path.isEmpty()) loadDiskLocked(); }
  const auto forcedSaveLocked = [&] {
    // 显式失效 = 用户可见的写路径语义：强制刷（不吞节流窗口）。
    if (m_diskFile.isEmpty())
    {
      m_diskDirtyCount = 0;
      return;
    }
    saveDiskLocked();
    m_diskDirtyCount = 0;
    m_lastDiskSaveMs = QDateTime::currentMSecsSinceEpoch();
  };
  if (path.isEmpty())
  {
    m_mem.clear();
    m_disk.clear();
    m_diskAge.clear();
    forcedSaveLocked();
    return;
  }
  const QString canon = PathCanon::canonicalize(path);
  for (auto it = m_mem.begin(); it != m_mem.end();)
    it = it.key().startsWith(canon + QLatin1Char('|')) ? m_mem.erase(it) : ++it;
  m_disk.remove(canon);
  m_diskAge.remove(canon);
  m_diskDirtyCount = qMax(m_diskDirtyCount, 1);
  forcedSaveLocked();
}

ShaCache::Counts ShaCache::counts() const
{
  QMutexLocker lock(&m_mutex);
  Counts c = m_counts;
  c.stored = m_disk.size();
  return c;
}

int ShaCache::memoryEntries() const
{
  QMutexLocker lock(&m_mutex);
  return m_mem.size();
}

void ShaCache::loadDiskLocked()
{
  QFile f(m_diskFile);
  if (!f.open(QIODevice::ReadOnly))
    return;
  const QJsonDocument doc = QJsonDocument::fromJson(f.readAll());
  if (!doc.isObject())
    return; // 坏表自愈：空表起步
  const QJsonObject obj = doc.object();
  m_disk.reserve(obj.size());
  for (auto it = obj.constBegin(); it != obj.constEnd(); ++it)
  {
    m_disk.insert(it.key(), it.value().toString());
    // LRU 收缩按 m_diskAge 找最旧——载入时不填年龄，flushDiskLocked 的收缩
    // 循环遇到无龄条目即 break，从未命中的存量条目永不被裁（磁盘表可长期
    // 停在 2×m_diskLimit），新算出的条目反被优先驱逐。载入条目置 0（最旧）：
    // 本次会话没命中过的存量条目最先让位。
    m_diskAge.insert(it.key(), 0);
  }
}

void ShaCache::saveDiskLocked()
{
  if (m_diskFile.isEmpty())
    return;
  QJsonObject obj;
  for (auto it = m_disk.constBegin(); it != m_disk.constEnd(); ++it)
    obj.insert(it.key(), it.value());
  const QFileInfo info(m_diskFile);
  if (!info.dir().exists() && !QDir().mkpath(info.absolutePath()))
    return; // 无盘缓存继续可用（内存层仍有效）
  QSaveFile f(m_diskFile);
  f.setDirectWriteFallback(false);
  if (!f.open(QIODevice::WriteOnly | QIODevice::Truncate))
    return;
  const QByteArray bytes = QJsonDocument(obj).toJson(QJsonDocument::Compact);
  if (f.write(bytes) != bytes.size() || !f.commit())
    return; // 落盘失败不致命——下次命中重新累计
}
