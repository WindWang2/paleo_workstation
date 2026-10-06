// 层：数据
#include "cachecore.h"

#include "../metadata/atomicfile.h"

#include <QCoreApplication>
#include <QDateTime>
#include <QDir>
#include <QFile>
#include <QFileInfo>
#include <QtEndian>

#include <cstring>

#ifdef PALEO_HAVE_ZSTD
#include <zstd.h>
#endif

namespace
{
  // 头布局：magic 8 + version 2 + flags 2 + headerCrc 4 + payloadCrc 4 +
  // payloadSize 8 + storedSize 8 = 36 字节（早期误标 32 导致写越界/读穿——
  // 见 wave/io-perf-cache D2.3 自愈测试）。
  constexpr int kHeaderSize = 36;

  // 标准 CRC-32（IEEE 802.3 反射多项式 0xEDB88320），slicing-by-8 表驱动。
  // 输出与逐字节查表实现逐位一致（磁盘格式不变）；吞吐约 4-6×——磁盘命中
  // 路径对整个 payload 做校验，逐字节版在 ~600KB 级 payload 上占 ~1ms，
  // 是二次打开（D1.1）的主要固定开销。表用函数内 static 初始化（线程安全）。
  struct Crc32Tables
  {
    quint32 t[8][256];
    Crc32Tables()
    {
      for (quint32 i = 0; i < 256; ++i)
      {
        quint32 c = i;
        for (int k = 0; k < 8; ++k)
          c = (c & 1u) ? (0xEDB88320u ^ (c >> 1)) : (c >> 1);
        t[0][i] = c;
      }
      for (quint32 i = 0; i < 256; ++i)
        for (int s = 1; s < 8; ++s)
          t[s][i] = (t[s - 1][i] >> 8) ^ t[0][t[s - 1][i] & 0xFFu];
    }
  };

  quint32 crc32Bytes(const QByteArray &data)
  {
    static const Crc32Tables tables;
    const auto &T = tables.t;
    const uchar *p = reinterpret_cast<const uchar *>(data.constData());
    qsizetype n = data.size();
    quint32 crc = 0xFFFFFFFFu;
    while (n >= 8)
    {
      const quint32 lo = crc ^ (quint32(p[0]) | (quint32(p[1]) << 8) |
                                (quint32(p[2]) << 16) | (quint32(p[3]) << 24));
      const quint32 hi = quint32(p[4]) | (quint32(p[5]) << 8) |
                         (quint32(p[6]) << 16) | (quint32(p[7]) << 24);
      crc = T[7][lo & 0xFFu] ^ T[6][(lo >> 8) & 0xFFu] ^ T[5][(lo >> 16) & 0xFFu] ^
            T[4][lo >> 24] ^ T[3][hi & 0xFFu] ^ T[2][(hi >> 8) & 0xFFu] ^
            T[1][(hi >> 16) & 0xFFu] ^ T[0][hi >> 24];
      p += 8;
      n -= 8;
    }
    while (n-- > 0)
      crc = T[0][(crc ^ *p++) & 0xFFu] ^ (crc >> 8);
    return crc ^ 0xFFFFFFFFu;
  }

  QByteArray makeHeader(const QByteArray &magic, quint16 version, quint16 flags,
                        quint32 payloadCrc, quint64 payloadSize, quint64 storedSize)
  {
    QByteArray h;
    h.resize(kHeaderSize);
    char *p = h.data();
    memset(p, 0, kHeaderSize);
    memcpy(p, magic.constData(), qMin<qsizetype>(8, magic.size()));
    qToLittleEndian<quint16>(version, p + 8);
    qToLittleEndian<quint16>(flags, p + 10);
    // headerCrc 先置 0 再算——覆盖前 16 字节。
    qToLittleEndian<quint32>(0, p + 12);
    const quint32 headerCrc = crc32Bytes(QByteArray(p, 16));
    qToLittleEndian<quint32>(headerCrc, p + 12);
    qToLittleEndian<quint32>(payloadCrc, p + 16);
    qToLittleEndian<quint64>(payloadSize, p + 20);
    qToLittleEndian<quint64>(storedSize, p + 28);
    return h;
  }
} // namespace

QByteArray cacheZstdCompress(const QByteArray &raw, bool *ok)
{
#ifdef PALEO_HAVE_ZSTD
  const size_t bound = ZSTD_compressBound(static_cast<size_t>(raw.size()));
  QByteArray out(static_cast<qsizetype>(bound), Qt::Uninitialized);
  const size_t written = ZSTD_compress(out.data(), bound, raw.constData(),
                                       static_cast<size_t>(raw.size()), 3);
  if (ZSTD_isError(written))
  {
    if (ok) *ok = false;
    return QByteArray();
  }
  out.truncate(static_cast<qsizetype>(written));
  if (ok) *ok = true;
  return out;
#else
  Q_UNUSED(raw);
  if (ok) *ok = false;
  return QByteArray();
#endif
}

QByteArray cacheZstdDecompress(const QByteArray &compressed, qint64 uncompressedSize,
                               bool *ok)
{
#ifdef PALEO_HAVE_ZSTD
  if (uncompressedSize < 0 || uncompressedSize > (1ll << 33))
  {
    if (ok) *ok = false;
    return QByteArray();
  }
  // #217：声明的解压尺寸来自缓存头（不在 headerCrc 覆盖内），单比特损坏即可
  // 驱动至多 8GB 分配。帧头自带 content size（ZSTD_compress 必写）时先比对，
  // 不符直接判损坏，不按声明值分配。
  const unsigned long long frameSize =
      ZSTD_getFrameContentSize(compressed.constData(), static_cast<size_t>(compressed.size()));
  if (frameSize == ZSTD_CONTENTSIZE_ERROR ||
      (frameSize != ZSTD_CONTENTSIZE_UNKNOWN &&
       frameSize != static_cast<unsigned long long>(uncompressedSize)))
  {
    if (ok) *ok = false;
    return QByteArray();
  }
  QByteArray out(static_cast<qsizetype>(uncompressedSize), Qt::Uninitialized);
  const size_t written = ZSTD_decompress(out.data(), static_cast<size_t>(uncompressedSize),
                                         compressed.constData(),
                                         static_cast<size_t>(compressed.size()));
  if (ZSTD_isError(written) || written != static_cast<size_t>(uncompressedSize))
  {
    if (ok) *ok = false;
    return QByteArray();
  }
  if (ok) *ok = true;
  return out;
#else
  Q_UNUSED(compressed);
  Q_UNUSED(uncompressedSize);
  if (ok) *ok = false;
  return QByteArray();
#endif
}

bool writeCacheFileAtomic(const QString &path, const QByteArray &magic,
                          quint16 formatVersion, quint16 flags,
                          const QByteArray &payload, QString *error)
{
  if (path.isEmpty())
  {
    if (error) *error = QStringLiteral("cache path is empty");
    return false;
  }
  const QFileInfo info(path);
  // D2.1：父目录递归建——rename 到不存在的目录必然 ENOENT。
  if (!info.dir().isReadable() && !QDir().mkpath(info.absolutePath()))
  {
    // mkpath 返回 false 也可能是并发已建好——再查一次。
    if (!QDir(info.absolutePath()).exists())
    {
      if (error) *error = QStringLiteral("cannot create cache dir %1").arg(info.absolutePath());
      return false;
    }
  }

  const bool compress = flags & CacheFlags::ZstdCompressed;
  QByteArray stored = payload;
  if (compress)
  {
    bool zok = false;
    stored = cacheZstdCompress(payload, &zok);
    if (!zok)
    {
      if (error) *error = QStringLiteral("zstd compress failed for %1").arg(path);
      return false;
    }
  }

  const quint32 payloadCrc = crc32Bytes(stored);
  const QByteArray header = makeHeader(magic, formatVersion, flags, payloadCrc,
                                       static_cast<quint64>(payload.size()),
                                       static_cast<quint64>(stored.size()));

  const QString tmp = QStringLiteral("%1.tmp%2_%3").arg(path).arg(QCoreApplication::applicationPid()).arg(QDateTime::currentMSecsSinceEpoch() & 0xFFFF);
  for (int attempt = 0; attempt < 2; ++attempt)
  {
    QFile f(tmp);
    if (f.open(QIODevice::WriteOnly | QIODevice::Truncate))
    {
      if (f.write(header) == header.size() && f.write(stored) == stored.size() && f.flush())
      {
        f.close();
        if (paleoReplaceFile(tmp, path))
          return true;
      }
      else
      {
        f.close();
      }
    }
    // 重试前把可能的残骸清掉（上次崩溃留下的只读 tmp / 磁满缓解等）。
    QFile::remove(tmp);
  }
  if (error) *error = QStringLiteral("cannot publish cache file %1").arg(path);
  return false;
}

bool readCacheFile(const QString &path, const QByteArray &magic,
                   quint16 minVersion, quint16 maxVersion,
                   QByteArray *payload, quint16 *flags, QString *reason)
{
  QFile f(path);
  if (!f.open(QIODevice::ReadOnly))
  {
    if (reason) *reason = QStringLiteral("cannot open");
    return false;
  }
  if (f.size() < kHeaderSize)
  {
    if (reason) *reason = QStringLiteral("truncated header");
    return false;
  }
  const QByteArray header = f.read(kHeaderSize);
  const char *p = header.constData();
  if (memcmp(p, magic.constData(), qMin<qsizetype>(8, magic.size())) != 0)
  {
    if (reason) *reason = QStringLiteral("bad magic");
    return false;
  }
  const quint16 version = qFromLittleEndian<quint16>(p + 8);
  if (version < minVersion || version > maxVersion)
  {
    if (reason) *reason = QStringLiteral("format version %1 not in [%2,%3]").arg(version).arg(minVersion).arg(maxVersion);
    return false;
  }
  const quint16 fileFlags = qFromLittleEndian<quint16>(p + 10);
  const quint32 headerCrcStored = qFromLittleEndian<quint32>(p + 12);
  QByteArray crcInput(header, 16);
  memcpy(crcInput.data() + 12, "\0\0\0\0", 4);
  if (crc32Bytes(crcInput) != headerCrcStored)
  {
    if (reason) *reason = QStringLiteral("header crc mismatch");
    return false;
  }
  const quint32 payloadCrc = qFromLittleEndian<quint32>(p + 16);
  const quint64 payloadSize = qFromLittleEndian<quint64>(p + 20);
  const quint64 storedSize = qFromLittleEndian<quint64>(p + 28);
  // #217：payloadSize/storedSize 不在 headerCrc 覆盖内——读前按文件实际大小
  // 封顶，损坏的尺寸字段走「损坏自愈」报因，而不是按声明值巨量分配崩溃。
  if (storedSize > static_cast<quint64>(f.size() - kHeaderSize))
  {
    if (reason) *reason = QStringLiteral("truncated payload");
    return false;
  }

  const QByteArray stored = f.read(static_cast<qint64>(storedSize));
  if (static_cast<quint64>(stored.size()) != storedSize)
  {
    if (reason) *reason = QStringLiteral("truncated payload");
    return false;
  }
  if (crc32Bytes(stored) != payloadCrc)
  {
    if (reason) *reason = QStringLiteral("payload crc mismatch");
    return false;
  }

  if (fileFlags & CacheFlags::ZstdCompressed)
  {
    bool zok = false;
    const QByteArray raw = cacheZstdDecompress(stored, static_cast<qint64>(payloadSize), &zok);
    if (!zok)
    {
      if (reason) *reason = QStringLiteral("zstd decompress failed");
      return false;
    }
    *payload = raw;
  }
  else
  {
    if (static_cast<quint64>(stored.size()) != payloadSize)
    {
      if (reason) *reason = QStringLiteral("payload size mismatch");
      return false;
    }
    *payload = stored;
  }
  if (flags) *flags = fileFlags;
  return true;
}

// ---------------------------------------------------------------------------
// 小端原语
// ---------------------------------------------------------------------------
namespace cacheio
{
  void putU16(QByteArray *out, quint16 v)
  {
    char b[2];
    qToLittleEndian<quint16>(v, b);
    out->append(b, 2);
  }
  void putU32(QByteArray *out, quint32 v)
  {
    char b[4];
    qToLittleEndian<quint32>(v, b);
    out->append(b, 4);
  }
  void putU64(QByteArray *out, quint64 v)
  {
    char b[8];
    qToLittleEndian<quint64>(v, b);
    out->append(b, 8);
  }
  void putI32(QByteArray *out, qint32 v) { putU32(out, static_cast<quint32>(v)); }
  void putI64(QByteArray *out, qint64 v) { putU64(out, static_cast<quint64>(v)); }
  void putF32(QByteArray *out, float v)
  {
    quint32 bits = 0;
    memcpy(&bits, &v, 4);
    putU32(out, bits);
  }
  void putF64(QByteArray *out, double v)
  {
    quint64 bits = 0;
    memcpy(&bits, &v, 8);
    putU64(out, bits);
  }
  void putStr(QByteArray *out, const QString &s)
  {
    const QByteArray utf8 = s.toUtf8();
    putU16(out, static_cast<quint16>(qMin<qsizetype>(0xFFFF, utf8.size())));
    out->append(utf8.constData(), static_cast<int>(utf8.size()));
  }

  namespace
  {
    bool take(const QByteArray &in, qint64 *pos, qint64 n, const char **out)
    {
      if (in.size() - *pos < n)
        return false;
      *out = in.constData() + *pos;
      *pos += n;
      return true;
    }
  } // namespace

  quint16 u16(const QByteArray &in, qint64 *pos, bool *ok)
  {
    const char *p = nullptr;
    if (!take(in, pos, 2, &p)) { if (ok) *ok = false; return 0; }
    if (ok) *ok = true;
    return qFromLittleEndian<quint16>(p);
  }
  quint32 u32(const QByteArray &in, qint64 *pos, bool *ok)
  {
    const char *p = nullptr;
    if (!take(in, pos, 4, &p)) { if (ok) *ok = false; return 0; }
    if (ok) *ok = true;
    return qFromLittleEndian<quint32>(p);
  }
  quint64 u64(const QByteArray &in, qint64 *pos, bool *ok)
  {
    const char *p = nullptr;
    if (!take(in, pos, 8, &p)) { if (ok) *ok = false; return 0; }
    if (ok) *ok = true;
    return qFromLittleEndian<quint64>(p);
  }
  qint32 i32(const QByteArray &in, qint64 *pos, bool *ok)
  {
    return static_cast<qint32>(u32(in, pos, ok));
  }
  qint64 i64(const QByteArray &in, qint64 *pos, bool *ok)
  {
    return static_cast<qint64>(u64(in, pos, ok));
  }
  float f32(const QByteArray &in, qint64 *pos, bool *ok)
  {
    const quint32 bits = u32(in, pos, ok);
    float v = 0.0f;
    memcpy(&v, &bits, 4);
    return v;
  }
  double f64(const QByteArray &in, qint64 *pos, bool *ok)
  {
    const quint64 bits = u64(in, pos, ok);
    double v = 0.0;
    memcpy(&v, &bits, 8);
    return v;
  }
  QString str(const QByteArray &in, qint64 *pos, bool *ok)
  {
    const quint16 len = u16(in, pos, ok);
    if (!ok || *ok == false)
      return QString();
    if (in.size() - *pos < len)
    {
      if (ok) *ok = false;
      return QString();
    }
    const QString s = QString::fromUtf8(in.constData() + *pos, len);
    *pos += len;
    return s;
  }
} // namespace cacheio
