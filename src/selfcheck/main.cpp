// paleo selfcheck — §44.1: checklist + rendered map.png prove the vendored QGIS stack is alive.
// Each line: PASS/FAIL with cause+fix hint on failure (§44.3 error contract).
#include <qgsapplication.h>
#include <qgsproviderregistry.h>
#include <qgsvectorlayer.h>
#include <qgsmapsettings.h>
#include <qgsmaprendererparalleljob.h>
#include <qgsmapcanvas.h>
#include <QImage>
#include <QFileInfo>
#include <cstdio>

static int failures = 0;
static void check(bool ok, const QString &name, const QString &fix = {})
{
  if (ok) { std::printf("  PASS %s\n", qPrintable(name)); return; }
  ++failures;
  std::printf("  FAIL %s\n", qPrintable(name));
  if (!fix.isEmpty()) std::printf("       fix: %s\n", qPrintable(fix));
}

int main(int argc, char *argv[])
{
  QElapsedTimer total; total.start();
  QgsApplication app(argc, argv, false);
  app.setPrefixPath(QStringLiteral("/usr"), true);
  app.initQgis();

  std::printf("paleo selfcheck\n");
  check(QgsApplication::instance() != nullptr, QStringLiteral("QgsApplication initialized"));

  const QStringList providers = QgsProviderRegistry::instance()->providerList();
  check(providers.size() > 0, QStringLiteral("providers loaded (%1)").arg(providers.size()),
        QStringLiteral("QGIS provider plugins not found — check prefix path / GDAL_DRIVER_PATH"));
  check(providers.contains(QStringLiteral("ogr")), QStringLiteral("ogr provider present"),
        QStringLiteral("GDAL provider plugin missing — verify vendor prefix has lib/qgis/plugins"));

  const QString srs = QgsApplication::srsDatabaseFilePath();
  check(QFile::exists(srs), QStringLiteral("srs.db at %1").arg(srs),
        QStringLiteral("share/qgis/resources missing from vendor prefix"));

  QgsVectorLayer layer(FIXTURE_GPKG "|layername=basin", QStringLiteral("basin"), QStringLiteral("ogr"));
  check(layer.isValid() && layer.featureCount() > 0,
        QStringLiteral("fixture.gpkg loaded (%1 features)").arg(layer.featureCount()),
        QStringLiteral("testdata/fixture.gpkg missing/corrupt — regenerate via ogr2ogr"));

  QImage img;
  if (layer.isValid()) {
    QgsMapSettings ms;
    ms.setLayers({&layer});
    ms.setExtent(layer.extent());
    ms.setOutputSize(QSize(400, 300));
    ms.setDestinationCrs(layer.crs());
    ms.setBackgroundColor(QColor(237, 241, 245)); // surface-alt
    QgsMapRendererParallelJob job(ms);
    job.start();
    job.waitForFinished();
    img = job.renderedImage();
  }
  check(!img.isNull(), QStringLiteral("map rendered"));

  bool uniform = true;
  if (!img.isNull()) {
    const quint32 first = img.pixel(0, 0);
    for (int y = 0; y < img.height() && uniform; ++y)
      for (int x = 0; x < img.width(); ++x)
        if (img.pixel(x, y) != first) { uniform = false; break; }
  }
  check(!uniform, QStringLiteral("render non-uniform (pipeline alive)"),
        QStringLiteral("uniform image — renderer produced blank output"));

  const QString out = QStringLiteral("vendor/logs/map.png");
  QFileInfo(out).dir().mkpath(QStringLiteral("."));
  check(img.save(out), QStringLiteral("map.png written to %1").arg(out));

  QgsApplication::exitQgis();
  std::printf("%s — %lld ms total\n", failures ? "SELFCHECK FAILED" : "SELFCHECK OK", (long long)total.elapsed());
  return failures ? 1 : 0;
}
