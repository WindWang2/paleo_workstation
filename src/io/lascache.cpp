// 层：数据
#include "lascache.h"

#include "pathcanon.h"
#include "welllogread.h" // 方向44：LAS/DLIS/LIS 分派

#include <QCryptographicHash>
#include <QDateTime>
#include <QDir>
#include <QElapsedTimer>
#include <QFile>
#include <QFileInfo>
#include <cstring>

namespace
{
  // v2（#167）：解析器不再把纯空白行/尾部 Ctrl-Z 解析成 NaN 幽灵行——旧缓存作废重建。
  constexpr quint16 kLasCacheVersion = 2;
  const char kLasCacheMagic[8] = {'P', 'L', 'A', 'S', 'C', 'C', 'H', 'E'};
  constexpr qint64 kDefaultMemBudget = 64 * 1024 * 1024;

  // 磁盘 payload 布局（cacheio 小端原语）：
  //   [str fingerprint][f64 null 占位][u32 curveCount][u32 rowCount]
  //   每曲线：[str name][str unit][str descr]
  //   每曲线：rowCount × f64
  QByteArray encodeDoc(const QString &fingerprint, const LasDoc &doc)
  {
    QByteArray out;
    cacheio::putStr(&out, fingerprint);
    cacheio::putF64(&out, -999.25); // NULL 值已在解析时映射为 NaN；占位保布局
    const quint32 curveCount = static_cast<quint32>(doc.curves.size());
    quint32 rowCount = 0;
    for (const LasCurve &c : doc.curves)
      rowCount = qMax<quint32>(rowCount, static_cast<quint32>(c.values.size()));
    cacheio::putU32(&out, curveCount);
    cacheio::putU32(&out, rowCount);
    for (const LasCurve &c : doc.curves)
    {
      cacheio::putStr(&out, c.name);
      cacheio::putStr(&out, c.unit);
      cacheio::putStr(&out, c.descr);
    }
    for (const LasCurve &c : doc.curves)
    {
#if Q_BYTE_ORDER == Q_LITTLE_ENDIAN
      const qint64 byteCount = static_cast<qint64>(c.values.size()) * static_cast<qint64>(sizeof(double));
      const int oldSize = out.size();
      out.resize(oldSize + static_cast<int>(byteCount));
      std::memcpy(out.data() + oldSize, c.values.constData(), static_cast<size_t>(byteCount));
#else
      for (double v : c.values)
        cacheio::putF64(&out, v);
#endif
    }
    return out;
  }

  bool decodeDoc(const QByteArray &payload, const QString &wantFingerprint,
                 std::shared_ptr<LasDoc> *out)
  {
    qint64 pos = 0;
    bool ok = true;
    const QString fp = cacheio::str(payload, &pos, &ok);
    if (!ok || fp != wantFingerprint)
      return false; // 指纹不符 = 过期条目（别的 mtime/size）
    /*double nullPlaceholder =*/ cacheio::f64(payload, &pos, &ok);
    const quint32 curveCount = cacheio::u32(payload, &pos, &ok);
    const quint32 rowCount = cacheio::u32(payload, &pos, &ok);
    if (!ok || curveCount == 0 || curveCount > 4096 || rowCount > 200000000)
      return false;
    auto doc = std::make_shared<LasDoc>();
    doc->curves.reserve(static_cast<int>(curveCount));
    for (quint32 i = 0; i < curveCount && ok; ++i)
    {
      LasCurve c;
      c.name = cacheio::str(payload, &pos, &ok);
      c.unit = cacheio::str(payload, &pos, &ok);
      c.descr = cacheio::str(payload, &pos, &ok);
      doc->curves.append(c);
      doc->curveNames.append(c.name);
    }
    if (!ok)
      return false;
    const qint64 curveBytes = static_cast<qint64>(rowCount) * static_cast<qint64>(sizeof(double));
    for (quint32 i = 0; i < curveCount && ok; ++i)
    {
      if (pos + curveBytes > payload.size())
        return false;
      QVector<double> vals;
      vals.resize(static_cast<int>(rowCount));
#if Q_BYTE_ORDER == Q_LITTLE_ENDIAN
      std::memcpy(vals.data(), payload.constData() + pos, static_cast<size_t>(curveBytes));
      pos += curveBytes;
#else
      for (quint32 r = 0; r < rowCount && ok; ++r)
        vals[r] = cacheio::f64(payload, &pos, &ok);
#endif
      doc->curves[int(i)].values = std::move(vals);
    }
    if (!ok)
      return false; // 截断——自愈
    doc->ok = true;
    *out = doc;
    return true;
  }

  quint16 compressionFlags()
  {
#ifdef PALEO_HAVE_ZSTD
    return CacheFlags::ZstdCompressed;
#else
    return CacheFlags::None;
#endif
  }
} // namespace

LasCache &LasCache::shared()
{
  static LasCache inst;
  return inst;
}

LasCache::LasCache()
    : m_mem(QStringLiteral("las-docs"), kDefaultMemBudget,
            [](const Entry &e) { return e.doc ? docBytes(*e.doc) : 0; })
{
}

qint64 LasCache::docBytes(const LasDoc &doc)
{
  qint64 total = 64;
  for (const LasCurve &c : doc.curves)
    total += c.name.size() + c.unit.size() + c.descr.size() +
             static_cast<qint64>(c.values.size()) * 8;
  return total;
}

void LasCache::setDiskRoot(const QString &dir)
{
  QMutexLocker lock(&m_cfgMutex);
  m_diskRoot = dir;
}

QString LasCache::diskRoot() const
{
  QMutexLocker lock(&m_cfgMutex);
  return m_diskRoot;
}

void LasCache::setMemoryBudget(qint64 bytes)
{
  m_mem.setCapacity(bytes);
}

void LasCache::bumpExtra(qint64 CacheStats::*field)
{
  QMutexLocker lock(&m_cfgMutex);
  ++(m_extra.*field);
}

QString LasCache::diskPathFor(const QString &fingerprint) const
{
  if (m_diskRoot.isEmpty())
    return QString();
  const QByteArray h = QCryptographicHash::hash(fingerprint.toUtf8(),
                                                QCryptographicHash::Sha256)
                           .toHex();
  return m_diskRoot + QLatin1Char('/') + QString::fromLatin1(h.left(24)) +
         QStringLiteral(".plc");
}

bool LasCache::writeDisk(const QString &fingerprint, const LasDoc &doc)
{
  if (m_diskRoot.isEmpty())
    return false;
  const QString path = diskPathFor(fingerprint);
  const QByteArray payload = encodeDoc(fingerprint, doc);
  if (!writeCacheFileAtomic(path, QByteArray(kLasCacheMagic, 8), kLasCacheVersion,
                            compressionFlags(), payload))
    return false; // 无缓存继续可用（D2.1 降级口径）
  return true;
}

std::shared_ptr<LasDoc> LasCache::readDisk(const QString &fingerprint)
{
  if (m_diskRoot.isEmpty())
    return nullptr;
  const QString path = diskPathFor(fingerprint);
  QByteArray payload;
  QString reason;
  if (!readCacheFile(path, QByteArray(kLasCacheMagic, 8), kLasCacheVersion,
                     kLasCacheVersion, &payload, nullptr, &reason))
  {
    if (QFile::exists(path))
    {
      QFile::remove(path); // 损坏/过版 → 自愈删除（D1.2）
      bumpExtra(&CacheStats::selfHeals);
    }
    return nullptr;
  }
  std::shared_ptr<LasDoc> doc;
  if (!decodeDoc(payload, fingerprint, &doc))
  {
    QFile::remove(path);
    bumpExtra(&CacheStats::selfHeals);
    return nullptr;
  }
  return doc;
}

LasDoc LasCache::load(const QString &path, QList<LasIssue> *issues)
{
  const QFileInfo info(path);
  if (!info.exists())
  {
    LasDoc doc;
    doc.error = QStringLiteral("no such file: %1").arg(path);
    return doc;
  }
  const QString canon = PathCanon::canonicalize(path);
  const QString fp = PathCanon::fingerprint(
      canon, info.lastModified().toMSecsSinceEpoch(), info.size());

  // 1) 内存命中（指纹匹配才算）。
  if (const auto hit = m_mem.get(canon))
  {
    if (hit->fingerprint == fp)
    {
      QElapsedTimer t;
      t.start();
      m_timings.memoryHitNs = t.nsecsElapsed();
      return *hit->doc;
    }
    m_mem.erase(canon); // 指纹失配：文件变了（D1.2/D1.10 兜底）
  }

  // 2) 磁盘命中 / 3) 冷解析。同指纹并发只跑一份（D4.7）。
  std::shared_ptr<LasDoc> fromDisk;
  const auto ticket = m_inflight.submit(
      fp, [this, &path, &fp, &issues, &fromDisk]() -> std::shared_ptr<LasDoc> {
        QElapsedTimer diskTimer;
        diskTimer.start();
        fromDisk = readDisk(fp);
        if (fromDisk)
        {
          m_timings.diskLoadNs = diskTimer.nsecsElapsed();
          bumpExtra(&CacheStats::diskHits);
          return fromDisk;
        }
        QElapsedTimer parseTimer;
        parseTimer.start();
        LasDoc doc = WellLogRead::parseDoc(path, issues); // 方向44：格式分派
        m_timings.coldParseNs = parseTimer.nsecsElapsed();
        if (!doc.ok)
          return nullptr;
        auto shared = std::make_shared<LasDoc>(std::move(doc));
        if (writeDisk(fp, *shared))
          bumpExtra(&CacheStats::diskWrites);
        return shared;
      });

  const std::shared_ptr<LasDoc> doc = ticket.future.get();
  if (!doc)
  {
    LasDoc failed;
    failed.error = QStringLiteral("parse failed: %1").arg(path);
    if (issues)
      for (const LasIssue &i : *issues)
        if (i.severity == LasIssue::Severity::Error)
          failed.error = i.message;
    return failed;
  }
  Entry e;
  e.fingerprint = fp;
  e.doc = doc;
  m_mem.insert(canon, e);
  return *doc;
}

int LasCache::prefetch(const QStringList &paths)
{
  int ok = 0;
  for (const QString &p : paths)
  {
    const LasDoc doc = load(p);
    if (doc.ok)
      ++ok;
  }
  return ok;
}

void LasCache::invalidate(const QString &path)
{
  if (path.isEmpty())
  {
    clearMemory();
    return;
  }
  m_mem.erase(PathCanon::canonicalize(path));
}

bool LasCache::isCached(const QString &path) const
{
  const QFileInfo info(path);
  if (!info.exists())
    return false;
  const QString canon = PathCanon::canonicalize(path);
  const auto hit = m_mem.peek(canon);
  if (!hit)
    return false;
  return hit->fingerprint ==
         PathCanon::fingerprint(canon, info.lastModified().toMSecsSinceEpoch(), info.size());
}

CacheStats LasCache::stats() const
{
  CacheStats s = m_mem.stats();
  QMutexLocker lock(&m_cfgMutex);
  s.diskHits += m_extra.diskHits;
  s.diskWrites += m_extra.diskWrites;
  s.selfHeals += m_extra.selfHeals;
  return s;
}

void LasCache::clearMemory()
{
  m_mem.clear();
}
