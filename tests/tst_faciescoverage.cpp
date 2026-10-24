#include <QtTest>
#include <QTemporaryDir>
#include <QJsonDocument>
#include <QJsonObject>
#include <QUuid>
#include <qgsapplication.h>
#include <qgsprocessingregistry.h>
#include <qgsprocessingcontext.h>
#include <qgsprocessingfeedback.h>
#include <qgsrasterlayer.h>
#include <qgsvectorlayer.h>
#include <qgsfeatureiterator.h>
#include <qgsfeature.h>
#include <qgsgeometry.h>
#include <gdal.h>
#include <cpl_conv.h>
#include <ogr_spatialref.h>
#include "../src/algorithms/paleoalgorithms.h"

class TestFaciesCoverage : public QObject {
  Q_OBJECT
  QTemporaryDir m_dir;
  QVariantMap m_result;
  QString raster(int width, int height, const QVector<int> &values) {
    const auto path = m_dir.filePath(QUuid::createUuid().toString() + ".tif");
    auto ds = GDALCreate(GDALGetDriverByName("GTiff"), path.toUtf8().constData(), width, height, 1, GDT_Int32, nullptr);
    if (!ds) return {};
    double gt[]{227000, 20, 0, 4000000, 0, -40};
    GDALSetGeoTransform(ds, gt);
    OGRSpatialReference crs; crs.importFromEPSG(32649);
    char *wkt = nullptr; crs.exportToWkt(&wkt); GDALSetProjection(ds, wkt); CPLFree(wkt);
    auto band = GDALGetRasterBand(ds, 1); GDALSetRasterNoDataValue(band, -9999);
    const auto ok = GDALRasterIO(band, GF_Write, 0, 0, width, height, const_cast<int *>(values.constData()), width, height, GDT_Int32, 0, 0);
    GDALClose(ds); return ok == CE_None ? path : QString();
  }
  QString run(const QString &input, QVariantMap params = {}, const QString &output = {}) {
    QgsRasterLayer layer(input, "input", "gdal");
    if (!layer.isValid()) return {};
    params.insert("INPUT", QVariant::fromValue(static_cast<QgsMapLayer *>(&layer)));
    params.insert("OUTPUT", output.isEmpty() ? m_dir.filePath(QUuid::createUuid().toString() + ".gpkg") : output);
    QgsProcessingContext context; QgsProcessingFeedback feedback;
    auto *algorithm = QgsApplication::processingRegistry()->algorithmById("paleo:paleo_facies_polygonize");
    m_result = algorithm->run(params, context, &feedback);
    return m_result.value("OUTPUT").toString();
  }
  QgsGeometry footprint(const QString &path, double *sum = nullptr) {
    QgsVectorLayer layer(path + "|layername=facies_polygons", "output", "ogr");
    if (!layer.isValid()) return {};
    QVector<QgsGeometry> pieces; QgsFeature f; auto it = layer.getFeatures();
    while (it.nextFeature(f)) {
      if (!f.geometry().isGeosValid()) return {};
      pieces << f.geometry(); if (sum) *sum += f.geometry().area();
    }
    return QgsGeometry::unaryUnion(pieces);
  }
private slots:
  void initTestCase() { QgsApplication::processingRegistry()->addProvider(new PaleoProvider()); }
  void ringsAndJunctionsPreserveFullCoverage_data() {
    QTest::addColumn<int>("seed");
    for (int seed : {7, 19, 42, 91}) QTest::newRow(qPrintable(QString::number(seed))) << seed;
  }
  void ringsAndJunctionsPreserveFullCoverage() {
    QFETCH(int, seed);
    constexpr int width = 48, height = 36;
    QVector<int> values(width * height, 6);
    quint32 state = seed;
    const auto random = [&state](int bound) { state = state * 1664525u + 1013904223u; return int((state >> 12) % bound); };
    for (int patch = 0; patch < 26; ++patch) {
      const int x = random(width), y = random(height), radius = 2 + random(7), code = 1 + random(5);
      for (int row = 0; row < height; ++row)
        for (int col = 0; col < width; ++col)
          if (std::abs(col - x) + std::abs(row - y) <= radius) values[row * width + col] = code;
    }
    const auto path = run(raster(width, height, values), {{"MIN_FRACTION", .0025}, {"SMOOTH", 1}, {"BOUNDARY_SMOOTH", 2}});
    QVERIFY2(!path.isEmpty(), "polygonize failed");
    double sum = 0; const auto actual = footprint(path, &sum);
    QVERIFY(!actual.isEmpty());
    const auto expected = QgsGeometry::fromRect(QgsRectangle(227000, 4000000 - 40 * height, 227000 + 20 * width, 4000000));
    QVERIFY2(actual.symDifference(expected).area() < .001, qPrintable(QString::number(actual.area() / expected.area(), 'g', 12)));
    QVERIFY(std::abs(sum - actual.area()) < .001);
  }
  void nodataAndOuterBoundaryStayFixedWhileSharedEdgesRound() {
    const QVector<int> values{1,1,1,2,-9999, 1,1,2,2,2, 1,2,-9999,2,2, 2,2,2,2,2, 2,2,2,2,2};
    const auto source = raster(5, 5, values);
    const QVariantMap basic{{"SMOOTH", 0}, {"MIN_CELLS", 0}, {"MIN_FRACTION", 0.0}};
    auto off = basic; off.insert("BOUNDARY_SMOOTH", 0);
    const auto expected = footprint(run(source, off));
    auto on = basic; on.insert("BOUNDARY_SMOOTH", 2); on.insert("SIMPLIFY", 5.0);
    const auto actual = footprint(run(source, on));
    QVERIFY(!actual.isEmpty());
    QVERIFY(actual.symDifference(expected).area() < .001);
    const auto provenance = QJsonDocument::fromJson(m_result.value("PROVENANCE").toString().toUtf8()).object();
    QVERIFY(provenance.value("smoothed_arcs").toInt() > 0);
    QVERIFY(provenance.value("coverage_difference_area").toDouble(1) < .001);
  }
  void localD61ReadOnlyAcceptance() {
    const auto input = qEnvironmentVariable("PALEO_FACIES_COVERAGE_INPUT");
    if (input.isEmpty()) QSKIP("Set PALEO_FACIES_COVERAGE_INPUT for real data acceptance");
    const auto output = qEnvironmentVariable("PALEO_FACIES_COVERAGE_OUTPUT");
    const auto path = run(input, {{"MIN_FRACTION", .0025}, {"SMOOTH", 1}, {"BOUNDARY_SMOOTH", 2}}, output);
    QVERIFY2(!path.isEmpty(), "D61 polygonize failed");
    const auto actual = footprint(path);
    QVERIFY(actual.area() > 211000000);
    const auto prov = QJsonDocument::fromJson(m_result.value("PROVENANCE").toString().toUtf8()).object();
    qInfo() << "D61 coverage" << actual.area() << prov.value("coverage_difference_area").toDouble(-1);
    QVERIFY(prov.value("coverage_difference_area").toDouble(-1) >= 0);
    QVERIFY(prov.value("coverage_difference_area").toDouble(1) < .1);
  }
};
int main(int argc, char **argv) {
  QTemporaryDir profile; qputenv("QGIS_CUSTOM_CONFIG_PATH", profile.path().toUtf8());
  QgsApplication app(argc, argv, false);
  app.setPrefixPath(qEnvironmentVariable("QGIS_PREFIX_PATH", "/usr"), true);
  app.init(); app.createDatabase(); app.initQgis(); GDALAllRegister();
  TestFaciesCoverage test; const int result = QTest::qExec(&test, argc, argv);
  QgsApplication::exitQgis(); return result;
}
#include "tst_faciescoverage.moc"
