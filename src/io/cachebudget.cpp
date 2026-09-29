// 层：数据
#include "cachebudget.h"

#include <QDateTime>
#include <QSettings>

#include <algorithm>

CacheBudgetManager *CacheBudgetManager::instance()
{
  // 泄漏式单例：见头注（析构序）。
  static CacheBudgetManager *inst = new CacheBudgetManager();
  return inst;
}

void CacheBudgetManager::registerCache(EvictableCache *cache)
{
  if (!cache)
    return;
  QMutexLocker lock(&m_mutex);
  if (!m_caches.contains(cache))
    m_caches.append(cache);
}

void CacheBudgetManager::unregisterCache(EvictableCache *cache)
{
  QMutexLocker lock(&m_mutex);
  const int i = m_caches.indexOf(cache);
  if (i >= 0)
    m_caches.remove(i);
}

void CacheBudgetManager::setBudgetBytes(qint64 bytes)
{
  QMutexLocker lock(&m_mutex);
  m_budgetBytes = qMax<qint64>(1, bytes);
  const qint64 budget = m_budgetBytes;
  lock.unlock();
  Q_UNUSED(budget);
  enforce();
}

qint64 CacheBudgetManager::usedBytes() const
{
  qint64 total = 0;
  QMutexLocker lock(&m_mutex);
  for (const EvictableCache *c : m_caches)
    total += c->bytes();
  return total;
}

int CacheBudgetManager::usedPercent() const
{
  QMutexLocker lock(&m_mutex);
  if (m_budgetBytes <= 0)
    return 0;
  qint64 total = 0;
  for (const EvictableCache *c : m_caches)
    total += c->bytes();
  return static_cast<int>(total * 100 / m_budgetBytes);
}

void CacheBudgetManager::loadFromSettings()
{
  QSettings settings(QStringLiteral("paleo"), QStringLiteral("paleo"));
  const qint64 mib = settings.value(QStringLiteral("cache/budgetMiB"),
                                    QVariant::fromValue<qlonglong>(kDefaultBudgetMiB))
                         .toLongLong();
  if (mib >= 1)
    setBudgetBytes(mib * 1024 * 1024);
}

qint64 CacheBudgetManager::enforce()
{
  qint64 budget = 0;
  QVector<EvictableCache *> snapshot;
  {
    QMutexLocker lock(&m_mutex);
    budget = m_budgetBytes;
    snapshot = m_caches;
  }
  qint64 used = 0;
  for (const EvictableCache *c : snapshot)
    used += c->bytes();
  if (used <= budget)
  {
    QMutexLocker lock(&m_mutex);
    m_lastNotifiedTier = 0;
    return 0;
  }

  // D6.3：目标压到 90% 预算；「最远未用」的缓存先收缩。
  const qint64 target = budget - (budget / 10);
  qint64 need = used - target;
  qint64 freed = 0;
  std::sort(snapshot.begin(), snapshot.end(),
            [](const EvictableCache *a, const EvictableCache *b) {
              return a->lastAccessMs() < b->lastAccessMs();
            });
  for (EvictableCache *c : snapshot)
  {
    if (need <= 0)
      break;
    const qint64 cacheBytes = c->bytes();
    if (cacheBytes <= 0)
      continue;
    // 从该缓存最多拿 need 字节（LRU 端逐出，pinned 不动）。
    const qint64 got = c->evictLRUEntries(qMax<qint64>(0, cacheBytes - need));
    freed += got;
    need -= got;
  }
  notifyPressure();
  return freed;
}

quint64 CacheBudgetManager::addPressureHandler(PressureHandler handler)
{
  QMutexLocker lock(&m_mutex);
  const quint64 id = m_nextHandlerId++;
  m_handlers.insert(id, std::move(handler));
  return id;
}

void CacheBudgetManager::removePressureHandler(quint64 id)
{
  QMutexLocker lock(&m_mutex);
  m_handlers.remove(id);
}

void CacheBudgetManager::notifyPressure()
{
  const int pct = usedPercent();
  const int tier = pct >= 90 ? 2 : (pct >= 75 ? 1 : 0);
  QVector<PressureHandler> toCall;
  {
    QMutexLocker lock(&m_mutex);
    if (tier <= m_lastNotifiedTier)
      return; // 未跨档不重复广播（同档内再超限由 enforce 的逐出兜底）
    m_lastNotifiedTier = tier;
    toCall = m_handlers.values();
  }
  for (const PressureHandler &h : toCall)
    h(pct);
}

QVector<CacheBudgetManager::NamedStats> CacheBudgetManager::allStats() const
{
  QVector<NamedStats> out;
  QMutexLocker lock(&m_mutex);
  for (const EvictableCache *c : m_caches)
    out.append({c->cacheId(), c->stats(), c->bytes(), c->capacity()});
  return out;
}

void CacheBudgetManager::noteLargeAllocation(const QString &where, qint64 bytes)
{
  if (bytes < kLargeAllocThreshold)
    return;
  QMutexLocker lock(&m_mutex);
  LargeAlloc a;
  a.where = where;
  a.bytes = bytes;
  a.whenMs = QDateTime::currentMSecsSinceEpoch();
  m_largeAllocs.prepend(a);
  while (m_largeAllocs.size() > 256)
    m_largeAllocs.removeLast();
}

QVector<CacheBudgetManager::LargeAlloc> CacheBudgetManager::largeAllocations(int maxCount) const
{
  QMutexLocker lock(&m_mutex);
  return m_largeAllocs.mid(0, qMax(0, maxCount));
}

void CacheBudgetManager::clearLargeAllocations()
{
  QMutexLocker lock(&m_mutex);
  m_largeAllocs.clear();
}
