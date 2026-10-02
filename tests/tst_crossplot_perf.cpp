#include "catalog/datacatalog.h"
#include "io/lasparser.h"
#include "metadata/paleoprojectstore.h"
#include "services/crossplotsamples.h"
#include "services/crossplotsources.h"
#include "services/faciesclassificationservice.h"
#include <QDir>
#include <QElapsedTimer>
#include <QFileInfo>
#include <QTemporaryDir>
#include <QtTest>
#include <cmath>
#include <gdal.h>
using namespace paleo::crossplot;
class TestPerf : public QObject {
  Q_OBJECT
private slots:
  void syntheticBudgets();
  void realArea();
};
static SampleSet samples(int n) {
  SampleSet s;
  s.names = {"VSH", "PHI", "RMS"};
  s.units = {"v/v", "v/v", "amplitude"};
  s.values.reserve(std::size_t(n) * 3);
  s.locations.resize(n);
  s.grid.rows = n / 1000;
  s.grid.cols = 1000;
  s.grid.spatial = true;
  for (int i = 0; i < n; ++i) {
    double c = i % 8;
    for (int j = 0; j < 3; ++j)
      s.values.push_back(c * 3 + double((i * 13 + j * 17) % 101) / 1000);
    s.locations[i].pixel = i;
    s.locations[i].hasXY = true;
  }
  return s;
}
void TestPerf::syntheticBudgets() {
  auto small = samples(100000);
  ClassificationOptions o;
  o.k = 8;
  QElapsedTimer clock;
  clock.start();
  auto r = FaciesClassificationService::classify(small, o);
  double smallMs = clock.nsecsElapsed() / 1e6;
  QVERIFY2(r.ok, qPrintable(r.error));
  QCOMPARE(r.labels.size(), std::size_t(100000));
  qInfo("BASELINE kmeans_100k_k8_ms = %.3f", smallMs);
  auto large = samples(1000000);
  clock.restart();
  auto big = FaciesClassificationService::classify(large, o);
  double largeMs = clock.nsecsElapsed() / 1e6;
  QVERIFY2(big.ok, qPrintable(big.error));
  QTemporaryDir dir;
  PaleoProjectStore store;
  QString error;
  clock.restart();
  auto write = store.enqueueWrite([&] {
    bool ok = FaciesClassificationService::writeRaster(
        dir.filePath("facies.tif"), large, big, &error);
    return PaleoProjectStore::WriteResult{ok, error};
  });
  double writeMs = clock.nsecsElapsed() / 1e6;
  QVERIFY2(write.ok, qPrintable(write.error));
  qInfo("BASELINE raster_1m_classify_ms = %.3f", largeMs);
  qInfo("BASELINE raster_1m_write_ms = %.3f", writeMs);
  qInfo("BASELINE raster_1m_total_ms = %.3f", largeMs + writeMs);
  qInfo("BASELINE kmeans_10x_scaling_ratio = %.3f", largeMs / smallMs);
  // Same deterministic 8-cloud workload; permits scheduler noise, catches
  // quadratic growth.
  QVERIFY2(
      largeMs < smallMs * 25,
      qPrintable(QString("10x samples scaling %1").arg(largeMs / smallMs)));
  auto ds =
      GDALOpen(dir.filePath("facies.tif").toUtf8().constData(), GA_ReadOnly);
  QVERIFY(ds);
  QCOMPARE(GDALGetRasterXSize(ds) * GDALGetRasterYSize(ds), 1000000);
  GDALClose(ds);
  int visits = 0;
  auto cancelled = FaciesClassificationService::classify(
      large, o, {[&] { return ++visits > 20; }, {}});
  QVERIFY(cancelled.cancelled);
  QVERIFY(cancelled.labels.empty());
}
void TestPerf::realArea() {
  const auto area = qEnvironmentVariable("PALEO_REAL_PROJECT_AREA");
  if (area.isEmpty())
    QSKIP("PALEO_REAL_PROJECT_AREA not set");
  DataCatalog cat;
  QString error;
  QVERIFY2(cat.open(area, &error), qPrintable(error));
  QVector<RasterSource> sources;
  for (const auto &a : cat.assets()) {
    const auto v = cat.currentVersion(a.id);
    const auto p = DataCatalog::resolvedVersionPath(area, v);
    const auto base = QFileInfo(p).baseName();
    if (base == "D53" || base == "D61" || base == "D62")
      sources << RasterSource{p, base, v.id, {}};
  }
  QCOMPARE(sources.size(), 3);
  QElapsedTimer timer;
  timer.start();
  auto result = CrossplotSamples::rasters(sources);
  QVERIFY2(result.ok, qPrintable(result.error));
  const auto sampleMs = timer.nsecsElapsed() / 1e6;
  QVERIFY(result.samples.rows() > 100000);
  QCOMPARE(result.samples.names.size(), 3);
  QCOMPARE(result.samples.parentVersionIds.size(), 3);
  QCOMPARE(qint64(result.samples.rows()) + result.samples.rejected,
           qint64(result.samples.grid.rows) * result.samples.grid.cols);
  // Independent validity intersection on the three real, identically
  // georeferenced rasters.
  const auto size =
      std::size_t(result.samples.grid.rows) * result.samples.grid.cols;
  std::vector<bool> valid(size, true);
  for (const auto &src : sources) {
    auto ds = GDALOpen(src.path.toUtf8().constData(), GA_ReadOnly);
    QVERIFY(ds);
    std::array<double, 6> gt;
    QCOMPARE(GDALGetGeoTransform(ds, gt.data()), CE_None);
    QVERIFY(gt == result.samples.grid.transform);
    QCOMPARE(GDALGetRasterXSize(ds), result.samples.grid.cols);
    QCOMPARE(GDALGetRasterYSize(ds), result.samples.grid.rows);
    std::vector<double> values(size);
    std::vector<unsigned char> mask(size);
    auto band = GDALGetRasterBand(ds, 1);
    QCOMPARE(GDALRasterIO(band, GF_Read, 0, 0, result.samples.grid.cols,
                          result.samples.grid.rows, values.data(),
                          result.samples.grid.cols, result.samples.grid.rows,
                          GDT_Float64, 0, 0),
             CE_None);
    QCOMPARE(GDALRasterIO(GDALGetMaskBand(band), GF_Read, 0, 0,
                          result.samples.grid.cols, result.samples.grid.rows,
                          mask.data(), result.samples.grid.cols,
                          result.samples.grid.rows, GDT_Byte, 0, 0),
             CE_None);
    int has = 0;
    double no = GDALGetRasterNoDataValue(band, &has);
    for (std::size_t i = 0; i < size; ++i)
      valid[i] = valid[i] && mask[i] && std::isfinite(values[i]) &&
                 (!has || values[i] != no);
    GDALClose(ds);
  }
  QCOMPARE(std::size_t(std::count(valid.begin(), valid.end(), true)),
           result.samples.rows());
  ClassificationOptions o;
  o.k = 8;
  timer.restart();
  auto r = FaciesClassificationService::classify(result.samples, o);
  double clusterMs = timer.nsecsElapsed() / 1e6;
  QVERIFY2(r.ok, qPrintable(r.error));
  QCOMPARE(r.labels.size(), result.samples.rows());
  qint64 count = 0;
  for (auto c : r.counts)
    count += c;
  QCOMPARE(count, qint64(result.samples.rows()));
  QTemporaryDir dir;
  PaleoProjectStore store;
  auto written = store.enqueueWrite([&] {
    bool ok = FaciesClassificationService::writeRaster(
        dir.filePath("real-facies.tif"), result.samples, r, &error);
    return PaleoProjectStore::WriteResult{ok, error};
  });
  QVERIFY2(written.ok, qPrintable(written.error));
  qInfo("BASELINE real_crossplot_rows = %lld",
        static_cast<long long>(result.samples.rows()));
  qInfo("BASELINE real_crossplot_rejected = %lld",
        static_cast<long long>(result.samples.rejected));
  qInfo("BASELINE real_crossplot_sample_ms = %.3f", sampleMs);
  qInfo("BASELINE real_crossplot_cluster_ms = %.3f", clusterMs);
  // Real LAS is allowed to contain all nulls; assert counts without inventing
  // facies.
  const auto las = QDir(area).filePath(QString::fromUtf8("井曲线/A1.Las"));
  LasHeaderInfo h;
  QVERIFY(LasParser::parseHeader(las, h));
  QVERIFY(h.curveNames.size() >= 3);
  Location loc;
  loc.wellId = "well-A1";
  auto logs =
      CrossplotSamples::las(las, {h.curveNames[1], h.curveNames[2]}, loc, {});
  QVERIFY2(logs.ok, qPrintable(logs.error));
  qInfo("BASELINE real_a1_crossplot_valid = %lld",
        static_cast<long long>(logs.samples.rows()));
  qInfo("BASELINE real_a1_crossplot_rejected = %lld",
        static_cast<long long>(logs.samples.rejected));
  QVERIFY(qint64(logs.samples.rows()) + logs.samples.rejected > 1000);
  if (!logs.samples.rows())
    QVERIFY(!FaciesClassificationService::classify(logs.samples, o).ok);
}
QTEST_APPLESS_MAIN(TestPerf)
#include "tst_crossplot_perf.moc"
