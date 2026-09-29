// 层：数据
#pragma once
#include <QHash>
#include <QMutex>
#include <QString>
#include <QVector>

#include "cachecore.h" // CacheStats / EvictableCache 所需口径

#include <functional>

// io/ — 全局缓存预算治理（wave/io-perf-cache D6）。
//
// EvictableCache：每个内存缓存（LAS doc / 金字塔瓦片 / SEG-Y 索引快照 …）
// 实现这个面并向管理器注册（D6.2）；管理器只认接口不认类型。
class EvictableCache
{
  public:
    virtual ~EvictableCache() = default;
    virtual QString cacheId() const = 0;
    virtual qint64 bytes() const = 0;          // 当前占用（字节，按条目 sizeFn 计）
    virtual qint64 capacity() const = 0;
    // 从最久未用端逐出，直到本缓存 bytes() <= targetBytes（pin 的条目不动）；
    // 返回实际释放的字节数。
    virtual qint64 evictLRUEntries(qint64 targetBytes) = 0;
    virtual CacheStats stats() const = 0;
    // 最近一次 get/insert 的墙钟（ms）——预算超限时按「最远未用的缓存先收缩」。
    virtual qint64 lastAccessMs() const = 0;
};

// D6.1 全局预算：默认 512MB，QSettings("paleo","paleo") 的 cache/budgetMiB
// 可覆盖（loadFromSettings()；UI/组装根启动时调一次即可，数据层自身无 UI）。
// 进程级泄漏式单例：缓存对象可能在 main 返回后才析构（静态 LRU 等），管理器
// 故意不随进程退出销毁，避免析构序悬垂。
class CacheBudgetManager
{
  public:
    static CacheBudgetManager *instance();

    // ---- 注册面（D6.2；LruCache 构造/析构自动调用）----
    void registerCache(EvictableCache *cache);
    void unregisterCache(EvictableCache *cache);

    // ---- 预算 ----
    qint64 budgetBytes() const { return m_budgetBytes; }
    void setBudgetBytes(qint64 bytes); // 立即 enforce
    qint64 usedBytes() const;
    int usedPercent() const;
    static constexpr qint64 kDefaultBudgetMiB = 512;
    void loadFromSettings(); // QSettings("paleo","paleo") cache/budgetMiB，缺省 512

    // ---- D6.3 超限逐出：最远未用的缓存先收缩，目标压到预算的 90%。----
    // 返回总共释放的字节数。
    qint64 enforce();

    // ---- D6.4 压力广播：usedPercent 跨过 75%/90% 阈值时向处理器广播 ----
    // （缓存自身可在回调里主动 shrink）。返回处理器 id（removePressureHandler 用）。
    using PressureHandler = std::function<void(int percentUsed)>;
    quint64 addPressureHandler(PressureHandler handler);
    void removePressureHandler(quint64 id);

    // ---- D6.5 每缓存统计（selfcheck/测试面）----
    struct NamedStats
    {
        QString id;
        CacheStats stats;
        qint64 bytes = 0;
        qint64 capacity = 0;
    };
    QVector<NamedStats> allStats() const;

    // ---- D6.6 大对象分配审计：单次 >10MB 的缓冲分配登记处 ----
    // （解析器/索引装载读大文件前的自报；selfcheck 输出审计面。）
    struct LargeAlloc
    {
        QString where;
        qint64 bytes = 0;
        qint64 whenMs = 0;
    };
    void noteLargeAllocation(const QString &where, qint64 bytes);
    QVector<LargeAlloc> largeAllocations(int maxCount = 64) const;
    void clearLargeAllocations();
    static constexpr qint64 kLargeAllocThreshold = 10 * 1024 * 1024;

  private:
    CacheBudgetManager() = default;

    void notifyPressure();

    mutable QMutex m_mutex; // 保护下面全部字段（缓存注册/处理器/审计）
    QVector<EvictableCache *> m_caches;
    qint64 m_budgetBytes = kDefaultBudgetMiB * 1024 * 1024;
    int m_lastNotifiedTier = 0; // 0=安全 1=>75% 2=>90%；跨档才广播，防刷屏
    QHash<quint64, PressureHandler> m_handlers;
    quint64 m_nextHandlerId = 1;
    QVector<LargeAlloc> m_largeAllocs;
};
