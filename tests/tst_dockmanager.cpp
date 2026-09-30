#include <QtTest>
#include <QDockWidget>
#include <QLabel>
#include <QMainWindow>
#include <QMenu>
#include <QScrollArea>
#include <QScrollBar>
#include <QSettings>
#include <QStackedLayout>
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
};
QTEST_MAIN(TestDockManager)
#include "tst_dockmanager.moc"
