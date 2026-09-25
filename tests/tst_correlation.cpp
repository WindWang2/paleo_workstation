#include <QtTest>
#include <QApplication>
#include <QGraphicsPathItem>
#include <QGraphicsScene>
#include <QGraphicsView>
#include <QLabel>
#include <QSignalSpy>

#include "../src/ui/correlationpanel.h"
#include "../src/linkage/selectioncontext.h"

// 连井剖面 (well correlation section) — the constraint page's linked
// sub-panel (§42.x). P1 covers the data model + scene scaffold only: one
// placeholder column per well, selection-follow highlighting, reorder, and
// click intents. No Qgs* involvement — a plain QApplication suffices.
class TestCorrelation : public QObject
{
  Q_OBJECT

  private:
    static QList<QPair<QString, QString>> threeWells()
    {
      return {{QStringLiteral("W1"), QStringLiteral("井1")},
              {QStringLiteral("W2"), QStringLiteral("井2")},
              {QStringLiteral("W3"), QStringLiteral("井3")}};
    }

    static QGraphicsScene *sceneOf(WellCorrelationPanel &panel)
    {
      auto *v = panel.findChild<QGraphicsView *>(QStringLiteral("correlationView"));
      return v ? v->scene() : nullptr;
    }

    // Column placeholders are the scene's top-level items (labels/ticks are
    // their children) — counted/ordered without depending on data roles.
    static QList<QGraphicsItem *> columns(WellCorrelationPanel &panel)
    {
      QList<QGraphicsItem *> out;
      if (auto *s = sceneOf(panel))
        for (QGraphicsItem *it : s->items(Qt::AscendingOrder)) // stacking, not pos
          if (!it->parentItem())
            out << it;
      return out;
    }

    static QList<QGraphicsItem *> columnsByX(WellCorrelationPanel &panel)
    {
      auto out = columns(panel);
      std::sort(out.begin(), out.end(), [](QGraphicsItem *a, QGraphicsItem *b) {
        return a->sceneBoundingRect().x() < b->sceneBoundingRect().x();
      });
      return out;
    }

  private slots:
    void nullContextIsSafe()
    {
      WellCorrelationPanel panel(nullptr);
      panel.setWells(threeWells());
      QCOMPARE(panel.wellCount(), 3);
      panel.reorder(0, 2); // must not crash without a SelectionContext
      QCOMPARE(panel.wellAt(2), QStringLiteral("W1"));
    }

    void emptyStateShowsGuidance()
    {
      WellCorrelationPanel panel(nullptr);
      panel.show(); // children of a never-shown window report isVisible()==false
      QVERIFY(QTest::qWaitForWindowExposed(&panel));
      auto *label = panel.findChild<QLabel *>(QStringLiteral("emptyLabel"));
      QVERIFY(label);
      // §42.4: empty panels show warm guidance text — never a blank view.
      QVERIFY(label->isVisible());
      QCOMPARE(label->text(), QStringLiteral("选择井以构建剖面"));
      QCOMPARE(columns(panel).size(), 0);

      panel.setWells(threeWells());
      QVERIFY(!label->isVisible());
      panel.setWells({});
      QVERIFY(label->isVisible());
    }

    void wellsCreateColumnsInOrder()
    {
      SelectionContext ctx;
      WellCorrelationPanel panel(&ctx);
      panel.setWells(threeWells());

      QCOMPARE(panel.wellCount(), 3);
      QCOMPARE(panel.wellAt(0), QStringLiteral("W1"));
      QCOMPARE(panel.wellAt(1), QStringLiteral("W2"));
      QCOMPARE(panel.wellAt(2), QStringLiteral("W3"));

      auto *view = panel.findChild<QGraphicsView *>(QStringLiteral("correlationView"));
      QVERIFY(view);
      QCOMPARE(columns(panel).size(), 3);
      QVERIFY(!panel.isWellHighlighted(QStringLiteral("W2")));
    }

    void selectionHighlightsOnlySelected()
    {
      SelectionContext ctx;
      WellCorrelationPanel panel(&ctx);
      panel.setWells(threeWells());

      ctx.setSelection({QStringLiteral("W2")}, QStringLiteral("wellpanel"));
      QVERIFY(panel.isWellHighlighted(QStringLiteral("W2")));
      QVERIFY(!panel.isWellHighlighted(QStringLiteral("W1")));
      QVERIFY(!panel.isWellHighlighted(QStringLiteral("W3")));

      // Only one column carries the primary (selected) pen (§41.3 linkage).
      int primary = 0, normal = 0;
      for (QGraphicsItem *it : columns(panel))
      {
        const auto *shape = qgraphicsitem_cast<const QAbstractGraphicsShapeItem *>(it);
        QVERIFY2(shape, "column items must be shape items with a pen");
        if (shape->pen().color() == QColor(QStringLiteral("#1B73D0")))
          ++primary;
        else
          ++normal;
      }
      QCOMPARE(primary, 1);
      QCOMPARE(normal, 2);

      ctx.setSelection({QStringLiteral("W1")}, QStringLiteral("canvas"));
      QVERIFY(panel.isWellHighlighted(QStringLiteral("W1")));
      QVERIFY(!panel.isWellHighlighted(QStringLiteral("W2")));

      // The panel's own broadcast origin must not re-enter (echo guard).
      ctx.setSelection({QStringLiteral("W3")}, QStringLiteral("correlation"));
      QVERIFY(panel.isWellHighlighted(QStringLiteral("W1")));
      QVERIFY(!panel.isWellHighlighted(QStringLiteral("W3")));

      ctx.clear(QStringLiteral("canvas"));
      QVERIFY(!panel.isWellHighlighted(QStringLiteral("W1")));
    }

    void selectionAppliesToWellsSetLater()
    {
      SelectionContext ctx;
      ctx.setSelection({QStringLiteral("W3")}, QStringLiteral("canvas"));
      WellCorrelationPanel panel(&ctx);
      panel.setWells(threeWells()); // arrives after the broadcast
      QVERIFY(panel.isWellHighlighted(QStringLiteral("W3")));
      QVERIFY(!panel.isWellHighlighted(QStringLiteral("W1")));
    }

    void reorderSwapsSectionOrder()
    {
      WellCorrelationPanel panel(nullptr);
      panel.setWells(threeWells());
      panel.reorder(0, 1); // QList::move semantics — drag-reorder equivalent
      QCOMPARE(panel.wellAt(0), QStringLiteral("W2"));
      QCOMPARE(panel.wellAt(1), QStringLiteral("W1"));
      QCOMPARE(panel.wellAt(2), QStringLiteral("W3"));
      QCOMPARE(panel.wellCount(), 3);

      // Rebuilt columns reflect the new order left-to-right.
      const auto cols = columnsByX(panel);
      QCOMPARE(cols.size(), 3);
      const QPointF first = cols.at(0)->sceneBoundingRect().topLeft();
      const QPointF last  = cols.at(2)->sceneBoundingRect().topLeft();
      QVERIFY(first.x() < last.x());

      // Out-of-range indexes are ignored, not fatal.
      panel.reorder(-1, 0);
      panel.reorder(0, 9);
      QCOMPARE(panel.wellCount(), 3);
      QCOMPARE(panel.wellAt(0), QStringLiteral("W2"));
    }

    void clickColumnEmitsWellClicked()
    {
      SelectionContext ctx;
      WellCorrelationPanel panel(&ctx);
      panel.setWells(threeWells());
      auto *view = panel.findChild<QGraphicsView *>(QStringLiteral("correlationView"));
      QVERIFY(view);
      panel.resize(720, 480);
      panel.show();
      QVERIFY(QTest::qWaitForWindowExposed(&panel));

      const auto cols = columnsByX(panel);
      QCOMPARE(cols.size(), 3);
      view->ensureVisible(cols.at(1));
      const QPoint at = view->mapFromScene(cols.at(1)->sceneBoundingRect().center());

      QSignalSpy spy(&panel, &WellCorrelationPanel::wellClicked);
      QTest::mouseClick(view->viewport(), Qt::LeftButton, Qt::NoModifier, at);
      QCOMPARE(spy.count(), 1);
      QCOMPARE(spy.first().at(0).toString(), QStringLiteral("W2"));
    }

    void doubleClickColumnEmitsWellDoubleClicked()
    {
      WellCorrelationPanel panel(nullptr);
      panel.setWells(threeWells());
      auto *view = panel.findChild<QGraphicsView *>(QStringLiteral("correlationView"));
      QVERIFY(view);
      panel.resize(720, 480);
      panel.show();
      QVERIFY(QTest::qWaitForWindowExposed(&panel));

      const auto cols = columnsByX(panel);
      QCOMPARE(cols.size(), 3);
      view->ensureVisible(cols.at(2));
      const QPoint at = view->mapFromScene(cols.at(2)->sceneBoundingRect().center());

      QSignalSpy single(&panel, &WellCorrelationPanel::wellClicked);
      QSignalSpy dbl(&panel, &WellCorrelationPanel::wellDoubleClicked);
      QTest::mouseDClick(view->viewport(), Qt::LeftButton, Qt::NoModifier, at);
      QCOMPARE(dbl.count(), 1);
      QCOMPARE(dbl.first().at(0).toString(), QStringLiteral("W3"));
      // The double-click press also reports a click (QListWidget semantics).
      QCOMPARE(single.count(), 1);
      QCOMPARE(single.first().at(0).toString(), QStringLiteral("W3"));
    }
};

int main(int argc, char *argv[])
{
  if (qgetenv("QT_QPA_PLATFORM").isEmpty())
    qputenv("QT_QPA_PLATFORM", "offscreen");
  QApplication app(argc, argv);
  TestCorrelation tc;
  return QTest::qExec(&tc, argc, argv);
}

#include "tst_correlation.moc"
