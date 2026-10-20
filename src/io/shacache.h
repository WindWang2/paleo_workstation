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
// 一致性口径：默认 mtime+size 未变视为内容未变（git 同款启发式）。对「mtime
// 可伪造」的敌意场景不设防——工程目录内的受管资产有自己的只读权限防线。
// #561 血缘复验加强档：sha256Hex(..., Fingerprint::MtimeSizeInode) 在指纹里
// 追加 POSIX inode（磁盘条目由 3 段升 4 段「mtime|size|inode|sha」，旧 3 段
// 条目强口径不认、重算后自愈覆写）。「同 mtime+size 换文件」在强口径下必击
// 穿缓存。Windows 无 inode：恒 0，退化回默认口径（segyindexstore::Identity
// 同款平台口径，size+mtime 已够）。
class ShaCache
{
  public:
    static ShaCache &shared();

    // 指定磁盘持久化文件（如 <project>/artifacts/index/sha.json）。空 = 纯内存。
    void setDiskFile(const QString &path, bool lazy = false);
    // 内存条目上限（默认 16384 条，磁盘表另算）。
    void setMemoryLimit(int entries);

    // 指纹口径（#561）：默认 MtimeSize（git 同款启发式）；MtimeSizeInode 供
    // 关键血缘复验（导入 dedup 身份 / 入库 sha 留底）防「mtime+size 可伪造」
    // 的缓存投毒。两种口径各自持有内存键，互不串味。
    enum class Fingerprint
    {
        MtimeSize,      // path|mtime|size（磁盘条目 3 段，历史格式）
        MtimeSizeInode, // path|mtime|size|inode（磁盘条目 4 段；Windows 恒 0）
    };

    // 命中缓存或重算。error 透传重算失败原因（命中时不动 error）。
    QString sha256Hex(const QString &path, QString *error = nullptr,
                      Fingerprint fp = Fingerprint::MtimeSize);

    // 失效（写路径后调用；空 path = 全清内存+磁盘）。
    void invalidate(const QString &path = QString());

    // A-01（full-audit-20261021）：磁盘层落盘节流——miss 路径不再每次把全表
    // （上限 65536 条 ≈ 8-13MB JSON）序列化落盘，改为「距上次落盘 ≥
    // intervalMs 或自上次落盘累计 ≥ dirtyEntries 条脏」双阈值惰性写；
    // setDiskFile 换代后首个 miss、invalidate（强制刷）与 flushDisk() 立即
    // 落盘。崩溃窗口
    // 从 0 放宽到单阈值区间（丢失代价 = 重算若干文件哈希，自愈）。测试口径
    // 用 setDiskSaveThrottle 注入。
    void setDiskSaveThrottle(qint64 intervalMs, int dirtyEntries);
    void flushDisk();

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
    void flushDiskLocked();     // A-01：收缩 + 落盘 + 节流复位（调用方持锁）
    void maybeSaveDiskLocked(); // A-01：节流判定（调用方持锁）

    mutable QMutex m_mutex;
    QHash<QString, QString> m_mem;       // fingerprint("path|mtime|size") -> sha
    QHash<QString, QString> m_disk;      // canonical path -> "mtimeMs|size|sha"
    QHash<QString, qint64> m_diskAge;    // canonical path -> 最近命中 ms（LRU 收缩）
    QString m_diskFile;
    quint64 m_diskGeneration = 0; // 每次 setDiskFile 递增
    bool m_diskPending = false;
    int m_memLimit = 16384;
    int m_diskLimit = 65536;
    qint64 m_diskSaveIntervalMs = 1000; // A-01 节流（0 = 每 miss 落盘）
    int m_diskDirtyCount = 0;
    int m_diskDirtyThreshold = 64;
    qint64 m_lastDiskSaveMs = 0;
    Counts m_counts;
};
