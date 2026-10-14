#include <QtTest>
#include <QDockWidget>
#include <QLabel>
#include <QMainWindow>
#include <QMenu>
#include <QScrollArea>
#include <QScreen>
#include <QScrollBar>
#include <QSettings>
#include <QStackedLayout>
#include <QStatusBar>
#include <QVBoxLayout>
#include "../src/ui/paleodockmanager.h"

class TestDockManager : public QObject
{
  Q_OBJECT
private slots:
  void dragGuidesAndEdgeDrop_data()
  {
    QTest::addColumn<int>("targetIndex");
    QTest::newRow("left") << 0;
    QTest::newRow("right") << 1;
    QTest::newRow("top") << 2;
    QTest::newRow("bottom") << 3;
  }

  void dragGuidesAndEdgeDrop()
  {
    QFETCH(int, targetIndex);
    QMainWindow window;
    window.resize(1000, 700);
    window.setCentralWidget(new QWidget);
    PaleoDockManager manager(&window, "test/drag");
    auto *dock = new QDockWidget("Drag panel");
    dock->setObjectName("dragPanel");
    dock->setWidget(new QLabel("content"));
    manager.addDock(Qt::LeftDockWidgetArea, dock);
    window.show();
    QTest::qWait(30);
    const QPoint press(dock->width() / 2, 8);
    QTest::mousePress(dock, Qt::LeftButton, Qt::NoModifier, press);
    QTest::mouseMove(dock, press + QPoint(180, 100));
    auto guides = window.findChildren<QLabel *>("dockDropGuide");
    QCOMPARE(guides.size(), 4);
    QTRY_VERIFY(guides.first()->isVisible());
    auto *guide = guides.at(targetIndex);
    const QPoint target = guide->mapToGlobal(guide->rect().center());
    QTest::mouseMove(dock, dock->mapFromGlobal(target));
    QTest::qWait(30);
    QTest::mouseRelease(dock, Qt::LeftButton, Qt::NoModifier, dock->mapFromGlobal(target));
    const Qt::DockWidgetArea areas[] = {Qt::LeftDockWidgetArea, Qt::RightDockWidgetArea,
                                        Qt::TopDockWidgetArea, Qt::BottomDockWidgetArea};
    QTRY_COMPARE(window.dockWidgetArea(dock), areas[targetIndex]);
    QTRY_VERIFY(!dock->isFloating());
    for (auto *guide : guides)
      QVERIFY(!guide->isVisible());
  }

  void contentChangesDoNotResizeDock()
  {
    QMainWindow window;
    window.resize(1200, 800);
    window.setCentralWidget(new QWidget);
    PaleoDockManager manager(&window, "test/layout");
    auto *dock = new QDockWidget("Parameters");
    dock->setObjectName("parameters");
    auto *content = new QWidget;
    auto *layout = new QVBoxLayout(content);
    auto *label = new QLabel("short");
    layout->addWidget(label);
    dock->setWidget(content);
    manager.addDock(Qt::RightDockWidgetArea, dock);
    window.show();
    QTest::qWait(30);
    window.resizeDocks({dock}, {260}, Qt::Horizontal);
    QTest::qWait(30);
    const QSize before = dock->size();
    label->setMinimumSize(900, 1100);
    label->setText(QString(500, 'W'));
    QTest::qWait(50);
    QCOMPARE(dock->size(), before);
    auto *scroll = qobject_cast<QScrollArea *>(dock->widget());
    QVERIFY(scroll);
    QVERIFY(scroll->horizontalScrollBar()->maximum() > 0);
    QVERIFY(scroll->verticalScrollBar()->maximum() > 0);
    dock->setFloating(true);
    dock->resize(300, 220);
    QTest::qWait(30);
    const QSize floatingSize = dock->size();
    label->setMinimumSize(1200, 1400);
    QTest::qWait(30);
    QCOMPARE(dock->size(), floatingSize);
    window.resize(1000, 700);
    QTest::qWait(30);
    QCOMPARE(dock->size(), floatingSize);
  }

  void pageSwitchAndManualResize()
  {
    QMainWindow window;
    window.resize(1000, 700);
    window.setCentralWidget(new QWidget);
    PaleoDockManager manager(&window, "test/layout");
    auto *dock = new QDockWidget("Stack");
    dock->setObjectName("stack");
    auto *content = new QWidget;
    auto *stack = new QStackedLayout(content);
    stack->addWidget(new QLabel("small page"));
    auto *large = new QLabel("large page");
    large->setMinimumSize(800, 900);
    stack->addWidget(large);
    dock->setWidget(content);
    manager.addDock(Qt::LeftDockWidgetArea, dock);
    window.show();
    QTest::qWait(30);
    window.resizeDocks({dock}, {210}, Qt::Horizontal);
    QTest::qWait(30);
    const QSize before = dock->size();
    stack->setCurrentIndex(1);
    QTest::qWait(30);
    QCOMPARE(dock->size(), before);
    window.resizeDocks({dock}, {170}, Qt::Horizontal);
    QTest::qWait(30);
    QVERIFY(dock->width() < before.width());
    QVERIFY(dock->width() < 240);
  }

  void layoutSaveRestoreAndLateDock()
  {
    QSettings settings("paleo", "paleo");
    settings.remove("test/layout");
    QMainWindow window;
    window.resize(1000, 700);
    window.setCentralWidget(new QWidget);
    PaleoDockManager manager(&window, "test/layout");
    auto *dock = new QDockWidget("Panel");
    dock->setObjectName("panel");
    dock->setWidget(new QLabel("content"));
    manager.addDock(Qt::LeftDockWidgetArea, dock);
    manager.captureDefaultLayout();
    window.show();
    QTest::qWait(30);
    QVERIFY(dock->features().testFlag(QDockWidget::DockWidgetFloatable));
    QCOMPARE(dock->allowedAreas(), Qt::AllDockWidgetAreas);
    QVERIFY(!window.isAnimated());
    dock->setFloating(true);
    dock->resize(400, 300);
    QTest::qWait(30);
    {
      QScopedPointer<QMenu> menu(manager.createMenu());
      menu->actions().at(1)->trigger(); // explicit save
    }
    manager.restoreDefaultLayout();
    QVERIFY(!dock->isFloating());
    QCOMPARE(window.dockWidgetArea(dock), Qt::LeftDockWidgetArea);
    {
      QScopedPointer<QMenu> menu(manager.createMenu());
      QVERIFY(menu->actions().at(2)->isEnabled());
      menu->actions().at(2)->trigger();
    }
    QVERIFY(dock->isFloating());
    QCOMPARE(dock->size(), QSize(400, 300));
    auto *late = new QDockWidget("Late");
    late->setObjectName("late");
    late->setWidget(new QLabel("late content"));
    manager.addDock(Qt::BottomDockWidgetArea, late);
    window.addDockWidget(Qt::TopDockWidgetArea, late);
    const QByteArray saved = window.saveState();
    manager.removeDock(late);
    delete late;
    QVERIFY(window.restoreState(saved));
    late = new QDockWidget("Late");
    late->setObjectName("late");
    late->setWidget(new QLabel("replacement"));
    manager.addDock(Qt::BottomDockWidgetArea, late);
    QCOMPARE(window.dockWidgetArea(late), Qt::TopDockWidgetArea);
  }

  // 意图钩子经菜单全链路：保存写 <key>Intent，恢复把「保存时的」意图
  // 交给 apply（不是恢复前的现值）。
  void intentHooksRoundTripViaMenu()
  {
    {
      QSettings s("paleo", "paleo");
      s.remove("test/intent");
      s.remove("test/intentIntent");
    }
    QMainWindow window;
    window.resize(1000, 700);
    window.setCentralWidget(new QWidget);
    window.setStatusBar(new QStatusBar);
    PaleoDockManager manager(&window, "test/intent");
    QVariantMap fake;
    QVariantMap applied;
    bool applyCalled = false;
    manager.setIntentHooks({[&fake] { return fake; },
                            [&applied, &applyCalled](const QVariantMap &m) {
                              applied = m;
                              applyCalled = true;
                            }});
    auto *dock = new QDockWidget("Panel");
    dock->setObjectName("panel");
    dock->setWidget(new QLabel("content"));
    manager.addDock(Qt::LeftDockWidgetArea, dock);
    window.show();
    QTest::qWait(30);
    fake = {{QStringLiteral("panel"), false}};
    {
      QScopedPointer<QMenu> menu(manager.createMenu());
      menu->actions().at(1)->trigger(); // 保存当前布局
    }
    {
      QSettings s("paleo", "paleo");
      QVERIFY(s.contains("test/intentIntent"));
      QCOMPARE(s.value("test/intentIntent").toMap(), fake);
    }
    // 保存后改变「当前意图」：恢复必须落保存时的快照。
    fake = {{QStringLiteral("panel"), true}};
    {
      QScopedPointer<QMenu> menu(manager.createMenu());
      QAction *restore = menu->actions().at(2);
      QVERIFY(restore->isEnabled());
      restore->trigger(); // 恢复保存的布局
    }
    QVERIFY(applyCalled);
    QCOMPARE(applied, QVariantMap({{QStringLiteral("panel"), false}}));
    QCOMPARE(window.statusBar()->currentMessage(),
             QCoreApplication::translate("PaleoDockManager", "已恢复保存的布局"));
  }

  // 旧版布局（只有 state、没有 Intent 键）：恢复成功后 apply 收到
  // 恢复前的意图快照。
  void intentHooksLegacyStateOnlyRestoresWithPreRestoreIntent()
  {
    QByteArray state;
    {
      QMainWindow seed;
      seed.resize(900, 650);
      seed.setCentralWidget(new QWidget);
      auto *d = new QDockWidget("Panel");
      d->setObjectName("panel");
      d->setWidget(new QLabel("seed"));
      seed.addDockWidget(Qt::RightDockWidgetArea, d);
      seed.show();
      state = seed.saveState();
    }
    {
      QSettings s("paleo", "paleo");
      s.setValue("test/legacy", state);
      s.remove("test/legacyIntent");
    }
    QMainWindow window;
    window.resize(1000, 700);
    window.setCentralWidget(new QWidget);
    window.setStatusBar(new QStatusBar);
    PaleoDockManager manager(&window, "test/legacy");
    const QVariantMap before{{QStringLiteral("panel"), true}};
    QVariantMap applied;
    bool applyCalled = false;
    manager.setIntentHooks({[&before] { return before; },
                            [&applied, &applyCalled](const QVariantMap &m) {
                              applied = m;
                              applyCalled = true;
                            }});
    auto *dock = new QDockWidget("Panel");
    dock->setObjectName("panel");
    dock->setWidget(new QLabel("content"));
    manager.addDock(Qt::LeftDockWidgetArea, dock);
    window.show();
    QTest::qWait(30);
    {
      QScopedPointer<QMenu> menu(manager.createMenu());
      menu->actions().at(2)->trigger(); // 恢复保存的布局
    }
    QVERIFY(applyCalled);
    QCOMPARE(applied, before);
    QCOMPARE(window.statusBar()->currentMessage(),
             QCoreApplication::translate("PaleoDockManager", "已恢复保存的布局"));
  }

  // state 键下是坏数据：restoreState 返回 false → 状态栏报错文案、
  // apply 不被调用、不崩。
  void intentHooksRestoreFailureReportsAndSkipsApply()
  {
    {
      QSettings s("paleo", "paleo");
      s.setValue("test/garbage", QByteArray("not-a-layout-state"));
      s.remove("test/garbageIntent");
    }
    QMainWindow window;
    window.resize(1000, 700);
    window.setCentralWidget(new QWidget);
    window.setStatusBar(new QStatusBar);
    PaleoDockManager manager(&window, "test/garbage");
    bool applyCalled = false;
    manager.setIntentHooks({[] { return QVariantMap{{QStringLiteral("panel"), true}}; },
                            [&applyCalled](const QVariantMap &) { applyCalled = true; }});
    auto *dock = new QDockWidget("Panel");
    dock->setObjectName("panel");
    dock->setWidget(new QLabel("content"));
    manager.addDock(Qt::LeftDockWidgetArea, dock);
    window.show();
    QTest::qWait(30);
    QVERIFY(!manager.restoreLayout(QByteArray("not-a-layout-state"), nullptr));
    QVERIFY(!applyCalled);
    {
      QScopedPointer<QMenu> menu(manager.createMenu());
      menu->actions().at(2)->trigger(); // 恢复保存的布局
    }
    QVERIFY(!applyCalled);
    QCOMPARE(window.statusBar()->currentMessage(),
             QCoreApplication::translate("PaleoDockManager",
                                         "保存的布局与当前面板不兼容，未能恢复"));
  }

  // 四条布局命令的用户反馈：保存与恢复默认各落一句状态栏文案。
  void layoutCommandFeedbackMessages()
  {
    {
      QSettings s("paleo", "paleo");
      s.remove("test/feedback");
      s.remove("test/feedbackIntent");
    }
    QMainWindow window;
    window.resize(1000, 700);
    window.setCentralWidget(new QWidget);
    window.setStatusBar(new QStatusBar);
    PaleoDockManager manager(&window, "test/feedback");
    auto *dock = new QDockWidget("Panel");
    dock->setObjectName("panel");
    dock->setWidget(new QLabel("content"));
    manager.addDock(Qt::LeftDockWidgetArea, dock);
    manager.captureDefaultLayout();
    window.show();
    QTest::qWait(30);
    {
      QScopedPointer<QMenu> menu(manager.createMenu());
      menu->actions().at(1)->trigger(); // 保存当前布局
    }
    QCOMPARE(window.statusBar()->currentMessage(),
             QCoreApplication::translate("PaleoDockManager", "布局已保存"));
    window.statusBar()->clearMessage();
    {
      QScopedPointer<QMenu> menu(manager.createMenu());
      menu->actions().at(3)->trigger(); // 恢复默认布局
    }
    QCOMPARE(window.statusBar()->currentMessage(),
             QCoreApplication::translate("PaleoDockManager", "已恢复默认布局"));
  }
  // B（多屏，非回归）：浮动 dock 在副屏上保存，副屏拔掉后恢复——dock 不得留在所有屏幕之外
  // （否则用户拖不回来）。标题栏抓手区必须落进可用屏幕。当前 Qt 已保证这一点；本用例钉住它，
  // 任何自绘恢复路径回退都会变红。前置断言保证「dock 确实先被放到屏外」，不会空洞通过。
  void floatingDockRestoredOffScreenStaysReachable()
  {
    QMainWindow window;
    window.resize(1000, 700);
    window.setCentralWidget(new QWidget);
    PaleoDockManager manager(&window, "test/offscreenfloat");
    auto *dock = new QDockWidget("Panel");
    dock->setObjectName("panel");
    dock->setWidget(new QLabel("content"));
    manager.addDock(Qt::LeftDockWidgetArea, dock);
    window.show();
    QTest::qWait(30);
    const QRect avail = window.screen()->availableGeometry();
    dock->setFloating(true);
    dock->move(avail.right() + 4000, avail.bottom() + 4000); // 拔掉副屏前保存的位置
    QTest::qWait(30);
    QVERIFY2(!avail.intersects(dock->frameGeometry()),
             "precondition: the platform must really keep the dock outside the screen");
    const QByteArray state = window.saveState();
    dock->setFloating(false);
    QTest::qWait(30);
    QVERIFY(manager.restoreLayout(state, nullptr));
    QTest::qWait(30);
    QVERIFY(dock->isFloating());
    const QRect frame = dock->frameGeometry();
    const QRect handle(frame.topLeft(), QSize(qMin(frame.width(), 120), 24));
    QVERIFY2(avail.contains(handle),
             qPrintable(QStringLiteral("floating dock frame %1,%2 %3x%4 is outside the available screen %5,%6 %7x%8")
                            .arg(frame.x()).arg(frame.y()).arg(frame.width()).arg(frame.height())
                            .arg(avail.x()).arg(avail.y()).arg(avail.width()).arg(avail.height())));
  }

  // 同一路径的正常场景（非回归）：屏内的浮动位置恢复后不被挪动。
  void floatingDockInsideScreenKeepsItsPosition()
  {
    QMainWindow window;
    window.resize(1000, 700);
    window.setCentralWidget(new QWidget);
    PaleoDockManager manager(&window, "test/insidefloat");
    auto *dock = new QDockWidget("Panel");
    dock->setObjectName("panel");
    dock->setWidget(new QLabel("content"));
    manager.addDock(Qt::LeftDockWidgetArea, dock);
    window.show();
    QTest::qWait(30);
    const QRect avail = window.screen()->availableGeometry();
    dock->setFloating(true);
    dock->move(avail.left() + 60, avail.top() + 80);
    QTest::qWait(30);
    const QPoint before = dock->frameGeometry().topLeft();
    const QByteArray state = window.saveState();
    dock->setFloating(false);
    QTest::qWait(30);
    QVERIFY(manager.restoreLayout(state, nullptr));
    QTest::qWait(30);
    QVERIFY(dock->isFloating());
    QCOMPARE(dock->frameGeometry().topLeft(), before);
  }
};
QTEST_MAIN(TestDockManager)
#include "tst_dockmanager.moc"
