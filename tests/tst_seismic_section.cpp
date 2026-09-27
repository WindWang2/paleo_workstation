#include <QTest>
#include <QSignalSpy>
#include <QApplication>
#include <QDoubleSpinBox>
#include <QSlider>
#include <QToolButton>
#include <QLabel>
#include <cmath>

#include "domain/seismic/nicestep.h"
#include "domain/seismic/timedepthmodel.h"
#include "domain/seismic/sectionwellprojector.h"
#include "domain/seismic/sgyvolume.h"
#include "domain/seismic/sgysectionbuilder.h"
#include "ui/seismicsection/seismicsectioncanvas.h"
#include "ui/seismicsection/seismicsectiondockwidget.h"

using namespace seismic;

class TestSeismicSection : public QObject {
    Q_OBJECT

private slots:
    void testNiceStepCalculations() {
        // Range 100 with 10 ticks -> nice step is 10
        double step = NiceStep::Calculate(100.0, 10);
        QCOMPARE(step, 10.0);

        // Range 250 with 8 ticks -> raw step 31.25 -> nice step is 50
        step = NiceStep::Calculate(250.0, 8);
        QCOMPARE(step, 50.0);

        // Range 12 with 6 ticks -> raw step 2.0 -> nice step is 2
        step = NiceStep::Calculate(12.0, 6);
        QCOMPARE(step, 2.0);

        // GenerateTicks
        const auto ticks = NiceStep::GenerateTicks(0.0, 1000.0, 100.0, 900.0, 10, QStringLiteral("%.0f"));
        QVERIFY(!ticks.empty());

        // Check monotonicity
        for (std::size_t i = 1; i < ticks.size(); ++i) {
            QVERIFY(ticks[i].value >= ticks[i - 1].value);
            QVERIFY(ticks[i].pixelPos >= ticks[i - 1].pixelPos);
        }

        // Check bounds
        for (const auto &tk : ticks) {
            QVERIFY(tk.value >= -1e-4 && tk.value <= 1000.0 + 1e-4);
            QVERIFY(tk.pixelPos >= 100.0 - 1e-4 && tk.pixelPos <= 900.0 + 1e-4);
            if (tk.isMajor) {
                QVERIFY(!tk.label.isEmpty());
            }
        }
    }

    void testTimeDepthModelConstantVelocity() {
        TimeDepthModel model(2500.0); // 2500 m/s
        QVERIFY(model.isValid());
        QCOMPARE(model.velocity(), 2500.0);

        // Depth 1250m -> twt = 2000 * 1250 / 2500 = 1000 ms
        const double twt = model.DepthToTwtMs(1250.0);
        QVERIFY(std::abs(twt - 1000.0) < 1e-3);

        // Inverse: 1000 ms -> 1250m
        const double depth = model.TwtMsToDepth(1000.0);
        QVERIFY(std::abs(depth - 1250.0) < 1e-3);

        // Change velocity to 3000 m/s
        model.setVelocity(3000.0);
        QCOMPARE(model.velocity(), 3000.0);
        // Depth 1500m -> twt = 2000 * 1500 / 3000 = 1000 ms
        QVERIFY(std::abs(model.DepthToTwtMs(1500.0) - 1000.0) < 1e-3);
    }

    void testTimeDepthModelPiecewiseCalibration() {
        TimeDepthModel model;
        std::vector<TdPoint> points = {
            {0.0, 0.0},
            {1000.0, 800.0},
            {2000.0, 1500.0},
            {3000.0, 2100.0}
        };
        model.setPoints(points);
        QVERIFY(model.hasCheckshots());

        // Exact points
        QVERIFY(std::abs(model.DepthToTwtMs(0.0) - 0.0) < 1e-3);
        QVERIFY(std::abs(model.DepthToTwtMs(1000.0) - 800.0) < 1e-3);
        QVERIFY(std::abs(model.DepthToTwtMs(2000.0) - 1500.0) < 1e-3);
        QVERIFY(std::abs(model.DepthToTwtMs(3000.0) - 2100.0) < 1e-3);

        // Linear interpolation: midpoints
        // At 500m -> 400 ms
        QVERIFY(std::abs(model.DepthToTwtMs(500.0) - 400.0) < 1e-3);
        // At 1500m -> (800 + 1500)/2 = 1150 ms
        QVERIFY(std::abs(model.DepthToTwtMs(1500.0) - 1150.0) < 1e-3);

        // Inverse check
        QVERIFY(std::abs(model.TwtMsToDepth(400.0) - 500.0) < 1e-3);
        QVERIFY(std::abs(model.TwtMsToDepth(1150.0) - 1500.0) < 1e-3);

        // Extrapolation beyond top and bottom
        QVERIFY(model.DepthToTwtMs(-100.0) < 0.0);
        QVERIFY(model.DepthToTwtMs(3500.0) > 2100.0);
    }

    void testSectionWellProjectorPolylineGeometry() {
        // Polyline L-shape: (0,0) -> (1000, 0) -> (1000, 1000)
        // Segment 1: length 1000, along X
        // Segment 2: length 1000, along Y
        // Total length = 2000m
        const std::vector<glm::dvec2> polyline = {
            {0.0, 0.0},
            {1000.0, 0.0},
            {1000.0, 1000.0}
        };
        const std::vector<double> vDist = {0.0, 1000.0, 2000.0};
        const std::vector<double> traceDist = {0.0, 500.0, 1000.0, 1500.0, 2000.0}; // 5 traces

        std::vector<SectionWellInfo> wells;

        // Well 1: at (500, 30) -> projected to (500, 0) on segment 1. Offset = 30m, cumDist = 500m.
        SectionWellInfo w1;
        w1.wellId = QStringLiteral("w1");
        w1.wellName = QStringLiteral("W-1");
        w1.surfaceX = 500.0;
        w1.surfaceY = 30.0;
        WellTopItem t1;
        t1.topName = QStringLiteral("D61");
        t1.tvd = 1000.0;
        w1.tops.push_back(t1);
        wells.push_back(w1);

        // Well 2: at (1000, 750) -> exactly on segment 2. Offset = 0m, cumDist = 1750m.
        SectionWellInfo w2;
        w2.wellId = QStringLiteral("w2");
        w2.wellName = QStringLiteral("W-2");
        w2.surfaceX = 1000.0;
        w2.surfaceY = 750.0;
        wells.push_back(w2);

        // Well 3: at (5000, 5000) -> far away (> 4000m)
        SectionWellInfo w3;
        w3.wellId = QStringLiteral("w3");
        w3.wellName = QStringLiteral("W-3");
        w3.surfaceX = 5000.0;
        w3.surfaceY = 5000.0;
        wells.push_back(w3);

        TimeDepthModel tdModel(2500.0);
        const auto projected = SectionWellProjector::ProjectWells(
            polyline, vDist, traceDist, wells, 200.0, tdModel);

        QCOMPARE(static_cast<int>(projected.size()), 3);

        // Verify Well 1
        QVERIFY(projected[0].isWithinBuffer);
        QVERIFY(std::abs(projected[0].cumulativeDistanceM - 500.0) < 1.0);
        QVERIFY(std::abs(std::abs(projected[0].offsetDistanceM) - 30.0) < 1.0);
        QVERIFY(std::abs(projected[0].tracePosition - 1.0) < 0.1); // Trace index 1 in 0..4
        // Check calibrated top twt: 1000m / 2500m/s * 2000 = 800ms
        QVERIFY(std::abs(projected[0].tops[0].twtMs - 800.0) < 1e-3);

        // Verify Well 2
        QVERIFY(projected[1].isWithinBuffer);
        QVERIFY(std::abs(projected[1].cumulativeDistanceM - 1750.0) < 1.0);
        QVERIFY(std::abs(projected[1].offsetDistanceM) < 1.0);
        QVERIFY(std::abs(projected[1].tracePosition - 3.5) < 0.1); // Between trace 3 (1500) and 4 (2000)

        // Verify Well 3
        QVERIFY(!projected[2].isWithinBuffer);
    }

    void testSeismicSectionCanvasLifecycleAndRecolor() {
        SeismicSectionCanvas canvas;
        canvas.resize(800, 600);

        // Synthetic 40 traces x 100 samples
        SgySliceImage slice;
        slice.width = 40;
        slice.height = 100;
        slice.valueMin = -1.0f;
        slice.valueMax = 1.0f;
        slice.values.resize(40 * 100, 0.0f);

        // Populate pattern
        for (int y = 0; y < 100; ++y) {
            for (int x = 0; x < 40; ++x) {
                if (x == 0 && y == 0) {
                    slice.values[y * 40 + x] = std::numeric_limits<float>::quiet_NaN();
                } else {
                    slice.values[y * 40 + x] = std::sin(x * 0.2f) * std::cos(y * 0.1f);
                }
            }
        }

        canvas.setSectionData(slice, 2.0f, 0.0f);
        QVERIFY(canvas.hasData());
        QCOMPARE(canvas.traceCount(), 40);
        QCOMPARE(canvas.sampleCount(), 100);
        QCOMPARE(canvas.sampleIntervalMs(), 2.0f);
        QCOMPARE(canvas.startSampleMs(), 0.0);

        // Colormap changes
        canvas.setColorMap(SectionColorMapType::Grayscale);
        QCOMPARE(canvas.colorMap(), SectionColorMapType::Grayscale);

        canvas.setColorMap(SectionColorMapType::Rainbow);
        QCOMPARE(canvas.colorMap(), SectionColorMapType::Rainbow);

        canvas.setColorMap(SectionColorMapType::RedWhiteBlue);
        QCOMPARE(canvas.colorMap(), SectionColorMapType::RedWhiteBlue);

        // Gain & Contrast
        canvas.setGain(2.0f);
        QCOMPARE(canvas.gain(), 2.0f);

        canvas.setContrast(1.8f);
        QCOMPARE(canvas.contrast(), 1.8f);

        // Units
        canvas.setVerticalUnit(SectionVerticalUnit::DepthMeters);
        QCOMPARE(canvas.verticalUnit(), SectionVerticalUnit::DepthMeters);

        // Coordinate transforms roundtrip
        const double t0 = 10.0;
        const double px = canvas.traceToPixelX(t0);
        const double backTrace = canvas.pixelToTrace(px);
        QVERIFY(std::abs(backTrace - t0) < 1e-4);

        const double timeVal = 50.0;
        const double py = canvas.timeToPixelY(timeVal);
        const double backTime = canvas.pixelToTime(py);
        QVERIFY(std::abs(backTime - timeVal) < 1e-4);

        // Zoom operations
        const double z0 = canvas.zoomX();
        canvas.zoomIn();
        QVERIFY(canvas.zoomX() > z0);
        canvas.zoomOut();
        canvas.fitToWindow();

        // Hover test
        QSignalSpy hoverSpy(&canvas, &SeismicSectionCanvas::traceHovered);
        const QPoint centerPt(qRound(canvas.traceToPixelX(20.0)), qRound(canvas.timeToPixelY(50.0)));
        QMouseEvent moveEvent(QEvent::MouseMove, centerPt, centerPt, Qt::NoButton, Qt::NoButton, Qt::NoModifier);
        QApplication::sendEvent(&canvas, &moveEvent);

        QVERIFY(hoverSpy.count() >= 1);
        const QList<QVariant> args = hoverSpy.takeFirst();
        QCOMPARE(args.at(0).toInt(), 20); // trace index
        QVERIFY(std::abs(args.at(1).toDouble() - 50.0) < 1.0); // TWT ms
    }

    void testSeismicSectionDockWidgetIntegration() {
        SeismicSectionDockWidget dock(QStringLiteral("测试剖面"));
        dock.resize(900, 650);

        auto *canvas = dock.canvas();
        QVERIFY(canvas != nullptr);

        // Verify toolbar controls
        auto *btnZoomIn = dock.findChild<QToolButton *>(QStringLiteral("btnSectionZoomIn"));
        auto *btnZoomOut = dock.findChild<QToolButton *>(QStringLiteral("btnSectionZoomOut"));
        auto *btnFit = dock.findChild<QToolButton *>(QStringLiteral("btnSectionFit"));
        auto *btnUnit = dock.findChild<QToolButton *>(QStringLiteral("btnSectionUnitToggle"));
        auto *sliderGain = dock.findChild<QSlider *>(QStringLiteral("sliderSectionGain"));
        auto *spinGain = dock.findChild<QDoubleSpinBox *>(QStringLiteral("spinSectionGain"));
        auto *lblCoords = dock.findChild<QLabel *>(QStringLiteral("lblSectionCoordinates"));

        QVERIFY(btnZoomIn && btnZoomOut && btnFit && btnUnit && sliderGain && spinGain && lblCoords);

        // Test Gain linkage
        spinGain->setValue(2.5);
        QCOMPARE(canvas->gain(), 2.5f);
        QCOMPARE(sliderGain->value(), 250);

        sliderGain->setValue(150);
        QCOMPARE(canvas->gain(), 1.5f);
        QCOMPARE(spinGain->value(), 1.5);

        // Unit toggle
        QCOMPARE(canvas->verticalUnit(), SectionVerticalUnit::TwoWayTimeMs);
        btnUnit->click();
        QCOMPARE(canvas->verticalUnit(), SectionVerticalUnit::DepthMeters);
        btnUnit->click();
        QCOMPARE(canvas->verticalUnit(), SectionVerticalUnit::TwoWayTimeMs);

        // Direct feed data
        SgySliceImage slice;
        slice.width = 30;
        slice.height = 80;
        slice.valueMin = -0.5f;
        slice.valueMax = 0.5f;
        slice.values.resize(30 * 80, 0.1f);

        dock.setLineTitle(QStringLiteral("剖面 A-A'"));
        dock.setSectionData(slice, 4.0f, 100.0f);
        QVERIFY(canvas->hasData());
        QCOMPARE(canvas->traceCount(), 30);
        QCOMPARE(canvas->sampleCount(), 80);
        QCOMPARE(canvas->startSampleMs(), 100.0);
        QCOMPARE(canvas->sampleIntervalMs(), 4.0f);
    }
};

QTEST_MAIN(TestSeismicSection)
#include "tst_seismic_section.moc"
