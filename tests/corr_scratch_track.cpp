#include <QtTest>
#include <QApplication>
#include <QElapsedTimer>
#include <QImage>
#include <QPair>
#include <QVector>

#include <cmath>
#include <limits>

#include "../src/ui/correlation/correlationtrack.h"

// Scratch TDD harness for subtask A: CorrelationTrack::render — the
// QgsLineChartPlot port of correlationpanel.cpp::addCurveItems. The frozen
// wave-base data/logic methods get spot checks; the meat is render behavior:
// ink on the section-shared depth window, flatten offset translation, NaN
// gap series semantics, per-series track color, the decimation budget and
// the empty/degenerate guards. Same ink probes the panel tests use, run
// straight on the returned QImage.
class TestCorrTrack : public QObject
{
    Q_OBJECT

  private:
    static float fnan() { return std::numeric_limits<float>::quiet_NaN(); }

    // {first,last} inked row (alpha > 8); {-1,-1} when fully transparent.
    static QPair<int, int> inkRows(const QImage &img)
    {
        int first = -1, last = -1;
        for (int y = 0; y < img.height(); ++y)
            for (int x = 0; x < img.width(); ++x)
            {
                if (qAlpha(img.pixel(x, y)) <= 8)
                    continue;
                if (first < 0)
                    first = y;
                last = y;
                break;
            }
        return {first, last};
    }

    // {first,last} inked column — proves x-axis scaling spans valueRange().
    static QPair<int, int> inkCols(const QImage &img)
    {
        int first = -1, last = -1;
        for (int x = 0; x < img.width(); ++x)
            for (int y = 0; y < img.height(); ++y)
            {
                if (qAlpha(img.pixel(x, y)) <= 8)
                    continue;
                if (first < 0)
                    first = x;
                last = x;
                break;
            }
        return {first, last};
    }

    static int inkPixelCount(const QImage &img)
    {
        int n = 0;
        for (int y = 0; y < img.height(); ++y)
            for (int x = 0; x < img.width(); ++x)
                if (qAlpha(img.pixel(x, y)) > 8)
                    ++n;
        return n;
    }

    // Smooth monotone depth ramp over [d0, d1] with a sine value wiggle in
    // [0, 10] (hits both x extremes, no NaNs).
    static CorrelationTrack wiggleTrack(float d0, float d1, int samples = 201,
                                        const QString &mn = QStringLiteral("GR"),
                                        const QString &un = QStringLiteral("GAPI"))
    {
        QVector<float> d, v;
        d.reserve(samples);
        v.reserve(samples);
        for (int i = 0; i < samples; ++i)
        {
            const float t = float(i) / float(samples - 1);
            d.append(d0 + t * (d1 - d0));
            v.append(5.f + 5.f * std::sin(t * 6.2831853f * 1.5f));
        }
        CorrelationTrack t(mn, un);
        t.setData(d, v);
        return t;
    }

  private slots:
    // --- frozen data/logic spot checks ---------------------------------------

    void seriesCountSplitsOnNaN()
    {
        CorrelationTrack solid(QStringLiteral("GR"), QString(),
                                {100.f, 110.f, 120.f, 130.f, 140.f},
                                {1.f, 2.f, 3.f, 4.f, 5.f});
        QVERIFY(!solid.isEmpty());
        QCOMPARE(solid.seriesCount(), 1);

        // Interior NULLs split contiguous finite runs — one series per run.
        CorrelationTrack one(QStringLiteral("GR"), QString(),
                             {100.f, 110.f, 120.f, 130.f, 140.f, 150.f},
                             {1.f, 2.f, fnan(), 4.f, 5.f, 6.f});
        QCOMPARE(one.seriesCount(), 2);
        CorrelationTrack two(QStringLiteral("GR"), QString(),
                             {100.f, 110.f, 120.f, 130.f, 140.f, 150.f, 160.f},
                             {1.f, 2.f, fnan(), 3.f, fnan(), 4.f, 5.f});
        QCOMPARE(two.seriesCount(), 2); // runs [1,2] and [4,5]; lone 3 draws nothing
        CorrelationTrack three(QStringLiteral("GR"), QString(),
                               {100.f, 110.f, 120.f, 130.f, 140.f, 150.f, 160.f, 170.f, 180.f},
                               {1.f, 2.f, fnan(), 3.f, 4.f, fnan(), 5.f, 6.f, 7.f});
        QCOMPARE(three.seriesCount(), 3);

        // NULL depths break runs exactly like NULL values.
        CorrelationTrack nullDepth(QStringLiteral("GR"), QString(),
                                   {100.f, 110.f, 120.f},
                                   {fnan(), 2.f, 3.f});
        QCOMPARE(nullDepth.seriesCount(), 1);

        // A single finite sample draws no line — no series from lone runs.
        CorrelationTrack lone(QStringLiteral("GR"), QString(),
                              {100.f, 110.f, 120.f}, {fnan(), 7.f, fnan()});
        QVERIFY(lone.isEmpty());
        QCOMPARE(lone.seriesCount(), 0);
        CorrelationTrack pair(QStringLiteral("GR"), QString(), {100.f, 110.f},
                              {7.f, 9.f});
        QCOMPARE(pair.seriesCount(), 1);
    }

    void valueRangeIgnoresNulls()
    {
        CorrelationTrack empty;
        const QPair<float, float> z = empty.valueRange();
        QCOMPARE(z.first, 0.f);
        QCOMPARE(z.second, 0.f);

        CorrelationTrack t(QStringLiteral("GR"), QString(),
                           {100.f, 110.f, 120.f, 130.f},
                           {9.f, fnan(), 3.f, 5.f});
        const QPair<float, float> r = t.valueRange();
        QCOMPARE(r.first, 3.f);
        QCOMPARE(r.second, 9.f);

        CorrelationTrack flat(QStringLiteral("GR"), QString(),
                              {100.f, 110.f}, {5.f, 5.f});
        const QPair<float, float> f = flat.valueRange();
        QCOMPARE(f.first, 5.f);
        QCOMPARE(f.second, 5.f);
    }

    void captionJoinsUnitWithMiddleDot()
    {
        CorrelationTrack withUnit(QStringLiteral("GR"), QStringLiteral("GAPI"));
        QCOMPARE(withUnit.caption(), QStringLiteral("GR · GAPI"));
        CorrelationTrack bare(QStringLiteral("GR"));
        QCOMPARE(bare.caption(), QStringLiteral("GR"));
        CorrelationTrack explicitEmpty(QStringLiteral("DT"), QString());
        QCOMPARE(explicitEmpty.caption(), QStringLiteral("DT"));
    }

    void defaultsAreTheFrozenTokens()
    {
        CorrelationTrack t;
        QCOMPARE(t.color(), QColor(QStringLiteral("#5D6E80"))); // DESIGN.md text-muted
        QCOMPARE(t.preferredWidth(), 90.0);
        t.setData({1.f, 2.f, 3.f}, {4.f, 5.f, 6.f});
        QCOMPARE(t.depths(), QVector<float>({1.f, 2.f, 3.f}));
        QCOMPARE(t.values(), QVector<float>({4.f, 5.f, 6.f}));
        QCOMPARE(t.sampleCount(), qsizetype(3));
    }

    // --- render guards ---------------------------------------------------------

    void emptyTrackRendersNullImage()
    {
        CorrelationTrack neverFed;
        QVERIFY(neverFed.isEmpty());
        QVERIFY(neverFed.render(90, 400, 1000.f, 2000.f).isNull());

        CorrelationTrack oneSample(QStringLiteral("GR"), QString(),
                                   {100.f}, {5.f});
        QVERIFY(oneSample.isEmpty());
        QVERIFY(oneSample.render(90, 400, 100.f, 200.f).isNull());

        CorrelationTrack allNull(QStringLiteral("GR"), QString(),
                                 {100.f, 110.f, 120.f}, {fnan(), fnan(), fnan()});
        QVERIFY(allNull.isEmpty());
        QVERIFY(allNull.render(90, 400, 100.f, 200.f).isNull());
    }

    void degenerateSizeClampsToEight()
    {
        const CorrelationTrack t = wiggleTrack(1000.f, 2000.f);
        const QImage img = t.render(2, 3, 1000.f, 2000.f);
        QVERIFY(!img.isNull());
        QCOMPARE(img.width(), 8);
        QCOMPARE(img.height(), 8);
        QVERIFY(inkRows(img).first >= 0); // still inks a real curve
    }

    // --- render body -----------------------------------------------------------

    void renderInksCurveAcrossValueAndDepthRange()
    {
        const CorrelationTrack t = wiggleTrack(1000.f, 2000.f);
        const QImage img = t.render(90, 400, 1000.f, 2000.f);
        QVERIFY(!img.isNull());
        QCOMPARE(img.size(), QSize(90, 400));

        // The sine hits both 0 and 10, so the curve spans the full value
        // (x) range, and the samples span the full depth (y) window.
        const QPair<int, int> rows = inkRows(img);
        QVERIFY2(rows.first >= 0, "track rendered no ink");
        QVERIFY(rows.first < 400 * 0.1);
        QVERIFY(rows.second > 400 * 0.9);
        const QPair<int, int> cols = inkCols(img);
        QVERIFY(cols.first >= 0);
        QVERIFY(cols.first < 90 * 0.1);
        QVERIFY(cols.second > 90 * 0.9);
        QVERIFY(inkPixelCount(img) > 200);
    }

    void shallowCurveInksOnlyTopOfSharedWindow()
    {
        // Port of the panel test: on the shared 1000–1700 axis, a curve with
        // samples 1000–1200 inks only the top band of its strip.
        const CorrelationTrack t = wiggleTrack(1000.f, 1200.f, 41);
        const QImage img = t.render(60, 400, 1000.f, 1700.f);
        const QPair<int, int> ink = inkRows(img);
        QVERIFY2(ink.first >= 0, "shallow track rendered no ink");
        QVERIFY(ink.first < 400 * 0.15);
        QVERIFY(ink.second < 400 * 0.45);
    }

    void deepCurveInksOnlyBottomOfSharedWindow()
    {
        const CorrelationTrack t = wiggleTrack(1500.f, 1700.f, 41);
        const QImage img = t.render(60, 400, 1000.f, 1700.f);
        const QPair<int, int> ink = inkRows(img);
        QVERIFY2(ink.first >= 0, "deep track rendered no ink");
        QVERIFY(ink.first > 400 * 0.55);
        QVERIFY(ink.second > 400 * 0.9);
    }

    void depthOffsetShiftsInkUpward()
    {
        // Samples at real 500–700 inside the fixed window [0, 700].
        CorrelationTrack t(QStringLiteral("GR"), QString(),
                           {500.f, 600.f, 700.f}, {8.f, 2.f, 9.f});

        const QImage unshifted = t.render(60, 400, 0.f, 700.f);
        const QPair<int, int> low = inkRows(unshifted);
        QVERIFY2(low.first >= 0, "unshifted render has no ink");
        QVERIFY(low.first > 400 * 0.6);  // 500/700 ≈ 71% down
        QVERIFY(low.second > 400 * 0.9);

        // depthOffset = 250 → display depths 250–450 → the ink rises to the
        // middle band, moved up by exactly offset/span of the strip height.
        const QImage shifted = t.render(60, 400, 0.f, 700.f, 250.f);
        const QPair<int, int> mid = inkRows(shifted);
        QVERIFY2(mid.first >= 0, "flattened render has no ink");
        QVERIFY(mid.first > 400 * 0.30);
        QVERIFY(mid.first < 400 * 0.40);
        QVERIFY(mid.second > 400 * 0.55);
        QVERIFY(mid.second < 400 * 0.68);
        QVERIFY(mid.second < low.first); // the ink actually moved up
    }

    void flattenAlignsDeepWellWithShallowWell()
    {
        // Two wells 500 m apart. On the flattened display axis [0, 700] both
        // markers map to 0, so both curves ink the same top band — the
        // section correlates them on the flattened datum.
        const CorrelationTrack w1 = wiggleTrack(1000.f, 1200.f, 41);
        const CorrelationTrack w2 = wiggleTrack(1500.f, 1700.f, 41);
        const QImage i1 = w1.render(60, 400, 0.f, 700.f, 1000.f);
        const QImage i2 = w2.render(60, 400, 0.f, 700.f, 1500.f);
        const QPair<int, int> ink1 = inkRows(i1);
        const QPair<int, int> ink2 = inkRows(i2);
        QVERIFY(ink1.first >= 0);
        QVERIFY(ink2.first >= 0);
        QVERIFY(ink1.first < 400 * 0.15);
        QVERIFY(ink1.second < 400 * 0.45);
        QVERIFY(ink2.first < 400 * 0.15);
        QVERIFY(ink2.second < 400 * 0.45);
    }

    void nanGapUnderInksContinuousRender()
    {
        // Same depth rows; the interior NULL drops a polyline segment, so
        // the gapped render strictly under-inks the continuous one.
        const QVector<float> d = {100.f, 110.f, 120.f, 130.f, 140.f};
        CorrelationTrack gapped(QStringLiteral("GR"), QString(), d,
                                {1.f, 2.f, fnan(), 4.f, 5.f});
        CorrelationTrack solid(QStringLiteral("GR"), QString(), d,
                               {1.f, 2.f, 3.f, 4.f, 5.f});
        const int gappedInk = inkPixelCount(gapped.render(60, 300, 100.f, 140.f));
        const int solidInk = inkPixelCount(solid.render(60, 300, 100.f, 140.f));
        QVERIFY2(gappedInk > 0, "gapped track rendered no ink");
        QVERIFY(gappedInk < solidInk);

        // A trailing NULL adds no ink beyond the all-finite prefix.
        CorrelationTrack trailing(QStringLiteral("GR"), QString(),
                                  {100.f, 110.f, 120.f, 130.f},
                                  {1.f, 2.f, 3.f, fnan()});
        const int trailingInk =
            inkPixelCount(trailing.render(60, 300, 100.f, 140.f));
        QVERIFY(trailingInk <= solidInk);
    }

    void everySeriesInheritsTrackColor()
    {
        // Pure red on a NaN-split curve: BOTH series must stroke in the
        // track color — a series left on a default symbol would carry green
        // or blue channels. Premultiplied ARGB keeps pure red at (a,0,0).
        CorrelationTrack t(QStringLiteral("GR"), QString(),
                           {100.f, 110.f, 120.f, 130.f, 140.f, 150.f},
                           {1.f, 5.f, fnan(), 2.f, 8.f, 4.f});
        t.setColor(QColor(255, 0, 0));
        const QImage img = t.render(60, 300, 100.f, 150.f);
        QVERIFY(!img.isNull());
        QCOMPARE(t.seriesCount(), 2);

        int strong = 0;
        bool topInk = false, bottomInk = false;
        for (int y = 0; y < img.height(); ++y)
            for (int x = 0; x < img.width(); ++x)
            {
                const QRgb px = img.pixel(x, y);
                if (qAlpha(px) <= 60)
                    continue;
                ++strong;
                QVERIFY2(qGreen(px) <= 2 && qBlue(px) <= 2,
                         "ink outside the track color leaked in");
                if (y < img.height() / 3)
                    topInk = true;
                if (y > 2 * img.height() / 3)
                    bottomInk = true;
            }
        QVERIFY(strong > 20);
        QVERIFY(topInk);    // series 0 (above the gap) rendered
        QVERIFY(bottomInk); // series 1 (below the gap) rendered
    }

    void hundredThousandSamplesRenderUnder300ms()
    {
        QVector<float> d(100000), v(100000);
        for (int i = 0; i < 100000; ++i)
        {
            d[i] = 1000.f + float(i) * 0.01f;
            v[i] = 5.f + 5.f * std::sin(float(i) * 0.05f);
        }
        CorrelationTrack t(QStringLiteral("GR"), QStringLiteral("GAPI"));
        t.setData(d, v);

        QElapsedTimer timer;
        timer.start();
        const QImage img = t.render(90, 600, 1000.f, 2000.f);
        const qint64 ms = timer.elapsed();
        QVERIFY(!img.isNull());
        QVERIFY2(ms < 300,
                 qPrintable(QStringLiteral("render of 100k samples took %1ms").arg(ms)));
        QVERIFY(inkRows(img).first >= 0);
    }
};

int main(int argc, char *argv[])
{
    if (qgetenv("QT_QPA_PLATFORM").isEmpty())
        qputenv("QT_QPA_PLATFORM", "offscreen");
    QApplication app(argc, argv);
    TestCorrTrack tc;
    return QTest::qExec(&tc, argc, argv);
}

#include "corr_scratch_track.moc"
