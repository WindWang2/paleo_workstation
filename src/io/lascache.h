// 层：数据
#pragma once
#include <QList>
#include <QMutex>
#include <QString>
#include <QStringList>

#include "cachecore.h"
#include "inflight.h"
#include "lasdoc.h" // LasDoc / LasIssue（载荷契约；解析入口 lasparser.h 不进缓存头）
#include "lrucache.h"

#include <functional>
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
    // issues 契约（RUNTIME-03，方向58）：本次调用**走到解析**时（执行者或同
    // 指纹并发搭车者），*issues 追加该次解析的完整诊断——每个调用方各自
    // 回填，不再只有首个提交者拿到。内存/磁盘命中不重放诊断（诊断不入缓存，
    // 与历史行为一致）；需要诊断的调用方应在命中前 invalidate 或直走 parser。
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

    // 测试钩子：冷解析前回调（在执行者线程上、job 内调用），用于确定性构造
    // 同指纹并发；ridersJoined = 合并器累计搭车数。生产不设钩子。
    void setColdParseHookForTest(std::function<void()> hook);
    int ridersJoinedForTest() const { return m_inflight.ridersJoined(); }

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
    // RUNTIME-03：合并器结果携带「文档 + 本次解析诊断」——诊断随 future 分发
    // 给每个等待方各自拷贝；job 绝不捕获任何调用方的 out 指针（旧实现按引用
    // 捕获首个提交者的 issues，搭车者永远拿不到诊断）。LasDoc 保持纯值类型、
    // 磁盘 payload 格式不变（未选「LasDoc 加 issues 字段」方案）。
    struct LoadOutcome
    {
        std::shared_ptr<LasDoc> doc;
        std::shared_ptr<const QList<LasIssue>> issues; // 走解析时非空指针
    };
    InflightCoalescer<QString, LoadOutcome> m_inflight;
    std::function<void()> m_coldParseHook; // 测试钩子（m_cfgMutex 保护）
    Timings m_timings; // 最近一次（粗粒度诊断面；不做多线程记账）
};
