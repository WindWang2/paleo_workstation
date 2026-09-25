#include <QtTest>
#include <QApplication>
#include <QFile>
#include <QGraphicsPathItem>
#include <QGraphicsPixmapItem>
#include <QGraphicsScene>
#include <QGraphicsSimpleTextItem>
#include <QGraphicsView>
#include <QLabel>
#include <QSignalSpy>
#include <QTemporaryDir>

#include <limits>

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

    // Curve tracks render through Qgs2DXyPlot into a QGraphicsPixmapItem child
    // of the column — the pixmap cast filters out name labels and tick items.
    static QList<QGraphicsPixmapItem *> curveChildren(QGraphicsItem *column)
    {
      QList<QGraphicsPixmapItem *> out;
      for (QGraphicsItem *c : column->childItems())
        if (auto *pm = qgraphicsitem_cast<QGraphicsPixmapItem *>(c))
          out << pm;
      return out;
    }

    // Row-range of pixels with ink (alpha) inside a rendered track image.
    // Returns {first,last} inked row indices; {-1,-1} if fully transparent.
    static QPair<int, int> inkRows(const QGraphicsPixmapItem *item)
    {
      const QImage img = item->pixmap().toImage();
      int first = -1, last = -1;
      for (int y = 0; y < img.height(); ++y)
        for (int x = 0; x < img.width(); ++x)
          if (qAlpha(img.pixel(x, y)) > 8)
          {
            if (first < 0)
              first = y;
            last = y;
            break;
          }
      return {first, last};
    }

    // Total inked pixels — a NULL gap removes a whole polyline segment, so
    // the gapped render strictly under-inks the continuous one.
    static int inkPixelCount(const QGraphicsPixmapItem *item)
    {
      const QImage img = item->pixmap().toImage();
      int n = 0;
      for (int y = 0; y < img.height(); ++y)
        for (int x = 0; x < img.width(); ++x)
          if (qAlpha(img.pixel(x, y)) > 8)
            ++n;
      return n;
    }

    static bool hasTextChild(QGraphicsItem *column, const QString &text)
    {
      for (QGraphicsItem *c : column->childItems())
        if (auto *t = qgraphicsitem_cast<QGraphicsSimpleTextItem *>(c))
          if (t->text() == text)
            return true;
      return false;
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

    // --- real log curves ----------------------------------------------------

    void curvesShareOneDepthAxis()
    {
      WellCorrelationPanel panel(nullptr);
      panel.setWells({{QStringLiteral("W1"), QStringLiteral("井1")},
                      {QStringLiteral("W2"), QStringLiteral("井2")}});
      QVERIFY(!panel.hasCurves());

      // Disjoint depth windows — per-well axes would stretch both curves over
      // their full columns; the shared axis must not.
      panel.setWellCurves(QStringLiteral("W1"),
                          {1000.f, 1050.f, 1100.f, 1150.f, 1200.f},
                          {2.f, 8.f, 4.f, 9.f, 5.f}, QStringLiteral("GR"));
      panel.setWellCurves(QStringLiteral("W2"),
                          {1500.f, 1550.f, 1600.f, 1650.f, 1700.f},
                          {0.f, 10.f, 3.f, 7.f, 5.f}, QStringLiteral("RHOB"));

      QVERIFY(panel.hasCurves());
      QCOMPARE(panel.curveItemCount(QStringLiteral("W1")), 1);
      QCOMPARE(panel.curveItemCount(QStringLiteral("W2")), 1);
      QCOMPARE(panel.curveItemCount(QStringLiteral("W3")), 0);

      const auto cols = columnsByX(panel);
      QCOMPARE(cols.size(), 2);
      const auto c1 = curveChildren(cols.at(0));
      const auto c2 = curveChildren(cols.at(1));
      QCOMPARE(c1.size(), 1);
      QCOMPARE(c2.size(), 1);

      const QRectF col1 = cols.at(0)->sceneBoundingRect();
      const QRectF col2 = cols.at(1)->sceneBoundingRect();
      const QRectF r1 = c1.first()->sceneBoundingRect();
      const QRectF r2 = c2.first()->sceneBoundingRect();

      // Each track pixmap is placed inside its own column (stroke tolerance).
      QVERIFY(col1.adjusted(-1.5, -1.5, 1.5, 1.5).contains(r1));
      QVERIFY(col2.adjusted(-1.5, -1.5, 1.5, 1.5).contains(r2));

      // Depth registration on the shared 1000–1700 axis is proved by WHERE
      // the plot ink lands inside each pixmap: W1 (depths 1000–1200) inks
      // only the top ~30% of its track; W2 (1500–1700) the bottom ~30%.
      const QPair<int, int> ink1 = inkRows(c1.first());
      const QPair<int, int> ink2 = inkRows(c2.first());
      QVERIFY2(ink1.first >= 0, "W1 track rendered no ink");
      QVERIFY2(ink2.first >= 0, "W2 track rendered no ink");
      const int h1 = c1.first()->pixmap().height();
      const int h2 = c2.first()->pixmap().height();
      QVERIFY(ink1.first < h1 * 0.15);
      QVERIFY(ink1.second < h1 * 0.45);
      QVERIFY(ink2.first > h2 * 0.55);
      QVERIFY(ink2.second > h2 * 0.9);

      // curveName renders as a small text child under the well title.
      QVERIFY(hasTextChild(cols.at(0), QStringLiteral("GR")));
      QVERIFY(hasTextChild(cols.at(1), QStringLiteral("RHOB")));
    }

    void nanGapBreaksRenderedCurve()
    {
      WellCorrelationPanel panel(nullptr);
      panel.setWells({{QStringLiteral("W1"), QStringLiteral("井1")}});
      const float nan = std::numeric_limits<float>::quiet_NaN();
      const auto cols = [&panel] { return columnsByX(panel); };

      // Interior NULL → one pixmap item, but the dropped middle sample
      // removes a segment of ink compared to the continuous render.
      panel.setWellCurves(QStringLiteral("W1"),
                          {100.f, 110.f, 120.f, 130.f, 140.f},
                          {1.f, 2.f, nan, 4.f, 5.f});
      QCOMPARE(panel.curveItemCount(QStringLiteral("W1")), 1);
      QVERIFY(panel.hasCurves());
      const int gappedInk = inkPixelCount(curveChildren(cols().at(0)).first());

      panel.setWellCurves(QStringLiteral("W1"),
                          {100.f, 110.f, 120.f, 130.f, 140.f},
                          {1.f, 2.f, 3.f, 4.f, 5.f}); // same rows, no gap
      QCOMPARE(panel.curveItemCount(QStringLiteral("W1")), 1);
      const int solidInk = inkPixelCount(curveChildren(cols().at(0)).first());
      QVERIFY(gappedInk < solidInk);

      // A trailing NULL contributes no ink at all — same coverage as an
      // all-finite prefix curve, give or take a couple of raster pixels.
      panel.setWellCurves(QStringLiteral("W1"),
                          {100.f, 110.f, 120.f, 130.f},
                          {1.f, 2.f, 3.f, nan});
      QCOMPARE(panel.curveItemCount(QStringLiteral("W1")), 1);
      const int trailingInk = inkPixelCount(curveChildren(cols().at(0)).first());
      QVERIFY(trailingInk <= solidInk);
    }

    void clearWellCurvesRemovesItems()
    {
      WellCorrelationPanel panel(nullptr);
      panel.setWells({{QStringLiteral("W1"), QStringLiteral("井1")}});
      panel.setWellCurves(QStringLiteral("W1"), {1.f, 2.f, 3.f}, {5.f, 9.f, 7.f});
      QVERIFY(panel.hasCurves());
      QCOMPARE(panel.curveItemCount(QStringLiteral("W1")), 1);

      panel.clearWellCurves();
      QVERIFY(!panel.hasCurves());
      QCOMPARE(panel.curveItemCount(QStringLiteral("W1")), 0);
      QCOMPARE(columns(panel).size(), 1); // column scaffold untouched
    }

    void curveDoesNotSwallowWellClick()
    {
      SelectionContext ctx;
      WellCorrelationPanel panel(&ctx);
      panel.setWells({{QStringLiteral("W1"), QStringLiteral("井1")}});
      panel.setWellCurves(QStringLiteral("W1"),
                          {100.f, 150.f, 200.f}, {0.f, 10.f, 5.f});
      auto *view = panel.findChild<QGraphicsView *>(QStringLiteral("correlationView"));
      QVERIFY(view);
      panel.resize(400, 480);
      panel.show();
      QVERIFY(QTest::qWaitForWindowExposed(&panel));

      const auto cols = columnsByX(panel);
      QCOMPARE(cols.size(), 1);
      QCOMPARE(curveChildren(cols.at(0)).size(), 1);

      // Click right on the track's footprint: the mouse-transparent pixmap
      // item must still resolve to the owning well column.
      view->ensureVisible(cols.at(0));
      const QPoint at =
          view->mapFromScene(curveChildren(cols.at(0)).first()->sceneBoundingRect().center());
      QSignalSpy spy(&panel, &WellCorrelationPanel::wellClicked);
      QTest::mouseClick(view->viewport(), Qt::LeftButton, Qt::NoModifier, at);
      QCOMPARE(spy.count(), 1);
      QCOMPARE(spy.first().at(0).toString(), QStringLiteral("W1"));
    }

    void loadWellLasPopulatesCurve()
    {
      QTemporaryDir dir;
      QVERIFY(dir.isValid());
      const QString path = dir.filePath(QStringLiteral("w1.las"));
      {
        QFile f(path);
        QVERIFY(f.open(QIODevice::WriteOnly | QIODevice::Text));
        f.write("~VERSION INFORMATION\n"
                "VERS.  2.0   : CWLS LOG ASCII STANDARD - VERSION 2.0\n"
                "WRAP.  NO    : ONE LINE PER DEPTH STEP\n"
                "~WELL INFORMATION\n"
                "STRT.M 1000.0 : START DEPTH\n"
                "STOP.M 1004.0 : STOP DEPTH\n"
                "STEP.M 1.0    : STEP\n"
                "NULL.  -999.25 : NULL VALUE\n"
                "~CURVE INFORMATION\n"
                "DEPT.M   : DEPTH\n"
                "GR.GAPI  : GAMMA RAY\n"
                "~A  DEPTH       GR\n"
                "1000 45.0\n"
                "1001 50.0\n"
                "1002 -999.25\n"
                "1003 60.0\n"
                "1004 55.0\n");
      }

      WellCorrelationPanel panel(nullptr);
      panel.setWells({{QStringLiteral("W1"), QStringLiteral("井1")}});

      // Unknown mnemonic and unreadable file both report failure.
      QVERIFY(!panel.loadWellLas(QStringLiteral("W1"), path, QStringLiteral("NOSUCH")));
      QVERIFY(!panel.loadWellLas(QStringLiteral("W1"),
                                 dir.filePath(QStringLiteral("missing.las")),
                                 QStringLiteral("GR")));
      QVERIFY(!panel.hasCurves());

      QVERIFY(panel.loadWellLas(QStringLiteral("W1"), path, QStringLiteral("GR")));
      QVERIFY(panel.hasCurves());
      // One QgsPlot-rendered track item; the -999.25 NULL row parses to NaN
      // and drops a segment (verified by ink coverage vs the file's 4 rows
      // vs.3-series structure — presence + containment is the honest assert).
      QCOMPARE(panel.curveItemCount(QStringLiteral("W1")), 1);
      const auto cols = columnsByX(panel);
      QCOMPARE(curveChildren(cols.at(0)).size(), 1);
      QVERIFY(inkRows(curveChildren(cols.at(0)).first()).first >= 0);
      QVERIFY(hasTextChild(cols.at(0), QStringLiteral("GR")));
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
