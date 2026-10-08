// 层：测试壳
// 方向54/M5 起源，方向76 合流后重定向至全局 ErrorHub：错误风暴压力。
// 54 系呈现件（NotificationManager 等）删除后，保留的 hub 级风暴语义：
// 事件循环节奏注入下主循环不被阻塞（最大延迟 < 50ms）、环形历史上限
// 500 条 FIFO 逐出、多线程并发风暴下上限保持。呈现级风暴（卡片上限/
// 排空/模态抑制）由 tst_notifications（64 栈）覆盖。
// CMake 侧保留 RUN_SERIAL + TIMEOUT 120 布线（风暴测试独占串行）。

#include <QtTest>
#include <QCoreApplication>
#include <QDeadlineTimer>
#include <QElapsedTimer>
#include <QEventLoop>
#include <QThread>
#include <QTimer>
#include <algorithm>
#include <atomic>
#include <cmath>
#include <vector>

#include "../src/services/errorhub.h"

class TestErrorHubStorm : public QObject
{
  Q_OBJECT

private:
  // 心跳与事件循环延迟探针（10ms PreciseTimer）。
  struct EventLoopProbe {
    QTimer timer;
    QElapsedTimer elapsed;
    qint64 lastTickMs = -1;
    qint64 maxGapMs = 0;
    qint64 maxLagMs = 0;
    std::vector<qint64> gaps;
    int tickCount = 0;

    void start(int intervalMs = 10) {
      gaps.clear();
      gaps.reserve(2000);
      maxGapMs = 0;
      maxLagMs = 0;
      tickCount = 0;
      lastTickMs = -1;

      timer.setTimerType(Qt::PreciseTimer);
      timer.setInterval(intervalMs);
      elapsed.start();

      QObject::connect(&timer, &QTimer::timeout, [this, intervalMs]() {
        const qint64 now = elapsed.elapsed();
        if (lastTickMs >= 0) {
          const qint64 gap = now - lastTickMs;
          gaps.push_back(gap);
          if (gap > maxGapMs) {
            maxGapMs = gap;
          }
          const qint64 lag = std::max<qint64>(0, gap - intervalMs);
          if (lag > maxLagMs) {
            maxLagMs = lag;
          }
        }
        lastTickMs = now;
        tickCount++;
      });
      timer.start();
    }

    void stop() {
      timer.stop();
      timer.disconnect();
    }

    qint64 p95GapMs() const {
      if (gaps.empty()) return 0;
      auto sorted = gaps;
      std::sort(sorted.begin(), sorted.end());
      const std::size_t idx = static_cast<std::size_t>(std::ceil(0.95 * sorted.size())) - 1;
      return sorted[std::min(idx, sorted.size() - 1)];
    }
  };

private slots:
  // =========================================================================
  // 1. 核心验收：100 条/秒 × 10 秒（1,000 条独立键）事件循环节奏注入。
  //    主循环不被阻塞（最大延迟 < 50ms）；环形历史上限 500；FIFO 逐出后
  //    保留 id 500..999 的键完整。
  // =========================================================================
  void sustainedStorm100Hz10s()
  {
    ErrorHub hub;
    QSignalSpy spy(&hub, &ErrorHub::errorRaised);

    EventLoopProbe probe;
    probe.start(10);

    constexpr int kTotalBatches = 100;
    constexpr int kErrorsPerBatch = 10;
    constexpr int kBatchIntervalMs = 100;

    int batchesSent = 0;
    QEventLoop loop;
    QTimer stormTimer;
    stormTimer.setTimerType(Qt::PreciseTimer);
    stormTimer.setInterval(kBatchIntervalMs);

    connect(&stormTimer, &QTimer::timeout, [&]() {
      for (int i = 0; i < kErrorsPerBatch; ++i) {
        const int errorId = batchesSent * kErrorsPerBatch + i;
        hub.raise(ErrorHub::Level::Error, QStringLiteral("storm"),
                  QStringLiteral("风暴错误 #%1").arg(errorId),
                  QStringLiteral("详细上下文 #%1").arg(errorId),
                  QStringLiteral("storm.sustained.%1").arg(errorId));
      }
      batchesSent++;
      if (batchesSent >= kTotalBatches) {
        stormTimer.stop();
        loop.quit();
      }
    });

    stormTimer.start();
    loop.exec();
    probe.stop();

    QVERIFY2(probe.maxLagMs < 50,
             qPrintable(QStringLiteral("主线程事件循环发生阻塞！最大延迟: %1 ms (阈值 50ms), 最大间隔: %2 ms, P95: %3 ms")
                            .arg(probe.maxLagMs).arg(probe.maxGapMs).arg(probe.p95GapMs())));
    QCOMPARE(batchesSent, kTotalBatches);
    QVERIFY(probe.tickCount > 0);

    // 环形缓冲区上限保持 500 条（最早的 500 条被 FIFO 逐出）
    QCOMPARE(hub.size(), ErrorHub::kCapacity);
    QCOMPARE(spy.count(), kTotalBatches * kErrorsPerBatch);
    QCOMPARE(hub.totalRaised(), quint64(kTotalBatches * kErrorsPerBatch));

    const auto history = hub.entries();
    QCOMPARE(history.size(), ErrorHub::kCapacity);
    QCOMPARE(history.first().dedupKey, QStringLiteral("storm.sustained.500"));
    QCOMPARE(history.last().dedupKey, QStringLiteral("storm.sustained.999"));
  }

  // =========================================================================
  // 2. 环形历史 500 条 FIFO 逐出极限压力（1,500 条连续注入，保留 1000..1499）。
  //    原用例 entryById 断言改为 id 连续性断言（全局 hub 查询面为 entries()）。
  // =========================================================================
  void ringFifoEvictionUnderStorm()
  {
    ErrorHub hub;

    constexpr int kTotalInjections = 1500;
    for (int i = 0; i < kTotalInjections; ++i) {
      hub.raise(ErrorHub::Level::Error, QStringLiteral("seismic"),
                QStringLiteral("地震切片解析故障 #%1").arg(i),
                QStringLiteral("chunk_%1").arg(i),
                QStringLiteral("seismic.evict.%1").arg(i));
      QCOMPARE(hub.size(), qMin(i + 1, ErrorHub::kCapacity));
    }

    const auto hist = hub.entries();
    QCOMPARE(hist.size(), ErrorHub::kCapacity);

    // 最早保留的记录是第 1000 条（id 连续区间 1001..1500）
    QCOMPARE(hist.first().dedupKey, QStringLiteral("seismic.evict.1000"));
    QCOMPARE(hist.last().dedupKey, QStringLiteral("seismic.evict.1499"));
    QCOMPARE(hist.first().id, quint64(1001));
    QCOMPARE(hist.last().id, quint64(1500));
    for (int i = 1; i < hist.size(); ++i) {
      QVERIFY2(hist.at(i).id == hist.at(i - 1).id + 1,
               qPrintable(QStringLiteral("id 不连续于 %1").arg(i)));
    }
  }

  // =========================================================================
  // 3. 多线程并发风暴：8 个工作线程同时上报（1,000 条跨线程错误）。
  //    发信号在锁外：主线程事件循环驱动等待期间不被阻塞，上限保持 500。
  // =========================================================================
  void multiThreadedStorm8Workers()
  {
    ErrorHub hub;

    constexpr int kThreadCount = 8;
    constexpr int kPerThreadErrors = 125; // 8 x 125 = 1,000 total

    std::atomic<bool> startFlag{false};
    std::atomic<int> completedWorkers{0};
    std::vector<QThread *> workers;

    for (int t = 0; t < kThreadCount; ++t) {
      workers.push_back(QThread::create([&hub, &startFlag, &completedWorkers, t]() {
        while (!startFlag.load(std::memory_order_relaxed)) {
          QThread::yieldCurrentThread();
        }
        for (int i = 0; i < kPerThreadErrors; ++i) {
          hub.raise(ErrorHub::Level::Error, QStringLiteral("ai"),
                    QStringLiteral("线程 %1 预测任务异常 #%2").arg(t).arg(i),
                    QStringLiteral("线程上下文"),
                    QStringLiteral("storm.thread.%1.%2").arg(t).arg(i));
        }
        completedWorkers.fetch_add(1);
      }));
    }
    for (auto *th : workers)
      th->start();

    startFlag.store(true, std::memory_order_release);

    QDeadlineTimer deadline(10000);
    while (completedWorkers.load() < kThreadCount && !deadline.hasExpired()) {
      QCoreApplication::processEvents(QEventLoop::AllEvents, 20);
    }

    for (auto *th : workers) {
      QVERIFY(th->wait(30000));
      delete th;
    }
    QCoreApplication::sendPostedEvents(nullptr, 0);

    QCOMPARE(completedWorkers.load(), kThreadCount);
    QCOMPARE(hub.size(), ErrorHub::kCapacity);
    QCOMPARE(hub.totalRaised(), quint64(kThreadCount * kPerThreadErrors));
  }
};

int main(int argc, char *argv[])
{
  QCoreApplication app(argc, argv);
  TestErrorHubStorm tc;
  return QTest::qExec(&tc, argc, argv);
}

#include "tst_errorhub_storm.moc"
