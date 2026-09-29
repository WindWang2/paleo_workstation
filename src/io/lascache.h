// 层：数据
#pragma once
#include <QList>
#include <QMutex>
#include <QString>
#include <QStringList>

#include "cachecore.h"
#include "inflight.h"
#include "lasdoc.h"
#include "lasparser.h"
#include "lrucache.h"

#include <memory>

// io/ — LAS 解析缓存（wave/io-perf-cache D1.1/D1.2/D1.10）。
//
// 键 = 规范化路径 + mtime + size（文件变更即刻失效，D1.2）；两级：
//   内存：LruCache<canonicalPath, Entry{fingerprint, doc}>（预算治理注册，
//         跨线程并发读安全，同指纹才命中）
//   磁盘：<diskRoot>/<sha256(canon)[0:16]>.plc —— cachecore 版本化格式，
//         payload = 曲线名/单位/描述 + null 值 + f64 曲线列；zstd 可用则压缩。
// 损坏/过版磁盘缓存：删除自愈重解析（selfHeals 计数）。
// 并发：同指纹并发 load 经 InflightCoalescer 只解析一份（D4.7）。
// 写路径（D1.10）：受管 LAS 写入后调 invalidate(path)；指纹失配也兜底。
//
// 性能口径（D1.1/D1.5）：15581 点 LAS 二次打开 <5ms（磁盘缓存直读 + 内存
// 命中在 ~0.1ms 级）。
class LasCache
{
  public:
    static LasCache &shared();

    // 磁盘缓存根（如 <project>/artifacts/index/las）。空 = 纯内存。
    void setDiskRoot(const QString &dir);
    QString diskRoot() const;

    // 内存缓存字节预算（默认 64MB）。0 = 不限（仅治理器收缩）。
    void setMemoryBudget(qint64 bytes);

    // 主入口：内存 → 磁盘 → 解析（顺路回填两级缓存）。
    // 解析失败时 doc.ok=false + doc.error 有文（与 LasParser::parseDoc 同构）。
    LasDoc load(const QString &path, QList<LasIssue> *issues = nullptr);

    // D1.3 批量预取：同 load() 逐个装载进缓存（后台线程调；调度是调用方
    // 的事——PreviewDocService::prefetch 负责排队与优先级）。返回成功条数。
    int prefetch(const QStringList &paths);

    // 失效：指定文件（写路径后）或全部。
    void invalidate(const QString &path = QString());

    // 内存里此刻是否持有效条目（指纹仍匹配）。
    bool isCached(const QString &path) const;

    CacheStats stats() const; // 含 diskHits/diskWrites/selfHeals

    // 只清内存（测试钩子；磁盘条目保留）。
    void clearMemory();

    struct Timings
    {
        qint64 coldParseNs = -1;   // 最近一次解析耗时
        qint64 diskLoadNs = -1;    // 最近一次磁盘命中耗时
        qint64 memoryHitNs = -1;   // 最近一次内存命中耗时
    };
    Timings lastTimings() const { return m_timings; }

  private:
    LasCache();

    struct Entry
    {
        QString fingerprint; // "canon|mtime|size"
        std::shared_ptr<LasDoc> doc;
    };

    static qint64 docBytes(const LasDoc &doc);
    void bumpExtra(qint64 CacheStats::*field);

    // 磁盘 payload 编解码（cachecore 版本化格式之上的一层）。
    bool writeDisk(const QString &fingerprint, const LasDoc &doc);
    std::shared_ptr<LasDoc> readDisk(const QString &fingerprint);

    QString diskPathFor(const QString &fingerprint) const;

    LruCache<QString, Entry> m_mem; // key = canonical path
    mutable QMutex m_cfgMutex;      // 保护 m_diskRoot / m_extra
    QString m_diskRoot;
    CacheStats m_extra;             // disk 层计数（与 m_mem.stats() 合并上报）
    InflightCoalescer<QString, std::shared_ptr<LasDoc>> m_inflight;
    Timings m_timings; // 最近一次（粗粒度诊断面；不做多线程记账）
};
