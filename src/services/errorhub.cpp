// 层：数据
#include "errorhub.h"

#include <QAtomicPointer>
#include <QDateTime>
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
