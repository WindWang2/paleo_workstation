#include <QtTest>
#include <qgsapplication.h>
#include <qgsproviderregistry.h>
#include <qgsvectorlayer.h>
#include <qgsmapsettings.h>
#include <qgsmaprendererparalleljob.h>
#include <QImage>

// Spike-1 acceptance as test: init, providers>0, srs.db, render GPKG -> non-uniform pixels
class TestBoot : public QObject
{
  Q_OBJECT
private slots:
  void initQgis()
  {
    QVERIFY(QgsApplication::instance() != nullptr);
    QVERIFY2(QFile::exists(QgsApplication::srsDatabaseFilePath()), qPrintable(QgsApplication::srsDatabaseFilePath()));
  }
  void providersLoaded()
  {
    const QStringList providers = QgsProviderRegistry::instance()->providerList();
    QVERIFY(providers.size() > 0);
    QVERIFY2(providers.contains(QStringLiteral("ogr")), qPrintable(providers.join(',')));
  }
  void rendersGpkgNonUniform()
  {
    QVERIFY2(QFile::exists(FIXTURE_GPKG), "testdata/fixture.gpkg missing");
    QgsVectorLayer layer(FIXTURE_GPKG "|layername=basin", QStringLiteral("basin"), QStringLiteral("ogr"));
    QVERIFY2(layer.isValid(), qPrintable(layer.error().message()));
    QVERIFY(layer.featureCount() > 0);

    QgsMapSettings ms;
    ms.setLayers({&layer});
    ms.setExtent(layer.extent());
    ms.setOutputSize(QSize(400, 300));
    ms.setDestinationCrs(layer.crs());
    QgsMapRendererParallelJob job(ms);
    job.start();
    job.waitForFinished();
    const QImage img = job.renderedImage();
    QVERIFY(!img.isNull());

    const quint32 first = img.pixel(0, 0);
    bool uniform = true;
    for (int y = 0; y < img.height() && uniform; ++y)
      for (int x = 0; x < img.width(); ++x)
        if (img.pixel(x, y) != first) { uniform = false; break; }
    QVERIFY2(!uniform, "rendered canvas is uniform — QGIS render pipeline not alive");
  }
};

int main(int argc, char *argv[])
{
  QgsApplication app(argc, argv, false);
  const QString prefix = qEnvironmentVariable("QGIS_PREFIX_PATH", QStringLiteral("/usr"));
  app.setPrefixPath(prefix, true);
  app.initQgis();
  TestBoot tc;
  const int rc = QTest::qExec(&tc, argc, argv);
  QgsApplication::exitQgis();
  return rc;
}

#include "tst_boot.moc"
