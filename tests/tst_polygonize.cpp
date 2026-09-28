#include <QtTest>
#include <qgsapplication.h>
#include <qgsprocessingregistry.h>
#include <qgsprocessingcontext.h>
#include <qgsprocessingfeedback.h>
#include <qgsvectorlayer.h>
#include <gdal.h>
#include <cpl_conv.h>
#include <QTemporaryDir>
#include "../spikes/polygonize/paleopolygonize.h"

// Spike-2 acceptance: algorithm registers in QgsProcessingRegistry and converts a
// small raster into >0 polygons entirely through the C++ path.
class TestPolygonize : public QObject
{
  Q_OBJECT
private slots:
  void registersAndRuns()
  {
    QgsApplication::processingRegistry()->addProvider(new PaleoProcessingProvider());
    QVERIFY(QgsApplication::processingRegistry()->algorithmById(QStringLiteral("paleospikes:paleo_polygonize")) != nullptr);

    QTemporaryDir dir;
    const QString rasterPath = dir.filePath(QStringLiteral("in.tif"));
    // 4x4 raster: two value zones -> expect >=2 polygons
    GDALDriverH drv = GDALGetDriverByName("GTiff");
    QVERIFY(drv != nullptr);
    GDALDatasetH ds = GDALCreate(drv, rasterPath.toUtf8().constData(), 4, 4, 1, GDT_Byte, nullptr);
    QVERIFY(ds != nullptr);
    unsigned char px[16] = {1,1,2,2, 1,1,2,2, 3,3,2,2, 3,3,2,2};
    QCOMPARE(GDALRasterIO(GDALGetRasterBand(ds, 1), GF_Write, 0, 0, 4, 4, px, 4, 4, GDT_Byte, 0, 0), CE_None);
    GDALClose(ds);

    const QString outPath = dir.filePath(QStringLiteral("out.gpkg"));
    QgsProcessingContext ctx;
    QgsProcessingFeedback fb;
    QVariantMap params;
    params.insert(QStringLiteral("INPUT"), rasterPath);
    params.insert(QStringLiteral("OUTPUT"), outPath);
    const QVariantMap res = QgsApplication::processingRegistry()
        ->algorithmById(QStringLiteral("paleospikes:paleo_polygonize"))
        ->run(params, ctx, &fb);
    QVERIFY(!res.isEmpty());

    QgsVectorLayer out(outPath + QStringLiteral("|layername=polygonized"), QStringLiteral("out"), QStringLiteral("ogr"));
    QVERIFY2(out.isValid(), qPrintable(out.error().message()));
    QVERIFY2(out.featureCount() >= 2, qPrintable(QString::number(out.featureCount())));
  }
};

int main(int argc, char *argv[])
{
  QgsApplication app(argc, argv, false);
  app.setPrefixPath(qEnvironmentVariable("QGIS_PREFIX_PATH", QStringLiteral("/usr")), true);
  app.initQgis();
  QgsApplication::processingRegistry(); // ensure registry alive
  TestPolygonize tc;
  const int rc = QTest::qExec(&tc, argc, argv);
  QgsApplication::exitQgis();
  return rc;
}

#include "tst_polygonize.moc"
