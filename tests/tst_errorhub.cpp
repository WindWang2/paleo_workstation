// 方向64：ErrorHub 单测——去重 60s 固定窗口 / 聚合计数 / 环形逐出 / 分级路由 /
// 过滤查询 / 边界（0 条、上限+1、同键并发）。时间经 setClockForTest 注入，
// 不依赖墙钟。
#include <QtTest>
#include <QSignalSpy>
#include <QThread>
#include <atomic>
#include <vector>

#include "../src/services/errorhub.h"

class TestErrorHub : public QObject
{
    Q_OBJECT

private slots:
    void emptyHub();
    void dedupWithinWindowAggregates();
    void dedupWindowIsFixedFromFirst();
    void distinctTextsDoNotMerge();
    void explicitKeyMergesDifferentTexts();
    void levelRoutingAndFilter();
    void severeOnlyOnError();
    void ringEvictsOldestAtCapPlusOne();
    void evictedKeyStartsFresh();
    void clearResetsButIdsMonotonic();
    void sameKeyConcurrentRaise();
    void globalInstall();

private:
    static qint64 s_now;
    static void useFakeClock(ErrorHub &hub)
    {
        s_now = 1'000'000;
        hub.setClockForTest([] { return s_now; });
    }
};

qint64 TestErrorHub::s_now = 0;

void TestErrorHub::emptyHub()
{
    ErrorHub hub;
    QCOMPARE(hub.size(), 0);
    QCOMPARE(hub.totalRaised(), quint64(0));
    QVERIFY(hub.entries().isEmpty());
    ErrorHub::Filter f;
    f.contains = QStringLiteral("x");
    QVERIFY(hub.entries(f).isEmpty());
    QSignalSpy cleared(&hub, &ErrorHub::historyCleared);
    hub.clear();  // 空清空不崩，仍发信号
    QCOMPARE(cleared.count(), 1);
    QCOMPARE(hub.size(), 0);
}

void TestErrorHub::dedupWithinWindowAggregates()
{
    ErrorHub hub;
    useFakeClock(hub);
    QSignalSpy spy(&hub, &ErrorHub::errorRaised);
    hub.raise(ErrorHub::Level::Warning, "welltops", "T", "boom");
    s_now += 1000;
    hub.raise(ErrorHub::Level::Warning, "welltops", "T", "boom");
    s_now += ErrorHub::kDedupWindowMs - 1001;  // 距首次 59 999 ms：仍在窗内
    const auto e = hub.raise(ErrorHub::Level::Warning, "welltops", "T", "boom");
    QCOMPARE(hub.size(), 1);
    QCOMPARE(e.count, 3);
    QCOMPARE(e.lastMs - e.firstMs, ErrorHub::kDedupWindowMs - 1);
    QCOMPARE(hub.totalRaised(), quint64(3));
    QCOMPARE(spy.count(), 3);
    QCOMPARE(spy.at(0).at(1).toBool(), true);
    QCOMPARE(spy.at(1).at(1).toBool(), false);
    QCOMPARE(spy.at(2).at(1).toBool(), false);
    QCOMPARE(spy.at(2).at(0).value<ErrorHub::Entry>().count, 3);
}

void TestErrorHub::dedupWindowIsFixedFromFirst()
{
    ErrorHub hub;
    useFakeClock(hub);
    QSignalSpy spy(&hub, &ErrorHub::errorRaised);
    // 每 10s 一次、持续 130s：固定窗口 → 第 0/60/120s 三次 firstInWindow。
    for (int t = 0; t <= 130; t += 10)
    {
        s_now = 1'000'000 + qint64(t) * 1000;
        hub.raise(ErrorHub::Level::Error, "mapping", "T", "same", QString(), true);
    }
    int firsts = 0;
    for (const auto &args : spy)
        firsts += args.at(1).toBool() ? 1 : 0;
    QCOMPARE(firsts, 3);
    const auto all = hub.entries();
    QCOMPARE(all.size(), 3);
    QCOMPARE(all.at(0).count, 6);  // 0..50s
    QCOMPARE(all.at(1).count, 6);  // 60..110s
    QCOMPARE(all.at(2).count, 2);  // 120..130s
}

void TestErrorHub::distinctTextsDoNotMerge()
{
    ErrorHub hub;
    useFakeClock(hub);
    hub.raise(ErrorHub::Level::Warning, "a", "T", "one");
    hub.raise(ErrorHub::Level::Warning, "a", "T", "two");
    hub.raise(ErrorHub::Level::Error, "a", "T", "one");   // 级别不同
    hub.raise(ErrorHub::Level::Warning, "b", "T", "one"); // 来源不同
    QCOMPARE(hub.size(), 4);
}

void TestErrorHub::explicitKeyMergesDifferentTexts()
{
    ErrorHub hub;
    useFakeClock(hub);
    hub.raise(ErrorHub::Level::Warning, "io", "T", "file a", "io.read");
    const auto e = hub.raise(ErrorHub::Level::Warning, "io", "T", "file b", "io.read");
    QCOMPARE(hub.size(), 1);
    QCOMPARE(e.count, 2);
    QCOMPARE(e.text, QStringLiteral("file a"));  // 首条文本保留
}

void TestErrorHub::levelRoutingAndFilter()
{
    ErrorHub hub;
    useFakeClock(hub);
    hub.raise(ErrorHub::Level::Info, "shell", "I", "info text");
    hub.raise(ErrorHub::Level::Warning, "welltops", "W", "Warn Text");
    hub.raise(ErrorHub::Level::Error, "mapping", "E", "error text");
    ErrorHub::Filter f;
    f.levelMask = 1 << int(ErrorHub::Level::Error);
    QCOMPARE(hub.entries(f).size(), 1);
    QCOMPARE(hub.entries(f).at(0).source, QStringLiteral("mapping"));
    f.levelMask = (1 << int(ErrorHub::Level::Warning)) | (1 << int(ErrorHub::Level::Info));
    QCOMPARE(hub.entries(f).size(), 2);
    ErrorHub::Filter g;
    g.contains = QStringLiteral("warn text");  // 大小写不敏感
    QCOMPARE(hub.entries(g).size(), 1);
    ErrorHub::Filter s;
    s.source = QStringLiteral("shell");
    QCOMPARE(hub.entries(s).size(), 1);
    s.levelMask = 0;
    QVERIFY(hub.entries(s).isEmpty());
    QCOMPARE(ErrorHub::levelName(ErrorHub::Level::Warning), QStringLiteral("warning"));
}

void TestErrorHub::severeOnlyOnError()
{
    ErrorHub hub;
    useFakeClock(hub);
    QVERIFY(!hub.raise(ErrorHub::Level::Warning, "a", "T", "x", {}, true).severe);
    QVERIFY(hub.raise(ErrorHub::Level::Error, "a", "T", "y", {}, true).severe);
    // 聚合时 severe 只升不降。
    hub.raise(ErrorHub::Level::Error, "a", "T", "z");
    QVERIFY(hub.raise(ErrorHub::Level::Error, "a", "T", "z", {}, true).severe);
}

void TestErrorHub::ringEvictsOldestAtCapPlusOne()
{
    ErrorHub hub;
    useFakeClock(hub);
    for (int i = 0; i < ErrorHub::kCapacity; ++i)
        hub.raise(ErrorHub::Level::Warning, "s", "T", QString::number(i));
    QCOMPARE(hub.size(), ErrorHub::kCapacity);
    QCOMPARE(hub.entries().first().text, QStringLiteral("0"));
    hub.raise(ErrorHub::Level::Warning, "s", "T", QStringLiteral("cap+1"));
    QCOMPARE(hub.size(), ErrorHub::kCapacity);
    const auto all = hub.entries();
    QCOMPARE(all.size(), ErrorHub::kCapacity);
    QCOMPARE(all.first().text, QStringLiteral("1"));    // 0 被逐出
    QCOMPARE(all.last().text, QStringLiteral("cap+1"));
    for (int i = 1; i < all.size(); ++i)
        QVERIFY(all.at(i).id == all.at(i - 1).id + 1);  // 旧→新连续
}

void TestErrorHub::evictedKeyStartsFresh()
{
    ErrorHub hub;
    useFakeClock(hub);
    hub.raise(ErrorHub::Level::Warning, "s", "T", QStringLiteral("victim"));
    for (int i = 0; i < ErrorHub::kCapacity; ++i)
        hub.raise(ErrorHub::Level::Warning, "s", "T", QString::number(i));
    // victim 已逐出：窗内同键再来 → 新条目，count=1，firstInWindow=true。
    QSignalSpy spy(&hub, &ErrorHub::errorRaised);
    const auto e = hub.raise(ErrorHub::Level::Warning, "s", "T", QStringLiteral("victim"));
    QCOMPARE(e.count, 1);
    QCOMPARE(spy.at(0).at(1).toBool(), true);
    QCOMPARE(hub.size(), ErrorHub::kCapacity);
}

void TestErrorHub::clearResetsButIdsMonotonic()
{
    ErrorHub hub;
    useFakeClock(hub);
    const auto a = hub.raise(ErrorHub::Level::Error, "s", "T", "x");
    hub.clear();
    QCOMPARE(hub.size(), 0);
    const auto b = hub.raise(ErrorHub::Level::Error, "s", "T", "x");
    QCOMPARE(b.count, 1);  // 清空后同键不再聚合到旧条目
    QVERIFY(b.id > a.id);
    QCOMPARE(hub.totalRaised(), quint64(2));
}

void TestErrorHub::sameKeyConcurrentRaise()
{
    ErrorHub hub;  // 系统时钟：8×250 次远小于 60s 窗
    constexpr int kThreads = 8;
    constexpr int kPer = 250;
    std::atomic<int> firsts{0};
    connect(&hub, &ErrorHub::errorRaised, &hub,
            [&](const ErrorHub::Entry &, bool first) { if (first) ++firsts; },
            Qt::DirectConnection);
    std::vector<QThread *> threads;
    for (int t = 0; t < kThreads; ++t)
        threads.push_back(QThread::create([&] {
            for (int i = 0; i < kPer; ++i)
                hub.raise(ErrorHub::Level::Warning, "conc", "T", "same");
        }));
    for (auto *th : threads)
        th->start();
    for (auto *th : threads)
    {
        QVERIFY(th->wait(30000));
        delete th;
    }
    QCOMPARE(hub.size(), 1);
    QCOMPARE(hub.entries().first().count, kThreads * kPer);
    QCOMPARE(hub.totalRaised(), quint64(kThreads * kPer));
    QCOMPARE(firsts.load(), 1);
}

void TestErrorHub::globalInstall()
{
    QVERIFY(ErrorHub::global() == nullptr);
    {
        ErrorHub hub;
        ErrorHub::installGlobal(&hub);
        QCOMPARE(ErrorHub::global(), &hub);
    }
    QVERIFY(ErrorHub::global() == nullptr);  // 析构自动摘除
}

QTEST_GUILESS_MAIN(TestErrorHub)
#include "tst_errorhub.moc"
