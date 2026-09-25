#include <QtTest>
#include <QApplication>
#include <QImage>
#include <QPainter>

#include <cmath>
#include <limits>

#include "../src/ui/correlation/depthruler.h"

// 连井剖面 subtask D — DepthRuler product-level scratch tests.
// Pure tick math is asserted without rasterizing; paint() is smoke-checked
// by painting straight into a QImage (offscreen) and looking for ink where
// the DESIGN.md token colors must land.
class TestCorrScratchRuler : public QObject
{
    Q_OBJECT

  private:
    static constexpr double kFtPerM = 3.280839895;

    // Any ink (alpha > 8) inside the rectangle [x0,y0]..[x1,y1].
    static bool hasInk(const QImage &img, int x0, int y0, int x1, int y1)
    {
      for (int y = y0; y <= y1; ++y)
        for (int x = x0; x <= x1; ++x)
          if (qAlpha(img.pixel(x, y)) > 8)
            return true;
      return false;
    }

    static int inkCount(const QImage &img)
    {
      int n = 0;
      for (int y = 0; y < img.height(); ++y)
        for (int x = 0; x < img.width(); ++x)
          if (qAlpha(img.pixel(x, y)) > 8)
            ++n;
      return n;
    }

    // Some pixel in the rect matches `c` within `tol` per channel.
    static bool hasColor(const QImage &img, int x0, int y0, int x1, int y1,
                         const QColor &c, int tol = 12)
    {
      for (int y = y0; y <= y1; ++y)
        for (int x = x0; x <= x1; ++x)
        {
          const QColor p = img.pixelColor(x, y);
          if (qAlpha(p.rgb()) < 200)
            continue;
          if (std::abs(p.red() - c.red()) <= tol &&
              std::abs(p.green() - c.green()) <= tol &&
              std::abs(p.blue() - c.blue()) <= tol)
            return true;
        }
      return false;
    }

    // Paints the ruler into a fresh image with a small margin so tick labels
    // that straddle y=0 / y=height are not clipped by the image edge.
    static QImage renderRuler(DepthRuler &r, int w, int h)
    {
      QImage img(w, h, QImage::Format_ARGB32_Premultiplied);
      img.fill(Qt::transparent);
      QPainter p(&img);
      p.translate(2.0, 14.0);
      r.paint(&p, nullptr, nullptr);
      p.end();
      return img;
    }

  private slots:
    // --- niceStep: 1/2/5 × 10^k ------------------------------------------------

    void niceStepBoundaries()
    {
      QCOMPARE(DepthRuler::niceStep(1.0), 0.2);    // span 1 m → 5 majors
      QCOMPARE(DepthRuler::niceStep(3.0), 0.5);
      QCOMPARE(DepthRuler::niceStep(5.0), 1.0);
      QCOMPARE(DepthRuler::niceStep(7.0), 1.0);    // 7 intervals, still ≤ 9 ticks
      QCOMPARE(DepthRuler::niceStep(8.9), 1.0);    // hardest 1×10^k case: 8.9 intervals
      QCOMPARE(DepthRuler::niceStep(9.0), 2.0);
      QCOMPARE(DepthRuler::niceStep(10.0), 2.0);
      QCOMPARE(DepthRuler::niceStep(100.0), 20.0);
      QCOMPARE(DepthRuler::niceStep(1000.0), 200.0);
      QCOMPARE(DepthRuler::niceStep(100.0, 1), 100.0);
      QCOMPARE(DepthRuler::niceStep(100.0, 3), 50.0);
    }

    void niceStepGuardsInvalidInput()
    {
      QCOMPARE(DepthRuler::niceStep(0.0), 1.0);
      QCOMPARE(DepthRuler::niceStep(-4.0), 1.0);
      QCOMPARE(DepthRuler::niceStep(std::numeric_limits<double>::quiet_NaN()), 1.0);
      QCOMPARE(DepthRuler::niceStep(10.0, 0), 1.0);
      QCOMPARE(DepthRuler::niceStep(10.0, -2), 1.0);
    }

    void niceStepIsOneTwoFiveAcrossMagnitudes()
    {
      for (double span = 1e-6; span < 1e7; span *= 1.7)
      {
        const double s = DepthRuler::niceStep(span);
        QVERIFY(s > 0.0);
        double mant = s / std::pow(10.0, std::floor(std::log10(s)));
        if (mant > 9.9)                    // 10·10^k normalizes to the next mag
          mant /= 10.0;
        const bool ok = qAbs(mant - 1.0) < 1e-6 || qAbs(mant - 2.0) < 1e-6 ||
                        qAbs(mant - 5.0) < 1e-6;
        QVERIFY2(ok, qPrintable(QStringLiteral("span=%1 step=%2 mant=%3")
                                    .arg(span).arg(s).arg(mant)));
      }
    }

    // --- readability guardrail: ≤ 9 major ticks for any range -----------------

    void majorTicksReadableAcrossMagnitudes()
    {
      DepthRuler r;
      // 4+ magnitudes of span, several offsets inside the span, incl. the
      // adversarial 20400 (step 2·10^k bucket: a 3.5 threshold yields 11).
      const QList<double> spans = {1.0, 2.7, 8.9, 8.99, 10.0, 47.0, 100.0,
                                   299.9, 1000.0, 2718.28, 10000.0, 20400.0};
      for (double span : spans)
      {
        for (double frac : {0.0, 0.31, 0.79})
        {
          r.setRange(static_cast<float>(frac * span), static_cast<float>((frac + 1.0) * span));
          const auto majors = r.majorDepths();
          const double step = r.majorStep();

          QVERIFY2(majors.size() <= 9,
                   qPrintable(QStringLiteral("span=%1 frac=%2 n=%3 step=%4")
                                  .arg(span).arg(frac).arg(majors.size()).arg(step)));
          // Upper bound is the guardrail; lower bound is what the 1/2/5
          // ladder proves at target 6 (span/step ∈ [3.6, 9) in every
          // bucket): at least 3 labeled ticks, typically 5–7.
          QVERIFY(majors.size() >= 3);
          QVERIFY(std::is_sorted(majors.cbegin(), majors.cend()));

          const double lo = r.rangeMin(), hi = r.rangeMax();
          const double tol = step * 1e-6;
          for (double v : majors)
          {
            QVERIFY(v >= lo - tol);
            QVERIFY(v <= hi + tol);
          }
          for (int i = 1; i < majors.size(); ++i)
            QVERIFY(qAbs(majors.at(i) - majors.at(i - 1) - step) < step * 1e-9);
        }
      }
    }

    void minorTicksAreFifthsWithoutMajorCollision()
    {
      DepthRuler r;
      r.setRange(1005.0f, 1105.0f);           // step 20 → minor 4
      const double minor = r.majorStep() / 5.0;
      QCOMPARE(minor, 4.0);

      const auto majors = r.majorDepths();
      const auto minors = r.minorDepths();
      QVERIFY(!minors.isEmpty());

      for (double v : minors)
      {
        // on the 1/5 lattice…
        const double idx = v / minor;
        QVERIFY2(qAbs(idx - std::round(idx)) < 1e-6,
                 qPrintable(QString::number(v)));
        // …but never on a major lattice point (5·minor).
        for (double m : majors)
          QVERIFY2(std::abs(m - v) > minor * 0.5,
                   qPrintable(QStringLiteral("minor %1 collides with major %2")
                                  .arg(v).arg(m)));
      }
      // 4 minors between two adjacent majors, so at most 4 per gap.
      QVERIFY(minors.size() <= 4 * majors.size());
      QVERIFY(std::is_sorted(minors.cbegin(), minors.cend()));
    }

    // --- ft display conversion -------------------------------------------------

    void ftModeTicksAreNaturalFeet()
    {
      DepthRuler r;
      r.setRange(0.0f, 100.0f);
      QCOMPARE(r.majorStep(), 20.0);          // meters first…

      r.setUnit(DepthRuler::Unit::Feet);     // …then re-niced in ft space
      QCOMPARE(r.unit(), DepthRuler::Unit::Feet);
      QCOMPARE(r.majorStep(), 50.0);         // 100 m = 328.08 ft → step 50 ft

      // Zoomed to a 2 m window around 1000 m: 1 ft steps, and 3280 ft
      // (the ft reading of 1000 m) is itself a labeled tick.
      r.setRange(999.0f, 1001.0f);
      const auto majors = r.majorDepths();
      QCOMPARE(r.majorStep(), 1.0);
      QVERIFY(majors.contains(3280.0));
      for (double v : majors)
      {
        const QString label = r.labelText(v);
        QVERIFY2(!label.contains(QLatin1Char('.')),
                 qPrintable(label));          // natural integers, no ".0"
        QCOMPARE(label, QString::number(v, 'f', 0));
      }

      // Wide window: 1000–2000 m → 3280.8–6561.7 ft → 500 ft majors.
      r.setRange(1000.0f, 2000.0f);
      QCOMPARE(r.majorStep(), 500.0);
      const auto wide = r.majorDepths();
      QVERIFY(wide.contains(3500.0));
      QVERIFY(wide.contains(6500.0));
      QVERIFY(wide.size() <= 9);
      for (double v : wide)
        QVERIFY(!r.labelText(v).contains(QLatin1Char('.')));

      // yForDepth stays in SOURCE (m) units regardless of display unit.
      r.setHeight(380.0);
      QCOMPARE(r.yForDepth(1000.0f), qreal(0.0));
      QCOMPARE(r.yForDepth(1500.0f), qreal(190.0));
      QCOMPARE(r.yForDepth(2000.0f), qreal(380.0));
    }

    // --- mapping & labels -------------------------------------------------------

    void yForDepthMapsEndpointsAndIsMonotonic()
    {
      DepthRuler r;
      r.setRange(100.0f, 200.0f);
      r.setHeight(380.0);
      QCOMPARE(r.height(), qreal(380.0));
      QCOMPARE(r.yForDepth(100.0f), qreal(0.0));
      QCOMPARE(r.yForDepth(200.0f), qreal(380.0));
      QCOMPARE(r.yForDepth(150.0f), qreal(190.0));

      qreal prev = -1.0;
      for (int i = 0; i <= 20; ++i)
      {
        const qreal y = r.yForDepth(100.0f + i * 5.0f);
        QVERIFY(y > prev);
        QVERIFY(y >= 0.0);
        QVERIFY(y <= 380.0);
        prev = y;
      }
    }

    void labelTextFormatsByStep()
    {
      DepthRuler r;
      r.setRange(1000.0f, 2000.0f);           // step 200 — integers
      QCOMPARE(r.labelText(600.0), QStringLiteral("600"));
      QCOMPARE(r.labelText(1234.56), QStringLiteral("1235"));
      QCOMPARE(r.labelText(-0.0), QStringLiteral("0"));   // no "-0"
      for (double v : r.majorDepths())
        QVERIFY2(!r.labelText(v).contains(QLatin1Char('.')),
                 qPrintable(r.labelText(v)));

      r.setRange(0.0f, 1.0f);                 // step 0.2 — one decimal kept
      QCOMPARE(r.majorStep(), 0.2);
      QCOMPARE(r.labelText(0.6000000001), QStringLiteral("0.6"));
      QCOMPARE(r.labelText(0.0), QStringLiteral("0.0"));   // precision kept…
      QCOMPARE(r.labelText(-0.0), QStringLiteral("0.0"));  // …but never "-0.0"
      QCOMPARE(r.labelText(-0.04), QStringLiteral("0.0")); // rounds-to-zero: no sign
      for (double v : r.majorDepths())
        QCOMPARE(r.labelText(v), QString::number(v, 'f', 1));
    }

    void setRangeRepairsReversedAndDegenerate()
    {
      DepthRuler r;
      r.setRange(200.0f, 100.0f);             // reversed → auto-corrected
      QCOMPARE(r.rangeMin(), 100.0f);
      QCOMPARE(r.rangeMax(), 200.0f);

      r.setRange(50.0f, 50.0f);               // degenerate → epsilon span
      QVERIFY(r.rangeMax() > r.rangeMin());
      QVERIFY(!r.majorDepths().isEmpty());

      r.setRange(std::numeric_limits<float>::quiet_NaN(), 100.0f);
      QCOMPARE(r.rangeMin(), 0.0f);           // NaN falls back to a safe window
      QCOMPARE(r.rangeMax(), 100.0f);
      QVERIFY(r.majorStep() > 0.0);
    }

    // --- geometry bookkeeping ---------------------------------------------------

    void boundingRectTracksGridWidth()
    {
      DepthRuler r;
      r.setRange(1000.0f, 1100.0f);
      r.setHeight(300.0);
      QCOMPARE(r.boundingRect().width(), qreal(46.0));   // label column only

      r.setGridWidth(200.0);
      QCOMPARE(r.gridWidth(), qreal(200.0));
      QCOMPARE(r.boundingRect().width(), qreal(246.0));  // grid reaches into the section

      r.setGridVisible(false);
      QVERIFY(!r.gridVisible());
      QCOMPARE(r.boundingRect().width(), qreal(46.0));

      r.setGridVisible(true);
      QCOMPARE(r.boundingRect().width(), qreal(246.0));

      // Vertical: labels straddle the first/last tick, so the rect must
      // extend beyond [0, height] while still covering the mapped range.
      QVERIFY(r.boundingRect().top() < 0.0);
      QVERIFY(r.boundingRect().bottom() > 300.0);
      QVERIFY(r.boundingRect().contains(QPointF(0.0, 0.0)));
      QVERIFY(r.boundingRect().contains(QPointF(246.0, 300.0)));
    }

    // --- paint smoke: no crash, ink in the right zones ---------------------------

    void paintSmokeRendersInk()
    {
      DepthRuler r;
      r.setRange(1005.0f, 1105.0f);           // first label below the unit tag
      r.setHeight(300.0);
      r.setGridWidth(200.0);

      const QImage img = renderRuler(r, 260, 340);
      QVERIFY(inkCount(img) > 100);

      // Unit tag ("m", 8pt muted) in the top-left of the label column.
      QVERIFY(hasInk(img, 3, 14, 20, 26));

      // Major tick at 1020 → y=45 → row 59: 8 px mark at the right edge of
      // the label column, in text-muted (#5D6E80), 1 px pen.
      QVERIFY(hasColor(img, 40, 57, 48, 61, QColor(0x5D, 0x6E, 0x80)));

      // Minor tick at 1008 → y=9 → row 23: only the last 4 px carry ink.
      QVERIFY(hasColor(img, 44, 21, 48, 25, QColor(0x5D, 0x6E, 0x80)));
      QVERIFY(!hasColor(img, 39, 21, 43, 25, QColor(0x5D, 0x6E, 0x80)));

      // Grid line at the same major tick runs into the section in border
      // color (#DFE5EC). (Stacking under the columns is the panel's z.)
      QVERIFY(hasColor(img, 100, 57, 104, 61, QColor(0xDF, 0xE5, 0xEC)));

      // Labels: ink left of the ticks around the 1020 row.
      QVERIFY(hasInk(img, 18, 53, 38, 65));

      // Grid off → nothing painted right of the label column.
      r.setGridVisible(false);
      const QImage noGrid = renderRuler(r, 260, 340);
      QVERIFY(inkCount(noGrid) > 100);
      QVERIFY(!hasInk(noGrid, 50, 0, 258, 339));

      // ft smoke: same pipeline, unit tag renders, still no crash.
      r.setGridVisible(true);
      r.setUnit(DepthRuler::Unit::Feet);
      const QImage ft = renderRuler(r, 260, 340);
      QVERIFY(inkCount(ft) > 100);
      QVERIFY(hasInk(ft, 3, 14, 24, 26));
    }
};

int main(int argc, char *argv[])
{
  if (qgetenv("QT_QPA_PLATFORM").isEmpty())
    qputenv("QT_QPA_PLATFORM", "offscreen");
  QApplication app(argc, argv);
  TestCorrScratchRuler t;
  return QTest::qExec(&t, argc, argv);
}

#include "corr_scratch_ruler.moc"
