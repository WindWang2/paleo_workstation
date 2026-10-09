#include <QtTest>
#include <QSemaphore>
#include <QScopeGuard>
#include <memory>
#include "../src/services/paleotaskservice.h"
#include <QLabel>
#include <QTreeWidget>

#include "../src/metadata/paleoprojectstore.h"
#include "../src/ui/taskpanel.h"
#include "uipolish_capture.h"

// TaskPanel mirrors the store's layer-busy registry: mark busy → row appears,
// mark free → back to empty state.
class TestTaskPanel : public QObject
{
  Q_OBJECT
private slots:
  void hiddenTasksBuildRowsOnlyWhenShown()
  {
    PaleoTaskService service;
    TaskPanel panel(nullptr, &service);
    auto *list = panel.findChild<QTreeWidget *>(QStringLiteral("busyList"));
    QVERIFY(list);
    auto gate = std::make_shared<QSemaphore>();
    const auto releaseOnExit = qScopeGuard([gate] { gate->release(); });
    auto *task = service.start("background load", [gate](PaleoTask *task) {
      task->reportBytes(512, 1024); gate->acquire(); return QString();
    });
    QTest::qWait(25); QCOMPARE(list->topLevelItemCount(), 0);
    panel.show();
    QTRY_COMPARE(list->topLevelItemCount(), 1);
    gate->release(); QTRY_VERIFY(!task->running());
    panel.hide(); service.clearFinished(); QTest::qWait(25);
    QCOMPARE(list->topLevelItemCount(), 1);
    panel.show(); QTRY_COMPARE(list->topLevelItemCount(), 0);
  }

  void reflectsBusyRegistry()
  {
    PaleoProjectStore store;
    TaskPanel panel(&store);

    auto *list = panel.findChild<QTreeWidget *>(QStringLiteral("busyList"));
    auto *count = panel.findChild<QLabel *>(QStringLiteral("busyCountLabel"));
    auto *empty = panel.findChild<QLabel *>(QStringLiteral("busyEmptyHint"));
    QVERIFY(list && count && empty);

    QCOMPARE(list->topLevelItemCount(), 0);
    QVERIFY(empty->isVisibleTo(&panel)); // visible when idle

    store.markLayerBusy(QStringLiteral("facies.T1"), QStringLiteral("task-7"),
                        QStringLiteral("IDW interpolation"));
    panel.refresh();
    QCOMPARE(list->topLevelItemCount(), 1);
    QCOMPARE(list->topLevelItem(0)->text(0), QStringLiteral("facies.T1"));
    QCOMPARE(list->topLevelItem(0)->text(1), QStringLiteral("task-7"));
    QCOMPARE(count->text(), QStringLiteral("忙图层：1"));
    QVERIFY(!empty->isVisibleTo(&panel));

    store.markLayerFree(QStringLiteral("facies.T1"));
    panel.refresh();
    QCOMPARE(list->topLevelItemCount(), 0);
    QVERIFY(empty->isVisibleTo(&panel));
  }

  void nullStoreSafe()
  {
    TaskPanel panel(nullptr);
    panel.refresh(); // must not crash
    QCOMPARE(panel.findChild<QTreeWidget *>(QStringLiteral("busyList"))
                 ->topLevelItemCount(),
             0);
  }

  // goal/ui-experience-polish：忙行 + 行内进度条的修前/修后截图证据
  //（PALEO_UI_CAPTURE 未设时零开销直通）。
  void captureEvidence()
  {
    PaleoProjectStore store;
    TaskPanel panel(&store);
    store.markLayerBusy(QStringLiteral("facies.T1"), QStringLiteral("task-7"),
                        QStringLiteral("IDW interpolation"));
    panel.refresh();
    uipolish::capturePanel(&panel, QStringLiteral("taskpanel"));
  }

  // goal/ui-experience-polish：任务列表键盘可达（↓ 首选行、↓/↑ 移动）。
  void keyboardNavigationOnBusyList()
  {
    PaleoProjectStore store;
    TaskPanel panel(&store);
    store.markLayerBusy(QStringLiteral("facies.T1"), QStringLiteral("task-7"),
                        QStringLiteral("IDW interpolation"));
    store.markLayerBusy(QStringLiteral("facies.T2"), QStringLiteral("task-8"),
                        QStringLiteral("IDW interpolation"));
    panel.refresh();
    auto *list = panel.findChild<QTreeWidget *>(QStringLiteral("busyList"));
    QVERIFY(list && list->topLevelItemCount() == 2);
    panel.show();
    QTest::qWaitForWindowExposed(&panel);
    list->setFocus();
    // 显式清起点（窗口激活时序下树会自动选首行——offscreen 不定）。
    list->setCurrentItem(nullptr);
    QCOMPARE(list->currentIndex().row(), -1);
    QTest::keyClick(list, Qt::Key_Down);
    QCOMPARE(list->currentIndex().row(), 0);
    QTest::keyClick(list, Qt::Key_Down);
    QCOMPARE(list->currentIndex().row(), 1);
    QTest::keyClick(list, Qt::Key_Up);
    QCOMPARE(list->currentIndex().row(), 0);
  }
};

QTEST_MAIN(TestTaskPanel)
#include "tst_taskpanel.moc"
