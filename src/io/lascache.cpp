// 层：数据
#include "nativemodel.h"
#include "lascache.h"
#include "lascachecodec_p.h"

#include "../domain/wellnumeric.h"
#include "pathcanon.h"
#include "welllogread.h" // 方向44：LAS/DLIS/LIS 分派

#include <QCryptographicHash>
#include <QDateTime>
#include <QDir>
#include <QElapsedTimer>
#include <QFile>
#include <QFileInfo>
#include <QtEndian>
#include <bit>
#include <limits>

namespace
{
  // v2（#167）：解析器不再把纯空白行/尾部 Ctrl-Z 解析成 NaN 幽灵行——旧缓存作废重建。
  constexpr quint16 kOldestLasCacheVersion = 2;
  constexpr quint16 kLasCacheVersion = 3;
  const char kLasCacheMagic[8] = {'P', 'L', 'A', 'S', 'C', 'C', 'H', 'E'};
  constexpr qint64 kDefaultMemBudget = 64 * 1024 * 1024;

  constexpr double kDecimalScale = 10000.0;
  constexpr qint32 kNullCode = std::numeric_limits<qint32>::min();
  constexpr quint64 kNullBits = Q_UINT64_C(0x7ff8000000000000);
  constexpr double kQuietNaN = std::bit_cast<double>(kNullBits);
  constexpr char kBinary64 = 0;
  constexpr char kFixed4 = 1;

  bool writeFixedColumn(const QVector<double> &values, char *dst)
  {
    for (qsizetype i = 0; i < values.size(); ++i)
    {
      const double &value = values[i];
      qint32 code = kNullCode;
      if (std::bit_cast<quint64>(value) != kNullBits)
      {
        if (!(value > double(kNullCode) / kDecimalScale &&
              value <= double(std::numeric_limits<qint32>::max()) / kDecimalScale))
          return false;
        code = qRound(value * kDecimalScale);
        const double restored = double(code) / kDecimalScale;
        if (code == kNullCode || std::bit_cast<quint64>(value) != std::bit_cast<quint64>(restored))
          return false;
      }
      qToLittleEndian(code, dst + i * sizeof(code));
    }
    return true;
  }
}

namespace paleo::lascache_detail {
  // 磁盘 payload 布局（cacheio 小端原语）：
  //   [str fingerprint][f64 null 占位][u32 curveCount][u32 rowCount]
  //   每曲线：[str name][str unit][str descr]
  //   v3 每曲线：[u8 encoding] + rowCount × (i32 / f64)。仅按位可逆才使用 i32。
  QByteArray encodeDoc(const QString &fingerprint, const LasDoc &doc)
  {
    QByteArray out;
    cacheio::putStr(&out, fingerprint);
    cacheio::putF64(&out, paleo::wellnumeric::kLasDefaultNull); // NULL 值已在解析时映射为 NaN；占位保布局
    if (doc.curves.isEmpty() || doc.curves.size() > 4096) return {};
    const qsizetype rowCount = doc.curves.first().values.size();
    if (rowCount > 200000000) return {};
    for (const LasCurve &c : doc.curves)
      if (c.values.size() != rowCount) return {};
    cacheio::putU32(&out, static_cast<quint32>(doc.curves.size()));
    cacheio::putU32(&out, static_cast<quint32>(rowCount));
    for (const LasCurve &c : doc.curves)
    {
      cacheio::putStr(&out, c.name);
      cacheio::putStr(&out, c.unit);
      cacheio::putStr(&out, c.descr);
    }
    for (const LasCurve &c : doc.curves)
    {
      const qsizetype offset = out.size();
      if (offset >= out.maxSize() || rowCount > (out.maxSize() - offset - 1) / qsizetype(sizeof(double))) return {};
      out.resizeForOverwrite(offset + 1 + rowCount * sizeof(double));
      char *column = out.data() + offset;
      if (writeFixedColumn(c.values, column + 1))
      {
        *column = kFixed4;
        out.truncate(offset + 1 + rowCount * sizeof(qint32));
      }
      else
      {
        *column = kBinary64;
        qToLittleEndian<quint64>(c.values.constData(), rowCount, column + 1);
      }
    }
    return out;
  }

  bool decodeDoc(const QByteArray &payload, const QString &wantFingerprint, quint16 flags,
                 std::shared_ptr<LasDoc> *out)
  {
    qint64 pos = 0;
    bool ok = true;
    const QString fp = cacheio::str(payload, &pos, &ok);
    if (!ok || fp != wantFingerprint || payload.size() - pos < 16)
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
      if (!ok) return false;
      c.unit = cacheio::str(payload, &pos, &ok);
      if (!ok) return false;
      c.descr = cacheio::str(payload, &pos, &ok);
      doc->curves.append(c);
      doc->curveNames.append(c.name);
    }
    if (!ok)
      return false;
    for (LasCurve &curve : doc->curves)
    {
      char encoding = kBinary64;
      if (flags & TaggedColumns)
      {
        if (pos >= payload.size()) return false;
        encoding = payload[pos++];
        if (encoding != kBinary64 && encoding != kFixed4) return false;
      }
      const qint64 bytes = qint64(rowCount) * (encoding == kFixed4 ? sizeof(qint32) : sizeof(double));
      if (bytes > payload.size() - pos) return false;
      curve.values.resizeForOverwrite(rowCount);
      const char *src = payload.constData() + pos;
      if (encoding == kFixed4)
      {
        double *dst = curve.values.data();
        for (quint32 r = 0; r < rowCount; ++r)
        {
          const qint32 code = qFromLittleEndian<qint32>(src + qsizetype(r) * sizeof(qint32));
          dst[r] = code == kNullCode ? kQuietNaN : double(code) / kDecimalScale;
        }
      }
      else if (rowCount)
        qFromLittleEndian<quint64>(src, rowCount, curve.values.data());
      pos += bytes;
    }
    if (pos != payload.size())
      return false; // 截断——自愈
    doc->ok = true;
    *out = doc;
    return true;
  }
}

namespace {
  quint16 lasCompressionFlags()
  {
#ifdef PALEO_HAVE_ZSTD
    return CacheFlags::ZstdCompressed | paleo::lascache_detail::TaggedColumns;
#else
    return paleo::lascache_detail::TaggedColumns;
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
  // m_diskRoot 由 setDiskRoot 在工程切换时改写——读侧必须持 m_cfgMutex 取
  // 局部拷贝（与写侧的锁声明一致），否则跨线程读写 QString 是数据竞争。
  QString diskRoot;
  {
    QMutexLocker lock(&m_cfgMutex);
    diskRoot = m_diskRoot;
  }
  if (diskRoot.isEmpty())
    return QString();
  const QByteArray h = QCryptographicHash::hash(fingerprint.toUtf8(),
                                                QCryptographicHash::Sha256)
                           .toHex();
  return diskRoot + QLatin1Char('/') + QString::fromLatin1(h.left(24)) +
         QStringLiteral(".plc");
}

bool LasCache::writeDisk(const QString &fingerprint, const LasDoc &doc)
{
  const QString path = diskPathFor(fingerprint);
  if (path.isEmpty()) return false;
  const QByteArray payload = paleo::lascache_detail::encodeDoc(fingerprint, doc);
  if (payload.isEmpty() || !writeCacheFileAtomic(path, QByteArray(kLasCacheMagic, 8), kLasCacheVersion,
                            lasCompressionFlags(), payload))
    return false; // 无缓存继续可用（D2.1 降级口径）
  return true;
}

std::shared_ptr<LasDoc> LasCache::readDisk(const QString &fingerprint)
{
  const QString path = diskPathFor(fingerprint);
  if (path.isEmpty()) return nullptr;
  QByteArray payload;
  QString reason;
  quint16 flags = 0;
  if (!readCacheFile(path, QByteArray(kLasCacheMagic, 8), kOldestLasCacheVersion,
                     kLasCacheVersion, &payload, &flags, &reason))
  {
    if (QFile::exists(path))
    {
      QFile::remove(path); // 损坏/过版 → 自愈删除（D1.2）
      bumpExtra(&CacheStats::selfHeals);
    }
    return nullptr;
  }
  std::shared_ptr<LasDoc> doc;
  if (!paleo::lascache_detail::decodeDoc(payload, fingerprint, flags, &doc))
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
      canon, info.lastModified().toMSecsSinceEpoch(), info.size()) + paleo::native_model::cacheToken(path);

  // 1) 内存命中（指纹匹配才算）。
  if (const auto hit = m_mem.get(canon))
  {
    if (hit->fingerprint == fp)
    {
      QElapsedTimer t;
      t.start();
      {
        QMutexLocker lock(&m_timingsMutex);
        m_timings.memoryHitNs = t.nsecsElapsed();
      }
      return *hit->doc;
    }
    m_mem.erase(canon); // 指纹失配：文件变了（D1.2/D1.10 兜底）
  }

  // 2) 磁盘命中 / 3) 冷解析。同指纹并发只跑一份（D4.7）。
  // RUNTIME-03：job 只按值/本地收集诊断，不捕获调用方 issues 指针。
  const auto ticket = m_inflight.submit(
      fp, [this, &path, &fp]() -> LoadOutcome {
        QElapsedTimer diskTimer;
        diskTimer.start();
        if (auto fromDisk = readDisk(fp))
        {
          {
            QMutexLocker lock(&m_timingsMutex);
            m_timings.diskLoadNs = diskTimer.nsecsElapsed();
          }
          bumpExtra(&CacheStats::diskHits);
          return LoadOutcome{std::move(fromDisk), nullptr};
        }
        std::function<void()> hook;
        {
          QMutexLocker lock(&m_cfgMutex);
          hook = m_coldParseHook;
        }
        if (hook)
          hook();
        QElapsedTimer parseTimer;
        parseTimer.start();
        auto diag = std::make_shared<QList<LasIssue>>();
        LasDoc doc = WellLogRead::parseDoc(path, diag.get()); // 方向44：格式分派
        {
          QMutexLocker lock(&m_timingsMutex);
          m_timings.coldParseNs = parseTimer.nsecsElapsed();
        }
        if (!doc.ok)
          return LoadOutcome{nullptr, std::move(diag)};
        auto shared = std::make_shared<LasDoc>(std::move(doc));
        if (writeDisk(fp, *shared))
          bumpExtra(&CacheStats::diskWrites);
        return LoadOutcome{std::move(shared), std::move(diag)};
      });

  const LoadOutcome outcome = ticket.future.get();
  if (issues && outcome.issues)
    issues->append(*outcome.issues); // 执行者与搭车者各自回填
  const std::shared_ptr<LasDoc> doc = outcome.doc;
  if (!doc)
  {
    LasDoc failed;
    failed.error = QStringLiteral("parse failed: %1").arg(path);
    if (outcome.issues)
      for (const LasIssue &i : *outcome.issues)
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

void LasCache::setColdParseHookForTest(std::function<void()> hook)
{
  QMutexLocker lock(&m_cfgMutex);
  m_coldParseHook = std::move(hook);
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
         PathCanon::fingerprint(canon, info.lastModified().toMSecsSinceEpoch(), info.size()) + paleo::native_model::cacheToken(path);
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
