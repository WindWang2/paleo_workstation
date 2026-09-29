// 层：数据
#pragma once
#include <QByteArray>
#include <QFileInfo>
#include <QString>
#include <QVector>

#include "../domain/sectiontrace.h" // SegyGeometry

// io/ — SEG-Y 道头索引的磁盘持久化（wave/io-perf-cache D2）。
//
// SegyReader::open() 每次会话都要重扫全部道头（大 survey 数百万道 × 240B =
// 分钟级盘 IO）。本存储把「道级索引 + survey 几何 + 身份指纹」落在
// <cacheDir>/segyidx_<sha(path)>.psx（cachecore 版本化格式 + zstd 压缩，
// D2.3/D2.4），open 时身份命中即免扫（D2.2：size/mtime/inode 任一变 → 重建）。
//
// Checkpoint（D2.8）：扫描被取消/中断时可落「部分索引」（complete=false +
// scannedOffset），下次打开从断点续扫（D2.5 增量语义同时覆盖「文件追加增长」
// ——只要前缀指纹吻合）。
//
// D2.1 的教训（Publishing failed: No such file or directory）由 cachecore 的
// writeCacheFileAtomic 兜住：mkdir -p + 重试 + 失败降级为无缓存。
class SegyIndexStore
{
  public:
    struct Identity
    {
        QString canonicalPath;
        qint64 size = -1;
        qint64 mtimeMs = -1;
        quint64 inode = 0; // POSIX；Windows 恒 0（size+mtime 已够）

        bool matches(const Identity &o) const
        {
          return canonicalPath == o.canonicalPath && size == o.size &&
                 mtimeMs == o.mtimeMs && inode == o.inode;
        }
    };

    // 道级索引快照（与 SegyReader 内部结构一一对应；inlineNos/xlineNos/offsets
    // 平行数组，顺序 = 文件道序）。badTraceOffsets 记录损坏被跳过的道（D2.7）。
    struct StoredIndex
    {
        Identity ident;
        int samplesPerTrace = 0;
        int sampleIntervalUs = 0;
        qint16 formatCode = 0;
        qint32 binLineNo = 0;
        qint64 firstTraceOffset = 0;
        SegyGeometry geometry;
        QVector<qint32> inlineNos;
        QVector<qint32> xlineNos;
        QVector<qint64> offsets;
        QVector<qint64> badTraceOffsets;
        bool complete = true;      // false = checkpoint（部分扫描）
        qint64 scannedOffset = 0;  // 已扫到的文件偏移（complete 时 = 文件尾）
        // 增量续扫的前缀指纹（首 64KB 哈希，十六进制；空 = 无）
        QByteArray prefixFingerprint;
    };

    explicit SegyIndexStore(QString cacheDir);

    // 读缓存：身份（size/mtime/inode）+ 前缀指纹任一失配 → nullopt + reason。
    // 损坏（CRC/截断/过版）→ 删除文件自愈 + nullopt。
    // partialOk=true 时允许返回 checkpoint（complete=false）条目。
    std::optional<StoredIndex> load(const QFileInfo &source, bool partialOk,
                                    QString *reason = nullptr) const;

    // D2.5 增量续扫装载：忽略 size/mtime 失配（文件可能只是被追加），判据换成
    // inode 一致 + 首 64KB 前缀指纹一致 + 存的是 checkpoint。文件被改写（前缀
    // 不同）→ nullopt。命中后调用方从 scannedOffset 续扫，完成后按新身份重存。
    std::optional<StoredIndex> loadForResume(const QFileInfo &source,
                                             QString *reason = nullptr) const;

    // 原子发布（mkdir -p + tmp + rename + 一次重试）；失败 false（调用方无缓存继续）。
    bool save(const StoredIndex &index, QString *error = nullptr) const;

    // 删除该源的缓存（invalidate）。
    bool remove(const QFileInfo &source) const;

    QString cacheFilePathFor(const QFileInfo &source) const;

    // ---- D2.6 索引统计（空洞报告）----
    struct IndexStats
    {
        qint64 traceCount = 0;
        qint64 badTraces = 0;
        qint32 inlineMin = 0, inlineMax = 0;
        qint32 xlineMin = 0, xlineMax = 0;
        qint64 gridCells = 0;      // bounding 网格应有道数
        qint64 presentCells = 0;   // 实际 (inline,xline) 去重数
        qint64 missingCells = 0;   // 空洞数
        double densityPercent = 0.0;
    };
    static IndexStats computeStats(const StoredIndex &index);

    // 统计口径的 JSON 化（selfcheck/诊断输出用）。
    static QString statsSummary(const IndexStats &st);

    // D2.1 配套：确保 vendor SgyIndexCache 的全局缓存目录存在（它 rename 发布
    // 前不建父目录——我们在外面把目录建好，发布就不会 ENOENT）。
    static void ensureLegacyGlobalCacheDir();

    // 源文件身份采样（stat + inode）。
    static Identity identityOf(const QFileInfo &source);
    // 首 64KB 前缀指纹（增量续扫的「文件没被改写只被追加」判据）。
    static QByteArray prefixFingerprintOf(const QString &path, qint64 uptoSize);

  private:
    static QByteArray encodePayload(const StoredIndex &index);
    static bool decodePayload(const QByteArray &payload, StoredIndex *out);

    QString m_cacheDir;
};
