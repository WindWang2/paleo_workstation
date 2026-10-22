#include <QtTest>
#include <QDirIterator>
#include <QEventLoop>
#include <QFile>
#include <QImage>
#include <QBuffer>
#include <QSqlDatabase>
#include <QSqlQuery>
#include <QTemporaryDir>
#include <QTimer>
#include <QUuid>
#include "qgis/qgisruntime.h"
#include "qgis/qgisprojectservice.h"
#include "qgis/qgislayerservice.h"
#include "qgis/onlinebasemap.h"
#include "metadata/layermanifest.h"
#include "services/paleotaskservice.h"
#include "workflow/offlinebasemapimport.h"
#include <gdal.h>
#include <ogr_srs_api.h>
#include <cpl_conv.h>
#include <qgsproject.h>
#include <qgsvectorlayer.h>
#include <qgsrasterlayer.h>
#include <qgslayertree.h>
#include <qgsmaprenderersequentialjob.h>
#include <qgsmapsettings.h>
namespace {
QString raster(const QString &path, int value = 100, int epsg = 3857) {
  auto *dataset = GDALCreate(GDALGetDriverByName("GTiff"), path.toUtf8().constData(), 16, 16, 1, GDT_Byte, nullptr);
  if (!dataset) return {};
  double transform[6]{100, 10, 0, 200, 0, -10}; GDALSetGeoTransform(dataset, transform);
  if (epsg) {
    auto srs = OSRNewSpatialReference(nullptr); OSRImportFromEPSG(srs, epsg); char *wkt = nullptr;
    OSRExportToWkt(srs, &wkt); GDALSetProjection(dataset, wkt); CPLFree(wkt); OSRDestroySpatialReference(srs);
  }
  const bool ok = GDALFillRaster(GDALGetRasterBand(dataset, 1), value, 0) == CE_None;
  GDALClose(dataset); return ok ? path : QString();
}
bool tiles(const QString &path) {
  QImage image(256, 256, QImage::Format_RGB32); image.fill(QColor(100, 120, 140)); QByteArray png;
  QBuffer buffer(&png); if (!buffer.open(QIODevice::WriteOnly) || !image.save(&buffer, "PNG")) return false;
  const auto name = QUuid::createUuid().toString(); bool ok = false;
  { auto db = QSqlDatabase::addDatabase("QSQLITE", name); db.setDatabaseName(path);
    if (db.open()) { QSqlQuery query(db);
      ok = query.exec("CREATE TABLE metadata(name TEXT,value TEXT)") &&
          query.exec("CREATE TABLE tiles(zoom_level INTEGER,tile_column INTEGER,tile_row INTEGER,tile_data BLOB)") &&
          query.exec("INSERT INTO metadata VALUES ('name','base'),('format','png'),('bounds','-180,-85,180,85'),('minzoom','0'),('maxzoom','0')") &&
          query.prepare("INSERT INTO tiles VALUES(0,0,0,?)");
      if (ok) { query.addBindValue(png); ok = query.exec(); } db.close();
    }
  } QSqlDatabase::removeDatabase(name); return ok;
}
struct Fixture {
  QTemporaryDir dir;
  QgisProjectService project;
  LayerManifest manifest{dir.filePath("project.sqlite")};
  QgisLayerService layers{&project, &manifest};
  PaleoTaskService tasks;
  bool init(bool registered = true) {
    if (!project.createProject(dir.filePath("area.qgz")) || !manifest.open()) return false;
    if (!registered) return true;
    auto config = project.mapConfiguration(); PaleoGeoreference reference;
    reference.kind = "crs"; reference.sourceCrs = "EPSG:3857"; reference.targetCrs = "EPSG:4326";
    config.georeference = reference; return project.updateMapConfiguration(config);
  }
  QPair<bool, QString> run(const PaleoOfflineBasemap::Request &request, const std::function<void(PaleoTask *)> &started = {}) {
    bool completed = false; QPair<bool, QString> result; QEventLoop loop; QString error;
    auto *task = PaleoOfflineBasemap::importFile(&project, &layers, &tasks, request, &loop,
        [&](bool success, const QString &message) { result = {success, message}; completed = true; loop.quit(); }, &error);
    if (!task) return {false, error};
    if (started) started(task);
    QTimer timeout; timeout.setSingleShot(true); QObject::connect(&timeout, &QTimer::timeout, &loop, &QEventLoop::quit); timeout.start(20000);
    if (!completed) loop.exec();
    if (!completed) { task->requestCancel(); return {false, "timeout"}; }
    return result;
  }
};
}
class TestOfflineBasemap : public QObject {
  Q_OBJECT
private slots:
  void importGeoTiffKeepsWellsAndSurvivesMovingProject() {
    Fixture f; QVERIFY(f.init()); QTemporaryDir source, moved;
    const auto path = raster(source.filePath("影像.tif")); QVERIFY(!path.isEmpty());
    auto *well = new QgsVectorLayer("Point?crs=EPSG:3857", "井点", "memory"); QVERIFY(well->isValid());
    const auto wellId = well->id(); f.project.project()->addMapLayer(well);
    paleo::onlinebasemap::saveChoice(f.project.project(), "esri_topo");
    const auto imported = f.run({path}); QVERIFY2(imported.first, qPrintable(imported.second));
    const auto member = f.project.mapConfiguration().basemapTopo; QVERIFY(member.startsWith("basemap/")); QVERIFY(member.endsWith(".tif"));
    QVERIFY(f.layers.layer("basemap.topo")); QVERIFY(f.layers.layer("basemap.topo")->isValid());
    auto *base = f.layers.layer("basemap.topo"); QgsMapSettings settings; settings.setLayers({base});
    settings.setDestinationCrs(base->crs()); settings.setExtent(base->extent()); settings.setOutputSize(QSize(128, 128));
    settings.setBackgroundColor(QColor(255, 0, 255));
    QgsMapRendererSequentialJob render(settings); render.start(); render.waitForFinished();
    QVERIFY(!render.renderedImage().isNull()); QVERIFY(render.renderedImage().pixelColor(64, 64) != QColor(255, 0, 255));
    QVERIFY(f.project.project()->mapLayer(wellId)); QVERIFY(f.project.project()->layerTreeRoot()->findLayer(wellId)->isVisible());
    QCOMPARE(paleo::onlinebasemap::savedChoice(f.project.project()), QString("none"));
    const auto count = QDir(f.dir.filePath("basemap")).entryList(QDir::Dirs | QDir::NoDotAndDotDot).size();
    const auto again = f.run({path}); QVERIFY2(again.first, qPrintable(again.second));
    QCOMPARE(f.project.mapConfiguration().basemapTopo, member);
    QCOMPARE(QDir(f.dir.filePath("basemap")).entryList(QDir::Dirs | QDir::NoDotAndDotDot).size(), count);
    f.project.closeProject();
    for (const auto &file : {QString("area.qgz"), QString("project.paleo")}) QVERIFY(QFile::copy(f.dir.filePath(file), moved.filePath(file)));
    QDirIterator files(f.dir.filePath("basemap"), QDir::Files, QDirIterator::Subdirectories);
    while (files.hasNext()) { const auto original = files.next(); const auto relative = QDir(f.dir.path()).relativeFilePath(original);
      QVERIFY(QDir().mkpath(QFileInfo(moved.filePath(relative)).absolutePath())); QVERIFY(QFile::copy(original, moved.filePath(relative))); }
    QVERIFY(QFile::remove(path));
    QgisProjectService reopened; QVERIFY(reopened.openProject(moved.filePath("project.paleo")));
    bool found = false;
    for (auto *layer : reopened.project()->mapLayers()) if (layer->customProperty("paleoLayerId").toString() == "basemap.topo") {
      found = true; QVERIFY2(layer->isValid(), qPrintable(layer->source())); QVERIFY(layer->source().startsWith(moved.path()));
    } QVERIFY(found);
  }
  void mbtilesAndHillshadeHaveIndependentSelections() {
    Fixture f; QVERIFY(f.init()); QTemporaryDir source;
    const auto path = source.filePath("tiles.mbtiles"); QVERIFY(tiles(path));
    auto result = f.run({path}); QVERIFY2(result.first, qPrintable(result.second));
    const auto topo = f.project.mapConfiguration().basemapTopo;
    result = f.run({raster(source.filePath("shade.tif")), true, true}); QVERIFY2(result.first, qPrintable(result.second));
    QCOMPARE(f.project.mapConfiguration().basemapTopo, topo);
    QVERIFY(f.project.mapConfiguration().basemapHillshade.endsWith("shade.tif")); QVERIFY(f.layers.layer("basemap.hillshade"));
    result = f.run({raster(source.filePath("replacement.tif"), 200)}); QVERIFY2(result.first, qPrintable(result.second));
    QVERIFY(f.project.mapConfiguration().basemapTopo != topo); QVERIFY(QFileInfo::exists(f.dir.filePath(topo)));
  }
  void rasterSidecarsTravelTogether() {
    Fixture f; QVERIFY(f.init()); QTemporaryDir source;
    const auto tif = raster(source.filePath("source.tif"));
    auto ds = GDALOpen(tif.toUtf8().constData(), GA_ReadOnly); QVERIFY(ds);
    const auto image = source.filePath("map.png");
    auto png = GDALCreateCopy(GDALGetDriverByName("PNG"), image.toUtf8().constData(), ds, false, nullptr, nullptr, nullptr);
    QVERIFY(png); GDALClose(png); GDALClose(ds); QVERIFY(QFileInfo::exists(image + ".aux.xml"));
    const auto result = f.run({image}); QVERIFY2(result.first, qPrintable(result.second));
    const auto member = f.dir.filePath(f.project.mapConfiguration().basemapTopo);
    QVERIFY(QFileInfo::exists(member + ".aux.xml"));
    QgsRasterLayer copied(member, "copy", "gdal"); QVERIFY(copied.isValid()); QVERIFY(copied.crs().isValid());
  }
  void invalidCancelledAndStaleImportsPreserveSettings() {
    Fixture f; QVERIFY(f.init()); QTemporaryDir source; const auto initial = f.project.mapConfiguration().basemapTopo;
    auto result = f.run({raster(source.filePath("unregistered.tif"), 100, 0)});
    QVERIFY(!result.first); QVERIFY(result.second.contains("坐标系")); QCOMPARE(f.project.mapConfiguration().basemapTopo, initial);
    const auto path = raster(source.filePath("good.tif"));
    result = f.run({path}, [](PaleoTask *task) { task->requestCancel(); });
    QVERIFY(!result.first); QCOMPARE(f.project.mapConfiguration().basemapTopo, initial);
    result = f.run({path}, [&](PaleoTask *) { f.project.closeProject(); });
    QVERIFY(!result.first); QVERIFY(f.project.projectPath().isEmpty());
  }
  void unregisteredProjectCanStoreWithoutDisplayAndReadOnlyRefuses() {
    Fixture f; QVERIFY(f.init(false)); QTemporaryDir source; const auto path = raster(source.filePath("map.tif"));
    auto result = f.run({path}); QVERIFY(!result.first);
    result = f.run({path, false, false}); QVERIFY2(result.first, qPrintable(result.second));
    QVERIFY(!f.project.mapConfiguration().basemapEnabled); QVERIFY(!f.layers.layer("basemap.topo"));
    QVERIFY(QFileInfo::exists(f.dir.filePath(f.project.mapConfiguration().basemapTopo)));
    f.project.setReadOnly(true); result = f.run({path, true, false}); QVERIFY(!result.first);
  }
};
int main(int argc, char **argv) {
  if (!QgisRuntime::initialize()) return 1; GDALAllRegister();
  TestOfflineBasemap test; const auto result = QTest::qExec(&test, argc, argv); QgisRuntime::shutdown(); return result;
}
#include "tst_offlinebasemap.moc"
