// 层：数据
#include "errorhub.h"

#include <QAtomicPointer>
#include <QMutexLocker>
#include <algorithm>

namespace
{
QAtomicPointer<ErrorHub> g_hub;
}

ErrorHub::ErrorHub(QObject *parent) : QObject(parent)
{
    qRegisterMetaType<ErrorHub::Entry>("ErrorHub::Entry");
    m_ring.resize(kCapacity);
    m_keyIndex.reserve(kCapacity);
}

ErrorHub::~ErrorHub()
{
    // 析构时若仍是全局实例则摘除，避免视图层拿到悬垂指针。
    g_hub.testAndSetOrdered(this, nullptr);
}

void ErrorHub::installGlobal(ErrorHub *hub)
{
    g_hub.storeRelease(hub);
}

ErrorHub *ErrorHub::global()
{
    return g_hub.loadAcquire();
}

QString ErrorHub::levelName(Level level)
{
    switch (level)
    {
    case Level::Info: return QStringLiteral("info");
    case Level::Warning: return QStringLiteral("warning");
    case Level::Error: return QStringLiteral("error");
    }
    return QStringLiteral("info");
}

QString ErrorHub::defaultKey(Level level, const QString &source,
                             const QString &title, const QString &text)
{
    return levelName(level) + QLatin1Char('|') + source + QLatin1Char('|') +
           title + QLatin1Char('|') + text;
}

void ErrorHub::setClockForTest(std::function<qint64()> clock)
{
    QMutexLocker lock(&m_mutex);
    m_clock = std::move(clock);
}

qint64 ErrorHub::nowMs() const
{
    return m_clock ? m_clock() : QDateTime::currentMSecsSinceEpoch();
}

ErrorHub::Entry *ErrorHub::findLive(quint64 id)
{
    // 活条目 id 区间 [m_nextId - m_size, m_nextId)；环位置 = id 对容量取模。
    if (id == 0 || id >= m_nextId || id < m_nextId - quint64(m_size))
        return nullptr;
    Entry &e = m_ring[int(id % quint64(kCapacity))];
    return e.id == id ? &e : nullptr;
}

ErrorHub::Entry ErrorHub::raise(Level level, const QString &source,
                                const QString &title, const QString &text,
                                const QString &dedupKey, bool severe,
                                bool historyOnly)
{
    Entry snapshot;
    bool first = true;
    {
        QMutexLocker lock(&m_mutex);
        const qint64 now = nowMs();
        const QString key = dedupKey.isEmpty()
                                ? defaultKey(level, source, title, text)
                                : dedupKey;
        ++m_total;
        const auto it = m_keyIndex.constFind(key);
        if (it != m_keyIndex.constEnd())
        {
            if (Entry *live = findLive(it.value()))
            {
                if (now - live->firstMs < kDedupWindowMs && now >= live->firstMs)
                {
                    ++live->count;
                    live->lastMs = now;
                    live->severe = live->severe || severe;
                    snapshot = *live;
                    snapshot.historyOnly = historyOnly;
                    first = false;
                }
            }
        }
        if (first)
        {
            // 新条目：写环（满则覆盖最旧，并作废其去重索引）。
            Entry &slot = m_ring[int(m_nextId % quint64(kCapacity))];
            if (m_size == kCapacity && slot.id != 0)
            {
                const auto old = m_keyIndex.constFind(slot.dedupKey);
                if (old != m_keyIndex.constEnd() && old.value() == slot.id)
                    m_keyIndex.erase(old);
            }
            else
            {
                ++m_size;
            }
            slot.id = m_nextId++;
            slot.level = level;
            slot.source = source;
            slot.title = title;
            slot.text = text;
            slot.dedupKey = key;
            slot.firstMs = now;
            slot.lastMs = now;
            slot.count = 1;
            slot.severe = severe && level == Level::Error;
            m_keyIndex.insert(key, slot.id);
            snapshot = slot;
            snapshot.historyOnly = historyOnly;
        }
    }
    emit errorRaised(snapshot, first);
    return snapshot;
}

QVector<ErrorHub::Entry> ErrorHub::entries() const
{
    return entries(Filter{});
}

QVector<ErrorHub::Entry> ErrorHub::entries(const Filter &f) const
{
    QMutexLocker lock(&m_mutex);
    QVector<Entry> out;
    out.reserve(m_size);
    for (quint64 id = m_nextId - quint64(m_size); id < m_nextId; ++id)
    {
        const Entry &e = m_ring[int(id % quint64(kCapacity))];
        if (!(f.levelMask & (1 << int(e.level))))
            continue;
        if (!f.source.isEmpty() && e.source != f.source)
            continue;
        if (!f.contains.isEmpty() &&
            !e.title.contains(f.contains, Qt::CaseInsensitive) &&
            !e.text.contains(f.contains, Qt::CaseInsensitive) &&
            !e.source.contains(f.contains, Qt::CaseInsensitive))
            continue;
        out.append(e);
    }
    return out;
}

int ErrorHub::size() const
{
    QMutexLocker lock(&m_mutex);
    return m_size;
}

quint64 ErrorHub::totalRaised() const
{
    QMutexLocker lock(&m_mutex);
    return m_total;
}

void ErrorHub::clear()
{
    {
        QMutexLocker lock(&m_mutex);
        for (Entry &e : m_ring)
            e = Entry{};
        m_size = 0;
        m_keyIndex.clear();
        // m_nextId 不回卷：清空后旧 id 永不复活。
    }
    emit historyCleared();
}

// ============================================================================
// 方向54（#251）并行实现：paleo::services::ErrorHub。
// ============================================================================
namespace paleo::services {

static ErrorHub *s_instance = nullptr;

QString errorLevelToString(ErrorLevel level) {
  switch (level) {
    case ErrorLevel::Info:     return QStringLiteral("Info");
    case ErrorLevel::Warning:  return QStringLiteral("Warning");
    case ErrorLevel::Error:    return QStringLiteral("Error");
    case ErrorLevel::Critical: return QStringLiteral("Critical");
  }
  return QStringLiteral("Error");
}

ErrorLevel stringToErrorLevel(const QString &str, ErrorLevel fallback) {
  if (str.compare(QStringLiteral("Info"), Qt::CaseInsensitive) == 0) return ErrorLevel::Info;
  if (str.compare(QStringLiteral("Warning"), Qt::CaseInsensitive) == 0) return ErrorLevel::Warning;
  if (str.compare(QStringLiteral("Error"), Qt::CaseInsensitive) == 0) return ErrorLevel::Error;
  if (str.compare(QStringLiteral("Critical"), Qt::CaseInsensitive) == 0) return ErrorLevel::Critical;
  return fallback;
}

QString ErrorEntry::makeDefaultKey(ErrorLevel level, const QString &domain, const QString &message) {
  return QStringLiteral("%1:%2:%3").arg(static_cast<int>(level)).arg(domain, message);
}

bool ErrorEntry::operator==(const ErrorEntry &other) const {
  return id == other.id &&
         level == other.level &&
         domain == other.domain &&
         deduplicationKey == other.deduplicationKey &&
         aggregationCount == other.aggregationCount &&
         message == other.message &&
         details == other.details &&
         isModal == other.isModal;
}

ErrorHub::ErrorHub(QObject *parent) : QObject(parent) {
  static const int metatypeRegistered = []() {
    qRegisterMetaType<paleo::services::ErrorEntry>();
    qRegisterMetaType<paleo::services::ErrorLevel>();
    return 0;
  }();
  Q_UNUSED(metatypeRegistered);

  if (!s_instance) {
    s_instance = this;
  }
}

ErrorHub::~ErrorHub() {
  if (s_instance == this) {
    s_instance = nullptr;
  }
}

ErrorHub *ErrorHub::instance() {
  if (!s_instance) {
    s_instance = new ErrorHub();
  }
  return s_instance;
}

void ErrorHub::setInstance(ErrorHub *customInstance) {
  s_instance = customInstance;
}

void ErrorHub::report(const ErrorEntry &entry) {
  ErrorEntry finalEntry = entry;
  if (!finalEntry.timestamp.isValid()) {
    finalEntry.timestamp = QDateTime::currentDateTime();
  }
  if (finalEntry.deduplicationKey.isEmpty()) {
    finalEntry.deduplicationKey = ErrorEntry::makeDefaultKey(finalEntry.level, finalEntry.domain, finalEntry.message);
  }

  bool isAggregated = false;
  const std::string keyStd = finalEntry.deduplicationKey.toStdString();

  {
    QMutexLocker locker(&m_mutex);
    auto it = m_keyToId.find(keyStd);
    if (it != m_keyToId.end()) {
      qint64 targetId = it->second;
      auto histIt = std::find_if(m_history.begin(), m_history.end(),
                                 [targetId](const ErrorEntry &e) { return e.id == targetId; });
      if (histIt != m_history.end()) {
        qint64 elapsedSecs = histIt->timestamp.secsTo(finalEntry.timestamp);
        if (elapsedSecs >= 0 && elapsedSecs < m_dedupWindowSecs) {
          // 命中 60s 去重聚合窗口：累加计数、刷新最新时间戳、合流附加详情
          histIt->aggregationCount++;
          histIt->timestamp = finalEntry.timestamp;
          if (!finalEntry.details.isEmpty() && !histIt->details.contains(finalEntry.details)) {
            if (!histIt->details.isEmpty()) {
              histIt->details += QLatin1Char('\n');
            }
            histIt->details += finalEntry.details;
          }
          finalEntry = *histIt;
          isAggregated = true;
        }
      }
    }

    if (!isAggregated) {
      finalEntry.id = ++m_nextId;
      finalEntry.firstSeen = finalEntry.timestamp;
      finalEntry.aggregationCount = 1;

      // 500 条环形缓冲上限：达到容量时 FIFO 逐出最早记录
      if (static_cast<int>(m_history.size()) >= m_maxCapacity && !m_history.empty()) {
        const ErrorEntry &evicted = m_history.front();
        std::string evictedKey = evicted.deduplicationKey.toStdString();
        auto mapIt = m_keyToId.find(evictedKey);
        if (mapIt != m_keyToId.end() && mapIt->second == evicted.id) {
          m_keyToId.erase(mapIt);
        }
        m_history.pop_front();
      }

      m_history.push_back(finalEntry);
      m_keyToId[keyStd] = finalEntry.id;
    }
  }

  // 锁外发射 Qt 信号，杜绝槽函数重入锁引发死锁
  if (isAggregated) {
    emit errorAggregated(finalEntry);
  } else {
    emit errorRaised(finalEntry);
  }
  emit historyChanged();
}

void ErrorHub::reportInfo(const QString &domain, const QString &message,
                          const QString &details, const QString &dedupKey) {
  ErrorEntry entry;
  entry.level = ErrorLevel::Info;
  entry.domain = domain;
  entry.message = message;
  entry.details = details;
  entry.deduplicationKey = dedupKey;
  entry.isModal = false;
  report(entry);
}

void ErrorHub::reportWarning(const QString &domain, const QString &message,
                             const QString &details, const QString &dedupKey) {
  ErrorEntry entry;
  entry.level = ErrorLevel::Warning;
  entry.domain = domain;
  entry.message = message;
  entry.details = details;
  entry.deduplicationKey = dedupKey;
  entry.isModal = false;
  report(entry);
}

void ErrorHub::reportError(const QString &domain, const QString &message,
                           const QString &details, const QString &dedupKey) {
  ErrorEntry entry;
  entry.level = ErrorLevel::Error;
  entry.domain = domain;
  entry.message = message;
  entry.details = details;
  entry.deduplicationKey = dedupKey;
  entry.isModal = false;
  report(entry);
}

void ErrorHub::reportCritical(const QString &domain, const QString &message,
                              const QString &details, const QString &dedupKey) {
  ErrorEntry entry;
  entry.level = ErrorLevel::Critical;
  entry.domain = domain;
  entry.message = message;
  entry.details = details;
  entry.deduplicationKey = dedupKey;
  entry.isModal = true;
  report(entry);
}

void ErrorHub::postInfo(const QString &domain, const QString &message,
                        const QString &details, const QString &dedupKey) {
  instance()->reportInfo(domain, message, details, dedupKey);
}

void ErrorHub::postWarning(const QString &domain, const QString &message,
                           const QString &details, const QString &dedupKey) {
  instance()->reportWarning(domain, message, details, dedupKey);
}

void ErrorHub::postError(const QString &domain, const QString &message,
                         const QString &details, const QString &dedupKey) {
  instance()->reportError(domain, message, details, dedupKey);
}

void ErrorHub::postCritical(const QString &domain, const QString &message,
                            const QString &details, const QString &dedupKey) {
  instance()->reportCritical(domain, message, details, dedupKey);
}

bool ErrorHub::checkOrReport(bool ok, ErrorLevel level, const QString &domain,
                             const QString &message, const QString &details,
                             const QString &dedupKey) {
  if (ok) {
    return true;
  }
  switch (level) {
    case ErrorLevel::Info:
      postInfo(domain, message, details, dedupKey);
      break;
    case ErrorLevel::Warning:
      postWarning(domain, message, details, dedupKey);
      break;
    case ErrorLevel::Error:
      postError(domain, message, details, dedupKey);
      break;
    case ErrorLevel::Critical:
      postCritical(domain, message, details, dedupKey);
      break;
  }
  return false;
}

QVector<ErrorEntry> ErrorHub::history() const {
  QMutexLocker locker(&m_mutex);
  QVector<ErrorEntry> list;
  list.reserve(static_cast<qsizetype>(m_history.size()));
  for (const auto &item : m_history) {
    list.append(item);
  }
  return list;
}

QVector<ErrorEntry> ErrorHub::query(const ErrorQueryFilter &filter) const {
  QMutexLocker locker(&m_mutex);
  QVector<ErrorEntry> results;
  for (const auto &e : m_history) {
    if (filter.level.has_value() && e.level != *filter.level) {
      continue;
    }
    if (filter.minLevel.has_value() && static_cast<int>(e.level) < static_cast<int>(*filter.minLevel)) {
      continue;
    }
    if (!filter.domain.isEmpty() && e.domain.compare(filter.domain, Qt::CaseInsensitive) != 0) {
      continue;
    }
    if (!filter.searchText.isEmpty()) {
      if (!e.message.contains(filter.searchText, Qt::CaseInsensitive) &&
          !e.details.contains(filter.searchText, Qt::CaseInsensitive)) {
        continue;
      }
    }
    if (filter.since.isValid() && e.timestamp < filter.since) {
      continue;
    }
    results.append(e);
    if (filter.limit > 0 && results.size() >= filter.limit) {
      break;
    }
  }
  return results;
}

QVector<ErrorEntry> ErrorHub::queryByDomain(const QString &domain) const {
  ErrorQueryFilter filter;
  filter.domain = domain;
  return query(filter);
}

QVector<ErrorEntry> ErrorHub::queryByLevel(ErrorLevel level) const {
  ErrorQueryFilter filter;
  filter.level = level;
  return query(filter);
}

QVector<ErrorEntry> ErrorHub::queryByMinLevel(ErrorLevel minLevel) const {
  ErrorQueryFilter filter;
  filter.minLevel = minLevel;
  return query(filter);
}

std::optional<ErrorEntry> ErrorHub::entryById(qint64 id) const {
  QMutexLocker locker(&m_mutex);
  for (const auto &e : m_history) {
    if (e.id == id) {
      return e;
    }
  }
  return std::nullopt;
}

int ErrorHub::count() const {
  QMutexLocker locker(&m_mutex);
  return static_cast<int>(m_history.size());
}

int ErrorHub::countByLevel(ErrorLevel level) const {
  QMutexLocker locker(&m_mutex);
  int c = 0;
  for (const auto &e : m_history) {
    if (e.level == level) {
      ++c;
    }
  }
  return c;
}

void ErrorHub::clear() {
  {
    QMutexLocker locker(&m_mutex);
    m_history.clear();
    m_keyToId.clear();
  }
  emit historyCleared();
  emit historyChanged();
}

int ErrorHub::maxCapacity() const {
  QMutexLocker locker(&m_mutex);
  return m_maxCapacity;
}

void ErrorHub::setMaxCapacity(int capacity) {
  if (capacity <= 0) {
    return;
  }
  {
    QMutexLocker locker(&m_mutex);
    m_maxCapacity = capacity;
    while (static_cast<int>(m_history.size()) > m_maxCapacity) {
      const ErrorEntry &evicted = m_history.front();
      std::string evictedKey = evicted.deduplicationKey.toStdString();
      auto mapIt = m_keyToId.find(evictedKey);
      if (mapIt != m_keyToId.end() && mapIt->second == evicted.id) {
        m_keyToId.erase(mapIt);
      }
      m_history.pop_front();
    }
  }
  emit historyChanged();
}

qint64 ErrorHub::dedupWindowSecs() const {
  QMutexLocker locker(&m_mutex);
  return m_dedupWindowSecs;
}

void ErrorHub::setDedupWindowSecs(qint64 secs) {
  QMutexLocker locker(&m_mutex);
  m_dedupWindowSecs = secs;
}

} // namespace paleo::services
