#include <QtTest>
#include <QBuffer>
#include <QDir>
#include <QFile>
#include <QImage>
#include <QSqlDatabase>
#include <QSqlQuery>
#include <QTemporaryDir>
#include <QUrlQuery>
#include <QUuid>
#include "qgis/qgisruntime.h"
#include "qgis/qgisprojectservice.h"
#include "qgis/projectmapreference.h"
#include "metadata/paleoprojectfile.h"
#include <qgsproject.h>
#include <qgsrasterlayer.h>

static bool makeTiles(const QString &path, QRgb color)
{
  QImage image(256, 256, QImage::Format_RGB32);
  image.fill(color);
  QByteArray png;
  QBuffer buffer(&png);
  if (!buffer.open(QIODevice::WriteOnly) || !image.save(&buffer, "PNG")) return false;
  const QString connection = QUuid::createUuid().toString();
  bool ok = false;
  {
    auto db = QSqlDatabase::addDatabase(QStringLiteral("QSQLITE"), connection);
    db.setDatabaseName(path);
    if (db.open()) {
      QSqlQuery q(db);
      ok = q.exec("CREATE TABLE metadata(name TEXT,value TEXT)") &&
           q.exec("CREATE TABLE tiles(zoom_level INTEGER,tile_column INTEGER,tile_row INTEGER,tile_data BLOB)") &&
           q.exec("INSERT INTO metadata VALUES ('name','map'),('format','png'),('bounds','-180,-85,180,85'),('minzoom','0'),('maxzoom','0'),('type','baselayer'),('version','1')") &&
           q.prepare("INSERT INTO tiles VALUES(0,0,0,?)");
      if (ok) { q.addBindValue(png); ok = q.exec(); }
      db.close();
    }
  }
  QSqlDatabase::removeDatabase(connection);
  return ok;
}

static QByteArray contents(const QString &path)
{
  QFile f(path);
  return f.open(QIODevice::ReadOnly) ? f.readAll() : QByteArray();
}

class TestBasemapProject : public QObject
{
  Q_OBJECT
private slots:
  void externalMapsTravelWithProject()
  {
    QTemporaryDir source, original, moved;
    QVERIFY(source.isValid() && original.isValid() && moved.isValid());
    const QString topo = source.filePath("topo.mbtiles");
    const QString shade = source.filePath("shade.mbtiles");
    const QDir realData(qEnvironmentVariable("PALEO_BASEMAP_TEST_DATA_ROOT"));
    if (!qEnvironmentVariableIsEmpty("PALEO_BASEMAP_TEST_DATA_ROOT")) {
      QVERIFY(QFile::copy(realData.filePath("basemap_topo.mbtiles"), topo));
      QVERIFY(QFile::copy(realData.filePath("basemap_hillshade.mbtiles"), shade));
    } else {
      QVERIFY(makeTiles(topo, qRgb(120, 140, 160)));
      QVERIFY(makeTiles(shade, qRgb(100, 100, 100)));
    }
    QString topoMember, shadeMember;
    {
      QgisProjectService project;
      QVERIFY(project.createProject(original.filePath("area.qgz")));
      auto config = project.mapConfiguration();
      PaleoGeoreference g;
      g.targetCrs = "EPSG:4326";
      g.anchorLonDeg = 108.05; g.anchorLatDeg = 36.1;
      g.metersPerDegLon = 90049.687955; g.metersPerDegLat = 110960.830261;
      g.a = 1; g.b = 0; g.tE = 0; g.tN = 0;
      config.georeference = g;
      config.basemapTopo = topo; config.basemapHillshade = shade;
      QString error;
      QVERIFY2(project.updateMapConfiguration(config, &error), qPrintable(error));
      topoMember = project.mapConfiguration().basemapTopo;
      shadeMember = project.mapConfiguration().basemapHillshade;
      QVERIFY(topoMember.startsWith("basemap/") && shadeMember.startsWith("basemap/"));
      QVERIFY(topoMember != shadeMember);
      QCOMPARE(contents(original.filePath(topoMember)), contents(topo));
      QCOMPARE(contents(original.filePath(shadeMember)), contents(shade));
      for (const auto &member : {topoMember, shadeMember}) {
        auto *layer = paleo::mapreference::offlineBasemap(original.filePath(member), member);
        QVERIFY(layer);
        layer->setCustomProperty("paleoLayerId", member == topoMember ? "basemap.topo" : "basemap.hillshade");
        project.project()->addMapLayer(layer);
      }
      QVERIFY2(project.writeProject(), qPrintable(project.lastErrors().join(';')));
      if (!qEnvironmentVariableIsEmpty("PALEO_BASEMAP_PROJECT_CAPTURE"))
        QVERIFY(QFile::copy(original.filePath("area.qgz"), qEnvironmentVariable("PALEO_BASEMAP_PROJECT_CAPTURE")));
    }
    QVERIFY(QDir(moved.path()).mkpath("basemap"));
    for (const auto &member : {QStringLiteral("area.qgz"), QStringLiteral("project.paleo"), topoMember, shadeMember})
      QVERIFY(QFile::copy(original.filePath(member), moved.filePath(member)));
    QVERIFY(QFile::remove(topo));
    QVERIFY(QFile::remove(shade));
    QgisProjectService reopened;
    QVERIFY2(reopened.openProject(moved.filePath("project.paleo")), qPrintable(reopened.lastErrors().join(';')));
    QCOMPARE(reopened.mapConfiguration().basemapTopo, topoMember);
    QCOMPARE(reopened.project()->mapLayers().size(), 2);
    for (auto *layer : reopened.project()->mapLayers()) {
      QVERIFY2(layer->isValid(), qPrintable(layer->source()));
      const QUrlQuery uri(layer->source());
      const QString path = layer->providerType() == QLatin1String("gdal")
          ? layer->source() : QUrl(uri.queryItemValue("url", QUrl::FullyDecoded)).toLocalFile();
      QVERIFY2(QFileInfo(path).absoluteFilePath().startsWith(moved.path()), qPrintable(path));
      QVERIFY(layer->customProperty("paleoBasemapPath").toString().startsWith(moved.path()));
    }
    reopened.closeProject();
    QVERIFY(reopened.openProjectAsync(moved.filePath("project.paleo")));
    QTRY_VERIFY_WITH_TIMEOUT(!reopened.isOpening(), 30000);
    QCOMPARE(reopened.project()->mapLayers().size(), 2);
    for (auto *layer : reopened.project()->mapLayers()) QVERIFY(layer->isValid());
    // Optional acceptance fixture: read the user's prepared project without
    // adopting, migrating, or writing any of its files.
    if (!qEnvironmentVariableIsEmpty("PALEO_BASEMAP_PROJECT_UNDER_TEST")) {
      QgsProject native;
      QVERIFY(native.read(qEnvironmentVariable("PALEO_BASEMAP_PROJECT_UNDER_TEST")));
      int basemaps = 0;
      for (auto *layer : native.mapLayers())
        if (layer->customProperty("paleoBasemap").toBool()) {
          QVERIFY2(layer->isValid(), qPrintable(layer->source()));
          ++basemaps;
        }
      QCOMPARE(basemaps, 2);
    }
  }

  void missingDisabledMapIsReportedAndFailedSavePreservesConfiguration()
  {
    QTemporaryDir dir;
    QgisProjectService project;
    QVERIFY(project.createProject(dir.filePath("area.qgz")));
    const QByteArray before = contents(dir.filePath("project.paleo"));
    auto file = project.mapConfiguration();
    file.basemapEnabled = false;
    file.basemapTopo = "basemap/missing.mbtiles";
    QVERIFY(missingMembers(dir.path(), file).join(';').contains("basemap.topo"));
    QString error;
    QVERIFY(!project.updateMapConfiguration(file, &error));
    QVERIFY(!error.isEmpty());
    QCOMPARE(contents(dir.filePath("project.paleo")), before);
    QVERIFY(project.mapConfiguration().basemapTopo.isEmpty());
  }

  void ordinarySaveAdoptsLegacyExternalMap()
  {
    QTemporaryDir source, dir;
    const QString map = source.filePath("legacy.mbtiles");
    QVERIFY(makeTiles(map, qRgb(120, 120, 120)));
    {
      QgisProjectService project;
      QVERIFY(project.createProject(dir.filePath("area.qgz")));
      auto file = project.mapConfiguration();
      file.basemapEnabled = false;
      file.basemapTopo = map;
      QVERIFY(writeProjectFile(dir.path(), file));
    }
    QgisProjectService project;
    QVERIFY(project.openProject(dir.filePath("project.paleo")));
    QVERIFY(project.writeProject());
    const auto file = readProjectFile(dir.filePath("project.paleo"));
    QVERIFY(file.basemapTopo.startsWith("basemap/"));
    QCOMPARE(contents(dir.filePath(file.basemapTopo)), contents(map));
    QVERIFY(project.project()->readEntry("paleo", "mapConfiguration").contains(file.basemapTopo));
  }
};

int main(int argc, char **argv)
{
  if (!QgisRuntime::initialize()) return 1;
  TestBasemapProject test;
  const int result = QTest::qExec(&test, argc, argv);
  QgisRuntime::shutdown();
  return result;
}
#include "tst_basemap_project.moc"
