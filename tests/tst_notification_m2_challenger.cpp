#include <QtTest>
#include <QSignalSpy>
#include <QWidget>
#include <QApplication>
#include <QCoreApplication>
#include <QEnterEvent>
#include <QLabel>
#include <QToolButton>
#include <thread>
#include <vector>
#include <atomic>

#include "../src/services/errorhub.h"
#include "../src/ui/notifications/notificationcard.h"
#include "../src/ui/notifications/notificationmanager.h"

using namespace paleo::services;
using namespace paleo::ui;

class TestNotificationM2Challenger : public QObject
{
  Q_OBJECT

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

  // 1. 验证离线/无头模式下确认对话框辅助函数的返回值与行为
  void testOffscreenConfirmationHelpersBehavior()
  {
    QWidget parentWidget;

    // A. 默认 offscreen auto-answer = true
    NotificationManager::setOffscreenAutoAnswer(true);
    QVERIFY(NotificationManager::confirmOkCancel(nullptr, QStringLiteral("TitleOk"), QStringLiteral("MsgOk")));
    QVERIFY(NotificationManager::confirmYesNo(nullptr, QStringLiteral("TitleYes"), QStringLiteral("MsgYes")));
    QVERIFY(NotificationManager::confirmDestructive(nullptr, QStringLiteral("TitleDel"), QStringLiteral("MsgDel")));

    // 验证带 parent 也是相同结果
    QVERIFY(NotificationManager::confirmOkCancel(&parentWidget, QStringLiteral("TitleOk"), QStringLiteral("MsgOk")));
    QVERIFY(NotificationManager::confirmYesNo(&parentWidget, QStringLiteral("TitleYes"), QStringLiteral("MsgYes")));
    QVERIFY(NotificationManager::confirmDestructive(&parentWidget, QStringLiteral("TitleDel"), QStringLiteral("MsgDel")));

    // B. offscreen auto-answer = false
    NotificationManager::setOffscreenAutoAnswer(false);
    QVERIFY(!NotificationManager::confirmOkCancel(nullptr, QStringLiteral("TitleOk"), QStringLiteral("MsgOk")));
    QVERIFY(!NotificationManager::confirmYesNo(nullptr, QStringLiteral("TitleYes"), QStringLiteral("MsgYes")));
    QVERIFY(!NotificationManager::confirmDestructive(nullptr, QStringLiteral("TitleDel"), QStringLiteral("MsgDel")));
    QVERIFY(!NotificationManager::confirmOkCancel(&parentWidget, QStringLiteral("TitleOk"), QStringLiteral("MsgOk")));

    // C. 验证测试 Hook 的优先拦截与 fallback 逻辑
    NotificationManager::setConfirmHookForTesting([](const QString &title, const QString &text) -> std::optional<bool> {
      Q_UNUSED(text);
      if (title == QStringLiteral("ForceAccept")) return true;
      if (title == QStringLiteral("ForceReject")) return false;
      return std::nullopt; // fallback
    });

    // 此时 auto-answer 依然为 false
    QVERIFY(NotificationManager::confirmOkCancel(nullptr, QStringLiteral("ForceAccept"), QStringLiteral("T")));
    QVERIFY(!NotificationManager::confirmOkCancel(nullptr, QStringLiteral("ForceReject"), QStringLiteral("T")));
    // fallback 到 false
    QVERIFY(!NotificationManager::confirmOkCancel(nullptr, QStringLiteral("FallbackTitle"), QStringLiteral("T")));

    // 改为 auto-answer 为 true 时，fallback 到 true
    NotificationManager::setOffscreenAutoAnswer(true);
    QVERIFY(NotificationManager::confirmOkCancel(nullptr, QStringLiteral("FallbackTitle"), QStringLiteral("T")));

    NotificationManager::setConfirmHookForTesting(nullptr);
  }

  // 2. 验证 errorAggregated 信号准确更新已有卡片角标与倒计时，且绝不增加新卡片
  void testErrorAggregatedSignalUpdatesBadgeAndResetsTimer()
  {
    QWidget mainWindow;
    mainWindow.resize(1000, 700);
    mainWindow.show();
    NotificationManager mgr(&mainWindow, paleo::services::ErrorHub::instance());

    const QString dedupKey = QStringLiteral("challenge.agg.key");

    // 1. 首次上报
    paleo::services::ErrorHub::instance()->reportWarning(ErrorDomain::General, QStringLiteral("标准去重告警文案"),
                                         QStringLiteral("详情1"), dedupKey);
    QCoreApplication::processEvents();

    QCOMPARE(mgr.activeCardCount(), 1);
    NotificationCard *card = mgr.activeCards().first();
    QVERIFY(card != nullptr);
    QCOMPARE(card->aggregationCount(), 1);
    QCOMPARE(card->message(), QStringLiteral("标准去重告警文案"));

    auto *badgeLabel = card->findChild<QLabel *>(QStringLiteral("cardBadgeLabel"));
    QVERIFY(badgeLabel != nullptr);
    QVERIFY(!badgeLabel->isVisible()); // 仅 1 条时角标隐藏
    const int initialRemaining = card->remainingMs();
    QCOMPARE(initialRemaining, card->totalTimeoutMs());

    // 2. 模拟时间流逝（等待 120ms）
    QTest::qWait(120);
    QVERIFY(card->remainingMs() < initialRemaining);

    // 3. 再次上报相同 dedupKey -> paleo::services::ErrorHub 聚合触发 errorAggregated 信号
    paleo::services::ErrorHub::instance()->reportWarning(ErrorDomain::General, QStringLiteral("标准去重告警文案"),
                                         QStringLiteral("详情2"), dedupKey);
    QCoreApplication::processEvents();

    // 严格断言：卡片数量依然为 1，不增加新卡片
    QCOMPARE(mgr.activeCardCount(), 1);
    QCOMPARE(card->aggregationCount(), 2);
    QCOMPARE(card->message(), QStringLiteral("标准去重告警文案"));
    QVERIFY(badgeLabel->isVisible());
    QCOMPARE(badgeLabel->text(), QStringLiteral("×2"));
    // 严格断言：倒计时重置回 totalTimeoutMs
    QCOMPARE(card->remainingMs(), card->totalTimeoutMs());

    // 4. 第三次上报相同 dedupKey
    QTest::qWait(80);
    QVERIFY(card->remainingMs() < card->totalTimeoutMs());

    paleo::services::ErrorHub::instance()->reportWarning(ErrorDomain::General, QStringLiteral("标准去重告警文案"),
                                         QStringLiteral("详情3"), dedupKey);
    QCoreApplication::processEvents();

    QCOMPARE(mgr.activeCardCount(), 1);
    QCOMPARE(card->aggregationCount(), 3);
    QCOMPARE(badgeLabel->text(), QStringLiteral("×3"));
    QCOMPARE(card->remainingMs(), card->totalTimeoutMs());
  }

  // 3. 验证排队中的项目收到聚合信号时正确更新聚合计数与文案，且不增加排队项
  void testErrorAggregatedWhileInPendingQueue()
  {
    QWidget mainWindow;
    mainWindow.resize(1000, 700);
    mainWindow.show();
    NotificationManager mgr(&mainWindow, paleo::services::ErrorHub::instance());

    // 填满 5 张活跃卡片
    for (int i = 0; i < 5; ++i) {
      paleo::services::ErrorHub::instance()->reportInfo(ErrorDomain::General, QStringLiteral("Active %1").arg(i),
                                       QString(), QStringLiteral("active.%1").arg(i));
    }
    QCoreApplication::processEvents();
    QCOMPARE(mgr.activeCardCount(), 5);
    QCOMPARE(mgr.pendingQueueCount(), 0);

    const QString pendingKey = QStringLiteral("pending.agg.key");

    // 注入第 6 条（进入排队）
    paleo::services::ErrorHub::instance()->reportInfo(ErrorDomain::General, QStringLiteral("Queued Msg 1"),
                                     QString(), pendingKey);
    QCoreApplication::processEvents();
    QCOMPARE(mgr.activeCardCount(), 5);
    QCOMPARE(mgr.pendingQueueCount(), 1);

    // 再次上报相同 key，排队中更新
    paleo::services::ErrorHub::instance()->reportInfo(ErrorDomain::General, QStringLiteral("Queued Msg 1"),
                                     QString(), pendingKey);
    QCoreApplication::processEvents();
    // 排队项依然只有 1 项，没有重复入队
    QCOMPARE(mgr.pendingQueueCount(), 1);

    // 关闭其中一张活跃卡片，促使排队项出队晋升为活跃卡片
    NotificationCard *firstActive = mgr.activeCards().first();
    firstActive->dismiss();
    QCoreApplication::processEvents();

    QCOMPARE(mgr.activeCardCount(), 5);
    QCOMPARE(mgr.pendingQueueCount(), 0);

    // 找到刚刚晋升的卡片
    NotificationCard *promotedCard = nullptr;
    for (auto *c : mgr.activeCards()) {
      if (c->dedupKey() == pendingKey) {
        promotedCard = c;
        break;
      }
    }
    QVERIFY(promotedCard != nullptr);
    QCOMPARE(promotedCard->aggregationCount(), 2);
    QCOMPARE(promotedCard->message(), QStringLiteral("Queued Msg 1"));
    auto *badge = promotedCard->findChild<QLabel *>(QStringLiteral("cardBadgeLabel"));
    QVERIFY(badge != nullptr);
    QVERIFY(badge->isVisible());
    QCOMPARE(badge->text(), QStringLiteral("×2"));
  }

  // 4. 多线程压力与跨线程信号派发验证（从工作线程并发上报错误）
  void testConcurrentErrorReportsFromWorkerThreads()
  {
    QWidget mainWindow;
    mainWindow.resize(1000, 700);
    NotificationManager mgr(&mainWindow, paleo::services::ErrorHub::instance());

    constexpr int kNumThreads = 6;
    constexpr int kReportsPerThread = 20;
    std::atomic<int> startBarrier{0};
    std::vector<std::thread> workers;

    for (int t = 0; t < kNumThreads; ++t) {
      workers.emplace_back([t, &startBarrier]() {
        startBarrier.fetch_add(1);
        while (startBarrier.load() < kNumThreads) {
          std::this_thread::yield();
        }
        for (int i = 0; i < kReportsPerThread; ++i) {
          // 混合上报：部分共享 key 测试并发聚合，部分独立 key 测试并发排队
          if (i % 2 == 0) {
            paleo::services::ErrorHub::postWarning(ErrorDomain::IO,
                                  QStringLiteral("Worker shared error"),
                                  QStringLiteral("details from thread %1").arg(t),
                                  QStringLiteral("worker.shared.key"));
          } else {
            paleo::services::ErrorHub::postError(ErrorDomain::IO,
                                QStringLiteral("Worker unique error %1-%2").arg(t).arg(i),
                                QString(),
                                QStringLiteral("worker.unique.%1.%2").arg(t).arg(i));
          }
          std::this_thread::sleep_for(std::chrono::milliseconds(2));
        }
      });
    }

    // 等待所有工作线程完成
    for (auto &w : workers) {
      if (w.joinable()) {
        w.join();
      }
    }

    // 主线程消费所有排队的跨线程 Qt 事件
    QCoreApplication::processEvents();

    // 验证状态机正常运转，无崩溃、无越界
    QVERIFY(mgr.activeCardCount() <= NotificationManager::kMaxVisibleCards);
    QVERIFY(mgr.pendingQueueCount() <= NotificationManager::kMaxPendingQueue);
    QVERIFY(mgr.activeCardCount() > 0);

    // 清空管理器并确认资源释放
    mgr.clearAll();
    QCOMPARE(mgr.activeCardCount(), 0);
    QCOMPARE(mgr.pendingQueueCount(), 0);
  }

  // 5. 模态弹窗 60s 去重抑制完整状态流（首弹窗 -> 60s 内抑制并转为卡片 -> 卡片再聚合）
  void testModalSuppressionAndCardAggregationFlow()
  {
    QWidget mainWindow;
    mainWindow.resize(1000, 700);
    mainWindow.show();
    NotificationManager mgr(&mainWindow, paleo::services::ErrorHub::instance());

    const QString modalKey = QStringLiteral("modal.flow.key");
    ErrorEntry modalEntry;
    modalEntry.level = ErrorLevel::Critical;
    modalEntry.domain = ErrorDomain::Project;
    modalEntry.message = QStringLiteral("致命工程解析异常");
    modalEntry.details = QStringLiteral("堆栈上下文");
    modalEntry.deduplicationKey = modalKey;
    modalEntry.isModal = true;

    // 1. 首次触发模态错误
    QVERIFY(!mgr.shouldSuppressModal(modalKey));
    paleo::services::ErrorHub::instance()->report(modalEntry);
    QCoreApplication::processEvents();

    // 首次发生时记录了弹窗时间戳，但因为是模态弹窗，不会直接生成非模态浮动卡片
    QVERIFY(mgr.shouldSuppressModal(modalKey));
    QCOMPARE(mgr.activeCardCount(), 0);

    // 2. 60s 内第二次触发相同模态错误：被抑制，转为非模态卡片呈现！
    paleo::services::ErrorHub::instance()->report(modalEntry);
    QCoreApplication::processEvents();

    QCOMPARE(mgr.activeCardCount(), 1);
    NotificationCard *suppressedCard = mgr.activeCards().first();
    QVERIFY(suppressedCard != nullptr);
    QCOMPARE(suppressedCard->dedupKey(), modalKey);
    QVERIFY(suppressedCard->title().contains(QStringLiteral("聚合抑制")));
    // 第二次发生时（首次转为卡片），卡片初始化计数为 1
    QCOMPARE(suppressedCard->aggregationCount(), 1);

    // 3. 60s 内第三次触发相同模态错误：命中 errorAggregated，卡片角标更新为 paleo::services::ErrorHub 总计数 3
    paleo::services::ErrorHub::instance()->report(modalEntry);
    QCoreApplication::processEvents();

    QCOMPARE(mgr.activeCardCount(), 1);
    QCOMPARE(suppressedCard->aggregationCount(), 3);
    auto *badge = suppressedCard->findChild<QLabel *>(QStringLiteral("cardBadgeLabel"));
    QVERIFY(badge != nullptr);
    QVERIFY(badge->isVisible());
    QCOMPARE(badge->text(), QStringLiteral("×3"));
  }

  // 6. 错误风暴压力测试（短时间注入大量错误，验证 FIFO 逐出与主线程事件循环不被锁死）
  void testErrorStormFifoBounds()
  {
    QWidget mainWindow;
    mainWindow.resize(1000, 700);
    NotificationManager mgr(&mainWindow, paleo::services::ErrorHub::instance());

    // 高频连续注入 200 条不同 key 的错误
    for (int i = 0; i < 200; ++i) {
      paleo::services::ErrorHub::instance()->reportWarning(ErrorDomain::General,
                                           QStringLiteral("Storm msg %1").arg(i),
                                           QString(),
                                           QStringLiteral("storm.key.%1").arg(i));
    }
    QCoreApplication::processEvents();

    // 活跃卡片严格受限于 5
    QCOMPARE(mgr.activeCardCount(), NotificationManager::kMaxVisibleCards);
    // 等待队列严格受限于 50
    QCOMPARE(mgr.pendingQueueCount(), NotificationManager::kMaxPendingQueue);

    // 测试批量清理
    mgr.clearAll();
    QCOMPARE(mgr.activeCardCount(), 0);
    QCOMPARE(mgr.pendingQueueCount(), 0);
  }

  // 7. 直接调用 showNotification 时的动态文案更新与角标
  void testDirectNotificationAggregation()
  {
    QWidget mainWindow;
    mainWindow.resize(1000, 700);
    NotificationManager mgr(&mainWindow);

    const QString directKey = QStringLiteral("direct.key");
    mgr.showNotification(ErrorLevel::Warning, QStringLiteral("Title"),
                         QStringLiteral("Msg 1"), QString(), directKey);
    QCOMPARE(mgr.activeCardCount(), 1);
    auto *card = mgr.activeCards().first();
    QCOMPARE(card->message(), QStringLiteral("Msg 1"));
    QCOMPARE(card->aggregationCount(), 1);

    // 直接调用 showNotification 支持更新最新文案
    mgr.showNotification(ErrorLevel::Warning, QStringLiteral("Title"),
                         QStringLiteral("Msg 2 updated"), QString(), directKey);
    QCOMPARE(mgr.activeCardCount(), 1);
    QCOMPARE(card->message(), QStringLiteral("Msg 2 updated"));
    QCOMPARE(card->aggregationCount(), 2);
  }

  // 8. 窗口销毁与生命周期安全
  void testDestructionSafety()
  {
    auto *parentWin = new QWidget();
    parentWin->resize(800, 600);
    auto *mgr = new NotificationManager(parentWin, paleo::services::ErrorHub::instance());

    // 注入多张卡片
    for (int i = 0; i < 5; ++i) {
      mgr->showNotification(ErrorLevel::Info, QStringLiteral("T%1").arg(i), QStringLiteral("M"));
    }
    QCOMPARE(mgr->activeCardCount(), 5);

    // 销毁主窗口，应安全析构 overlayContainer 与 cards，无双重释放
    delete parentWin;
  }

  // 9. 0 超时卡片边界行为（持久卡片不自动超时消失，但可被手动关闭或聚合）
  void testCardZeroTimeoutBehavior()
  {
    NotificationCard card(QStringLiteral("zero.timeout.key"), ErrorLevel::Info,
                          QStringLiteral("Zero Timeout Title"), QStringLiteral("Zero Timeout Msg"),
                          QString(), 0);
    QCOMPARE(card.totalTimeoutMs(), 0);
    QCOMPARE(card.remainingMs(), 0);
    QSignalSpy spy(&card, &NotificationCard::dismissed);
    card.show();

    // 等待 150ms，验证绝不自动发出 dismissed 信号
    QTest::qWait(150);
    QCOMPARE(spy.count(), 0);

    // 悬停进出事件不崩溃且不启动非预期的倒计时
    QEnterEvent enterEv(QPointF(10, 10), QPointF(10, 10), QPointF(10, 10));
    QCoreApplication::sendEvent(&card, &enterEv);
    QVERIFY(card.isHovered());

    QEvent leaveEv(QEvent::Leave);
    QCoreApplication::sendEvent(&card, &leaveEv);
    QVERIFY(!card.isHovered());

    QTest::qWait(100);
    QCOMPARE(spy.count(), 0);

    // 点击关闭按钮可正常关闭
    auto *closeBtn = card.findChild<QToolButton *>(QStringLiteral("cardCloseButton"));
    QVERIFY(closeBtn != nullptr);
    closeBtn->click();
    QCOMPARE(spy.count(), 1);
  }

  // 10. 高频快速创建与销毁压力测试（验证无野指针与双重析构）
  void testRapidCreationAndDestructionStress()
  {
    constexpr int kIterations = 300;
    for (int i = 0; i < kIterations; ++i) {
      auto *card = new NotificationCard(QStringLiteral("rapid.card.%1").arg(i),
                                        (i % 2 == 0) ? ErrorLevel::Info : ErrorLevel::Error,
                                        QStringLiteral("Title %1").arg(i),
                                        QStringLiteral("Message %1").arg(i),
                                        QString(),
                                        (i % 3 == 0) ? 0 : 5000);
      card->show();
      if (i % 2 == 0) {
        card->dismiss();
      }
      delete card;
    }

    for (int i = 0; i < 50; ++i) {
      QWidget win;
      win.resize(600, 400);
      auto *mgr = new NotificationManager(&win);
      for (int j = 0; j < 10; ++j) {
        mgr->showNotification(ErrorLevel::Warning, QStringLiteral("W"), QStringLiteral("M"));
      }
      mgr->clearAll();
      delete mgr;
    }
  }

  // 11. 悬停状态高速往复切换压力测试（验证定时器不乱序、不冻结）
  void testHoverToggleRapidCyclingStress()
  {
    NotificationCard card(QStringLiteral("hover.cycle.key"), ErrorLevel::Warning,
                          QStringLiteral("Hover Title"), QStringLiteral("Hover Msg"),
                          QString(), 1000);
    card.show();

    QEnterEvent enterEv(QPointF(10, 10), QPointF(10, 10), QPointF(10, 10));
    QEvent leaveEv(QEvent::Leave);

    // 快速切换 200 次
    for (int i = 0; i < 200; ++i) {
      QCoreApplication::sendEvent(&card, &enterEv);
      QCoreApplication::sendEvent(&card, &leaveEv);
    }
    QVERIFY(!card.isHovered());

    // 确认 leave 后定时器仍在正常扣减
    const int remBefore = card.remainingMs();
    QTest::qWait(120);
    const int remAfter = card.remainingMs();
    QVERIFY(remAfter < remBefore);
  }

  // 12. 队列容量严格边界与 FIFO 逐出全链路顺序验证 (49, 50, 51 边界及出队序)
  void testPendingQueueCapacityBoundary49_50_51_DrainOrder()
  {
    QWidget mainWindow;
    mainWindow.resize(1000, 700);
    NotificationManager mgr(&mainWindow);

    // 1. 占满 5 张活跃卡片
    for (int i = 0; i < 5; ++i) {
      mgr.showNotification(ErrorLevel::Info, QStringLiteral("Active %1").arg(i),
                            QStringLiteral("M"), QString(), QStringLiteral("active.%1").arg(i));
    }
    QCOMPARE(mgr.activeCardCount(), 5);
    QCOMPARE(mgr.pendingQueueCount(), 0);

    // 2. 依次入队 49 项
    for (int i = 0; i < 49; ++i) {
      mgr.showNotification(ErrorLevel::Info, QStringLiteral("P%1").arg(i),
                            QStringLiteral("Msg %1").arg(i), QString(),
                            QStringLiteral("p.%1").arg(i));
    }
    QCOMPARE(mgr.pendingQueueCount(), 49);

    // 3. 入队第 50 项 -> 达到上限
    mgr.showNotification(ErrorLevel::Info, QStringLiteral("P49"),
                          QStringLiteral("Msg 49"), QString(), QStringLiteral("p.49"));
    QCOMPARE(mgr.pendingQueueCount(), 50);

    // 4. 入队第 51 项 -> 触发 FIFO 逐出，队列长度保持 50 (最老的 p.0 被逐出)
    mgr.showNotification(ErrorLevel::Info, QStringLiteral("P50"),
                          QStringLiteral("Msg 50"), QString(), QStringLiteral("p.50"));
    QCOMPARE(mgr.pendingQueueCount(), 50);

    // 5. 入队第 52 项 -> p.1 被逐出，队列保持 50
    mgr.showNotification(ErrorLevel::Info, QStringLiteral("P51"),
                          QStringLiteral("Msg 51"), QString(), QStringLiteral("p.51"));
    QCOMPARE(mgr.pendingQueueCount(), 50);

    // 6. 依次关闭活跃卡片，验证晋升出的卡片严格按 FIFO 顺序 (从 p.2 开始，直到 p.51)
    // 当前队列中应包含 p.2 ~ p.51 共 50 项
    for (int expectedIdx = 2; expectedIdx <= 51; ++expectedIdx) {
      // 必须有 5 张活跃卡片
      QCOMPARE(mgr.activeCardCount(), 5);
      // 关闭其中一张最早的活跃卡片
      NotificationCard *toDismiss = mgr.activeCards().first();
      toDismiss->dismiss();
      QCoreApplication::processEvents();

      // 检查最新晋升到末尾的卡片 dedupKey 是否为期望的 p.<expectedIdx>
      NotificationCard *latestPromoted = mgr.activeCards().last();
      QCOMPARE(latestPromoted->dedupKey(), QStringLiteral("p.%1").arg(expectedIdx));
    }

    // 此时 50 项排队已全部晋升完毕，队列应为空
    QCOMPARE(mgr.pendingQueueCount(), 0);
    QCOMPARE(mgr.activeCardCount(), 5);
  }

  // 13. 模态弹窗 60s 去重严格边界验证（空 key、跨域隔离、时间窗临界）
  void testModalDeduplicationStrict60sBoundary()
  {
    NotificationManager mgr;

    // 空 key 永远不应被抑制
    QVERIFY(!mgr.shouldSuppressModal(QString()));
    mgr.recordModalShown(QString());
    QVERIFY(!mgr.shouldSuppressModal(QString()));

    // 独立 key 互不干扰
    const QString keyA = QStringLiteral("modal.key.A");
    const QString keyB = QStringLiteral("modal.key.B");
    mgr.recordModalShown(keyA);
    QVERIFY(mgr.shouldSuppressModal(keyA));
    QVERIFY(!mgr.shouldSuppressModal(keyB));

    // 自定义窗口精准边界测试
    mgr.setModalDedupWindowMs(100);
    QCOMPARE(mgr.modalDedupWindowMs(), 100);

    const QString timedKey = QStringLiteral("modal.timed.key");
    mgr.recordModalShown(timedKey);
    QVERIFY(mgr.shouldSuppressModal(timedKey));

    // 等待 110ms，跨过 100ms 窗口
    QTest::qWait(110);
    QVERIFY(!mgr.shouldSuppressModal(timedKey));
  }

  // 14. 极高频事件注入与队列排空极限压力测试（1000 条混合请求）
  void testStormHighRateNotificationStress()
  {
    QWidget mainWindow;
    mainWindow.resize(1000, 700);
    NotificationManager mgr(&mainWindow, paleo::services::ErrorHub::instance());

    constexpr int kTotalMessages = 1000;
    for (int i = 0; i < kTotalMessages; ++i) {
      if (i % 5 == 0) {
        // 重复聚合 key
        mgr.showNotification(ErrorLevel::Warning, QStringLiteral("Storm Shared"),
                             QStringLiteral("Shared %1").arg(i), QString(),
                             QStringLiteral("storm.shared"));
      } else {
        // 独立 key
        mgr.showNotification(ErrorLevel::Info, QStringLiteral("Storm Unique %1").arg(i),
                             QStringLiteral("Msg %1").arg(i), QString(),
                             QStringLiteral("storm.unique.%1").arg(i));
      }
    }
    QCoreApplication::processEvents();

    QCOMPARE(mgr.activeCardCount(), NotificationManager::kMaxVisibleCards);
    QCOMPARE(mgr.pendingQueueCount(), NotificationManager::kMaxPendingQueue);

    // 循环排空
    while (mgr.activeCardCount() > 0 || mgr.pendingQueueCount() > 0) {
      auto cards = mgr.activeCards();
      for (auto *c : cards) {
        if (c) c->dismiss();
      }
      QCoreApplication::processEvents();
    }

    QCOMPARE(mgr.activeCardCount(), 0);
    QCOMPARE(mgr.pendingQueueCount(), 0);
  }
};

int main(int argc, char *argv[])
{
  if (qgetenv("QT_QPA_PLATFORM").isEmpty()) {
    qputenv("QT_QPA_PLATFORM", "offscreen");
  }
  QApplication app(argc, argv);
  TestNotificationM2Challenger tc;
  return QTest::qExec(&tc, argc, argv);
}

#include "tst_notification_m2_challenger.moc"
