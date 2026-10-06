// 层：测试壳
// 方向54/M5：错误风暴压力与事件循环防冻结测试
// 验收标准：100 条/秒 x 10 秒错误风暴注入下主线程事件循环不被阻塞（最大延迟 < 50ms），通知卡片并发不超过 5 张且不溢出屏幕。

#include <QtTest>
#include <QSignalSpy>
#include <QWidget>
#include <QApplication>
#include <QCoreApplication>
#include <QElapsedTimer>
#include <QEventLoop>
#include <QTimer>
#include <QPointer>
#include <QDeadlineTimer>
#include <QCursor>
#include <QVBoxLayout>
#include <atomic>
#include <chrono>
#include <thread>
#include <vector>
#include <algorithm>
#include <cmath>

#include "../src/services/errorhub.h"
#include "../src/ui/notifications/notificationcard.h"
#include "../src/ui/notifications/notificationmanager.h"

using namespace paleo::services;
using namespace paleo::ui;

class TestErrorHubStorm : public QObject
{
  Q_OBJECT

private:
  // 心跳与事件循环延迟探针
  struct EventLoopProbe {
    QTimer timer;
    QElapsedTimer elapsed;
    qint64 lastTickMs = -1;
    qint64 maxGapMs = 0;
    qint64 maxLagMs = 0;
    std::vector<qint64> gaps;
    int tickCount = 0;
    NotificationManager *mgr = nullptr;
    bool cardLimitViolated = false;
    bool queueLimitViolated = false;

    void start(int intervalMs = 10, NotificationManager *targetMgr = nullptr) {
      mgr = targetMgr;
      gaps.clear();
      gaps.reserve(2000);
      maxGapMs = 0;
      maxLagMs = 0;
      tickCount = 0;
      lastTickMs = -1;
      cardLimitViolated = false;
      queueLimitViolated = false;

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

        // 持续断言：卡片并发上限 ≤ 5，排队上限 ≤ 50
        if (mgr) {
          if (mgr->activeCardCount() > NotificationManager::kMaxVisibleCards) {
            cardLimitViolated = true;
          }
          if (mgr->pendingQueueCount() > NotificationManager::kMaxPendingQueue) {
            queueLimitViolated = true;
          }
        }
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
  void initTestCase()
  {
    QCOMPARE(QGuiApplication::platformName(), QStringLiteral("offscreen"));
  }

  void init()
  {
    QCursor::setPos(1000, 1000);
    if (paleo::services::ErrorHub::instance()) {
      paleo::services::ErrorHub::instance()->clear();
      paleo::services::ErrorHub::instance()->setMaxCapacity(paleo::services::ErrorHub::kDefaultMaxHistory);
      paleo::services::ErrorHub::instance()->setDedupWindowSecs(paleo::services::ErrorHub::kDefaultDedupWindowSecs);
    }
    if (NotificationManager::instance()) {
      NotificationManager::instance()->clearAll();
    }
    NotificationManager::setConfirmHookForTesting(nullptr);
    NotificationManager::setOffscreenAutoAnswer(true);
  }

  void cleanup()
  {
    if (NotificationManager::instance()) {
      NotificationManager::instance()->clearAll();
    }
    NotificationManager::setConfirmHookForTesting(nullptr);
    NotificationManager::setOffscreenAutoAnswer(true);
    if (paleo::services::ErrorHub::instance()) {
      paleo::services::ErrorHub::instance()->clear();
    }
  }

  // =========================================================================
  // 1. 核心验收测试：100 条/秒 x 10 秒持续错误风暴注入 (1,000 条总错误)
  // =========================================================================
  void testSustainedErrorStorm100Hz10s()
  {
    QWidget mainWindow;
    mainWindow.resize(1000, 700);
    mainWindow.show();

    NotificationManager mgr(&mainWindow, paleo::services::ErrorHub::instance());

    EventLoopProbe probe;
    probe.start(10, &mgr); // 10ms 精度心跳探针

    constexpr int kTotalBatches = 100;
    constexpr int kErrorsPerBatch = 10;
    constexpr int kBatchIntervalMs = 100; // 100ms 一批，共 10.0 秒

    int batchesSent = 0;
    QEventLoop loop;
    QTimer stormTimer;
    stormTimer.setTimerType(Qt::PreciseTimer);
    stormTimer.setInterval(kBatchIntervalMs);

    connect(&stormTimer, &QTimer::timeout, [&]() {
      for (int i = 0; i < kErrorsPerBatch; ++i) {
        const int errorId = batchesSent * kErrorsPerBatch + i;
        paleo::services::ErrorHub::instance()->reportError(
            ErrorDomain::General,
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

    // 1. 验证事件循环响应度：最大延迟 < 50ms
    QVERIFY2(!probe.cardLimitViolated, "风暴注入期间活动卡片数曾经超过 kMaxVisibleCards (5)");
    QVERIFY2(!probe.queueLimitViolated, "风暴注入期间等待队列长度曾经超过 kMaxPendingQueue (50)");
    QVERIFY2(probe.maxLagMs < 50,
             qPrintable(QStringLiteral("主线程事件循环发生阻塞！最大延迟: %1 ms (阈值 50ms), 最大间隔: %2 ms, P95: %3 ms")
                            .arg(probe.maxLagMs).arg(probe.maxGapMs).arg(probe.p95GapMs())));

    // 2. 验证卡片呈现上限及屏幕溢出防护
    QCOMPARE(mgr.activeCardCount(), NotificationManager::kMaxVisibleCards);
    QCOMPARE(mgr.pendingQueueCount(), NotificationManager::kMaxPendingQueue);

    QWidget *overlay = mgr.overlayContainer();
    QVERIFY(overlay != nullptr);
    QVERIFY(overlay->height() <= mainWindow.height());

    // 3. 验证 paleo::services::ErrorHub 环形缓冲区上限保持 500 条
    QCOMPARE(paleo::services::ErrorHub::instance()->count(), paleo::services::ErrorHub::kDefaultMaxHistory);

    // 最早的 500 条被 FIFO 逐出，当前保留 id 500..999
    const auto history = paleo::services::ErrorHub::instance()->history();
    QCOMPARE(history.size(), 500);
    QCOMPARE(history.first().deduplicationKey, QStringLiteral("storm.sustained.500"));
    QCOMPARE(history.last().deduplicationKey, QStringLiteral("storm.sustained.999"));
  }

  // =========================================================================
  // 2. 混合风暴测试：50% 独立键 + 50% 重复键 (去重聚合与角标更新)
  // =========================================================================
  void testMixedUniqueAndDeduplicatedStorm()
  {
    QWidget mainWindow;
    mainWindow.resize(1000, 700);
    mainWindow.show();
    NotificationManager mgr(&mainWindow, paleo::services::ErrorHub::instance());

    EventLoopProbe probe;
    probe.start(10, &mgr);

    constexpr int kTotalBatches = 50;
    constexpr int kErrorsPerBatch = 20; // 20 errors / 100ms = 1,000 errors across 5s
    int batchesSent = 0;

    QEventLoop loop;
    QTimer stormTimer;
    stormTimer.setTimerType(Qt::PreciseTimer);
    stormTimer.setInterval(100);

    connect(&stormTimer, &QTimer::timeout, [&]() {
      for (int i = 0; i < kErrorsPerBatch; ++i) {
        if (i % 2 == 0) {
          // 重复键：映射到 5 个共享键之一
          const int sharedKeyId = (i / 2) % 5;
          paleo::services::ErrorHub::instance()->reportWarning(
              ErrorDomain::IO,
              QStringLiteral("共享警告 %1").arg(sharedKeyId),
              QStringLiteral("IO 发生抖动"),
              QStringLiteral("storm.shared.%1").arg(sharedKeyId));
        } else {
          // 独立键
          const int uniqueId = batchesSent * kErrorsPerBatch + i;
          paleo::services::ErrorHub::instance()->reportWarning(
              ErrorDomain::Catalog,
              QStringLiteral("独立警告 %1").arg(uniqueId),
              QStringLiteral("资产解析异常"),
              QStringLiteral("storm.unique.%1").arg(uniqueId));
        }
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

    QVERIFY2(!probe.cardLimitViolated, "混合风暴下活跃卡片超过 5 张");
    QVERIFY2(!probe.queueLimitViolated, "混合风暴下等待队列超过 50 条");
    QVERIFY2(probe.maxLagMs < 50, qPrintable(QStringLiteral("混合风暴事件循环延迟过高: %1 ms").arg(probe.maxLagMs)));
    QVERIFY(mgr.activeCardCount() <= NotificationManager::kMaxVisibleCards);
    QVERIFY(mgr.pendingQueueCount() <= NotificationManager::kMaxPendingQueue);

    // 检查是否有卡片命中了聚合
    bool foundAggregatedCard = false;
    for (auto *card : mgr.activeCards()) {
      if (card && card->aggregationCount() > 1) {
        foundAggregatedCard = true;
        break;
      }
    }
    // 共享键被重复上报，至少有卡片聚合或者历史聚合
    const auto history = paleo::services::ErrorHub::instance()->history();
    bool foundAggregatedHistory = false;
    for (const auto &entry : history) {
      if (entry.aggregationCount > 1) {
        foundAggregatedHistory = true;
        break;
      }
    }
    QVERIFY(foundAggregatedCard || foundAggregatedHistory);
  }

  // =========================================================================
  // 3. 环形历史缓冲区 500 条 FIFO 逐出极限压力测试 (1,500 条连续注入)
  // =========================================================================
  void testRingBuffer500ItemFifoEvictionUnderStorm()
  {
    paleo::services::ErrorHub hub;
    hub.setMaxCapacity(500);

    constexpr int kTotalInjections = 1500;
    for (int i = 0; i < kTotalInjections; ++i) {
      hub.reportError(ErrorDomain::Seismic,
                      QStringLiteral("地震切片解析故障 #%1").arg(i),
                      QStringLiteral("chunk_%1").arg(i),
                      QStringLiteral("seismic.evict.%1").arg(i));
      if (i < 500) {
        QCOMPARE(hub.count(), i + 1);
      } else {
        QCOMPARE(hub.count(), 500); // 严格保持 500 条上限
      }
    }

    const auto hist = hub.history();
    QCOMPARE(hist.size(), 500);

    // 验证最早保留的记录是第 1000 条 (索引 1000..1499)
    QCOMPARE(hist.first().deduplicationKey, QStringLiteral("seismic.evict.1000"));
    QCOMPARE(hist.last().deduplicationKey, QStringLiteral("seismic.evict.1499"));

    // 被逐出的 ID 查询应返回 nullopt (1..1000)
    QVERIFY(!hub.entryById(1).has_value());
    QVERIFY(!hub.entryById(500).has_value());
    QVERIFY(!hub.entryById(1000).has_value());
    // 当前保留的 ID 查询有效 (1001..1500)
    QVERIFY(hub.entryById(1001).has_value());
    QVERIFY(hub.entryById(1500).has_value());
  }

  // =========================================================================
  // 4. 多线程并发风暴：8 个后台工作线程同时上报 (1,000 条跨线程错误)
  // =========================================================================
  void testMultiThreadedConcurrentStorm8Workers()
  {
    QWidget mainWindow;
    mainWindow.resize(1000, 700);
    mainWindow.show();
    NotificationManager mgr(&mainWindow, paleo::services::ErrorHub::instance());

    EventLoopProbe probe;
    probe.start(10, &mgr);

    constexpr int kThreadCount = 8;
    constexpr int kPerThreadErrors = 125; // 8 x 125 = 1,000 total

    std::atomic<bool> startFlag{false};
    std::atomic<int> completedWorkers{0};
    std::vector<std::thread> workers;

    for (int t = 0; t < kThreadCount; ++t) {
      workers.emplace_back([t, &startFlag, &completedWorkers]() {
        while (!startFlag.load(std::memory_order_relaxed)) {
          std::this_thread::yield();
        }
        for (int i = 0; i < kPerThreadErrors; ++i) {
          paleo::services::ErrorHub::postError(
              ErrorDomain::AI,
              QStringLiteral("线程 %1 预测任务异常 #%2").arg(t).arg(i),
              QStringLiteral("线程上下文"),
              QStringLiteral("storm.thread.%1.%2").arg(t).arg(i));
          if (i % 25 == 0) {
            std::this_thread::sleep_for(std::chrono::milliseconds(2));
          }
        }
        completedWorkers.fetch_add(1);
      });
    }

    startFlag.store(true, std::memory_order_release);

    // 主线程驱动事件循环，直到所有工作线程完成
    QDeadlineTimer deadline(10000);
    while (completedWorkers.load() < kThreadCount && !deadline.hasExpired()) {
      QCoreApplication::processEvents(QEventLoop::AllEvents, 20);
    }

    for (auto &w : workers) {
      if (w.joinable()) w.join();
    }

    // 处理遗留事件
    QCoreApplication::sendPostedEvents(nullptr, 0);
    QCoreApplication::processEvents(QEventLoop::AllEvents, 100);
    probe.stop();

    QCOMPARE(completedWorkers.load(), kThreadCount);
    QVERIFY2(!probe.cardLimitViolated, "多线程风暴下活跃卡片超过 5 张");
    QVERIFY2(probe.maxLagMs < 50, qPrintable(QStringLiteral("多线程风暴事件循环延迟过高: %1 ms").arg(probe.maxLagMs)));
    QCOMPARE(mgr.activeCardCount(), NotificationManager::kMaxVisibleCards);
    QCOMPARE(paleo::services::ErrorHub::instance()->count(), paleo::services::ErrorHub::kDefaultMaxHistory);
  }

  // =========================================================================
  // 5. 风暴后队列排空与内存/对象生命周期稳定性
  // =========================================================================
  void testStormQueueDrainAndMemoryStability()
  {
    QWidget mainWindow;
    mainWindow.resize(1000, 700);
    mainWindow.show();
    NotificationManager mgr(&mainWindow, paleo::services::ErrorHub::instance());

    // 注入 100 条快速风暴填满卡片和排队
    for (int i = 0; i < 100; ++i) {
      paleo::services::ErrorHub::postInfo(ErrorDomain::General, QStringLiteral("快速信息 %1").arg(i),
                         QString(), QStringLiteral("drain.%1").arg(i));
    }
    QCoreApplication::processEvents();

    QCOMPARE(mgr.activeCardCount(), NotificationManager::kMaxVisibleCards);
    QCOMPARE(mgr.pendingQueueCount(), NotificationManager::kMaxPendingQueue);

    // 跟踪所有当前活跃及后续弹出的卡片指针
    QList<QPointer<NotificationCard>> cardPointers;
    for (auto *c : mgr.activeCards()) {
      if (c && !cardPointers.contains(c)) {
        cardPointers.append(c);
      }
    }
    QCOMPARE(cardPointers.size(), NotificationManager::kMaxVisibleCards);

    // 逐一 dismiss 卡片并排空队列
    int maxSafetyIterations = 200;
    while ((mgr.activeCardCount() > 0 || mgr.pendingQueueCount() > 0) && --maxSafetyIterations > 0) {
      auto cards = mgr.activeCards();
      for (auto *c : cards) {
        if (c) {
          if (!cardPointers.contains(c)) {
            cardPointers.append(c);
          }
          c->dismiss();
        }
      }
      QCoreApplication::processEvents();
    }

    QCOMPARE(mgr.activeCardCount(), 0);
    QCOMPARE(mgr.pendingQueueCount(), 0);

    // 确保所有延迟删除事件被处理
    QCoreApplication::sendPostedEvents(nullptr, QEvent::DeferredDelete);
    QCoreApplication::processEvents();

    // 验证所有卡片 QWidget 已被 deleteLater 彻底析构，无悬挂指针泄漏
    for (const auto &ptr : cardPointers) {
      QVERIFY(ptr.isNull());
    }

    // 验证容器内无残留子控件
    QWidget *overlay = mgr.overlayContainer();
    if (overlay && overlay->layout()) {
      QCOMPARE(overlay->layout()->count(), 0);
    }
  }

  // =========================================================================
  // 6. 微突发风暴测试：100ms 内瞬间涌入 100 条错误
  // =========================================================================
  void testFastMicroBurstStorm100ErrorsIn100Ms()
  {
    QWidget mainWindow;
    mainWindow.resize(1000, 700);
    mainWindow.show();
    NotificationManager mgr(&mainWindow, paleo::services::ErrorHub::instance());

    EventLoopProbe probe;
    probe.start(10, &mgr);

    // 瞬间注入 100 条
    for (int i = 0; i < 100; ++i) {
      paleo::services::ErrorHub::postError(ErrorDomain::Crossplot,
                          QStringLiteral("交会图样本异常 #%1").arg(i),
                          QString(), QStringLiteral("microburst.%1").arg(i));
    }
    QTest::qWait(100);
    probe.stop();

    QCOMPARE(mgr.activeCardCount(), NotificationManager::kMaxVisibleCards);
    QCOMPARE(mgr.pendingQueueCount(), NotificationManager::kMaxPendingQueue);
    QVERIFY2(!probe.cardLimitViolated, "微突发下活跃卡片超过 5 张");
    QVERIFY2(probe.maxLagMs < 50, qPrintable(QStringLiteral("微突发事件循环延迟过高: %1 ms").arg(probe.maxLagMs)));
  }

  // =========================================================================
  // 7. 模态风暴抑制测试：100 条致命错误注入下的去重抑制
  // =========================================================================
  void testModalSuppressionUnderStorm()
  {
    QWidget mainWindow;
    mainWindow.resize(1000, 700);
    mainWindow.show();
    NotificationManager mgr(&mainWindow, paleo::services::ErrorHub::instance());

    int modalDialogShowCount = 0;
    NotificationManager::setConfirmHookForTesting(
        [&modalDialogShowCount](const QString &, const QString &) -> std::optional<bool> {
          modalDialogShowCount++;
          return true; // 模拟确认
        });

    // 连续注入 100 条同 key Critical 错误
    const QString modalKey = QStringLiteral("storm.critical.key");
    for (int i = 0; i < 100; ++i) {
      paleo::services::ErrorHub::postCritical(ErrorDomain::Project,
                             QStringLiteral("工程损坏 #%1").arg(i),
                             QStringLiteral("崩溃日志"),
                             modalKey);
    }
    QCoreApplication::processEvents();

    // 第 1 条触发模态弹窗，后续 99 条由于 60s 去重抑制窗口，转为非模态通知卡片呈现
    QCOMPARE(modalDialogShowCount, 1);
    QCOMPARE(mgr.activeCardCount(), 1);
    QVERIFY(mgr.activeCards().first()->title().contains(QStringLiteral("聚合抑制")));
  }
};

int main(int argc, char *argv[])
{
  if (qgetenv("QT_QPA_PLATFORM").isEmpty()) {
    qputenv("QT_QPA_PLATFORM", "offscreen");
  }
  QApplication app(argc, argv);
  TestErrorHubStorm tc;
  return QTest::qExec(&tc, argc, argv);
}

#include "tst_errorhub_storm.moc"
