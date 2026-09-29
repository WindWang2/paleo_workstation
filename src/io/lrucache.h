// 层：数据
#pragma once
#include <QDateTime>
#include <QHash>
#include <QMutex>
#include <QString>
#include <QVector>

#include "cachebudget.h"

#include <functional>
#include <iterator>
#include <list>
#include <optional>

// io/ — 通用线程安全 LRU 缓存模板（wave/io-perf-cache D6 基建）。
//
// · 条目大小经 SizeFn 估算（字节），容量按字节计——预算管理器按同一口径收账。
// · get() 会「触摸」条目（MRU 端移动）并计数命中/未命中；peek() 只读不触摸。
// · pin（D6.3）：被 pin 的条目逐出时跳过（正在使用的瓦片/文档不被回收）。
// · 构造时向 CacheBudgetManager 注册、析构时注销——预算治理自动覆盖。
// · Value 拷贝必须便宜：存 shared_ptr<T> 是推荐姿势（get 返回指针拷贝，
//   缓存内部重组不会让使用方悬垂）。
// · 并发口径：任意方法可从任意线程调；enforce() 触发的逐出与本缓存插入
//   都在同一互斥下串行，不重入（evictLocked 不回调外部代码）。
template <typename Key, typename Value>
class LruCache final : public EvictableCache
{
  public:
    using SizeFn = std::function<qint64(const Value &)>;

    // capacityBytes <= 0 表示「不限容量」（纯注册进预算治理，靠 enforce 收缩）。
    LruCache(QString id, qint64 capacityBytes, SizeFn sizeFn = nullptr)
        : m_id(std::move(id)), m_capacity(capacityBytes), m_sizeFn(std::move(sizeFn))
    {
      CacheBudgetManager::instance()->registerCache(this);
    }

    ~LruCache() override
    {
      CacheBudgetManager::instance()->unregisterCache(this);
    }

    LruCache(const LruCache &) = delete;
    LruCache &operator=(const LruCache &) = delete;

    // 插入/替换：已在缓存中则换值并移到 MRU 端；随后按容量逐出（LRU 端、
    // 跳过 pinned）。返回本次插入是否触发过逐出。
    bool insert(const Key &key, Value value)
    {
      qint64 evictedAny = 0;
      {
        QMutexLocker lock(&m_mutex);
        const qint64 sz = sizeOf(value);
        const auto it = m_map.find(key);
        if (it != m_map.end())
        {
          m_bytes -= it.value()->size;
          it.value()->value = std::move(value);
          it.value()->size = sz;
          m_bytes += sz;
          m_lru.splice(m_lru.begin(), m_lru, it.value());
        }
        else
        {
          Node node;
          node.key = key;
          node.value = std::move(value);
          node.size = sz;
          node.pinned = false;
          m_lru.push_front(std::move(node));
          m_map.insert(key, m_lru.begin());
          m_bytes += sz;
        }
        m_lastAccess = nowMs();
        evictedAny = evictLocked(m_capacity);
      }
      CacheBudgetManager::instance()->enforce();
      return evictedAny > 0;
    }

    // 取值并触摸（MRU）。命中 → 值拷贝；未命中 → nullopt。
    std::optional<Value> get(const Key &key)
    {
      QMutexLocker lock(&m_mutex);
      const auto it = m_map.find(key);
      if (it == m_map.end())
      {
        ++m_stats.misses;
        return std::nullopt;
      }
      m_lru.splice(m_lru.begin(), m_lru, it.value());
      ++m_stats.hits;
      m_lastAccess = nowMs();
      return it.value()->value;
    }

    bool contains(const Key &key) const
    {
      QMutexLocker lock(&m_mutex);
      return m_map.contains(key);
    }

    // 只读不触摸。
    std::optional<Value> peek(const Key &key) const
    {
      QMutexLocker lock(&m_mutex);
      const auto it = m_map.find(key);
      if (it == m_map.end())
        return std::nullopt;
      return it.value()->value;
    }

    void erase(const Key &key)
    {
      QMutexLocker lock(&m_mutex);
      const auto it = m_map.find(key);
      if (it == m_map.end())
        return;
      m_bytes -= it.value()->size;
      m_lru.erase(it.value());
      m_map.erase(it);
    }

    void clear()
    {
      QMutexLocker lock(&m_mutex);
      m_lru.clear();
      m_map.clear();
      m_bytes = 0;
    }

    void setPin(const Key &key, bool pinned)
    {
      QMutexLocker lock(&m_mutex);
      const auto it = m_map.find(key);
      if (it != m_map.end())
        it.value()->pinned = pinned;
    }

    int pinCount() const
    {
      QMutexLocker lock(&m_mutex);
      int n = 0;
      for (const Node &node : m_lru)
        if (node.pinned)
          ++n;
      return n;
    }

    int size() const
    {
      QMutexLocker lock(&m_mutex);
      return static_cast<int>(m_map.size());
    }

    // MRU 在前的键序（测试/诊断）。
    QVector<Key> keys() const
    {
      QMutexLocker lock(&m_mutex);
      QVector<Key> out;
      out.reserve(static_cast<int>(m_lru.size()));
      for (const Node &node : m_lru)
        out.append(node.key);
      return out;
    }

    // ---- EvictableCache ----
    QString cacheId() const override { return m_id; }
    qint64 bytes() const override
    {
      QMutexLocker lock(&m_mutex);
      return m_bytes;
    }
    qint64 capacity() const override
    {
      QMutexLocker lock(&m_mutex);
      return m_capacity;
    }
    CacheStats stats() const override
    {
      QMutexLocker lock(&m_mutex);
      return m_stats;
    }
    qint64 lastAccessMs() const override
    {
      QMutexLocker lock(&m_mutex);
      return m_lastAccess;
    }

    qint64 evictLRUEntries(qint64 targetBytes) override
    {
      QMutexLocker lock(&m_mutex);
      return evictLocked(targetBytes);
    }

    // 容量变更：收缩立即执行。
    void setCapacity(qint64 capacityBytes)
    {
      QMutexLocker lock(&m_mutex);
      m_capacity = capacityBytes;
      evictLocked(m_capacity);
    }

  private:
    struct Node
    {
        Key key;
        Value value;
        qint64 size = 0;
        bool pinned = false;
    };

    static qint64 nowMs() { return QDateTime::currentMSecsSinceEpoch(); }

    qint64 sizeOf(const Value &v) const
    {
      return m_sizeFn ? m_sizeFn(v) : static_cast<qint64>(sizeof(Value));
    }

    // 从 LRU 端（最久未用）逐出直到 bytes <= limit；pinned 跳过；全部 pinned
    // 或已到限即停。返回释放量。调用方必须已持锁。
    qint64 evictLocked(qint64 limit)
    {
      if (limit <= 0)
        return 0;
      qint64 freed = 0;
      while (m_bytes > limit)
      {
        typename std::list<Node>::iterator victim = m_lru.end();
        for (auto rit = m_lru.rbegin(); rit != m_lru.rend(); ++rit)
        {
          if (rit->pinned)
          {
            ++m_stats.pinnedSkips;
            continue;
          }
          victim = std::next(rit).base();
          break;
        }
        if (victim == m_lru.end())
          break; // 只剩 pinned——逐无可逐
        m_bytes -= victim->size;
        freed += victim->size;
        m_map.remove(victim->key);
        m_lru.erase(victim);
        ++m_stats.evictions;
      }
      return freed;
    }

    const QString m_id;
    qint64 m_capacity;
    const SizeFn m_sizeFn;
    mutable QMutex m_mutex;
    std::list<Node> m_lru; // front = MRU
    QHash<Key, typename std::list<Node>::iterator> m_map;
    qint64 m_bytes = 0;
    qint64 m_lastAccess = 0;
    CacheStats m_stats;
};
