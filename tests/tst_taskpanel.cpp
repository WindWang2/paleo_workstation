#include <QtTest>
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
};

QTEST_MAIN(TestTaskPanel)
#include "tst_taskpanel.moc"
