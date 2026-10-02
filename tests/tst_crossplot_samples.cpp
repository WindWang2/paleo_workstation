#include "io/perffixtures.h"
#include "services/crossplotsamples.h"
#include "services/crossplotsources.h"
#include <QDataStream>
#include <QJsonDocument>
#include <QJsonObject>
#include <QTemporaryDir>
#include <QtTest>
#include <gdal.h>
using namespace paleo::crossplot;
class TestSamples : public QObject {
  Q_OBJECT
private slots:
  void depthAlignment();
  void rasterPairs();
  void projections();
  void attributeHorizon();
  void satrSource();
};
void TestSamples::depthAlignment() {
  Channel a{"GR", "API", "well-A", "ver-a", {0, 1, 2, 3, 4}, {1, 2, 3, 4, 5},
            100,  200,   true};
  Channel b{"PHI", "v/v", "well-A", "ver-b", {0, 2, 4}, {10, 20, 30}};
  Channel c = b;
  c.name = "SW";
  c.values = {0, 1, qQNaN()};
  auto r = CrossplotSamples::well({a, b, c});
  QVERIFY2(r.ok, qPrintable(r.error));
  QCOMPARE(r.samples.rows(), std::size_t(3));
  QCOMPARE(r.samples.rejected, 2);
  QCOMPARE(r.samples.values[4], 15.);
  QCOMPARE(r.samples.locations[1].depth, 1.);
  QCOMPARE(r.samples.parentVersionIds.size(), 2);
  paleo::cluster::Matrix matrix{3, r.samples.values};
  QVERIFY(paleo::cluster::kmeans(matrix).ok);
  b.depths = {1, 1, 2};
  QVERIFY(!CrossplotSamples::well({a, b}).ok);
  b.depths = {1, 2, 3};
  b.values = {10, 20, 30};
  r = CrossplotSamples::well({a, b});
  QVERIFY(r.ok);
  QCOMPARE(r.samples.rows(), std::size_t(3));
  QCOMPARE(r.samples.locations[0].depth, 1.);
  auto cancelled = CrossplotSamples::well({a, b}, {[] { return true; }, {}});
  QVERIFY(cancelled.cancelled);
  QVERIFY(cancelled.samples.values.empty());
}
void TestSamples::rasterPairs() {
  QTemporaryDir dir;
  GDALAllRegister();
  double gt[]{100, 2, 0, 200, 0, -2};
  for (int b = 0; b < 2; ++b) {
    auto path = dir.filePath(QString("%1.tif").arg(b));
    auto ds =
        GDALCreate(GDALGetDriverByName("GTiff"), path.toUtf8().constData(), 4,
                   3, 1, GDT_Float32, nullptr);
    QVERIFY(ds);
    QCOMPARE(GDALSetGeoTransform(ds, gt), CE_None);
    float data[12];
    for (int i = 0; i < 12; ++i)
      data[i] = float(i + b * 100);
    data[b] = -9999;
    auto band = GDALGetRasterBand(ds, 1);
    GDALSetRasterNoDataValue(band, -9999);
    QCOMPARE(
        GDALRasterIO(band, GF_Write, 0, 0, 4, 3, data, 4, 3, GDT_Float32, 0, 0),
        CE_None);
    GDALClose(ds);
  }
  auto r = CrossplotSamples::rasters(
      {{dir.filePath("0.tif"), "attribute", "v0", "layer0"},
       {dir.filePath("1.tif"), "horizon", "v1", "layer1"}});
  QVERIFY2(r.ok, qPrintable(r.error));
  QCOMPARE(r.samples.rows(), std::size_t(10));
  QCOMPARE(r.samples.rejected, 2);
  QCOMPARE(r.samples.locations[0].pixel, 2);
  QCOMPARE(r.samples.locations[0].x, 105.);
  QCOMPARE(r.samples.locations[0].y, 199.);
  for (std::size_t i = 0; i < r.samples.rows(); ++i)
    QCOMPARE(r.samples.values[i * 2 + 1] - r.samples.values[i * 2], 100.);
  // A georeferenced seismic attribute plane is accepted unchanged, >=3
  // channels.
  Plane a{"RMS", "va", r.samples.grid, std::vector<double>(12, 1)}, b = a,
                                                                    c = a;
  b.name = "TWT";
  c.name = "coherence";
  a.values[3] = qQNaN();
  auto p = CrossplotSamples::planes({a, b, c});
  QVERIFY(p.ok);
  QCOMPARE(p.samples.rows(), std::size_t(11));
  QCOMPARE(p.samples.locations[3].pixel, 4);
  b.grid.spatial = false;
  QVERIFY(!CrossplotSamples::planes({a, b}).ok);
}
void TestSamples::projections() {
  SampleSet s;
  s.names = {"X", "Y", "Z"};
  s.units = {"", "", ""};
  s.values = {0, 0, 0, 1, 1, 1, .5, .5, .5};
  s.locations = {{"a"}, {"b"}, {"a"}};
  auto f = CrossplotSamples::project(s, {});
  QCOMPARE(f.points.size(), 3);
  auto sel =
      CrossplotSamples::select(s, f, {{0, 0}, {.6, 0}, {.6, .6}, {0, .6}});
  QCOMPARE(sel.indices.size(), 2);
  QCOMPARE(sel.means[0], .25);
  QCOMPARE(sel.fraction, 2. / 3);
  QCOMPARE(sel.wellIds, QStringList{"a"});
  QCOMPARE(CrossplotSamples::nearest(f, {1, 1}, .02), 1);
  Axes axes;
  axes.z = 2;
  f = CrossplotSamples::project(s, axes);
  QVERIFY(f.is3d);
  QCOMPARE(f.points.size(), 3);
}
void TestSamples::attributeHorizon() {
  Grid grid;
  grid.rows = 1;
  grid.cols = 4;
  grid.spatial = true;
  Plane horizon{"TWT", "vh", grid, {0, 2, 4, qQNaN()}};
  AttributeSection a;
  a.plane = {"RMS", "va", {}, {20, 21, 22, 23, 10, 11, 12, 13, 0, 1, 2, 3}};
  a.plane.grid.rows = 3;
  a.plane.grid.cols = 4;
  a.stepMs = 2;
  a.traceXY = {{.5, -.5}, {1.5, -.5}, {2.5, -.5}, {3.5, -.5}};
  auto r = CrossplotSamples::attributeHorizon({a}, {horizon});
  QVERIFY2(r.ok, qPrintable(r.error));
  QCOMPARE(r.samples.rows(), std::size_t(3));
  QCOMPARE(r.samples.rejected, 1);
  QCOMPARE(r.samples.values[0], 0.);
  QCOMPARE(r.samples.values[2], 11.);
  QCOMPARE(r.samples.values[4], 22.);
  QCOMPARE(r.samples.locations[2].pixel, 2);
  a.stepMs = 0;
  QVERIFY(!CrossplotSamples::attributeHorizon({a}, {horizon}).ok);
}
void TestSamples::satrSource() {
  QTemporaryDir dir;
  const auto source = dir.filePath("survey.sgy");
  QCOMPARE(PerfFixtures::makeSyntheticSegy(source, 2, 4, 3), 8);
  const auto attrPath = dir.filePath("rms.sattr");
  QFile file(attrPath);
  QVERIFY(file.open(QIODevice::WriteOnly));
  QDataStream stream(&file);
  stream.setByteOrder(QDataStream::LittleEndian);
  stream.setFloatingPointPrecision(QDataStream::SinglePrecision);
  const auto json = QJsonDocument(QJsonObject{{"section", "il"},
                                              {"sectionIndex", 1000},
                                              {"attrId", "RMS"},
                                              {"sourceSgyPath", source}})
                        .toJson(QJsonDocument::Compact);
  stream.writeRawData("SATR", 4);
  stream << quint32(1) << qint32(4) << qint32(3) << quint32(json.size());
  stream.writeRawData(json.constData(), json.size());
  for (int row = 0; row < 3; ++row)
    for (int col = 0; col < 4; ++col)
      stream << float(row * 10 + col);
  file.close();
  AttributeSection section;
  QString error;
  QVERIFY2(CrossplotSources::attributeSection(attrPath, &section, &error),
           qPrintable(error));
  QCOMPARE(section.traceXY.size(), 4);
  // Existing survey fit accepts residuals up to 25m; 50m trace bins here.
  QVERIFY(std::abs(section.traceXY[0].x() - 500000) < 25.);
  qInfo("BASELINE satr_mapper_dy_m = %.9f", section.traceXY[0].y() - 4000000);
  QVERIFY(std::abs(section.traceXY[0].y() - 4000000) < 25.);
  QCOMPARE(section.stepMs, 2.);
  const auto horizonPath = dir.filePath("horizon.tif");
  GDALAllRegister();
  auto ds =
      GDALCreate(GDALGetDriverByName("GTiff"), horizonPath.toUtf8().constData(),
                 4, 1, 1, GDT_Float32, nullptr);
  QVERIFY(ds);
  double gt[]{499975, 50, 0, 4000050, 0, -100};
  QCOMPARE(GDALSetGeoTransform(ds, gt), CE_None);
  float time[]{0, 2, 4, -9999};
  auto band = GDALGetRasterBand(ds, 1);
  GDALSetRasterNoDataValue(band, -9999);
  QCOMPARE(
      GDALRasterIO(band, GF_Write, 0, 0, 4, 1, time, 4, 1, GDT_Float32, 0, 0),
      CE_None);
  GDALClose(ds);
  SourceSpec attr;
  attr.choice = {"a", "RMS", "attribute"};
  attr.path = attrPath;
  attr.versionId = "va";
  SourceSpec horizon;
  horizon.choice = {"h", "TWT", "raster"};
  horizon.path = horizonPath;
  horizon.versionId = "vh";
  horizon.timeHorizon = true;
  auto r = CrossplotSources::load({attr, horizon});
  QVERIFY2(r.ok, qPrintable(r.error));
  QCOMPARE(r.samples.rows(), std::size_t(3));
  QCOMPARE(r.samples.values[0], 20.);
  QCOMPARE(r.samples.values[2], 11.);
  QCOMPARE(r.samples.values[4], 2.);
  QFile truncated(attrPath);
  QVERIFY(truncated.open(QIODevice::WriteOnly));
  truncated.write("SATR");
  truncated.close();
  QVERIFY(!CrossplotSources::attributeSection(attrPath, &section, &error));
}
QTEST_APPLESS_MAIN(TestSamples)
#include "tst_crossplot_samples.moc"
