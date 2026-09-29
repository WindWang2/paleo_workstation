// 层：数据
#pragma once
#include <QByteArray>
#include <QtGlobal>
#include <QString>

// io/ — 磁盘缓存公共底座（wave/io-perf-cache D2.1/D2.3/D2.4）。
// 所有落盘缓存（LAS 解析缓存 / SEG-Y 道索引 / SHA 摘要）共用同一格式：
//   [magic 8B][formatVersion u16][flags u16][headerCrc u32]  （头共 36 字节定长小端）
//   [payloadCrc u32][payloadSize u64（解压后）][storedSize u64（落盘字节数）]
//   [payload ……]
// 头 36 字节定长小端。headerCrc 覆盖前 16 字节；payloadCrc 覆盖（可能压缩过
// 的）落盘 payload。读侧任一校验失败 → 调用方删除缓存文件自愈重建，绝不
// 把坏数据当命中（D2.2/D2.3）。
//
// D2.1 的教训固化在这里：发布 = 写 <path>.tmp<pid> → paleoReplaceFile 原子
// 替换；替换前先递归建父目录（「Publishing the cache file failed: No such
// file or directory」的根因就是父目录没人建），rename 失败重试一次，仍失败
// 则如实返回 false——调用方降级为「无缓存继续可用」，不向上抛错。

// 统一缓存统计口径（D6.5：selfcheck/测试直接读）。
struct CacheStats
{
  qint64 hits = 0;        // 内存命中
  qint64 misses = 0;
  qint64 evictions = 0;   // LRU 逐出（含预算收缩触发）
  qint64 diskHits = 0;    // 磁盘命中（免重解析/重建）
  qint64 diskWrites = 0;
  qint64 selfHeals = 0;   // 损坏/过版缓存被删除重建的次数
  qint64 pinnedSkips = 0; // 因 pin 跳过的逐出

  double hitRatePercent() const
  {
    const qint64 total = hits + misses;
    return total > 0 ? 100.0 * hits / total : 0.0;
  }
};

// 落盘缓存的 flag 位（flags 字段）。
namespace CacheFlags
{
  constexpr quint16 None = 0;
  constexpr quint16 ZstdCompressed = 1u << 0; // payload 经 zstd 压缩（读侧按位解压）
}

// zstd 压缩/解压（PALEO_HAVE_ZSTD 定义时可用；否则 ok=false 回空调用方自行
// 走未压缩路径）。压缩对「未压缩重复性强的整数/浮点数组」收益最大（D2.4）。
QByteArray cacheZstdCompress(const QByteArray &raw, bool *ok);
QByteArray cacheZstdDecompress(const QByteArray &compressed, qint64 uncompressedSize, bool *ok);

// 原子写一个缓存文件：mkdir -p 父目录 → 写临时文件 → rename（失败重试一次）。
// 失败只返回 false + error（例如目录只读盘）——调用方必须能在无缓存下继续。
bool writeCacheFileAtomic(const QString &path, const QByteArray &magic,
                          quint16 formatVersion, quint16 flags,
                          const QByteArray &payload, QString *error = nullptr);

// 读缓存文件。magic 不符 / version 超出 [minVersion, maxVersion] / 尺寸与头
// 不符 / 任一 CRC 失败 → false，reason 带拒因；调用方删除文件自愈。
// 成功 → payload 为（已解压的）原始字节，flags 回填写入时的 flag 位。
bool readCacheFile(const QString &path, const QByteArray &magic,
                   quint16 minVersion, quint16 maxVersion,
                   QByteArray *payload, quint16 *flags = nullptr,
                   QString *reason = nullptr);

// 定长小端序列化原语（缓存格式专用；保持与 Qt 版本无关的字节布局）。
namespace cacheio
{
  void putU16(QByteArray *out, quint16 v);
  void putU32(QByteArray *out, quint32 v);
  void putU64(QByteArray *out, quint64 v);
  void putI32(QByteArray *out, qint32 v);
  void putI64(QByteArray *out, qint64 v);
  void putF32(QByteArray *out, float v);
  void putF64(QByteArray *out, double v);
  void putStr(QByteArray *out, const QString &s); // u16 长度 + UTF-8

  quint16 u16(const QByteArray &in, qint64 *pos, bool *ok);
  quint32 u32(const QByteArray &in, qint64 *pos, bool *ok);
  quint64 u64(const QByteArray &in, qint64 *pos, bool *ok);
  qint32 i32(const QByteArray &in, qint64 *pos, bool *ok);
  qint64 i64(const QByteArray &in, qint64 *pos, bool *ok);
  float f32(const QByteArray &in, qint64 *pos, bool *ok);
  double f64(const QByteArray &in, qint64 *pos, bool *ok);
  QString str(const QByteArray &in, qint64 *pos, bool *ok);
} // namespace cacheio
