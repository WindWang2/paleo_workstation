// 层：测试壳
// Challenger M5-2 Empirical Adversarial Stress Harness

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
#include <QVBoxLayout>
#include <atomic>
#include <chrono>
#include <thread>
#include <vector>
#include <algorithm>

#include "../src/services/errorhub.h"
#include "../src/ui/notifications/notificationcard.h"
#include "../src/ui/notifications/notificationmanager.h"

using namespace paleo::services;
using namespace paleo::ui;

class ChallengerM5Harness : public QObject
{
  Q_OBJECT

private slots:
  void initTestCase()
  {
    QCOMPARE(QGuiApplication::platformName(), QStringLiteral("offscreen"));
  }

  void init()
  {
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
    if (paleo::services::ErrorHub::instance()) {
      paleo::services::ErrorHub::instance()->clear();
    }
  }

  // -------------------------------------------------------------------------
  // 1. Empirical Verification: 8 Workers x 125 = 1,000 errors
  // Check exact signal count, absence of dropped notifications, ID progression
  // -------------------------------------------------------------------------
  void verify8WorkerStormNoDroppedNotifications()
  {
    QWidget mainWindow;
    mainWindow.resize(1000, 700);
    mainWindow.show();
    NotificationManager mgr(&mainWindow, paleo::services::ErrorHub::instance());

    QSignalSpy spy(paleo::services::ErrorHub::instance(), &paleo::services::ErrorHub::errorRaised);
    QVERIFY(spy.isValid());

    constexpr int kThreadCount = 8;
    constexpr int kPerThreadErrors = 125; // 8 * 125 = 1,000

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
              QStringLiteral("线程 %1 任务 %2").arg(t).arg(i),
              QStringLiteral("详情"),
              QStringLiteral("harness.t%1.e%2").arg(t).arg(i));
        }
        completedWorkers.fetch_add(1);
      });
    }

    startFlag.store(true, std::memory_order_release);

    QDeadlineTimer deadline(10000);
    while (completedWorkers.load() < kThreadCount && !deadline.hasExpired()) {
      QCoreApplication::processEvents(QEventLoop::AllEvents, 20);
    }

    for (auto &w : workers) {
      if (w.joinable()) w.join();
    }

    // Drain all queued events
    QCoreApplication::sendPostedEvents(nullptr, 0);
    QCoreApplication::processEvents(QEventLoop::AllEvents, 100);

    QCOMPARE(completedWorkers.load(), kThreadCount);

    // Assert NO missed notifications: exactly 1,000 unique errorRaised signals received!
    QCOMPARE(spy.count(), 1000);

    // Assert ring buffer capacity is capped at 500
    QCOMPARE(paleo::services::ErrorHub::instance()->count(), 500);

    // Assert newest entry has id == 1000
    const auto history = paleo::services::ErrorHub::instance()->history();
    QCOMPARE(history.size(), 500);
    QCOMPARE(history.last().id, 1000);
    // Oldest retained is id 501
    QCOMPARE(history.first().id, 501);

    // Assert NotificationManager bounds
    QCOMPARE(mgr.activeCardCount(), 5);
    QCOMPARE(mgr.pendingQueueCount(), 50);
  }

  // -------------------------------------------------------------------------
  // 2. High-Stress Multi-Thread Storm: 16 Workers x 250 = 4,000 errors
  // Zero sleep to maximize lock contention and detect potential deadlocks/races
  // -------------------------------------------------------------------------
  void verify16WorkerHighContentionStorm()
  {
    QWidget mainWindow;
    mainWindow.resize(1000, 700);
    mainWindow.show();
    NotificationManager mgr(&mainWindow, paleo::services::ErrorHub::instance());

    QSignalSpy spy(paleo::services::ErrorHub::instance(), &paleo::services::ErrorHub::errorRaised);
    QVERIFY(spy.isValid());

    constexpr int kThreadCount = 16;
    constexpr int kPerThreadErrors = 250; // 16 * 250 = 4,000 errors

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
              QStringLiteral("重载线程 %1 任务 %2").arg(t).arg(i),
              QStringLiteral("详情"),
              QStringLiteral("heavy.t%1.e%2").arg(t).arg(i));
        }
        completedWorkers.fetch_add(1);
      });
    }

    startFlag.store(true, std::memory_order_release);

    QDeadlineTimer deadline(15000);
    while (completedWorkers.load() < kThreadCount && !deadline.hasExpired()) {
      QCoreApplication::processEvents(QEventLoop::AllEvents, 20);
    }

    for (auto &w : workers) {
      if (w.joinable()) w.join();
    }

    QCoreApplication::sendPostedEvents(nullptr, 0);
    QCoreApplication::processEvents(QEventLoop::AllEvents, 100);

    QCOMPARE(completedWorkers.load(), kThreadCount);
    // All 4,000 signals must arrive without any loss or deadlock
    QCOMPARE(spy.count(), 4000);
    QCOMPARE(paleo::services::ErrorHub::instance()->count(), 500);
    const auto history = paleo::services::ErrorHub::instance()->history();
    QCOMPARE(history.size(), 500);
    // Initial nextId was 1000, plus 4000 new errors = 5000
    QCOMPARE(history.last().id, 5000);
    QCOMPARE(history.first().id, 4501);
  }

  // -------------------------------------------------------------------------
  // 3. Complete 1,500-Item FIFO Ring Buffer Boundary & Query Verification
  // Check every individual item from 1 to 1500 for entryById and FIFO preservation
  // -------------------------------------------------------------------------
  void verifyComplete1500ItemFifoEvictionAndQuery()
  {
    paleo::services::ErrorHub hub;
    hub.setMaxCapacity(500);

    constexpr int kTotalInjections = 1500;
    for (int i = 0; i < kTotalInjections; ++i) {
      hub.reportError(ErrorDomain::Seismic,
                      QStringLiteral("地震切片解析故障 #%1").arg(i),
                      QStringLiteral("chunk_%1").arg(i),
                      QStringLiteral("seismic.evict.%1").arg(i));
    }

    QCOMPARE(hub.count(), 500);
    const auto hist = hub.history();
    QCOMPARE(hist.size(), 500);

    // Verify all 500 retained entries are strictly contiguous and in FIFO order
    for (int idx = 0; idx < 500; ++idx) {
      const qint64 expectedId = 1001 + idx;
      const QString expectedKey = QStringLiteral("seismic.evict.%1").arg(1000 + idx);
      QCOMPARE(hist[idx].id, expectedId);
      QCOMPARE(hist[idx].deduplicationKey, expectedKey);
    }

    // Exact eviction test: EVERY id from 1 to 1000 MUST be nullopt
    for (qint64 id = 1; id <= 1000; ++id) {
      auto res = hub.entryById(id);
      QVERIFY2(!res.has_value(), qPrintable(QStringLiteral("Evicted item id %1 still queryable!").arg(id)));
    }

    // Exact retention test: EVERY id from 1001 to 1500 MUST be present
    for (qint64 id = 1001; id <= 1500; ++id) {
      auto res = hub.entryById(id);
      QVERIFY2(res.has_value(), qPrintable(QStringLiteral("Retained item id %1 not queryable!").arg(id)));
      const QString expectedKey = QStringLiteral("seismic.evict.%1").arg(id - 1);
      QCOMPARE(res->deduplicationKey, expectedKey);
    }

    // Query filter checks
    auto seismicEntries = hub.queryByDomain(ErrorDomain::Seismic);
    QCOMPARE(seismicEntries.size(), 500);
    auto generalEntries = hub.queryByDomain(ErrorDomain::General);
    QCOMPARE(generalEntries.size(), 0);
  }

  // -------------------------------------------------------------------------
  // 4. Exact FIFO Order During Queue Drainage & 100% Widget Deletion Verification
  // -------------------------------------------------------------------------
  void verifyQueueDrainageExactFifoOrderAndWidgetDeletion()
  {
    QWidget mainWindow;
    mainWindow.resize(1000, 700);
    mainWindow.show();
    NotificationManager mgr(&mainWindow, paleo::services::ErrorHub::instance());

    // Inject 100 errors: drain.0 .. drain.99
    for (int i = 0; i < 100; ++i) {
      paleo::services::ErrorHub::postInfo(ErrorDomain::General,
                         QStringLiteral("消息 %1").arg(i),
                         QString(),
                         QStringLiteral("drain.%1").arg(i));
    }
    QCoreApplication::processEvents();

    QCOMPARE(mgr.activeCardCount(), 5);
    QCOMPARE(mgr.pendingQueueCount(), 50);

    // Initial 5 active cards should be drain.0 .. drain.4
    QStringList initialActiveKeys;
    QList<QPointer<NotificationCard>> allCreatedCards;
    for (auto *card : mgr.activeCards()) {
      QVERIFY(card != nullptr);
      initialActiveKeys.append(card->dedupKey());
      allCreatedCards.append(card);
    }
    QCOMPARE(initialActiveKeys, QStringList({
      QStringLiteral("drain.0"),
      QStringLiteral("drain.1"),
      QStringLiteral("drain.2"),
      QStringLiteral("drain.3"),
      QStringLiteral("drain.4")
    }));

    // The pending queue evicted drain.5..drain.49 and retained drain.50..drain.99
    // As cards are dismissed one by one, verify that newly promoted cards appear in EXACT FIFO order!
    QStringList drainedKeys;
    int safetyLoops = 100;
    while ((mgr.activeCardCount() > 0 || mgr.pendingQueueCount() > 0) && --safetyLoops > 0) {
      auto cards = mgr.activeCards();
      QVERIFY(!cards.isEmpty());
      NotificationCard *firstCard = cards.first();
      firstCard->dismiss();
      QCoreApplication::processEvents();

      // Check newly active cards
      for (auto *c : mgr.activeCards()) {
        if (c && !allCreatedCards.contains(c)) {
          allCreatedCards.append(c);
          drainedKeys.append(c->dedupKey());
        }
      }
    }

    QCOMPARE(mgr.activeCardCount(), 0);
    QCOMPARE(mgr.pendingQueueCount(), 0);

    // Verify exactly 50 items were drained in strict FIFO order: drain.50 .. drain.99
    QCOMPARE(drainedKeys.size(), 50);
    for (int i = 0; i < 50; ++i) {
      const QString expectedKey = QStringLiteral("drain.%1").arg(50 + i);
      QCOMPARE(drainedKeys[i], expectedKey);
    }

    // Verify total cards created = 5 initial + 50 drained = 55
    QCOMPARE(allCreatedCards.size(), 55);

    // Process deferred deletion events
    QCoreApplication::sendPostedEvents(nullptr, QEvent::DeferredDelete);
    QCoreApplication::processEvents();

    // Verify ALL 55 QPointer instances evaluated to NULL (widgets cleanly destroyed)
    for (int i = 0; i < allCreatedCards.size(); ++i) {
      QVERIFY2(allCreatedCards[i].isNull(),
               qPrintable(QStringLiteral("Card widget index %1 leaked! Not null.").arg(i)));
    }

    // Verify overlay container layout has 0 widgets
    QWidget *overlay = mgr.overlayContainer();
    if (overlay && overlay->layout()) {
      QCOMPARE(overlay->layout()->count(), 0);
    }
  }
};

int main(int argc, char *argv[])
{
  if (qgetenv("QT_QPA_PLATFORM").isEmpty()) {
    qputenv("QT_QPA_PLATFORM", "offscreen");
  }
  QApplication app(argc, argv);
  ChallengerM5Harness tc;
  return QTest::qExec(&tc, argc, argv);
}

#include "tst_errorhub_challenger_m5_harness.moc"
