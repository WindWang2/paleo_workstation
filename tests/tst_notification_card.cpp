#include <QtTest>
#include <QSignalSpy>
#include <QWidget>
#include <QApplication>
#include <QCoreApplication>
#include <QEnterEvent>
#include <QToolButton>

#include "../src/services/errorhub.h"
#include "../src/ui/notifications/notificationcard.h"
#include "../src/ui/notifications/notificationmanager.h"

using namespace paleo::services;
using namespace paleo::ui;

class TestNotificationCard : public QObject
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
    if (ErrorHub::instance()) {
      ErrorHub::instance()->clear();
    }
  }

  void cleanup()
  {
    if (NotificationManager::instance()) {
      NotificationManager::instance()->clearAll();
    }
    NotificationManager::setConfirmHookForTesting(nullptr);
    NotificationManager::setOffscreenAutoAnswer(true);
  }

  // 1. 卡片基础属性与创建
  void testCardCreationAndProperties()
  {
    NotificationCard card(QStringLiteral("test.key"), ErrorLevel::Warning,
                          QStringLiteral("警告标题"), QStringLiteral("详细错误文本"),
                          QStringLiteral("堆栈或上下文"), 5000);
    QCOMPARE(card.dedupKey(), QStringLiteral("test.key"));
    QCOMPARE(card.level(), ErrorLevel::Warning);
    QCOMPARE(card.title(), QStringLiteral("警告标题"));
    QCOMPARE(card.message(), QStringLiteral("详细错误文本"));
    QCOMPARE(card.detail(), QStringLiteral("堆栈或上下文"));
    QCOMPARE(card.aggregationCount(), 1);
    QCOMPARE(card.totalTimeoutMs(), 5000);
    QCOMPARE(card.remainingMs(), 5000);
    QCOMPARE(card.remainingTimeoutMs(), 5000);
    QVERIFY(!card.isHovered());
  }

  // 2. 超时自动消失信号
  void testCardAutoDismissTimeout()
  {
    NotificationCard card(QStringLiteral("k1"), ErrorLevel::Info,
                          QStringLiteral("T"), QStringLiteral("M"),
                          QString(), 100);
    QSignalSpy spy(&card, &NotificationCard::dismissed);
    card.show();

    // 等待触发超时 onTick
    QTRY_COMPARE_WITH_TIMEOUT(spy.count(), 1, 500);
  }

  // 3. 鼠标悬停暂停倒计时
  void testCardHoverPausesCountdown()
  {
    NotificationCard card(QStringLiteral("k2"), ErrorLevel::Info,
                          QStringLiteral("T"), QStringLiteral("M"),
                          QString(), 100);
    QSignalSpy spy(&card, &NotificationCard::dismissed);
    card.show();

    // 模拟 Hover 移入
    QEnterEvent enterEv(QPointF(10, 10), QPointF(10, 10), QPointF(10, 10));
    QCoreApplication::sendEvent(&card, &enterEv);
    QVERIFY(card.isHovered());

    // 等待超过原定超时时间
    QTest::qWait(150);
    QCOMPARE(spy.count(), 0); // 必须被暂停，未被触发

    // 模拟 Leave 移出
    QEvent leaveEv(QEvent::Leave);
    QCoreApplication::sendEvent(&card, &leaveEv);
    QVERIFY(!card.isHovered());

    // 恢复倒计时后应触发消失
    QTRY_COMPARE_WITH_TIMEOUT(spy.count(), 1, 500);
  }

  // 4. 同键聚合动态更新
  void testCardAggregationUpdate()
  {
    NotificationCard card(QStringLiteral("k_agg"), ErrorLevel::Error,
                          QStringLiteral("Title"), QStringLiteral("Old Msg"),
                          QString(), 5000);
    card.updateAggregation(3, QStringLiteral("New Msg"));
    QCOMPARE(card.aggregationCount(), 3);
    QCOMPARE(card.message(), QStringLiteral("New Msg"));
    QCOMPARE(card.remainingMs(), 5000);
  }

  // 5. 悬浮容器右下角锚定与重排
  void testOverlayPositionAndResize()
  {
    QWidget mainWindow;
    mainWindow.resize(1000, 700);
    mainWindow.show();

    NotificationManager mgr(&mainWindow);
    mgr.showNotification(ErrorLevel::Info, QStringLiteral("T"), QStringLiteral("M"));
    QCoreApplication::processEvents();

    QWidget *overlay = mgr.overlayContainer();
    QVERIFY(overlay != nullptr);
    QVERIFY(overlay->isVisible());

    // 验证右下角坐标
    const int expectedX = mainWindow.width() - 360 - 16;
    QCOMPARE(overlay->x(), expectedX);

    // 缩放窗口测试响应式跟随
    mainWindow.resize(1200, 800);
    QCoreApplication::processEvents();
    const int newExpectedX = mainWindow.width() - 360 - 16;
    QCOMPARE(overlay->x(), newExpectedX);
  }

  // 6. 最大并发可见卡片上限 (kMaxVisibleCards = 5)
  void testMaxVisibleCardsLimit()
  {
    QWidget mainWindow;
    mainWindow.resize(1000, 700);
    NotificationManager mgr(&mainWindow);

    // 注入 5 张卡片
    for (int i = 0; i < 5; ++i) {
      mgr.showNotification(ErrorLevel::Warning, QStringLiteral("T%1").arg(i),
                            QStringLiteral("M%1").arg(i), QString(),
                            QStringLiteral("key_%1").arg(i));
    }
    QCOMPARE(mgr.activeCardCount(), 5);
    QCOMPARE(mgr.pendingQueueCount(), 0);

    // 注入第 6 张卡片，应进入等待队列
    mgr.showNotification(ErrorLevel::Warning, QStringLiteral("T5"),
                          QStringLiteral("M5"), QString(), QStringLiteral("key_5"));
    QCOMPARE(mgr.activeCardCount(), 5);
    QCOMPARE(mgr.pendingQueueCount(), 1);
  }

  // 7. 排队上限与 FIFO 逐出 (kMaxPendingQueue = 50)
  void testPendingQueueFifoEviction()
  {
    QWidget mainWindow;
    mainWindow.resize(1000, 700);
    NotificationManager mgr(&mainWindow);

    // 填满 5 张活跃卡片
    for (int i = 0; i < 5; ++i) {
      mgr.showNotification(ErrorLevel::Info, QStringLiteral("A%1").arg(i), QStringLiteral("M"),
                            QString(), QStringLiteral("act_%1").arg(i));
    }

    // 填满 50 条排队
    for (int i = 0; i < 50; ++i) {
      mgr.showNotification(ErrorLevel::Info, QStringLiteral("Q%1").arg(i), QStringLiteral("M"),
                            QString(), QStringLiteral("q_%1").arg(i));
    }
    QCOMPARE(mgr.pendingQueueCount(), 50);

    // 注入第 51 条排队：FIFO 逐出最老的一条，队列保持 50
    mgr.showNotification(ErrorLevel::Info, QStringLiteral("Q50"), QStringLiteral("M"),
                          QString(), QStringLiteral("q_50"));
    QCOMPARE(mgr.pendingQueueCount(), 50);
  }

  // 8. 卡片消失自动流转出队
  void testQueueDrainOnCardDismissed()
  {
    QWidget mainWindow;
    mainWindow.resize(1000, 700);
    NotificationManager mgr(&mainWindow);

    for (int i = 0; i < 5; ++i) {
      mgr.showNotification(ErrorLevel::Info, QStringLiteral("A%1").arg(i), QStringLiteral("M"),
                            QString(), QStringLiteral("act_%1").arg(i), 50); // 50ms 超时
    }
    mgr.showNotification(ErrorLevel::Info, QStringLiteral("Pending1"), QStringLiteral("M"),
                          QString(), QStringLiteral("p1"), 5000);
    QCOMPARE(mgr.pendingQueueCount(), 1);

    // 等待活跃卡片超时消失
    QTRY_COMPARE_WITH_TIMEOUT(mgr.pendingQueueCount(), 0, 1000);
  }

  // 9. 活跃卡片同键去重聚合
  void testActiveCardDedupAggregation()
  {
    QWidget mainWindow;
    mainWindow.resize(1000, 700);
    NotificationManager mgr(&mainWindow);

    mgr.showNotification(ErrorLevel::Warning, QStringLiteral("T"), QStringLiteral("M1"),
                          QString(), QStringLiteral("same_key"));
    QCOMPARE(mgr.activeCardCount(), 1);

    // 再次上报相同 key，卡片不增加，仅累加计数
    mgr.showNotification(ErrorLevel::Warning, QStringLiteral("T"), QStringLiteral("M2"),
                          QString(), QStringLiteral("same_key"));
    QCOMPARE(mgr.activeCardCount(), 1);
  }

  // 10. 模态弹窗 60s 去重抑制窗口
  void testModalSuppression60sWindow()
  {
    NotificationManager mgr;
    const QString key = QStringLiteral("modal.test.key");

    // 首次不应被抑制
    QVERIFY(!mgr.shouldSuppressModal(key));

    // 记录弹窗展示
    mgr.recordModalShown(key);

    // 窗口期内应被抑制
    QVERIFY(mgr.shouldSuppressModal(key));

    // 不同 key 不受影响
    QVERIFY(!mgr.shouldSuppressModal(QStringLiteral("other.key")));

    // 自定义窗口缩短至 50ms 验证超时失效
    mgr.setModalDedupWindowMs(50);
    QTest::qWait(60);
    QVERIFY(!mgr.shouldSuppressModal(key)); // 已过期，恢复允许弹窗
  }

  // 11. 模态确认辅助函数无头自动应答与探针覆盖
  void testModalConfirmationHelpersOffscreen()
  {
    // 默认 offscreen 自动返回 true
    NotificationManager::setOffscreenAutoAnswer(true);
    QVERIFY(NotificationManager::confirmOkCancel(nullptr, QStringLiteral("Title"), QStringLiteral("Text")));
    QVERIFY(NotificationManager::confirmYesNo(nullptr, QStringLiteral("Title"), QStringLiteral("Text")));
    QVERIFY(NotificationManager::confirmDestructive(nullptr, QStringLiteral("Title"), QStringLiteral("Text")));

    // 探针设为 false 覆盖拒绝路径
    NotificationManager::setOffscreenAutoAnswer(false);
    QVERIFY(!NotificationManager::confirmOkCancel(nullptr, QStringLiteral("Title"), QStringLiteral("Text")));
    QVERIFY(!NotificationManager::confirmYesNo(nullptr, QStringLiteral("Title"), QStringLiteral("Text")));
    QVERIFY(!NotificationManager::confirmDestructive(nullptr, QStringLiteral("Title"), QStringLiteral("Text")));

    // 复原
    NotificationManager::setOffscreenAutoAnswer(true);
  }

  // 12. 静态便捷方法分流
  void testStaticConvenienceMethods()
  {
    QWidget mainWindow;
    mainWindow.resize(1000, 700);
    NotificationManager mgr(&mainWindow);
    NotificationManager::setInstance(&mgr);

    NotificationManager::showInfo(&mainWindow, QStringLiteral("InfoTitle"), QStringLiteral("InfoMsg"));
    NotificationManager::showWarning(&mainWindow, QStringLiteral("WarnTitle"), QStringLiteral("WarnMsg"));
    NotificationManager::showError(&mainWindow, QStringLiteral("ErrTitle"), QStringLiteral("ErrMsg"));

    QCOMPARE(mgr.activeCardCount(), 3);
    NotificationManager::setInstance(nullptr);
  }

  // 13. ErrorHub 信号全链路订阅与聚合联动
  void testErrorHubSignalSubscription()
  {
    QWidget mainWindow;
    mainWindow.resize(1000, 700);
    NotificationManager mgr(&mainWindow);

    ErrorHub hub;
    connect(&hub, &ErrorHub::errorRaised, &mgr, &NotificationManager::onErrorRaised);
    connect(&hub, &ErrorHub::errorAggregated, &mgr, &NotificationManager::onErrorAggregated);

    // 1. 上报普通错误 -> 触发 errorRaised -> 弹出通知卡片
    hub.reportWarning(ErrorDomain::IO, QStringLiteral("文件载入失败"), QString(), QStringLiteral("io.read"));
    QCOMPARE(mgr.activeCardCount(), 1);

    // 2. 60s 内再次上报 -> ErrorHub 聚合 -> 触发 errorAggregated -> 卡片不新增
    hub.reportWarning(ErrorDomain::IO, QStringLiteral("文件载入再次失败"), QString(), QStringLiteral("io.read"));
    QCOMPARE(mgr.activeCardCount(), 1);

    // 3. 上报致命/模态错误 -> 受到 60s 去重抑制保护
    ErrorEntry crit;
    crit.level = ErrorLevel::Critical;
    crit.domain = ErrorDomain::Project;
    crit.message = QStringLiteral("工程损坏");
    crit.deduplicationKey = QStringLiteral("proj.corrupt");
    crit.isModal = true;

    // 首次允许
    hub.report(crit);
    // 第二次被抑制为非模态卡片
    hub.report(crit);
    QVERIFY(mgr.shouldSuppressModal(QStringLiteral("proj.corrupt")));
  }

  // 14. 卡片手动关闭按钮点击
  void testCardCloseButtonClick()
  {
    NotificationCard card(QStringLiteral("k_close"), ErrorLevel::Info,
                          QStringLiteral("Info"), QStringLiteral("Close test"),
                          QString(), 5000);

    QSignalSpy spy(&card, &NotificationCard::dismissed);
    auto *closeBtn = card.findChild<QToolButton *>(QStringLiteral("cardCloseButton"));
    QVERIFY(closeBtn != nullptr);
    closeBtn->click();
    QCOMPARE(spy.count(), 1);
  }

  // 15. 确认 Hook 拦截
  void testConfirmationHookCustomAnswer()
  {
    NotificationManager::setConfirmHookForTesting([](const QString &title, const QString &text) -> std::optional<bool> {
      if (title == QStringLiteral("HookReject"))
        return false;
      return true;
    });

    QVERIFY(NotificationManager::confirmOkCancel(nullptr, QStringLiteral("HookAccept"), QStringLiteral("T")));
    QVERIFY(!NotificationManager::confirmOkCancel(nullptr, QStringLiteral("HookReject"), QStringLiteral("T")));
    NotificationManager::setConfirmHookForTesting(nullptr);
  }
};

int main(int argc, char *argv[])
{
  if (qgetenv("QT_QPA_PLATFORM").isEmpty()) {
    qputenv("QT_QPA_PLATFORM", "offscreen");
  }
  QApplication app(argc, argv);
  TestNotificationCard tc;
  return QTest::qExec(&tc, argc, argv);
}

#include "tst_notification_card.moc"
