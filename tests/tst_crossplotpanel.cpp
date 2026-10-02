#include "services/crossplotsamples.h"
#include "ui/crossplot/crossplotpanel.h"
#include "ui/paleotheme.h"
#include <QComboBox>
#include <QElapsedTimer>
#include <QListWidget>
#include <QPushButton>
#include <QSignalSpy>
#include <QTemporaryDir>
#include <QtTest>
using namespace paleo::crossplot;
class TestPanel : public QObject {
  Q_OBJECT
private slots:
  void intents();
  void density();
};
void TestPanel::intents() {
  PaleoTheme::pinRenderEnvironment();
  CrossplotPanel panel;
  panel.resize(900, 480);
  panel.show();
  panel.setSources(
      {{"a", "GR", "well"}, {"b", "RHOB", "well"}, {"c", "PHI", "well"}});
  panel.setDimensions({"GR", "RHOB", "PHI"});
  QSignalSpy axes(&panel, &CrossplotPanel::axesRequested),
      load(&panel, &CrossplotPanel::samplesRequested),
      run(&panel, &CrossplotPanel::classifyRequested),
      lasso(&panel, &CrossplotPanel::lassoRequested),
      point(&panel, &CrossplotPanel::pointRequested);
  panel.findChild<QComboBox *>("crossplotX")->setCurrentIndex(2);
  QCOMPARE(axes.count(), 1);
  QCOMPARE(panel.axes().x, 2);
  auto *sources = panel.findChild<QListWidget *>("crossplotSources");
  sources->item(0)->setSelected(true);
  sources->item(1)->setSelected(true);
  panel.findChild<QPushButton *>("crossplotLoad")->click();
  QCOMPARE(load.count(), 1);
  QCOMPARE(load[0][0].toStringList().size(), 2);
  panel.findChild<QPushButton *>("crossplotRun")->click();
  QCOMPARE(run.count(), 1);
  panel.setClassified(true, {3, 4});
  QVERIFY(panel.findChild<QPushButton *>("crossplotWrite")->isEnabled());
  panel.setBusy(true);
  QVERIFY(!panel.findChild<QPushButton *>("crossplotWrite")->isEnabled());
  panel.setBusy(false);
  SampleSet samples;
  samples.names = {"a", "b"};
  samples.units = {"", ""};
  samples.values = {0, 0, 1, 1, .5, .5};
  samples.locations.resize(3);
  auto frame = CrossplotSamples::project(samples, {});
  panel.setFrame(frame);
  auto *canvas = panel.canvas();
  auto r = canvas->plotRect();
  QTest::mouseClick(canvas, Qt::LeftButton, Qt::NoModifier,
                    r.center().toPoint());
  QCOMPARE(point.count(), 1);
  const QPoint start = (r.topLeft() + QPointF(5, 5)).toPoint(),
               end = (r.bottomRight() - QPointF(5, 5)).toPoint();
  QTest::mousePress(canvas, Qt::LeftButton, Qt::ShiftModifier, start);
  QTest::mouseMove(canvas, end);
  QTest::mouseRelease(canvas, Qt::LeftButton, Qt::ShiftModifier, end);
  QCOMPARE(lasso.count(), 1);
  auto vertices = qvariant_cast<QVector<QPointF>>(lasso[0][0]);
  auto selected = CrossplotSamples::select(samples, frame, vertices);
  QCOMPARE(selected.indices.size(), 1);
  QCOMPARE(selected.means[0], .5);
  panel.setSelection(selected, samples.names);
}
void TestPanel::density() {
  CrossplotPanel panel;
  panel.resize(1000, 550);
  panel.show();
  SampleSet samples;
  samples.names = {"X", "Y", "Z"};
  samples.units = {"", "", ""};
  samples.values.reserve(300000);
  samples.locations.resize(100000);
  for (int i = 0; i < 100000; ++i) {
    samples.values.push_back(i % 1000);
    samples.values.push_back(i / 1000);
    samples.values.push_back(i % 23);
  }
  panel.setDimensions(samples.names);
  panel.setSources(
      {{"a", "VSH", "well"}, {"b", "PHI", "well"}, {"c", "RMS", "raster"}});
  QElapsedTimer timer;
  timer.start();
  auto frame = CrossplotSamples::project(samples, {});
  panel.setFrame(frame);
  panel.canvas()->grab();
  double ms = timer.nsecsElapsed() / 1e6;
  QCOMPARE(frame.points.size(), 100000);
  QVERIFY(!frame.density.isEmpty());
  qint64 total = 0;
  for (int n : frame.density)
    total += n;
  QCOMPARE(total, 100000);
  qInfo("BASELINE crossplot_100k_project_paint_ms = %.3f", ms);
  timer.restart();
  Axes a{2, 0, 1, 70, 30};
  frame = CrossplotSamples::project(samples, a);
  panel.setFrame(frame);
  panel.canvas()->grab();
  qInfo("BASELINE crossplot_100k_axis_switch_3d_ms = %.3f",
        timer.nsecsElapsed() / 1e6);
  QVERIFY(frame.is3d);
  std::vector<int> labels(samples.rows());
  for (std::size_t i = 0; i < labels.size(); ++i)
    labels[i] = int(i % 8);
  frame = CrossplotSamples::project(samples, a, labels);
  QVERIFY(!frame.densityClass.isEmpty());
  panel.setFrame(frame);
  panel.setClassified(true, QVector<qint64>(8, 12500));

  if (auto path = qEnvironmentVariable("PALEO_CROSSPLOT_SCREENSHOT");
      !path.isEmpty())
    QVERIFY(panel.grab().save(path));
}
QTEST_MAIN(TestPanel)
#include "tst_crossplotpanel.moc"
