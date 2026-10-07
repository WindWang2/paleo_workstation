// 层：数据
#include "segyindexstore.h"

#include "cachecore.h"
#include "pathcanon.h"
#include "../domain/seismic/sgyindexcache.h"

#include <QCryptographicHash>
#include <QDir>
#include <QFile>
#include <QFileInfo>
#include <QHash>
#include <QSet>

#include <sys/stat.h>

namespace
{
  // v4：并行扫描补齐 demo 工区方言（crossline 取 CDP、角点取 181-188）——
  // v3 及更早的大文件索引可能带退化号域（xline 全 0、角点全 0），过版自愈重建。
  constexpr quint16 kSegyIndexVersion = 4;
  const char kSegyIndexMagic[8] = {'P', 'S', 'G', 'Y', 'I', 'D', 'X', '1'};

  quint16 segyIndexCompressionFlags()
  {
#ifdef PALEO_HAVE_ZSTD
    return CacheFlags::ZstdCompressed;
#else
    return CacheFlags::None;
#endif
  }
} // namespace

SegyIndexStore::SegyIndexStore(QString cacheDir) : m_cacheDir(std::move(cacheDir)) {}

SegyIndexStore::Identity SegyIndexStore::identityOf(const QFileInfo &source)
{
  Identity id;
  id.canonicalPath = PathCanon::canonicalize(source.absoluteFilePath());
  id.size = source.size();
  id.mtimeMs = source.lastModified().toMSecsSinceEpoch();
  struct stat st;
  if (::stat(QFile::encodeName(id.canonicalPath).constData(), &st) == 0)
    id.inode = static_cast<quint64>(st.st_ino);
  return id;
}

QByteArray SegyIndexStore::prefixFingerprintOf(const QString &path, qint64 uptoSize)
{
  QFile f(path);
  if (!f.open(QIODevice::ReadOnly))
    return QByteArray();
  const qint64 take = qMin<qint64>(64 * 1024, uptoSize);
  if (take <= 0)
    return QByteArray();
  const QByteArray head = f.read(take);
  if (head.size() != static_cast<int>(take))
    return QByteArray();
  return QCryptographicHash::hash(head, QCryptographicHash::Sha256).toHex();
}

QString SegyIndexStore::cacheFilePathFor(const QFileInfo &source) const
{
  if (m_cacheDir.isEmpty())
    return QString();
  const QString canon = PathCanon::canonicalize(source.absoluteFilePath());
  const QByteArray h =
      QCryptographicHash::hash(canon.toUtf8(), QCryptographicHash::Sha256).toHex();
  return m_cacheDir + QLatin1Char('/') + QStringLiteral("segyidx_") +
         QString::fromLatin1(h.left(24)) + QStringLiteral(".psx");
}

QByteArray SegyIndexStore::encodePayload(const StoredIndex &index)
{
  QByteArray out;
  // 身份块
  cacheio::putStr(&out, index.ident.canonicalPath);
  cacheio::putI64(&out, index.ident.size);
  cacheio::putI64(&out, index.ident.mtimeMs);
  cacheio::putU64(&out, index.ident.inode);
  cacheio::putStr(&out, QString::fromLatin1(index.prefixFingerprint));
  // 头块
  cacheio::putI32(&out, index.samplesPerTrace);
  cacheio::putI32(&out, index.sampleIntervalUs);
  cacheio::putI32(&out, index.formatCode);
  cacheio::putI32(&out, index.binLineNo);
  for (int offset : index.headerWordOffsets) cacheio::putI32(&out, offset);
  cacheio::putI64(&out, index.firstTraceOffset);
  cacheio::putF64(&out, index.geometry.inlineMin);
  cacheio::putF64(&out, index.geometry.inlineMax);
  cacheio::putF64(&out, index.geometry.xlineMin);
  cacheio::putF64(&out, index.geometry.xlineMax);
  for (int i = 0; i < 4; ++i)
  {
    cacheio::putF64(&out, index.geometry.cornerX[i]);
    cacheio::putF64(&out, index.geometry.cornerY[i]);
  }
  cacheio::putF64(&out, index.geometry.startTimeMs);
  // 计数 + checkpoint 状态
  const quint32 n = static_cast<quint32>(index.inlineNos.size());
  const quint32 bad = static_cast<quint32>(index.badTraceOffsets.size());
  cacheio::putU32(&out, n);
  cacheio::putU32(&out, bad);
  out.append(index.complete ? 'C' : 'P'); // complete / partial
  cacheio::putI64(&out, index.scannedOffset);
  // 数组（平行，顺序 = 文件道序）
  for (quint32 i = 0; i < n; ++i)
    cacheio::putI32(&out, index.inlineNos.at(int(i)));
  for (quint32 i = 0; i < n; ++i)
    cacheio::putI32(&out, index.xlineNos.at(int(i)));
  for (quint32 i = 0; i < n; ++i)
    cacheio::putI64(&out, index.offsets.at(int(i)));
  for (quint32 i = 0; i < bad; ++i)
    cacheio::putI64(&out, index.badTraceOffsets.at(int(i)));
  return out;
}

bool SegyIndexStore::decodePayload(const QByteArray &payload, StoredIndex *out)
{
  qint64 pos = 0;
  bool ok = true;
  out->ident.canonicalPath = cacheio::str(payload, &pos, &ok);
  out->ident.size = cacheio::i64(payload, &pos, &ok);
  out->ident.mtimeMs = cacheio::i64(payload, &pos, &ok);
  out->ident.inode = cacheio::u64(payload, &pos, &ok);
  out->prefixFingerprint = cacheio::str(payload, &pos, &ok).toLatin1();
  out->samplesPerTrace = cacheio::i32(payload, &pos, &ok);
  out->sampleIntervalUs = cacheio::i32(payload, &pos, &ok);
  out->formatCode = static_cast<qint16>(cacheio::i32(payload, &pos, &ok));
  out->binLineNo = cacheio::i32(payload, &pos, &ok);
  for (int &offset : out->headerWordOffsets)
  {
    offset = cacheio::i32(payload, &pos, &ok);
    if (offset < 0 || offset > 236) return false;
  }
  out->firstTraceOffset = cacheio::i64(payload, &pos, &ok);
  out->geometry.inlineMin = cacheio::f64(payload, &pos, &ok);
  out->geometry.inlineMax = cacheio::f64(payload, &pos, &ok);
  out->geometry.xlineMin = cacheio::f64(payload, &pos, &ok);
  out->geometry.xlineMax = cacheio::f64(payload, &pos, &ok);
  for (int i = 0; i < 4 && ok; ++i)
  {
    out->geometry.cornerX[i] = cacheio::f64(payload, &pos, &ok);
    out->geometry.cornerY[i] = cacheio::f64(payload, &pos, &ok);
  }
  out->geometry.startTimeMs = cacheio::f64(payload, &pos, &ok);
  const quint32 n = cacheio::u32(payload, &pos, &ok);
  const quint32 bad = cacheio::u32(payload, &pos, &ok);
  if (!ok || pos >= payload.size())
    return false;
  const char marker = payload.at(int(pos));
  ++pos;
  out->complete = (marker == 'C');
  out->scannedOffset = cacheio::i64(payload, &pos, &ok);
  if (!ok || n > 100000000 || bad > 10000000)
    return false; // 病态计数——按损坏自愈
  // 容量合理性：道数 × (4+4+8) 字节必须装得下剩余 payload。
  const qint64 need = qint64(n) * 16 + qint64(bad) * 8;
  if (payload.size() - pos < need)
    return false; // 截断
  out->inlineNos.resize(int(n));
  out->xlineNos.resize(int(n));
  out->offsets.resize(int(n));
  for (quint32 i = 0; i < n && ok; ++i)
    out->inlineNos[int(i)] = cacheio::i32(payload, &pos, &ok);
  for (quint32 i = 0; i < n && ok; ++i)
    out->xlineNos[int(i)] = cacheio::i32(payload, &pos, &ok);
  for (quint32 i = 0; i < n && ok; ++i)
    out->offsets[int(i)] = cacheio::i64(payload, &pos, &ok);
  out->badTraceOffsets.resize(int(bad));
  for (quint32 i = 0; i < bad && ok; ++i)
    out->badTraceOffsets[int(i)] = cacheio::i64(payload, &pos, &ok);
  return ok;
}

std::optional<SegyIndexStore::StoredIndex> SegyIndexStore::load(const QFileInfo &source,
                                                                bool partialOk,
                                                                QString *reason) const
{
  const QString path = cacheFilePathFor(source);
  if (path.isEmpty())
  {
    if (reason) *reason = QStringLiteral("no cache dir");
    return std::nullopt;
  }
  QByteArray payload;
  QString why;
  if (!readCacheFile(path, QByteArray(kSegyIndexMagic, 8), kSegyIndexVersion,
                     kSegyIndexVersion, &payload, nullptr, &why))
  {
    if (QFile::exists(path))
      QFile::remove(path); // 损坏/过版 → 自愈删除（D2.3）
    if (reason) *reason = why;
    return std::nullopt;
  }
  StoredIndex stored;
  if (!decodePayload(payload, &stored))
  {
    QFile::remove(path);
    if (reason) *reason = QStringLiteral("payload decode failed");
    return std::nullopt;
  }
  // D2.2 身份校验：size/mtime/inode 任一变化 → 重建（文件被改写过）。
  const Identity now = identityOf(source);
  if (!stored.ident.matches(now))
  {
    if (reason)
      *reason = QStringLiteral("identity changed (size/mtime/inode)");
    return std::nullopt;
  }
  if (!stored.complete && !partialOk)
  {
    if (reason) *reason = QStringLiteral("checkpoint not requested");
    return std::nullopt;
  }
  // D2.8 checkpoint 完整性审计：
  //  · scannedOffset 必须落在 (firstTraceOffset, size] 且 ≥ 已含道覆盖的偏移。
  //  · complete 时 scannedOffset 应等于文件尾。
  const qint64 traceSpan = stored.offsets.isEmpty()
                               ? 0
                               : stored.offsets.last() + 240 +
                                     qint64(stored.samplesPerTrace) * 4;
  if (stored.scannedOffset < stored.firstTraceOffset ||
      stored.scannedOffset > now.size + 4096 || // 容忍尾部截断的少量越界？不——>
      stored.scannedOffset < traceSpan ||
      (stored.complete && stored.scannedOffset != now.size))
  {
    if (reason) *reason = QStringLiteral("checkpoint audit failed");
    QFile::remove(path);
    return std::nullopt;
  }
  return stored;
}

std::optional<SegyIndexStore::StoredIndex> SegyIndexStore::loadForResume(const QFileInfo &source,
                                                                        QString *reason) const
{
  const QString path = cacheFilePathFor(source);
  if (path.isEmpty())
  {
    if (reason) *reason = QStringLiteral("no cache dir");
    return std::nullopt;
  }
  if (!QFile::exists(path))
  {
    if (reason) *reason = QStringLiteral("no cached index");
    return std::nullopt;
  }
  QByteArray payload;
  QString why;
  if (!readCacheFile(path, QByteArray(kSegyIndexMagic, 8), kSegyIndexVersion,
                     kSegyIndexVersion, &payload, nullptr, &why))
  {
    QFile::remove(path); // 损坏/过版 → 自愈删除（D2.3）
    if (reason) *reason = why;
    return std::nullopt;
  }
  StoredIndex stored;
  if (!decodePayload(payload, &stored))
  {
    QFile::remove(path);
    if (reason) *reason = QStringLiteral("payload decode failed");
    return std::nullopt;
  }
  if (stored.complete)
  {
    if (reason) *reason = QStringLiteral("not a checkpoint");
    return std::nullopt;
  }
  const Identity now = identityOf(source);
  if (stored.ident.canonicalPath != now.canonicalPath || stored.ident.inode != now.inode)
  {
    if (reason) *reason = QStringLiteral("different file (path/inode)");
    return std::nullopt;
  }
  if (now.size < stored.scannedOffset)
  {
    if (reason) *reason = QStringLiteral("file shrank below checkpoint");
    return std::nullopt;
  }
  // 前缀指纹：文件没被改写、只是变长（或没变——上次只是取消）。
  if (stored.prefixFingerprint.isEmpty())
  {
    if (reason) *reason = QStringLiteral("checkpoint has no prefix fingerprint");
    return std::nullopt;
  }
  const QByteArray nowFp = prefixFingerprintOf(now.canonicalPath, qMin<qint64>(64 * 1024, now.size));
  if (nowFp != stored.prefixFingerprint)
  {
    if (reason) *reason = QStringLiteral("prefix rewritten");
    return std::nullopt;
  }
  return stored;
}

bool SegyIndexStore::save(const StoredIndex &index, QString *error) const
{
  const QFileInfo source(index.ident.canonicalPath);
  const QString path = cacheFilePathFor(source);
  if (path.isEmpty())
  {
    if (error) *error = QStringLiteral("no cache dir");
    return false;
  }
  const QByteArray payload = encodePayload(index);
  if (!writeCacheFileAtomic(path, QByteArray(kSegyIndexMagic, 8), kSegyIndexVersion,
                            segyIndexCompressionFlags(), payload, error))
    return false; // 调用方降级为无缓存（D2.1）
  return true;
}

bool SegyIndexStore::remove(const QFileInfo &source) const
{
  const QString path = cacheFilePathFor(source);
  if (path.isEmpty() || !QFile::exists(path))
    return true;
  return QFile::remove(path);
}

SegyIndexStore::IndexStats SegyIndexStore::computeStats(const StoredIndex &index)
{
  IndexStats st;
  st.traceCount = index.inlineNos.size();
  st.badTraces = index.badTraceOffsets.size();
  if (st.traceCount == 0)
    return st;
  st.inlineMin = st.inlineMax = index.inlineNos.first();
  st.xlineMin = st.xlineMax = index.xlineNos.first();
  QSet<quint64> cells;
  cells.reserve(int(st.traceCount));
  for (int i = 0; i < index.inlineNos.size(); ++i)
  {
    st.inlineMin = qMin(st.inlineMin, index.inlineNos.at(i));
    st.inlineMax = qMax(st.inlineMax, index.inlineNos.at(i));
    st.xlineMin = qMin(st.xlineMin, index.xlineNos.at(i));
    st.xlineMax = qMax(st.xlineMax, index.xlineNos.at(i));
    cells.insert((quint64(quint32(index.inlineNos.at(i))) << 32) |
                 quint32(index.xlineNos.at(i)));
  }
  st.presentCells = cells.size();
  st.gridCells = qint64(st.inlineMax - st.inlineMin + 1) * qint64(st.xlineMax - st.xlineMin + 1);
  st.missingCells = qMax<qint64>(0, st.gridCells - st.presentCells);
  st.densityPercent = st.gridCells > 0 ? 100.0 * double(st.presentCells) / double(st.gridCells) : 0.0;
  return st;
}

QString SegyIndexStore::statsSummary(const IndexStats &st)
{
  return QStringLiteral("traces=%1 bad=%2 inline=[%3..%4] xline=[%5..%6] cells=%7/%8 (%9%) missing=%10")
      .arg(st.traceCount)
      .arg(st.badTraces)
      .arg(st.inlineMin)
      .arg(st.inlineMax)
      .arg(st.xlineMin)
      .arg(st.xlineMax)
      .arg(st.presentCells)
      .arg(st.gridCells)
      .arg(QString::number(st.densityPercent, 'f', 2))
      .arg(st.missingCells);
}

void SegyIndexStore::ensureLegacyGlobalCacheDir()
{
  static bool done = false;
  if (done)
    return;
  done = true;
  // vendor/sbm SgyIndexCache 在 rename 发布前不建父目录——我们把目录建好，它的
  // 原子发布不再 ENOENT（D2.1 的仓外根治）。目录解析直接问 vendor（同一份
  // 口径，不再在这里复刻）：SEISMIC_INDEX_CACHE_DIR → %LOCALAPPDATA% /
  // $XDG_CACHE_HOME / ~/.cache → tmp/paleo_workstation-<uid>（审计 01：不再
  // 落多用户共享的 /tmp/paleo_workstation）。
  const QString dir =
      QString::fromStdU16String(seismic::SgyIndexCache::CacheDirectory().u16string());
  QDir().mkpath(dir);
  // 我们建的 paleo_workstation 层收紧为仅属主可访问（索引内容 = 工区文件路径与几何）。
  const QString parent = QFileInfo(dir).absolutePath();
  if (QFileInfo(parent).fileName().startsWith(QLatin1String("paleo_workstation")))
    QFile::setPermissions(parent, QFileDevice::ReadOwner | QFileDevice::WriteOwner |
                                      QFileDevice::ExeOwner | QFileDevice::ReadUser |
                                      QFileDevice::WriteUser | QFileDevice::ExeUser);
}
