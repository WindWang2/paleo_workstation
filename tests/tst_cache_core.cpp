// tst_cache_core — wave/io-perf-cache D6 基建：LruCache / CacheBudgetManager /
// 版本化缓存文件格式（cachecore）。
#include <QtTest>

#include "io/cachecore.h"
#include "io/cachebudget.h"
#include "io/lrucache.h"

#include <QDir>
#include <QFile>
#include <QTemporaryDir>
#include <QtConcurrent>
#include <atomic>
#include <thread>

class CacheCoreTests : public QObject
{
    Q_OBJECT

  private slots:
    // ---- LruCache（D6 基建）----
    void lruInsertGetPeek();
    void lruEvictsLeastRecentlyUsed();
    void lruGetTouches();
    void lruPinPreventsEviction();
    void lruReplaceUpdatesBytes();
    void lruStatsCounts();

    // ---- CacheBudgetManager（D6.1-D6.5）----
    void budgetDefaultAndSet();
    void budgetEnforceEvictsOldestAccess();
    void budgetPressureHandlers();
    void budgetLargeAllocAudit();
    void twoPhaseLatchingAndEvictionRefCount();

    // ---- cachecore 版本化格式（D2.3 自愈）----
    void cacheFileRoundtrip();
    void cacheFileRejectsBadMagic();
    void cacheFileRejectsVersionOutOfRange();
    void cacheFileRejectsCorruptPayload();
    void cacheFileSelfHealsTruncated();
    void cacheFileRejectsCorruptSizeFields();
    void cacheFileAtomicCreatesParentDirs();
    void cacheIoPrimitivesRoundtrip();
    void zstdRoundtrip();
    void lruThreadSafety();
};

namespace
{
  qint64 fixedSize(const QByteArray &)
  {
    return 100; // 每条目固定 100 字节，便于断言
  }
} // namespace

void CacheCoreTests::lruInsertGetPeek()
{
  LruCache<QString, QByteArray> cache(QStringLiteral("t1"), 1000, fixedSize);
  cache.insert(QStringLiteral("a"), QByteArray("A"));
  QVERIFY(cache.get(QStringLiteral("a")).has_value());
  QCOMPARE(*cache.get(QStringLiteral("a")), QByteArray("A"));
  QVERIFY(!cache.get(QStringLiteral("missing")).has_value());
  QVERIFY(cache.contains(QStringLiteral("a")));
  // peek 不触摸：get b 后 b 是 MRU，peek a 不改变位置。
  cache.insert(QStringLiteral("b"), QByteArray("B"));
  cache.peek(QStringLiteral("a"));
  QCOMPARE(cache.keys().front(), QStringLiteral("b"));
}

void CacheCoreTests::lruEvictsLeastRecentlyUsed()
{
  LruCache<QString, QByteArray> cache(QStringLiteral("t2"), 250, fixedSize); // 2.5 条容量
  cache.insert(QStringLiteral("a"), QByteArray("1"));
  cache.insert(QStringLiteral("b"), QByteArray("2"));
  cache.insert(QStringLiteral("c"), QByteArray("3"));
  QVERIFY(!cache.contains(QStringLiteral("a"))); // a 最久未用被逐
  QVERIFY(cache.contains(QStringLiteral("b")));
  QVERIFY(cache.contains(QStringLiteral("c")));
  QCOMPARE(cache.stats().evictions, qint64(1));
  QCOMPARE(cache.bytes(), qint64(200));
}

void CacheCoreTests::lruGetTouches()
{
  LruCache<QString, QByteArray> cache(QStringLiteral("t3"), 250, fixedSize);
  cache.insert(QStringLiteral("a"), QByteArray("1"));
  cache.insert(QStringLiteral("b"), QByteArray("2"));
  cache.get(QStringLiteral("a")); // a 触摸成 MRU
  cache.insert(QStringLiteral("c"), QByteArray("3")); // 逐出的应是 b
  QVERIFY(cache.contains(QStringLiteral("a")));
  QVERIFY(!cache.contains(QStringLiteral("b")));
}

void CacheCoreTests::lruPinPreventsEviction()
{
  LruCache<QString, QByteArray> cache(QStringLiteral("t4"), 250, fixedSize);
  cache.insert(QStringLiteral("a"), QByteArray("1"));
  cache.setPin(QStringLiteral("a"), true); // D6.3 pinning
  cache.insert(QStringLiteral("b"), QByteArray("2"));
  cache.insert(QStringLiteral("c"), QByteArray("3")); // 容量 2.5：只能逐 b
  QVERIFY(cache.contains(QStringLiteral("a"))); // pin 挡住
  QVERIFY(!cache.contains(QStringLiteral("b")));
  QCOMPARE(cache.pinCount(), 1);
  cache.setPin(QStringLiteral("a"), false);
  cache.insert(QStringLiteral("d"), QByteArray("4")); // unpin 后 a 可逐
  QVERIFY(!cache.contains(QStringLiteral("a")));
  QVERIFY(cache.stats().pinnedSkips > 0);
}

void CacheCoreTests::lruReplaceUpdatesBytes()
{
  LruCache<QString, QByteArray> cache(QStringLiteral("t5"), 10000, fixedSize);
  cache.insert(QStringLiteral("a"), QByteArray("1"));
  QCOMPARE(cache.bytes(), qint64(100));
  cache.insert(QStringLiteral("a"), QByteArray("2")); // 替换不新增条目
  QCOMPARE(cache.size(), 1);
  QCOMPARE(cache.bytes(), qint64(100));
}

void CacheCoreTests::lruStatsCounts()
{
  LruCache<QString, QByteArray> cache(QStringLiteral("t6"), 10000, fixedSize);
  cache.insert(QStringLiteral("a"), QByteArray("1"));
  cache.get(QStringLiteral("a"));
  cache.get(QStringLiteral("a"));
  cache.get(QStringLiteral("x"));
  const CacheStats st = cache.stats();
  QCOMPARE(st.hits, qint64(2));
  QCOMPARE(st.misses, qint64(1));
  QCOMPARE(st.hitRatePercent(), 2.0 / 3.0 * 100.0);
}

void CacheCoreTests::budgetDefaultAndSet()
{
  CacheBudgetManager *mgr = CacheBudgetManager::instance();
  const qint64 saved = mgr->budgetBytes();
  mgr->setBudgetBytes(64 * 1024 * 1024);
  QCOMPARE(mgr->budgetBytes(), qint64(64 * 1024 * 1024));
  mgr->setBudgetBytes(saved);
  QVERIFY(mgr->budgetBytes() >= 1);
}

void CacheCoreTests::budgetEnforceEvictsOldestAccess()
{
  CacheBudgetManager *mgr = CacheBudgetManager::instance();
  const qint64 savedBudget = mgr->budgetBytes();
  // D6.3：预算超限时「最远未用」的缓存先收缩。
  LruCache<QString, QByteArray> older(QStringLiteral("t7"), 100000, fixedSize);
  LruCache<QString, QByteArray> newer(QStringLiteral("t8"), 100000, fixedSize);
  older.insert(QStringLiteral("a"), QByteArray("1"));
  QTest::qSleep(2);
  newer.insert(QStringLiteral("b"), QByteArray("2"));
  mgr->setBudgetBytes(150); // 总 200 > 150：older 先收光，newer 收 50
  QVERIFY(!older.contains(QStringLiteral("a"))); // 最老的全逐
  QVERIFY(mgr->usedBytes() <= 150);
  mgr->setBudgetBytes(savedBudget);
}

void CacheCoreTests::budgetPressureHandlers()
{
  CacheBudgetManager *mgr = CacheBudgetManager::instance();
  const qint64 saved = mgr->budgetBytes();
  int seen = 0;
  int lastPct = 0;
  const quint64 id = mgr->addPressureHandler([&](int pct) {
    ++seen;
    lastPct = pct;
  });
  LruCache<QString, QByteArray> big(QStringLiteral("t9"), 100000000, fixedSize);
  for (int i = 0; i < 30; ++i)
    big.insert(QStringLiteral("k%1").arg(i), QByteArray("x")); // 3000B
  mgr->setBudgetBytes(2000); // 3000 > 2000：超限 + 广播（D6.4）
  QVERIFY(seen >= 1);
  QVERIFY(lastPct > 75);
  mgr->removePressureHandler(id);
  mgr->setBudgetBytes(saved);
}

void CacheCoreTests::budgetLargeAllocAudit()
{
  CacheBudgetManager *mgr = CacheBudgetManager::instance();
  mgr->clearLargeAllocations();
  mgr->noteLargeAllocation(QStringLiteral("unit-test/big"), 42 * 1024 * 1024); // D6.6
  mgr->noteLargeAllocation(QStringLiteral("unit-test/small"), 1024);          // 低于阈值不记
  const auto allocs = mgr->largeAllocations();
  QCOMPARE(allocs.size(), 1);
  QCOMPARE(allocs.first().where, QStringLiteral("unit-test/big"));
  QCOMPARE(allocs.first().bytes, qint64(42 * 1024 * 1024));
  mgr->clearLargeAllocations();
  QVERIFY(mgr->largeAllocations().isEmpty());
}

namespace
{
class MockEvictableCache : public EvictableCache
{
public:
  MockEvictableCache(QString id, qint64 bytes)
    : m_id(std::move(id)), m_bytes(bytes)
  {
    CacheBudgetManager::instance()->registerCache(this);
  }
  ~MockEvictableCache() override
  {
    CacheBudgetManager::instance()->unregisterCache(this);
  }
  QString cacheId() const override { return m_id; }
  qint64 bytes() const override { return m_bytes; }
  qint64 capacity() const override { return 100000; }
  CacheStats stats() const override { return {}; }
  qint64 lastAccessMs() const override { return 1000; }

  std::function<void()> onEvict;

  qint64 evictLRUEntries(qint64 targetBytes) override
  {
    if (onEvict)
      onEvict();
    const qint64 freed = qMax<qint64>(0, m_bytes - targetBytes);
    m_bytes -= freed;
    return freed;
  }

private:
  QString m_id;
  qint64 m_bytes = 0;
};
} // namespace

void CacheCoreTests::twoPhaseLatchingAndEvictionRefCount()
{
  CacheBudgetManager *mgr = CacheBudgetManager::instance();
  const qint64 savedBudget = mgr->budgetBytes();

  auto *mock = new MockEvictableCache(QStringLiteral("mock_evict"), 2000);
  QCOMPARE(mock->activeEvictionRefs(), 0);

  std::atomic_bool unregisterStarted{false};
  std::atomic_bool unregisterDone{false};
  std::atomic_bool evictionRefObserved{false};

  mock->onEvict = [&]() {
    if (mock->activeEvictionRefs() > 0)
      evictionRefObserved.store(true);

    // In a background thread, attempt to unregister mock while eviction is active
    std::thread unregisterThread([&]() {
      unregisterStarted.store(true);
      mgr->unregisterCache(mock);
      unregisterDone.store(true);
    });
    unregisterThread.detach();

    // Give the unregister thread time to run and block on QWaitCondition
    while (!unregisterStarted.load())
    {
      QTest::qSleep(5);
    }
    QTest::qSleep(40);
    // Unregister must NOT have finished yet because eviction refs are active!
    QVERIFY(!unregisterDone.load());
  };

  mgr->setBudgetBytes(1000); // Triggers enforce(), calling evictLRUEntries on mock
  QVERIFY(evictionRefObserved.load());

  // After enforce() finishes, eviction refs are released and unregister completes
  for (int i = 0; i < 200 && !unregisterDone.load(); ++i)
    QTest::qSleep(5);

  QVERIFY(unregisterDone.load());
  delete mock;

  mgr->setBudgetBytes(savedBudget);
}

void CacheCoreTests::cacheFileRoundtrip()
{
  QTemporaryDir dir;
  const QString path = dir.filePath("x.cache");
  const QByteArray magic("TSTMAGIC");
  const QByteArray payload(1000, '\xAB');
  QVERIFY(writeCacheFileAtomic(path, magic, 3, CacheFlags::None, payload));
  QByteArray back;
  quint16 flags = 0;
  QString reason;
  QVERIFY(readCacheFile(path, magic, 3, 3, &back, &flags, &reason));
  QCOMPARE(back, payload);
  QCOMPARE(flags, quint16(CacheFlags::None));
}

void CacheCoreTests::cacheFileRejectsBadMagic()
{
  QTemporaryDir dir;
  const QString path = dir.filePath("m.cache");
  QVERIFY(writeCacheFileAtomic(path, QByteArray("AAAAAAA1"), 1, CacheFlags::None, QByteArray("p")));
  QString reason;
  QVERIFY(!readCacheFile(path, QByteArray("BBBBBBBB"), 1, 1, nullptr, nullptr, &reason));
  QVERIFY(reason.contains(QStringLiteral("magic")));
}

void CacheCoreTests::cacheFileRejectsVersionOutOfRange()
{
  QTemporaryDir dir;
  const QString path = dir.filePath("v.cache");
  QVERIFY(writeCacheFileAtomic(path, QByteArray("VERTEST!"), 5, CacheFlags::None, QByteArray("p")));
  QString reason;
  // D2.3：过版缓存拒绝（调用方删文件重建完成迁移）。
  QVERIFY(!readCacheFile(path, QByteArray("VERTEST!"), 1, 4, nullptr, nullptr, &reason));
  QVERIFY(reason.contains(QStringLiteral("version")));
  QVERIFY(!readCacheFile(path, QByteArray("VERTEST!"), 6, 9, nullptr, nullptr, &reason));
}

void CacheCoreTests::cacheFileRejectsCorruptPayload()
{
  QTemporaryDir dir;
  const QString path = dir.filePath("c.cache");
  QVERIFY(writeCacheFileAtomic(path, QByteArray("CRPTTST!"), 1, CacheFlags::None,
                               QByteArray(500, 'z')));
  // 翻转 payload 中段一个字节。
  QFile f(path);
  QVERIFY(f.open(QIODevice::ReadWrite));
  f.seek(40);
  const char flip = 'X';
  f.write(&flip, 1);
  f.close();
  QString reason;
  QVERIFY(!readCacheFile(path, QByteArray("CRPTTST!"), 1, 1, nullptr, nullptr, &reason));
  QVERIFY(reason.contains(QStringLiteral("crc")));
}

void CacheCoreTests::cacheFileSelfHealsTruncated()
{
  QTemporaryDir dir;
  const QString path = dir.filePath("t.cache");
  QVERIFY(writeCacheFileAtomic(path, QByteArray("TRNCTST!"), 1, CacheFlags::None,
                               QByteArray(4000, 'q')));
  QFile f(path);
  QVERIFY(f.open(QIODevice::ReadWrite));
  f.resize(2000);
  f.close();
  QString reason;
  QVERIFY(!readCacheFile(path, QByteArray("TRNCTST!"), 1, 1, nullptr, nullptr, &reason));
  QVERIFY(reason.contains(QStringLiteral("truncated")) || reason.contains(QStringLiteral("size")));
}

void CacheCoreTests::cacheFileRejectsCorruptSizeFields()
{
  // #217：storedSize（字节 28-35）/payloadSize（20-27）不在 headerCrc 覆盖内。
  // 高位单比特翻转必须走「损坏」报因（调用方据此自愈删除），不得按声明值分配。
  QTemporaryDir dir;
  const QString path = dir.filePath("s.cache");
  QVERIFY(writeCacheFileAtomic(path, QByteArray("SIZETST!"), 1, CacheFlags::None,
                               QByteArray(4000, 'q')));
  {
    QFile f(path);
    QVERIFY(f.open(QIODevice::ReadWrite));
    QVERIFY(f.seek(28 + 5)); // storedSize 第 6 字节 → 声明 ~2^40 字节
    QVERIFY(f.putChar(char(0x01)));
  }
  QString reason;
  QVERIFY(!readCacheFile(path, QByteArray("SIZETST!"), 1, 1, nullptr, nullptr, &reason));
  QCOMPARE(reason, QStringLiteral("truncated payload"));

#ifdef PALEO_HAVE_ZSTD
  // 压缩文件的 payloadSize 损坏：与 zstd 帧头 content size 不符 → 直接判失败。
  const QString zpath = dir.filePath("z.cache");
  QVERIFY(writeCacheFileAtomic(zpath, QByteArray("SIZETST!"), 1, CacheFlags::ZstdCompressed,
                               QByteArray(100000, 'z')));
  {
    QFile f(zpath);
    QVERIFY(f.open(QIODevice::ReadWrite));
    QVERIFY(f.seek(20 + 4)); // payloadSize 第 5 字节 → 声明 ~4GB
    QVERIFY(f.putChar(char(0x01)));
  }
  reason.clear();
  QByteArray back;
  QVERIFY(!readCacheFile(zpath, QByteArray("SIZETST!"), 1, 1, &back, nullptr, &reason));
  QVERIFY(back.isEmpty());
  QVERIFY2(reason.contains(QStringLiteral("zstd")), qPrintable(reason));
#endif
}

void CacheCoreTests::cacheFileAtomicCreatesParentDirs()
{
  // D2.1 的核心教训：发布路径父目录不存在 → rename ENOENT。原子写必须先建目录。
  QTemporaryDir dir;
  const QString path = dir.filePath("a/b/c/d/x.cache");
  QVERIFY(writeCacheFileAtomic(path, QByteArray("MKDIRTST"), 1, CacheFlags::None, QByteArray("ok")));
  QVERIFY(QFile::exists(path));
  QByteArray back;
  QVERIFY(readCacheFile(path, QByteArray("MKDIRTST"), 1, 1, &back));
  QCOMPARE(back, QByteArray("ok"));
}

void CacheCoreTests::cacheIoPrimitivesRoundtrip()
{
  QByteArray out;
  cacheio::putU16(&out, 0xABCD);
  cacheio::putU32(&out, 0xDEADBEEF);
  cacheio::putU64(&out, Q_UINT64_C(0x1122334455667788));
  cacheio::putI32(&out, -123456);
  cacheio::putF64(&out, 3.25);
  cacheio::putF32(&out, -1.5f);
  cacheio::putStr(&out, QStringLiteral("井-42"));

  qint64 pos = 0;
  bool ok = true;
  QCOMPARE(cacheio::u16(out, &pos, &ok), quint16(0xABCD));
  QCOMPARE(cacheio::u32(out, &pos, &ok), quint32(0xDEADBEEF));
  QCOMPARE(cacheio::u64(out, &pos, &ok), Q_UINT64_C(0x1122334455667788));
  QCOMPARE(cacheio::i32(out, &pos, &ok), qint32(-123456));
  QCOMPARE(cacheio::f64(out, &pos, &ok), 3.25);
  QCOMPARE(cacheio::f32(out, &pos, &ok), -1.5f);
  QCOMPARE(cacheio::str(out, &pos, &ok), QStringLiteral("井-42"));
  QVERIFY(ok);
  // 越界读 → ok=false（截断自愈路径的判定基础）。
  cacheio::u64(out, &pos, &ok);
  QVERIFY(!ok);
}

void CacheCoreTests::zstdRoundtrip()
{
  // zstd 可用则 roundtrip；不可用则如实 ok=false（未压缩降级路径）。
  QByteArray raw(200000, 0);
  for (int i = 0; i < raw.size(); ++i)
    raw[i] = char(i * 7 + (i >> 8)); // 半可压内容
  bool ok = false;
  const QByteArray packed = cacheZstdCompress(raw, &ok);
#ifdef PALEO_HAVE_ZSTD
  QVERIFY(ok);
  QVERIFY(packed.size() < raw.size());
  bool ok2 = false;
  const QByteArray back = cacheZstdDecompress(packed, raw.size(), &ok2);
  QVERIFY(ok2);
  QCOMPARE(back, raw);
  // 错误的解压尺寸 → 失败（不越界）。
  bool ok3 = true;
  cacheZstdDecompress(packed, raw.size() + 1, &ok3);
  QVERIFY(!ok3);
#else
  QVERIFY(!ok);
  QVERIFY(packed.isEmpty());
#endif
}

void CacheCoreTests::lruThreadSafety()
{
  // 并发 insert/get/erase 不崩、总量守恒（退出后 bytes 与条目数一致）。
  LruCache<int, QByteArray> cache(QStringLiteral("t10"), 100000, fixedSize);
  QVector<QFuture<void>> futs;
  std::atomic_bool stop{false};
  for (int t = 0; t < 4; ++t)
    futs.append(QtConcurrent::run([&cache, &stop, t]() {
      for (int i = 0; i < 4000 && !stop.load(); ++i)
      {
        const int k = (i * 13 + t * 97) % 200;
        if (i % 3 == 0)
          cache.insert(k, QByteArray("v"));
        else if (i % 3 == 1)
          cache.get(k);
        else
          cache.erase(k);
      }
    }));
  for (auto &f : futs)
    f.waitForFinished();
  // #151：真实不变量（旧的 size() >= 0 恒真）。键域 [0,200)、每条 100 字节、
  // 容量 100000 不触发淘汰 → 条目数 ≤ 200，字节数 = 条目数 × 100 ≤ 容量。
  QVERIFY(cache.size() <= 200);
  QCOMPARE(cache.bytes(), qint64(cache.size()) * 100);
  QVERIFY(cache.bytes() <= cache.capacity());
  cache.clear();
  QCOMPARE(cache.bytes(), qint64(0));
}

QTEST_MAIN(CacheCoreTests)
#include "tst_cache_core.moc"
