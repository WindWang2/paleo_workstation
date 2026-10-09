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
  m_diskPending = lazy && !path.isEmpty();
  if (!lazy && !path.isEmpty())
    loadDiskLocked();
}

void ShaCache::setMemoryLimit(int entries)
{
  QMutexLocker lock(&m_mutex);
  m_memLimit = qMax(16, entries);
}

QString ShaCache::sha256Hex(const QString &path, QString *error)
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
  const QString fp = QStringLiteral("%1|%2|%3").arg(canon).arg(mtime).arg(size);

  quint64 diskGen = 0;
  {
    QMutexLocker lock(&m_mutex);
    if (m_diskPending) { m_diskPending = false; loadDiskLocked(); }
    const auto it = m_mem.constFind(fp);
    if (it != m_mem.constEnd())
    {
      ++m_counts.hits;
      return it.value();
    }
    // 磁盘层：键是 canonical path，值自带 mtime|size——失配即弃。
    const auto dit = m_disk.constFind(canon);
    if (dit != m_disk.constEnd())
    {
      const QStringList parts = dit.value().split(QLatin1Char('|'));
      if (parts.size() == 3 && parts.at(0) == QString::number(mtime) &&
          parts.at(1) == QString::number(size) && !parts.at(2).isEmpty())
      {
        m_mem.insert(fp, parts.at(2));
        m_diskAge.insert(canon, QDateTime::currentMSecsSinceEpoch());
        ++m_counts.diskHits;
        ++m_counts.hits;
        return parts.at(2);
      }
      // 指纹失配：文件变了——清掉旧条目，下面重算。
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
  m_mem.insert(fp, sha);
  // setDiskFile 换代后不得把这次哈希写入新 sha.json（内存指纹仍保留）。
  if (diskGen == m_diskGeneration && !m_diskFile.isEmpty())
  {
    m_disk.insert(canon, QStringLiteral("%1|%2|%3").arg(mtime).arg(size).arg(sha));
    m_diskAge.insert(canon, QDateTime::currentMSecsSinceEpoch());
    saveDiskLocked(); // 惰性持久化：算一次大文件的哈希值得立即落盘
  }
  // 内存层收缩：按指纹代内任意序截断（哈希表本身无 LRU 语义，超限丢旧即可
  // ——哈希重算成本有限，不值得为它建完整 LRU）。
  while (m_mem.size() > m_memLimit)
    m_mem.erase(m_mem.begin());
  if (diskGen == m_diskGeneration)
  {
    while (m_disk.size() > m_diskLimit)
    {
      // 磁盘层按最近命中时间收缩。
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
  }
  return sha;
}

void ShaCache::invalidate(const QString &path)
{
  QMutexLocker lock(&m_mutex);
  if (m_diskPending) { m_diskPending = false; if (!path.isEmpty()) loadDiskLocked(); }
  if (path.isEmpty())
  {
    m_mem.clear();
    m_disk.clear();
    m_diskAge.clear();
    if (!m_diskFile.isEmpty())
      saveDiskLocked();
    return;
  }
  const QString canon = PathCanon::canonicalize(path);
  for (auto it = m_mem.begin(); it != m_mem.end();)
    it = it.key().startsWith(canon + QLatin1Char('|')) ? m_mem.erase(it) : ++it;
  m_disk.remove(canon);
  m_diskAge.remove(canon);
  if (!m_diskFile.isEmpty())
    saveDiskLocked();
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
    m_disk.insert(it.key(), it.value().toString());
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
