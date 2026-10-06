#include <QtTest>
#include <QSignalSpy>
#include <atomic>
#include <thread>
#include <vector>

#include "../src/services/errorhub.h"

using namespace paleo::services;

class TestErrorHub : public QObject {
  Q_OBJECT

private slots:
  void init();
  void cleanup();

  void testBasicPostingAndFields();
  void testStaticConvenienceAndCheckOrReport();
  void testDeduplication60sWindow();
  void testCustomDedupWindow();
  void testRingBufferBoundaryAndFifoEviction();
  void testDynamicCapacityChange();
  void testQueryAndFiltering();
  void testThreadSafety();
  void testHighConcurrencyStress8Threads();
  void testRingBufferNeverExceeds500UnderStorm();
  void testConcurrentIdenticalKeyAggregation();
  void testReentrantSlotExecution();
};

void TestErrorHub::init() {
  ErrorHub::instance()->clear();
  ErrorHub::instance()->setMaxCapacity(ErrorHub::kDefaultMaxHistory);
  ErrorHub::instance()->setDedupWindowSecs(ErrorHub::kDefaultDedupWindowSecs);
}

void TestErrorHub::cleanup() {
  ErrorHub::instance()->clear();
}

void TestErrorHub::testBasicPostingAndFields() {
  ErrorHub hub;
  QSignalSpy raisedSpy(&hub, &ErrorHub::errorRaised);
  QSignalSpy changedSpy(&hub, &ErrorHub::historyChanged);

  ErrorEntry entry;
  entry.level = ErrorLevel::Warning;
  entry.domain = ErrorDomain::Seismic;
  entry.message = QStringLiteral("测试警告信息");
  entry.details = QStringLiteral("堆栈或上下文详情");
  entry.deduplicationKey = QStringLiteral("custom.key.warn");

  hub.report(entry);

  QCOMPARE(raisedSpy.count(), 1);
  QCOMPARE(changedSpy.count(), 1);
  QCOMPARE(hub.count(), 1);

  const auto history = hub.history();
  QCOMPARE(history.size(), 1);

  const auto &stored = history.first();
  QVERIFY(stored.id > 0);
  QCOMPARE(stored.level, ErrorLevel::Warning);
  QCOMPARE(stored.domain, ErrorDomain::Seismic);
  QCOMPARE(stored.message, QStringLiteral("测试警告信息"));
  QCOMPARE(stored.details, QStringLiteral("堆栈或上下文详情"));
  QCOMPARE(stored.deduplicationKey, QStringLiteral("custom.key.warn"));
  QCOMPARE(stored.aggregationCount, 1);
  QVERIFY(stored.timestamp.isValid());
  QVERIFY(stored.firstSeen.isValid());
  QCOMPARE(stored.timestamp, stored.firstSeen);
  QCOMPARE(stored.isModal, false);

  // 默认去重键验证
  ErrorEntry defaultKeyEntry;
  defaultKeyEntry.level = ErrorLevel::Error;
  defaultKeyEntry.domain = ErrorDomain::Well;
  defaultKeyEntry.message = QStringLiteral("井数据解析失败");
  hub.report(defaultKeyEntry);

  QCOMPARE(hub.count(), 2);
  const auto updated = hub.history();
  QCOMPARE(updated.last().deduplicationKey,
           ErrorEntry::makeDefaultKey(ErrorLevel::Error, ErrorDomain::Well, QStringLiteral("井数据解析失败")));
}

void TestErrorHub::testStaticConvenienceAndCheckOrReport() {
  ErrorHub *hub = ErrorHub::instance();
  QSignalSpy raisedSpy(hub, &ErrorHub::errorRaised);

  ErrorHub::postInfo(ErrorDomain::General, QStringLiteral("系统启动"), QString(), QStringLiteral("sys.boot"));
  ErrorHub::postWarning(ErrorDomain::IO, QStringLiteral("缓存命中率偏低"), QString(), QStringLiteral("io.cache"));
  ErrorHub::postError(ErrorDomain::Gridding, QStringLiteral("插值网格发散"), QStringLiteral("div by 0"), QStringLiteral("grid.div"));
  ErrorHub::postCritical(ErrorDomain::Project, QStringLiteral("工程元数据库损坏"), QStringLiteral("sqlite error"), QStringLiteral("proj.db"));

  QCOMPARE(raisedSpy.count(), 4);
  QCOMPARE(hub->count(), 4);
  QCOMPARE(hub->countByLevel(ErrorLevel::Info), 1);
  QCOMPARE(hub->countByLevel(ErrorLevel::Warning), 1);
  QCOMPARE(hub->countByLevel(ErrorLevel::Error), 1);
  QCOMPARE(hub->countByLevel(ErrorLevel::Critical), 1);

  // 验证 checkOrReport
  bool resTrue = ErrorHub::checkOrReport(true, ErrorLevel::Error, ErrorDomain::IO, QStringLiteral("无事发生"));
  QVERIFY(resTrue);
  QCOMPARE(hub->count(), 4);

  bool resFalse = ErrorHub::checkOrReport(false, ErrorLevel::Error, ErrorDomain::IO, QStringLiteral("文件写入被拒"), QString(), QStringLiteral("io.denied"));
  QVERIFY(!resFalse);
  QCOMPARE(hub->count(), 5);
}

void TestErrorHub::testDeduplication60sWindow() {
  ErrorHub hub;
  hub.setDedupWindowSecs(60);

  QSignalSpy raisedSpy(&hub, &ErrorHub::errorRaised);
  QSignalSpy aggregatedSpy(&hub, &ErrorHub::errorAggregated);

  const QDateTime t0 = QDateTime::currentDateTime();
  const QString key = QStringLiteral("dedup.test.key");

  // 1. T0: 首次触发，应触发 errorRaised
  ErrorEntry e1;
  e1.level = ErrorLevel::Error;
  e1.domain = ErrorDomain::Catalog;
  e1.message = QStringLiteral("连接超时");
  e1.details = QStringLiteral("尝试 1");
  e1.deduplicationKey = key;
  e1.timestamp = t0;
  hub.report(e1);

  QCOMPARE(raisedSpy.count(), 1);
  QCOMPARE(aggregatedSpy.count(), 0);
  QCOMPARE(hub.count(), 1);
  QCOMPARE(hub.history().first().aggregationCount, 1);
  const qint64 firstId = hub.history().first().id;

  // 2. T0 + 15s (在 60s 窗口内)：相同 key，应聚合并累加计数
  ErrorEntry e2;
  e2.level = ErrorLevel::Error;
  e2.domain = ErrorDomain::Catalog;
  e2.message = QStringLiteral("连接超时");
  e2.details = QStringLiteral("尝试 2");
  e2.deduplicationKey = key;
  e2.timestamp = t0.addSecs(15);
  hub.report(e2);

  QCOMPARE(raisedSpy.count(), 1); // 未新增新条目
  QCOMPARE(aggregatedSpy.count(), 1); // 触发聚合信号
  QCOMPARE(hub.count(), 1); // 历史条目数仍为 1
  auto current = hub.history().first();
  QCOMPARE(current.id, firstId);
  QCOMPARE(current.aggregationCount, 2);
  QCOMPARE(current.timestamp, t0.addSecs(15));
  QCOMPARE(current.firstSeen, t0);
  QVERIFY(current.details.contains(QStringLiteral("尝试 1")));
  QVERIFY(current.details.contains(QStringLiteral("尝试 2")));

  // 3. T0 + 59s (仍处于 60s 窗口内)：再次聚合
  ErrorEntry e3 = e2;
  e3.timestamp = t0.addSecs(59);
  hub.report(e3);

  QCOMPARE(raisedSpy.count(), 1);
  QCOMPARE(aggregatedSpy.count(), 2);
  QCOMPARE(hub.count(), 1);
  QCOMPARE(hub.history().first().aggregationCount, 3);

  // 4. T0 + 61s (> 60s 窗口过期)：应判定为新周期事件，新增一条记录
  ErrorEntry e4 = e2;
  e4.timestamp = t0.addSecs(120); // 距离上次发生已超过 60s
  hub.report(e4);

  QCOMPARE(raisedSpy.count(), 2); // 新条目触发 errorRaised
  QCOMPARE(aggregatedSpy.count(), 2);
  QCOMPARE(hub.count(), 2); // 历史队列新增至 2
  QCOMPARE(hub.history().last().aggregationCount, 1);
  QVERIFY(hub.history().last().id > firstId);

  // 5. 异键条目即使在 60s 内也立即创建新记录
  ErrorEntry eDiff;
  eDiff.deduplicationKey = QStringLiteral("different.key");
  eDiff.message = QStringLiteral("另一类错误");
  eDiff.timestamp = t0.addSecs(20);
  hub.report(eDiff);

  QCOMPARE(raisedSpy.count(), 3);
  QCOMPARE(hub.count(), 3);
}

void TestErrorHub::testCustomDedupWindow() {
  ErrorHub hub;
  hub.setDedupWindowSecs(10); // 自定义 10 秒窗口
  QCOMPARE(hub.dedupWindowSecs(), 10);

  const QDateTime t0 = QDateTime::currentDateTime();
  const QString key = QStringLiteral("custom.window.key");

  ErrorEntry e1;
  e1.deduplicationKey = key;
  e1.message = QStringLiteral("短窗口错误");
  e1.timestamp = t0;
  hub.report(e1);
  QCOMPARE(hub.count(), 1);

  // 5 秒时命中
  ErrorEntry e2 = e1;
  e2.timestamp = t0.addSecs(5);
  hub.report(e2);
  QCOMPARE(hub.count(), 1);
  QCOMPARE(hub.history().first().aggregationCount, 2);

  // 16 秒时已超出 10 秒窗口
  ErrorEntry e3 = e1;
  e3.timestamp = t0.addSecs(16);
  hub.report(e3);
  QCOMPARE(hub.count(), 2);
  QCOMPARE(hub.history().last().aggregationCount, 1);
}

void TestErrorHub::testRingBufferBoundaryAndFifoEviction() {
  ErrorHub hub;
  hub.clear();
  QCOMPARE(hub.count(), 0);
  QVERIFY(hub.history().isEmpty());

  constexpr int capacity = 500;
  hub.setMaxCapacity(capacity);
  QCOMPARE(hub.maxCapacity(), capacity);

  // 写入刚好 500 条互不相同的记录
  for (int i = 1; i <= capacity; ++i) {
    ErrorEntry e;
    e.level = ErrorLevel::Error;
    e.domain = ErrorDomain::General;
    e.message = QStringLiteral("Message %1").arg(i);
    e.deduplicationKey = QStringLiteral("key_%1").arg(i);
    hub.report(e);
  }

  QCOMPARE(hub.count(), capacity);
  auto hist = hub.history();
  QCOMPARE(hist.first().message, QStringLiteral("Message 1"));
  QCOMPARE(hist.last().message, QStringLiteral("Message 500"));

  // 写入第 501 条记录：触发 FIFO 逐出，队列长度保持 500
  ErrorEntry e501;
  e501.level = ErrorLevel::Error;
  e501.domain = ErrorDomain::General;
  e501.message = QStringLiteral("Message 501");
  e501.deduplicationKey = QStringLiteral("key_501");
  hub.report(e501);

  QCOMPARE(hub.count(), capacity);
  hist = hub.history();
  QCOMPARE(hist.first().message, QStringLiteral("Message 2")); // 最早的 Message 1 已被逐出
  QCOMPARE(hist.last().message, QStringLiteral("Message 501"));

  // 验证已被逐出的 key_1 不会再与新上报发生幽灵聚合（索引同步清理）
  ErrorEntry e1New;
  e1New.message = QStringLiteral("Message 1 再度发生");
  e1New.deduplicationKey = QStringLiteral("key_1");
  hub.report(e1New);

  QCOMPARE(hub.count(), capacity);
  hist = hub.history();
  QCOMPARE(hist.first().message, QStringLiteral("Message 3")); // Message 2 也被逐出
  QCOMPARE(hist.last().message, QStringLiteral("Message 1 再度发生"));
  QCOMPARE(hist.last().aggregationCount, 1); // 作为全新条目存入
}

void TestErrorHub::testDynamicCapacityChange() {
  ErrorHub hub;
  hub.setMaxCapacity(50);
  for (int i = 0; i < 50; ++i) {
    hub.reportInfo(ErrorDomain::General, QStringLiteral("Item %1").arg(i), QString(), QStringLiteral("k_%1").arg(i));
  }
  QCOMPARE(hub.count(), 50);

  // 缩容至 20：最早的 30 条被丢弃，保留最新的 20 条
  hub.setMaxCapacity(20);
  QCOMPARE(hub.count(), 20);
  auto hist = hub.history();
  QCOMPARE(hist.first().message, QStringLiteral("Item 30"));
  QCOMPARE(hist.last().message, QStringLiteral("Item 49"));
}

void TestErrorHub::testQueryAndFiltering() {
  ErrorHub hub;
  hub.clear();

  const QDateTime baseTime = QDateTime(QDate(2026, 10, 1), QTime(12, 0, 0));

  ErrorEntry e1;
  e1.level = ErrorLevel::Info;
  e1.domain = ErrorDomain::Seismic;
  e1.message = QStringLiteral("地震测网载入完成");
  e1.details = QStringLiteral("耗时 320ms");
  e1.deduplicationKey = QStringLiteral("q1");
  e1.timestamp = baseTime.addSecs(100);
  hub.report(e1);

  ErrorEntry e2;
  e2.level = ErrorLevel::Warning;
  e2.domain = ErrorDomain::Seismic;
  e2.message = QStringLiteral("道头坐标存在 NaN");
  e2.details = QStringLiteral("道号 4050");
  e2.deduplicationKey = QStringLiteral("q2");
  e2.timestamp = baseTime.addSecs(200);
  hub.report(e2);

  ErrorEntry e3;
  e3.level = ErrorLevel::Error;
  e3.domain = ErrorDomain::Well;
  e3.message = QStringLiteral("分层深度倒置");
  e3.details = QStringLiteral("井 W-01 层位 T1");
  e3.deduplicationKey = QStringLiteral("q3");
  e3.timestamp = baseTime.addSecs(300);
  hub.report(e3);

  ErrorEntry e4;
  e4.level = ErrorLevel::Critical;
  e4.domain = ErrorDomain::Project;
  e4.message = QStringLiteral("工程只读锁定失败");
  e4.details = QStringLiteral("权限拒绝");
  e4.deduplicationKey = QStringLiteral("q4");
  e4.timestamp = baseTime.addSecs(400);
  hub.report(e4);

  // 1. 按 Domain 过滤
  auto seismicList = hub.queryByDomain(ErrorDomain::Seismic);
  QCOMPARE(seismicList.size(), 2);

  const auto allEntries = hub.history();
  QCOMPARE(allEntries.size(), 4);
  const qint64 id2 = allEntries[1].id;
  const qint64 id3 = allEntries[2].id;

  auto wellList = hub.queryByDomain(ErrorDomain::Well);
  QCOMPARE(wellList.size(), 1);
  QCOMPARE(wellList.first().message, QStringLiteral("分层深度倒置"));

  // 2. 按精确 Level 过滤
  auto warnList = hub.queryByLevel(ErrorLevel::Warning);
  QCOMPARE(warnList.size(), 1);
  QCOMPARE(warnList.first().id, id2);
  QCOMPARE(warnList.first().message, QStringLiteral("道头坐标存在 NaN"));

  // 3. 按最低 Level 过滤 (Warning, Error, Critical)
  auto minWarnList = hub.queryByMinLevel(ErrorLevel::Warning);
  QCOMPARE(minWarnList.size(), 3);

  // 4. 文本模糊过滤
  ErrorQueryFilter filterText;
  filterText.searchText = QStringLiteral("NaN");
  auto textResults = hub.query(filterText);
  QCOMPARE(textResults.size(), 1);
  QCOMPARE(textResults.first().message, QStringLiteral("道头坐标存在 NaN"));

  // 详情模糊过滤
  ErrorQueryFilter filterDetail;
  filterDetail.searchText = QStringLiteral("权限拒绝");
  auto detailResults = hub.query(filterDetail);
  QCOMPARE(detailResults.size(), 1);
  QCOMPARE(detailResults.first().level, ErrorLevel::Critical);

  // 5. 时间下限过滤
  ErrorQueryFilter filterSince;
  filterSince.since = baseTime.addSecs(250);
  auto sinceResults = hub.query(filterSince);
  QCOMPARE(sinceResults.size(), 2); // e3 and e4

  // 6. 条数 limit
  ErrorQueryFilter filterLimit;
  filterLimit.limit = 2;
  auto limitResults = hub.query(filterLimit);
  QCOMPARE(limitResults.size(), 2);

  // 7. entryById
  auto optEntry = hub.entryById(id3);
  QVERIFY(optEntry.has_value());
  QCOMPARE(optEntry->message, QStringLiteral("分层深度倒置"));

  auto optNone = hub.entryById(999999);
  QVERIFY(!optNone.has_value());

  // 8. 清空与信号
  QSignalSpy clearedSpy(&hub, &ErrorHub::historyCleared);
  QSignalSpy changedSpy(&hub, &ErrorHub::historyChanged);
  hub.clear();
  QCOMPARE(clearedSpy.count(), 1);
  QCOMPARE(changedSpy.count(), 1);
  QCOMPARE(hub.count(), 0);
  QVERIFY(hub.history().isEmpty());
}

void TestErrorHub::testThreadSafety() {
  ErrorHub hub;
  hub.clear();
  hub.setMaxCapacity(500);

  constexpr int threadCount = 4;
  constexpr int perThreadCount = 200;
  std::vector<std::thread> workers;
  workers.reserve(threadCount);

  for (int t = 0; t < threadCount; ++t) {
    workers.emplace_back([&hub, t]() {
      for (int i = 0; i < perThreadCount; ++i) {
        ErrorEntry entry;
        entry.level = (i % 2 == 0) ? ErrorLevel::Warning : ErrorLevel::Error;
        entry.domain = ErrorDomain::General;
        entry.message = QStringLiteral("Thread %1 iter %2").arg(t).arg(i);
        // 让部分 key 产生聚合，部分产生新增
        entry.deduplicationKey = QStringLiteral("thread_%1_key_%2").arg(t).arg(i % 50);
        hub.report(entry);

        if (i % 20 == 0) {
          auto list = hub.history();
          Q_UNUSED(list);
          auto q = hub.queryByLevel(ErrorLevel::Warning);
          Q_UNUSED(q);
        }
      }
    });
  }

  for (auto &w : workers) {
    w.join();
  }

  // 验证多线程并发后状态完好且不超上限 500
  QVERIFY(hub.count() <= 500);
  QVERIFY(hub.count() > 0);
  const auto all = hub.history();
  QCOMPARE(hub.count(), static_cast<int>(all.size()));
}

void TestErrorHub::testHighConcurrencyStress8Threads() {
  ErrorHub hub;
  hub.clear();
  hub.setMaxCapacity(500);

  constexpr int writerCount = 8;
  constexpr int perWriterCount = 500; // 8 * 500 = 4,000 writes total
  constexpr int readerCount = 4;
  std::atomic<bool> stopReaders{false};
  std::atomic<int> readViolations{0};
  std::atomic<int> readIterations{0};

  // Launch reader threads
  std::vector<std::thread> readers;
  readers.reserve(readerCount);
  for (int r = 0; r < readerCount; ++r) {
    readers.emplace_back([&hub, &stopReaders, &readViolations, &readIterations]() {
      while (!stopReaders.load()) {
        int c = hub.count();
        if (c < 0 || c > 500) {
          readViolations++;
        }
        auto h = hub.history();
        if (h.size() > 500) {
          readViolations++;
        }
        for (const auto &entry : h) {
          if (entry.id <= 0 || entry.aggregationCount < 1 || entry.message.isEmpty()) {
            readViolations++;
          }
        }
        auto q = hub.queryByLevel(ErrorLevel::Warning);
        if (q.size() > 500) {
          readViolations++;
        }
        readIterations++;
      }
    });
  }

  // Launch writer threads
  std::vector<std::thread> writers;
  writers.reserve(writerCount);
  for (int w = 0; w < writerCount; ++w) {
    writers.emplace_back([&hub, w]() {
      for (int i = 0; i < perWriterCount; ++i) {
        ErrorEntry entry;
        entry.level = (i % 3 == 0) ? ErrorLevel::Error : ((i % 3 == 1) ? ErrorLevel::Warning : ErrorLevel::Info);
        entry.domain = ErrorDomain::General;
        entry.message = QStringLiteral("W%1_I%2").arg(w).arg(i);
        if (i < 250) {
          // Unique keys forcing FIFO eviction past 500
          entry.deduplicationKey = QStringLiteral("w%1_unique_%2").arg(w).arg(i);
        } else {
          // Shared keys forcing concurrent aggregation
          entry.deduplicationKey = QStringLiteral("shared_key_%1").arg(i % 20);
        }
        hub.report(entry);
      }
    });
  }

  for (auto &w : writers) {
    w.join();
  }

  stopReaders.store(true);
  for (auto &r : readers) {
    r.join();
  }

  QCOMPARE(readViolations.load(), 0);
  QVERIFY(readIterations.load() > 0);
  QCOMPARE(hub.count(), 500);
  const auto finalHistory = hub.history();
  QCOMPARE(finalHistory.size(), 500);

  // Check monotonic ID ordering in ring buffer
  for (int i = 1; i < finalHistory.size(); ++i) {
    QVERIFY(finalHistory[i].id > finalHistory[i - 1].id);
  }
}

void TestErrorHub::testRingBufferNeverExceeds500UnderStorm() {
  ErrorHub hub;
  hub.clear();
  hub.setMaxCapacity(500);

  // High-frequency write storm: 5,000 distinct items reported in tight loop
  constexpr int totalWrites = 5000;
  for (int i = 1; i <= totalWrites; ++i) {
    ErrorEntry entry;
    entry.level = ErrorLevel::Error;
    entry.domain = ErrorDomain::IO;
    entry.message = QStringLiteral("Storm error %1").arg(i);
    entry.deduplicationKey = QStringLiteral("storm_distinct_%1").arg(i);
    hub.report(entry);

    if (i % 100 == 0) {
      QCOMPARE(hub.count(), std::min(i, 500));
      QCOMPARE(hub.history().size(), std::min(i, 500));
    }
  }

  QCOMPARE(hub.count(), 500);
  auto h = hub.history();
  QCOMPARE(h.size(), 500);
  // Oldest remaining item should be totalWrites - 500 + 1 = 4501
  QCOMPARE(h.first().message, QStringLiteral("Storm error 4501"));
  QCOMPARE(h.last().message, QStringLiteral("Storm error 5000"));
}

void TestErrorHub::testConcurrentIdenticalKeyAggregation() {
  ErrorHub hub;
  hub.clear();
  hub.setMaxCapacity(500);
  hub.setDedupWindowSecs(60);

  constexpr int threadCount = 4;
  constexpr int perThreadWrites = 500; // total 2,000 writes to the exact same key
  const QString targetKey = QStringLiteral("contended.single.key");
  const QDateTime fixedTime = QDateTime::currentDateTime();

  std::vector<std::thread> workers;
  workers.reserve(threadCount);

  for (int t = 0; t < threadCount; ++t) {
    workers.emplace_back([&hub, &targetKey, fixedTime, t]() {
      for (int i = 0; i < perThreadWrites; ++i) {
        ErrorEntry entry;
        entry.level = ErrorLevel::Error;
        entry.domain = ErrorDomain::Catalog;
        entry.message = QStringLiteral("Contended lock event");
        entry.deduplicationKey = targetKey;
        entry.timestamp = fixedTime;
        entry.details = QStringLiteral("T%1_I%2").arg(t).arg(i);
        hub.report(entry);
      }
    });
  }

  for (auto &w : workers) {
    w.join();
  }

  QCOMPARE(hub.count(), 1);
  const auto history = hub.history();
  QCOMPARE(history.size(), 1);
  QCOMPARE(history.first().aggregationCount, threadCount * perThreadWrites);
}

void TestErrorHub::testReentrantSlotExecution() {
  ErrorHub hub;
  hub.clear();

  int slotCallCount = 0;
  int reentrantReportCount = 0;

  QObject::connect(&hub, &ErrorHub::errorRaised, [&](const ErrorEntry &entry) {
    slotCallCount++;
    // In slot: query history while inside signal notification
    auto h = hub.history();
    Q_UNUSED(h);
    int c = hub.count();
    Q_UNUSED(c);

    // Conditionally report another error (testing re-entry without deadlock)
    if (entry.deduplicationKey == QStringLiteral("trigger.reentrant")) {
      reentrantReportCount++;
      hub.reportInfo(ErrorDomain::General, QStringLiteral("Reentrant log"),
                     QString(), QStringLiteral("reentrant.done"));
    }
  });

  hub.reportWarning(ErrorDomain::General, QStringLiteral("Trigger error"),
                    QString(), QStringLiteral("trigger.reentrant"));

  QCOMPARE(slotCallCount, 2); // Initial trigger + reentrant trigger
  QCOMPARE(reentrantReportCount, 1);
  QCOMPARE(hub.count(), 2);
}

QTEST_MAIN(TestErrorHub)
#include "tst_errorhub_d54.moc"
