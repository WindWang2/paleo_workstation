// 方向64：错误呈现层 offscreen 测试——通知卡出现/到时消失/堆叠上限/同键不重弹、
// severe 模态同键 60s 单弹、info 走状态栏、PaleoNotify 回落/入账分流、
// 错误风暴（100 条/秒 × 10s）主线程不冻结（事件处理计数断言，无墙钟毫秒断言）
// + 通知路径零新增控件分配（卡片池计数）。
#include <QtTest>
#include <QApplication>
#include <QClipboard>
#include <QPushButton>
#include <QTableWidget>
#include <QLabel>
#include <QMainWindow>
#include <QPushButton>
#include <QMessageBox>
#include <QStatusBar>
#include <QThread>
#include <QTimer>

#include "../src/services/errorhub.h"
#include "../src/ui/notifications/errorhistorypanel.h"
#include "../src/ui/notifications/notificationcenter.h"
#include "../src/ui/notifications/paleonotify.h"

namespace
{
qint64 g_now = 0;

struct Rig
{
    QMainWindow win;
    ErrorHub hub;
    NotificationCenter *center = nullptr;
    Rig()
    {
        g_now = 5'000'000;
        hub.setClockForTest([] { return g_now; });
        win.resize(1200, 800);
        center = new NotificationCenter(&win, &hub);
        center->setStatusBar(win.statusBar());
        win.show();
        QVERIFY(QTest::qWaitForWindowExposed(&win) || true);
    }
};

int countSevereBoxes()
{
    int n = 0;
    for (QWidget *w : QApplication::topLevelWidgets())
        if (w->objectName() == QLatin1String("paleoSevereErrorBox") && w->isVisible())
            ++n;
    return n;
}

void closeSevereBoxes()
{
    for (QWidget *w : QApplication::topLevelWidgets())
        if (auto *mb = qobject_cast<QMessageBox *>(w))
            mb->close();
    QCoreApplication::processEvents();
}
} // namespace

class TestNotifications : public QObject
{
    Q_OBJECT

private slots:
    void cardAppearsAndExpires();
    void stackCapEvictsOldest();
    void sameKeyBumpsNoNewCard();
    void sameKeyAfterExpiryInWindowNotReshown();
    void severeModalOncePerWindow();
    void infoGoesToStatusBar();
    void cardsAnchoredBottomRight();
    void paleoNotifyFallsBackWithoutPresenter();
    void paleoNotifyRoutesThroughHub();
    void askReturnsAcceptButton();
    void stormDoesNotFreezeMainThread();
    void stormFromWorkerThreadDelivered();
    void historyFilterCopyClear();
    void historyRingEvictionVisible();
};

void TestNotifications::cardAppearsAndExpires()
{
    Rig r;
    r.center->setDismissMsForTest(50, 60, 80);
    r.hub.raise(ErrorHub::Level::Warning, "t", "标题", "出错了");
    QCOMPARE(r.center->visibleCardCount(), 1);
    QCOMPARE(r.center->visibleCardTexts(), QStringList{QStringLiteral("出错了")});
    QTRY_COMPARE_WITH_TIMEOUT(r.center->visibleCardCount(), 0, 5000);
    r.hub.raise(ErrorHub::Level::Error, "t", "标题", "错误卡");
    QCOMPARE(r.center->visibleCardCount(), 1);
    QTRY_COMPARE_WITH_TIMEOUT(r.center->visibleCardCount(), 0, 5000);
}

void TestNotifications::stackCapEvictsOldest()
{
    Rig r;
    for (int i = 0; i < NotificationCenter::kMaxCards + 2; ++i)
        r.hub.raise(ErrorHub::Level::Warning, "t", "T", QStringLiteral("m%1").arg(i));
    QCOMPARE(r.center->visibleCardCount(), NotificationCenter::kMaxCards);
    QCOMPARE(r.center->visibleCardTexts(),
             (QStringList{"m2", "m3", "m4", "m5"}));  // m0/m1 提前收起
    QCOMPARE(r.center->cardAllocations(), NotificationCenter::kMaxCards);
}

void TestNotifications::sameKeyBumpsNoNewCard()
{
    Rig r;
    for (int i = 0; i < 5; ++i)
        r.hub.raise(ErrorHub::Level::Warning, "t", "T", "same");
    QCOMPARE(r.center->visibleCardCount(), 1);
    auto *count = r.win.findChild<QLabel *>(QStringLiteral("toastCount"));
    bool sawX5 = false;
    for (QLabel *l : r.win.findChildren<QLabel *>(QStringLiteral("toastCount")))
        sawX5 = sawX5 || (l->isVisible() && l->text() == QStringLiteral("\u00d75"));
    QVERIFY(count);
    QVERIFY(sawX5);
}

void TestNotifications::sameKeyAfterExpiryInWindowNotReshown()
{
    Rig r;
    r.center->setDismissMsForTest(30, 30, 30);
    r.hub.raise(ErrorHub::Level::Warning, "t", "T", "same");
    QTRY_COMPARE_WITH_TIMEOUT(r.center->visibleCardCount(), 0, 5000);
    g_now += 10'000;  // 仍在 60s 窗内
    r.hub.raise(ErrorHub::Level::Warning, "t", "T", "same");
    QCOMPARE(r.center->visibleCardCount(), 0);  // 不重弹，只记历史
    QCOMPARE(r.hub.entries().first().count, 2);
    g_now += ErrorHub::kDedupWindowMs;           // 出窗
    r.hub.raise(ErrorHub::Level::Warning, "t", "T", "same");
    QCOMPARE(r.center->visibleCardCount(), 1);
}

void TestNotifications::severeModalOncePerWindow()
{
    Rig r;
    for (int i = 0; i < 5; ++i)
    {
        g_now += 1000;
        r.hub.raise(ErrorHub::Level::Error, "t", "严重", "崩了", QString(), true);
    }
    QCOMPARE(r.center->modalShownCount(), 1);
    QCOMPARE(countSevereBoxes(), 1);
    QCOMPARE(r.center->visibleCardCount(), 0);  // severe 不另起通知卡
    closeSevereBoxes();
    g_now += ErrorHub::kDedupWindowMs;
    r.hub.raise(ErrorHub::Level::Error, "t", "严重", "崩了", QString(), true);
    QCOMPARE(r.center->modalShownCount(), 2);
    closeSevereBoxes();
}

void TestNotifications::infoGoesToStatusBar()
{
    Rig r;
    r.hub.raise(ErrorHub::Level::Info, "t", "完成", "第一行\n第二行");
    QCOMPARE(r.win.statusBar()->currentMessage(), QStringLiteral("第一行 第二行"));
    QCOMPARE(r.center->visibleCardCount(), 0);
    QCOMPARE(r.center->statusShownCount(), 1);
}

void TestNotifications::cardsAnchoredBottomRight()
{
    Rig r;
    r.hub.raise(ErrorHub::Level::Warning, "t", "T", "a");
    r.hub.raise(ErrorHub::Level::Warning, "t", "T", "b");
    QList<QWidget *> cards;
    for (QWidget *w : r.win.findChildren<QWidget *>(QStringLiteral("paleoToastCard")))
        if (w->isVisible())
            cards << w;
    QCOMPARE(cards.size(), 2);
    for (QWidget *c : cards)
    {
        QVERIFY(c->geometry().right() <= r.win.width());
        QVERIFY(c->geometry().right() >= r.win.width() - 32);
        QVERIFY(c->geometry().bottom() <= r.win.height());
    }
    QVERIFY(cards[0]->geometry().top() != cards[1]->geometry().top());  // 堆叠不重叠
    QVERIFY(!cards[0]->geometry().intersects(cards[1]->geometry()));
    r.win.resize(900, 600);
    QCoreApplication::processEvents();
    for (QWidget *c : cards)
        QVERIFY(c->geometry().right() <= 900);
}

void TestNotifications::paleoNotifyFallsBackWithoutPresenter()
{
    QVERIFY(ErrorHub::global() == nullptr);
    bool sawBox = false;
    QTimer::singleShot(0, [&] {
        for (QWidget *w : QApplication::topLevelWidgets())
            if (auto *mb = qobject_cast<QMessageBox *>(w); mb && mb->isVisible())
            {
                sawBox = mb->text() == QStringLiteral("旧路径");
                mb->accept();
            }
    });
    PaleoNotify::warning(nullptr, QStringLiteral("T"), QStringLiteral("旧路径"));
    QVERIFY(sawBox);
}

void TestNotifications::paleoNotifyRoutesThroughHub()
{
    Rig r;
    ErrorHub::installGlobal(&r.hub);
    PaleoNotify::warning(&r.win, QStringLiteral("T"), QStringLiteral("新路径"));
    QCOMPARE(r.center->visibleCardCount(), 1);
    const auto e = r.hub.entries().last();
    QCOMPARE(e.level, ErrorHub::Level::Warning);
    QCOMPARE(e.source, QStringLiteral("QMainWindow"));
    QCOMPARE(e.text, QStringLiteral("新路径"));
    PaleoNotify::information(&r.win, QStringLiteral("T"), QStringLiteral("提示"));
    QCOMPARE(r.win.statusBar()->currentMessage(), QStringLiteral("提示"));
    PaleoNotify::critical(&r.win, QStringLiteral("T"), QStringLiteral("致命"));
    QCOMPARE(r.center->modalShownCount(), 1);
    closeSevereBoxes();
    ErrorHub::installGlobal(nullptr);
}

void TestNotifications::askReturnsAcceptButton()
{
    QTimer::singleShot(0, [] {
        if (auto *mb = qobject_cast<QMessageBox *>(QApplication::activeModalWidget()))
            mb->button(QMessageBox::Ok)->click();
    });
    QVERIFY(PaleoNotify::ask(nullptr, "T", "?", PaleoNotify::AskButtons::OkCancel,
                             PaleoNotify::AskDefault::Reject));
    QTimer::singleShot(0, [] {
        if (auto *mb = qobject_cast<QMessageBox *>(QApplication::activeModalWidget()))
        {
            QCOMPARE(mb->defaultButton(), mb->button(QMessageBox::No));
            mb->button(QMessageBox::No)->click();
        }
    });
    QVERIFY(!PaleoNotify::ask(nullptr, "T", "?", PaleoNotify::AskButtons::YesNo,
                              PaleoNotify::AskDefault::Reject));
    QTimer::singleShot(0, [] {
        if (auto *mb = qobject_cast<QMessageBox *>(QApplication::activeModalWidget()))
            mb->reject();
    });
    QCOMPARE(PaleoNotify::askSaveDiscard(nullptr, PaleoNotify::AskIcon::Warning, "T", "?",
                                         "S", "D", "C"),
             PaleoNotify::SaveChoice::Cancel);
}

void TestNotifications::stormDoesNotFreezeMainThread()
{
    // 100 条/秒 × 10s = 1000 条（模拟时钟每条 +10ms）；每 100ms 一批后处理事件。
    // 每批投递一个排队心跳，断言每批心跳都被处理（主线程事件循环持续推进）。
    Rig r;
    int heartbeats = 0;
    int batches = 0;
    int maxVisible = 0;
    for (int i = 0; i < 1000; ++i)
    {
        g_now += 10;
        const QString text = (i % 7 == 0) ? QStringLiteral("unique %1").arg(i)
                                          : QStringLiteral("recurring %1").arg(i % 20);
        r.hub.raise(i % 3 == 0 ? ErrorHub::Level::Error : ErrorHub::Level::Warning,
                    "storm", "T", text);
        if (i % 10 == 9)
        {
            QMetaObject::invokeMethod(&r.win, [&heartbeats] { ++heartbeats; },
                                      Qt::QueuedConnection);
            QCoreApplication::processEvents();
            ++batches;
            QCOMPARE(heartbeats, batches);
            maxVisible = std::max(maxVisible, r.center->visibleCardCount());
        }
    }
    QCOMPARE(batches, 100);
    QVERIFY(maxVisible <= NotificationCenter::kMaxCards);
    QCOMPARE(r.center->cardAllocations(), NotificationCenter::kMaxCards);  // 零新增分配
    QCOMPARE(r.hub.totalRaised(), quint64(1000));
    QVERIFY(r.hub.size() <= ErrorHub::kCapacity);
    QCOMPARE(r.center->modalShownCount(), 0);
}

void TestNotifications::stormFromWorkerThreadDelivered()
{
    Rig r;
    int delivered = 0;
    connect(&r.hub, &ErrorHub::errorRaised, &r.win,
            [&delivered](const ErrorHub::Entry &, bool) { ++delivered; });  // 排队到主线程
    QThread *worker = QThread::create([&r] {
        for (int i = 0; i < 1000; ++i)
            r.hub.raise(ErrorHub::Level::Warning, "storm", "T",
                        QStringLiteral("w %1").arg(i % 50));
    });
    int loops = 0;
    int heartbeats = 0;
    worker->start();
    while (!worker->isFinished() || delivered < 1000)
    {
        QMetaObject::invokeMethod(&r.win, [&heartbeats] { ++heartbeats; },
                                  Qt::QueuedConnection);
        QCoreApplication::processEvents();
        ++loops;
        QCOMPARE(heartbeats, loops);  // 每轮主线程都能处理自身事件
        QVERIFY(loops < 1'000'000);
    }
    worker->wait();
    delete worker;
    QCOMPARE(delivered, 1000);
    QVERIFY(r.center->visibleCardCount() <= NotificationCenter::kMaxCards);
    QCOMPARE(r.center->cardAllocations(), NotificationCenter::kMaxCards);
}

void TestNotifications::historyFilterCopyClear()
{
    ErrorHub hub;
    g_now = 7'000'000;
    hub.setClockForTest([] { return g_now; });
    hub.raise(ErrorHub::Level::Error, "mapping", "E1", "坏网格");
    hub.raise(ErrorHub::Level::Warning, "welltops", "W1", "缺分层\n第二行");
    hub.raise(ErrorHub::Level::Warning, "welltops", "W1", "缺分层\n第二行");  // 聚合
    hub.raise(ErrorHub::Level::Info, "shell", "I1", "已保存");
    ErrorHistoryPanel panel(&hub);
    panel.show();
    QCOMPARE(panel.rowCount(), 3);
    panel.setLevelFilter(2);  // 警告
    QCOMPARE(panel.rowCount(), 1);
    const QString tsv = panel.copyText();
    QVERIFY(tsv.contains(QStringLiteral("\twelltops\t2\tW1\t缺分层 第二行")));
    QVERIFY(!tsv.contains(QLatin1Char('\n')));  // 单行，多行正文压平
    panel.setLevelFilter(0);
    panel.setTextFilter(QStringLiteral("网格"));
    QCOMPARE(panel.rowCount(), 1);
    panel.setTextFilter(QString());
    // 选中第一行（最新 = info）只复制该行。
    auto *table = panel.findChild<QTableWidget *>(QStringLiteral("errorHistoryTable"));
    table->selectRow(0);
    QCOMPARE(panel.copyText().count(QLatin1Char('\n')), 0);
    QVERIFY(panel.copyText().contains(QStringLiteral("已保存")));
    panel.findChild<QPushButton *>(QStringLiteral("errorHistoryCopy"))->click();
    QCOMPARE(QApplication::clipboard()->text(), panel.copyText());
    // 新条目合并刷新（150ms 单发）。
    hub.raise(ErrorHub::Level::Error, "io", "E2", "读失败");
    QTRY_COMPARE_WITH_TIMEOUT(panel.rowCount(), 4, 5000);
    panel.findChild<QPushButton *>(QStringLiteral("errorHistoryClear"))->click();
    QCOMPARE(hub.size(), 0);
    QCOMPARE(panel.rowCount(), 0);
    QVERIFY(!panel.findChild<QPushButton *>(QStringLiteral("errorHistoryClear"))->isEnabled());
}

void TestNotifications::historyRingEvictionVisible()
{
    ErrorHub hub;
    for (int i = 0; i < ErrorHub::kCapacity + 1; ++i)
        hub.raise(ErrorHub::Level::Warning, "s", "T", QStringLiteral("n%1").arg(i));
    ErrorHistoryPanel panel(&hub);
    panel.show();
    QCOMPARE(panel.rowCount(), ErrorHub::kCapacity);
    panel.setTextFilter(QStringLiteral("n0"));
    QCOMPARE(panel.rowCount(), 0);  // n0 已逐出（n0 不是任何其他条目的子串）
    panel.setTextFilter(QStringLiteral("n500"));
    QCOMPARE(panel.rowCount(), 1);
}

QTEST_MAIN(TestNotifications)
#include "tst_notifications.moc"
