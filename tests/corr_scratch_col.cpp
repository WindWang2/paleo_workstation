#include <QtTest>
#include <QApplication>
#include <QGraphicsLineItem>
#include <QGraphicsPathItem>
#include <QGraphicsPixmapItem>
#include <QGraphicsScene>
#include <QGraphicsSimpleTextItem>

#include "../src/ui/correlation/correlationwellcolumn.h"
#include "../src/ui/correlation/correlationtrack.h"

// A2 scratch: CorrelationWellColumn scene composition — tracks side-by-side
// under a header band, per-track captions/separators, QImage cache, and
// null-image tolerance while CorrelationTrack::render is still a stub.
class TestCorrWellColumn : public QObject
{
  Q_OBJECT

  private:
    static CorrelationTrack grTrack(float base = 0.f)
    {
      return CorrelationTrack(QStringLiteral("GR"), QStringLiteral("GAPI"),
                              {1000.f, 1100.f, 1200.f, 1300.f, 1400.f},
                              {base + 1.f, base + 8.f, base + 4.f, base + 9.f, base + 5.f});
    }

    static CorrelationTrack rhobTrack()
    {
      return CorrelationTrack(QStringLiteral("RHOB"), QString(),
                              {1200.f, 1300.f, 1400.f},
                              {2.0f, 2.4f, 2.2f});
    }

    static QList<QGraphicsPixmapItem *> pixmapChildren(QGraphicsItem *column)
    {
      QList<QGraphicsPixmapItem *> out;
      for (QGraphicsItem *c : column->childItems())
        if (auto *pm = qgraphicsitem_cast<QGraphicsPixmapItem *>(c))
          out << pm;
      return out;
    }

    static QList<QGraphicsSimpleTextItem *> textChildren(QGraphicsItem *column)
    {
      QList<QGraphicsSimpleTextItem *> out;
      for (QGraphicsItem *c : column->childItems())
        if (auto *t = qgraphicsitem_cast<QGraphicsSimpleTextItem *>(c))
          out << t;
      return out;
    }

    static QList<QGraphicsLineItem *> lineChildren(QGraphicsItem *column)
    {
      QList<QGraphicsLineItem *> out;
      for (QGraphicsItem *c : column->childItems())
        if (auto *l = qgraphicsitem_cast<QGraphicsLineItem *>(c))
          out << l;
      return out;
    }

    // render() is a stub (null image) until subtask A lands, so the expected
    // pixmap count is measured from the very render() the column calls —
    // self-calibrating: 0 now, N once real rendering arrives.
    static int renderableTracks(const CorrelationWellColumn &col, int w, int h,
                                float dMin, float dMax, float dOff = 0.f)
    {
      int n = 0;
      for (const QString &mn : col.mnemonics())
        if (!col.track(mn)->render(w, h, dMin, dMax, dOff).isNull())
          ++n;
      return n;
    }

  private slots:
    void trackModelOrderAndDedup()
    {
      CorrelationWellColumn col(QStringLiteral("W1"), QStringLiteral("井1"));
      QVERIFY(col.addTrack(grTrack()));
      QVERIFY(col.addTrack(rhobTrack()));
      QCOMPARE(col.trackCount(), 2);
      QVERIFY(col.hasTrack(QStringLiteral("GR")));
      QVERIFY(col.hasTrack(QStringLiteral("RHOB")));
      QCOMPARE(col.mnemonics().join(u','), QStringLiteral("GR,RHOB"));

      // Same mnemonic replaces in place — order and count preserved.
      CorrelationTrack replacement = grTrack(50.f);
      replacement.setColor(QColor(QStringLiteral("#123456")));
      QVERIFY(col.addTrack(replacement));
      QCOMPARE(col.trackCount(), 2);
      QCOMPARE(col.mnemonics().join(u','), QStringLiteral("GR,RHOB"));
      QCOMPARE(col.track(QStringLiteral("GR"))->color(), QColor(QStringLiteral("#123456")));

      // Empty mnemonics are rejected; unknown removes report failure.
      QVERIFY(!col.addTrack(CorrelationTrack(QString(), QString())));
      QVERIFY(!col.removeTrack(QStringLiteral("NOSUCH")));
      QVERIFY(col.removeTrack(QStringLiteral("GR")));
      QCOMPARE(col.mnemonics().join(u','), QStringLiteral("RHOB"));
      QVERIFY(!col.track(QStringLiteral("GR")));
      QCOMPARE(col.track(QStringLiteral("RHOB"))->sampleCount(), 3);

      col.clearTracks();
      QCOMPARE(col.trackCount(), 0);
      QVERIFY(!col.hasTrack(QStringLiteral("RHOB")));
      QVERIFY(col.mnemonics().isEmpty());
    }

    void rebuildBuildsColumnItemWithRoles()
    {
      QGraphicsScene scene;
      CorrelationWellColumn col(QStringLiteral("W7"), QStringLiteral("井7"));
      QVERIFY(col.addTrack(grTrack()));

      QGraphicsPathItem *item =
          col.rebuild(&scene, QPointF(10, 20), 300.f, 1000.f, 2000.f, false);
      QVERIFY(item);
      QCOMPARE(item->data(CorrelationItemRoles::WellId).toString(), QStringLiteral("W7"));
      QCOMPARE(item->data(CorrelationItemRoles::Highlight).toBool(), false);

      // Chrome: 4px rounded white column, border pen 1.0 #DFE5EC.
      const QPen pen = item->pen();
      QCOMPARE(pen.color(), QColor(QStringLiteral("#DFE5EC")));
      QCOMPARE(pen.widthF(), 1.0);
      QCOMPARE(item->brush().color(), QColor(QStringLiteral("#FFFFFF")));
      const QRectF br = item->path().boundingRect();
      QCOMPARE(br.topLeft(), QPointF(10, 20));
      QCOMPARE(br.width(), col.width()); // 1 track → trackWidth
      QCOMPARE(br.height(), col.headerHeight() + 300.0);
      QVERIFY(item->path().contains(QPointF(10 + 2, 20 + 2)));

      // Null scene is tolerated.
      QVERIFY(!col.rebuild(nullptr, QPointF(0, 0), 300.f, 1000.f, 2000.f, false));
    }

    void highlightSwitchesPenToPrimary()
    {
      QGraphicsScene scene;
      CorrelationWellColumn col(QStringLiteral("W1"), QStringLiteral("井1"));
      col.addTrack(grTrack());

      auto *plain = col.rebuild(&scene, QPointF(0, 0), 200.f, 1000.f, 2000.f, false);
      QCOMPARE(plain->pen().color(), QColor(QStringLiteral("#DFE5EC")));
      QCOMPARE(plain->data(CorrelationItemRoles::Highlight).toBool(), false);

      auto *hot = col.rebuild(&scene, QPointF(0, 0), 200.f, 1000.f, 2000.f, true);
      QCOMPARE(hot->pen().color(), QColor(QStringLiteral("#1B73D0"))); // selected token
      QCOMPARE(hot->pen().widthF(), 2.0);
      QCOMPARE(hot->data(CorrelationItemRoles::Highlight).toBool(), true);
    }

    void rebuildComposesTrackItems()
    {
      QGraphicsScene scene;
      CorrelationWellColumn col(QStringLiteral("W1"), QStringLiteral("井1"));
      col.addTrack(grTrack());
      col.addTrack(rhobTrack()); // no unit → caption is exactly the mnemonic

      const int stripW = qRound(col.trackWidth());
      const int stripH = 300;
      const int expectedPix = renderableTracks(col, stripW, stripH, 1000.f, 2000.f);

      auto *column = col.rebuild(&scene, QPointF(0, 0), stripH, 1000.f, 2000.f, false);
      QVERIFY(column);

      // Header text child: well name, 9pt body token, text color.
      bool headerFound = false;
      const auto texts = textChildren(column);
      for (const auto *t : texts)
      {
        if (t->text() != QStringLiteral("井1"))
          continue;
        headerFound = true;
        QCOMPARE(t->font().pointSize(), 9);
        QCOMPARE(t->brush().color(), QColor(QStringLiteral("#24303E")));
        // Centered inside the header band.
        const QRectF cb = t->sceneBoundingRect();
        QVERIFY(cb.top() >= column->path().boundingRect().top() - 0.5);
        QVERIFY(cb.bottom() <= column->path().boundingRect().top() + col.headerHeight() + 0.5);
      }
      QVERIFY(headerFound);

      // N caption children (one per track) + 1 header + 1 unit tag = 4.
      // Lead adjudication: the mnemonic renders as its OWN text item
      // (legacy tests walk text children for exactly "GR") and the unit
      // becomes a smaller sibling tag.
      QCOMPARE(texts.size(), 4);
      const auto hasCaption = [&texts](const QString &s) {
        for (const auto *t : texts)
          if (t->text() == s)
            return true;
        return false;
      };
      QVERIFY(hasCaption(QStringLiteral("GR")));  // mnemonic-only caption item
      QVERIFY(hasCaption(QStringLiteral("GAPI"))); // unit as its own 7pt tag
      QVERIFY(hasCaption(QStringLiteral("RHOB"))); // exactly the mnemonic, no unit
      for (const auto *t : texts)
        if (t->text() != QStringLiteral("井1") && t->text() != QStringLiteral("GAPI"))
        {
          QCOMPARE(t->font().pointSize(), 8); // mnemonic captions: label token
          QCOMPARE(t->brush().color(), QColor(QStringLiteral("#5D6E80")));
        }

      // N-1 separator lines between N tracks, border token.
      const auto lines = lineChildren(column);
      QCOMPARE(lines.size(), 1);
      QCOMPARE(lines.first()->pen().color(), QColor(QStringLiteral("#DFE5EC")));
      const QRectF colRect = column->path().boundingRect();
      const QLineF l = lines.first()->line();
      QCOMPARE(l.p1().x(), colRect.left() + col.trackWidth()); // between track 0 and 1
      QCOMPARE(l.p1().y(), colRect.top() + col.headerHeight());
      QCOMPARE(l.p2().y(), colRect.bottom());

      // Pixmap children exactly match the renderable tracks (0 while the
      // CorrelationTrack render stub returns null; N after subtask A lands —
      // the test needs no change either way).
      const auto pix = pixmapChildren(column);
      QCOMPARE(pix.size(), expectedPix);
      if (expectedPix > 0)
      {
        // Each strip: full trackWidth × bodyHeight inside the column, with
        // owner/mnemonic roles and click transparency for well resolution.
        int i = 0;
        for (const auto *p : pix)
        {
          const QRectF want(colRect.left() + i * col.trackWidth(),
                            colRect.top() + col.headerHeight(),
                            col.trackWidth(), colRect.height() - col.headerHeight());
          const QRectF got = p->sceneBoundingRect();
          QVERIFY(QRectF(want).adjusted(-1.0, -1.0, 1.0, 1.0).contains(got));
          QCOMPARE(p->acceptedMouseButtons(), Qt::MouseButtons(Qt::NoButton));
          QCOMPARE(p->data(CorrelationItemRoles::CurveOwner).toString(), QStringLiteral("W1"));
          QVERIFY(p->data(CorrelationItemRoles::TrackMnemonic).isValid());
          ++i;
        }
      }
    }

    void captionsStayInsideColumnFrame()
    {
      QGraphicsScene scene;
      CorrelationWellColumn col(QStringLiteral("W1"), QStringLiteral("井1"));
      col.addTrack(grTrack());
      col.addTrack(rhobTrack());

      auto *column = col.rebuild(&scene, QPointF(30, 40), 260.f, 1000.f, 2000.f, false);
      const QRectF frame = column->sceneBoundingRect();
      for (const auto *t : textChildren(column))
      {
        if (t->text() == QStringLiteral("井1"))
          continue;
        // Track captions sit at the top of their strip, inside the column
        // (stroke tolerance like the legacy panel test).
        QVERIFY(frame.adjusted(-1.5, -1.5, 1.5, 1.5).contains(t->sceneBoundingRect()));
        QVERIFY(t->sceneBoundingRect().top() >= frame.top() + col.headerHeight() - 1.5);
      }
    }

    void emptyColumnKeepsOneStripFrame()
    {
      QGraphicsScene scene;
      CorrelationWellColumn col(QStringLiteral("W9"), QStringLiteral("空井"));
      QCOMPARE(col.trackCount(), 0);
      QCOMPARE(col.width(), 90.0); // default trackWidth

      auto *column = col.rebuild(&scene, QPointF(5, 5), 120.f, 0.f, 100.f, false);
      QVERIFY(column);
      QCOMPARE(column->path().boundingRect().width(), 90.0);
      QCOMPARE(column->path().boundingRect().height(), col.headerHeight() + 120.0);
      QCOMPARE(pixmapChildren(column).size(), 0);
      QCOMPARE(lineChildren(column).size(), 0);
      // Well name header still present; no captions.
      QCOMPARE(textChildren(column).size(), 1);
      QCOMPARE(textChildren(column).first()->text(), QStringLiteral("空井"));
    }

    void widthMathFollowsTrackWidth()
    {
      CorrelationWellColumn col(QStringLiteral("W1"), QStringLiteral("井1"));
      QCOMPARE(col.trackWidth(), 90.0);
      col.setTrackWidth(60.0);
      QCOMPARE(col.trackWidth(), 60.0);
      QCOMPARE(col.width(), 60.0); // empty column: one strip
      col.addTrack(grTrack());
      col.addTrack(rhobTrack());
      QCOMPARE(col.width(), 120.0); // 2 × 60
      col.setTrackWidth(5.0);       // degenerate width refused
      QCOMPARE(col.trackWidth(), 60.0);

      col.setHeaderHeight(34.0);
      QCOMPARE(col.headerHeight(), 34.0);
      QGraphicsScene scene;
      auto *column = col.rebuild(&scene, QPointF(0, 0), 100.f, 0.f, 10.f, false);
      QCOMPARE(column->path().boundingRect().height(), 134.0);
    }

    void cacheHitsWithoutChangesAndInvalidatesOnDataChange()
    {
      CorrelationWellColumn col(QStringLiteral("W1"), QStringLiteral("井1"));
      col.addTrack(grTrack());
      col.addTrack(rhobTrack());

      QGraphicsScene scene;
      col.rebuild(&scene, QPointF(0, 0), 200.f, 1000.f, 2000.f, false);
      col.resetCacheStats();
      QCOMPARE(col.cacheHits(), 0);

      // Second rebuild, nothing changed → both tracks served from cache.
      col.rebuild(&scene, QPointF(0, 0), 200.f, 1000.f, 2000.f, false);
      QCOMPARE(col.cacheHits(), 2);
      // Axis or geometry changes must miss (key covers size + depth window).
      col.rebuild(&scene, QPointF(0, 0), 201.f, 1000.f, 2000.f, false);
      QCOMPARE(col.cacheHits(), 2);
      col.rebuild(&scene, QPointF(0, 0), 201.f, 1000.f, 2000.f, false);
      QCOMPARE(col.cacheHits(), 4);

      // setData path: mutating the track through track() must invalidate.
      col.resetCacheStats();
      col.rebuild(&scene, QPointF(0, 0), 201.f, 1000.f, 2000.f, false);
      QCOMPARE(col.cacheHits(), 2);
      col.track(QStringLiteral("GR"))->setData({1000.f, 1050.f, 1100.f}, {1.f, 5.f, 3.f});
      col.rebuild(&scene, QPointF(0, 0), 201.f, 1000.f, 2000.f, false);
      QCOMPARE(col.cacheHits(), 3); // GR missed; RHOB still hit
      col.rebuild(&scene, QPointF(0, 0), 201.f, 1000.f, 2000.f, false);
      QCOMPARE(col.cacheHits(), 5); // re-cached under the new fingerprint

      // addTrack replace path invalidates too.
      col.addTrack(grTrack(20.f));
      col.rebuild(&scene, QPointF(0, 0), 201.f, 1000.f, 2000.f, false);
      QCOMPARE(col.cacheHits(), 6); // RHOB only
      col.rebuild(&scene, QPointF(0, 0), 201.f, 1000.f, 2000.f, false);
      QCOMPARE(col.cacheHits(), 8);

      // depthOffset participates in the key (flatten-on-marker re-renders).
      col.rebuild(&scene, QPointF(0, 0), 201.f, 1000.f, 2000.f, false, 12.5f);
      QCOMPARE(col.cacheHits(), 8);
      col.rebuild(&scene, QPointF(0, 0), 201.f, 1000.f, 2000.f, false, 12.5f);
      QCOMPARE(col.cacheHits(), 10);

      // invalidateCache drops everything.
      col.invalidateCache();
      QCOMPARE(col.cacheHits(), 0);
      col.rebuild(&scene, QPointF(0, 0), 201.f, 1000.f, 2000.f, false, 12.5f);
      QCOMPARE(col.cacheHits(), 0);
      col.rebuild(&scene, QPointF(0, 0), 201.f, 1000.f, 2000.f, false, 12.5f);
      QCOMPARE(col.cacheHits(), 2);
    }

    void nullImageTracksStillDrawChrome()
    {
      // CorrelationTrack::render is currently a stub returning a null image:
      // the column must skip the pixmap but keep captions and separators.
      QGraphicsScene scene;
      CorrelationWellColumn col(QStringLiteral("W3"), QStringLiteral("井3"));
      col.addTrack(grTrack());
      col.addTrack(rhobTrack());

      auto *column = col.rebuild(&scene, QPointF(0, 0), 300.f, 1000.f, 2000.f, false);
      QVERIFY(column);
      QVERIFY(!column->childItems().isEmpty());          // header/captions live
      QCOMPARE(lineChildren(column).size(), 1);          // separator still drawn
      bool grCaption = false;
      for (const auto *t : textChildren(column))
        if (t->text() == QStringLiteral("GR"))
          grCaption = true;
      QVERIFY(grCaption);
      if (renderableTracks(col, qRound(col.trackWidth()), 300, 1000.f, 2000.f) == 0)
        QCOMPARE(pixmapChildren(column).size(), 0);

      // Rebuilding again (cache now holds null images) stays safe + hits.
      col.resetCacheStats();
      col.rebuild(&scene, QPointF(0, 0), 300.f, 1000.f, 2000.f, false);
      QCOMPARE(col.cacheHits(), 2);
    }
};

int main(int argc, char *argv[])
{
  if (qgetenv("QT_QPA_PLATFORM").isEmpty())
    qputenv("QT_QPA_PLATFORM", "offscreen");
  QApplication app(argc, argv);
  TestCorrWellColumn tc;
  return QTest::qExec(&tc, argc, argv);
}

#include "corr_scratch_col.moc"
