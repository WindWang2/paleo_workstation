// 层：数据
#pragma once
#include <QHash>
#include <QMutex>
#include <QString>

#include "cachecore.h" // CacheStats

// io/ — SHA-256 计算缓存（wave/io-perf-cache D7.7）。
// 导入去重/外链复验/incremental plan 的 versionBySha256 都会对同一文件反复
// 全量重哈希（966MB 的 SEG-Y 每次都是几秒盘 IO）。这里按「规范化路径 +
// mtime + size」键做两层缓存：内存 LRU + 磁盘 JSON（artifacts/index/sha.json，
// 崩溃重启后仍命中）。指纹失配（文件变了）→ 重算；重算失败 → 如实回空串。
//
// 一致性口径：mtime+size 未变视为内容未变（git 同款启发式）。对「mtime 可
// 伪造」的敌意场景不设防——工程目录内的受管资产有自己的只读权限防线。
class ShaCache
{
  public:
    static ShaCache &shared();

    // 指定磁盘持久化文件（如 <project>/artifacts/index/sha.json）。空 = 纯内存。
    void setDiskFile(const QString &path);
    // 内存条目上限（默认 16384 条，磁盘表另算）。
    void setMemoryLimit(int entries);

    // 命中缓存或重算。error 透传重算失败原因（命中时不动 error）。
    QString sha256Hex(const QString &path, QString *error = nullptr);

    // 失效（写路径后调用；空 path = 全清内存+磁盘）。
    void invalidate(const QString &path = QString());

    struct Counts
    {
        qint64 hits = 0;      // 内存命中
        qint64 diskHits = 0;  // 磁盘命中
        qint64 misses = 0;    // 重算
        qint64 stored = 0;    // 磁盘表条目数
    };
    Counts counts() const;

    // 测试/诊断：当前内存条目数。
    int memoryEntries() const;

  private:
    ShaCache() = default;

    struct Entry
    {
        QString sha; // 空 = 负缓存？不做——重算失败如实返回，不记。
    };

    void loadDiskLocked();
    void saveDiskLocked();

    mutable QMutex m_mutex;
    QHash<QString, QString> m_mem;       // fingerprint("path|mtime|size") -> sha
    QHash<QString, QString> m_disk;      // canonical path -> "mtimeMs|size|sha"
    QHash<QString, qint64> m_diskAge;    // canonical path -> 最近命中 ms（LRU 收缩）
    QString m_diskFile;
    int m_memLimit = 16384;
    int m_diskLimit = 65536;
    Counts m_counts;
};
