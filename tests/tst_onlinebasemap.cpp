// 层：测试壳
#include <QtTest>
#include <QSettings>
#include <QTemporaryDir>
#include <QImage>
#include <QUrl>
#include <algorithm>
#include <memory>
#include "../src/qgis/onlinebasemap.h"
#include <qgsapplication.h>
#include <qgscoordinatereferencesystem.h>
#include <qgsdatasourceuri.h>
#include <qgslayertree.h>
#include <qgslayertreelayer.h>
#include <qgsproject.h>
#include <qgsvectorlayer.h>
#include <qgsrasterlayer.h>
#include <qgsrasterdataprovider.h>
#include <qgsrasterblock.h>

namespace ob = paleo::onlinebasemap;

class TestOnlineBasemap : public QObject
{
  Q_OBJECT
private slots:
  void presetsHaveCreditsAndCorrectNativeZoom()
  {
    QSet<QString> ids;
    for (const auto &source : ob::sources()) {
      QVERIFY(!ids.contains(source.id));
      ids.insert(source.id);
      QVERIFY(source.url.startsWith(QStringLiteral("https://")));
      QVERIFY(source.url.contains(QStringLiteral("{x}")));
      QVERIFY(source.url.contains(QStringLiteral("{y}")));
      QVERIFY(source.url.contains(QStringLiteral("{z}")));
      QVERIFY(!source.attribution.isEmpty());
      QVERIFY(!source.description.isEmpty());
      QVERIFY(!source.termsUrl.isEmpty());
    }
    for (const auto &id : {QStringLiteral("nasa_blue_marble"), QStringLiteral("nasa_relief_bathymetry")})
      QCOMPARE(ob::sourceById(id).zmax, 8);
    for (const auto &id : {QStringLiteral("nasa_aster_color"), QStringLiteral("nasa_aster_gray")})
      QCOMPARE(ob::sourceById(id).zmax, 12);
    const auto presets = ob::sources();
    const int nasa = int(std::find_if(presets.cbegin(), presets.cend(), [](const auto &s) {
      return s.id.startsWith(QStringLiteral("nasa_"));
    }) - presets.cbegin());
    for (const auto &id : {QStringLiteral("esri_imagery"), QStringLiteral("esri_street"),
                           QStringLiteral("esri_topo"), QStringLiteral("esri_light_gray"),
                           QStringLiteral("esri_dark_gray"), QStringLiteral("esri_relief")}) {
      const auto source = ob::sourceById(id);
      QVERIFY(!source.url.isEmpty());
      const int index = int(std::find_if(presets.cbegin(), presets.cend(), [&id](const auto &s) {
        return s.id == id;
      }) - presets.cbegin());
      QVERIFY(index < nasa);
    }
    QCOMPARE(ob::sourceById(QStringLiteral("esri_relief")).zmax, 13);
  }

  void xyzUriPreservesTemplateAndIdentifiesApplication()
  {
    auto source = ob::sourceById(QStringLiteral("nasa_blue_marble"));
    source.url += QStringLiteral("?a=1&label=地形%20图");
    QgsDataSourceUri uri;
    uri.setEncodedUri(ob::xyzUri(source).toUtf8());
    QCOMPARE(uri.param(QStringLiteral("url")), source.url);
    QCOMPARE(uri.param(QStringLiteral("zmax")), QStringLiteral("8"));
    QCOMPARE(uri.param(QStringLiteral("crs")), QStringLiteral("EPSG:3857"));
    QVERIFY(uri.httpHeaders().headers().value(QStringLiteral("User-Agent")).toString()
                .startsWith(QStringLiteral("PaleoWorkbench")));
  }

  void tileProviderReadsLocalPixels()
  {
    QTemporaryDir temp;
    QVERIFY(temp.isValid());
    QVERIFY(QDir(temp.path()).mkpath(QStringLiteral("0/0")));
    QImage tile(256, 256, QImage::Format_RGB32);
    tile.fill(qRgb(120, 80, 40));
    QVERIFY(tile.save(temp.filePath(QStringLiteral("0/0/0.png"))));
    ob::Source source{QStringLiteral("local-test"), QStringLiteral("local"),
        QUrl::fromLocalFile(temp.path()).toString() + QStringLiteral("/{z}/{x}/{y}.png"),
        QStringLiteral("test fixture"), 0};
    QString error;
    std::unique_ptr<QgsMapLayer> map(ob::createLayer(source, nullptr, &error));
    auto *layer = qobject_cast<QgsRasterLayer *>(map.get());
    QVERIFY2(layer, qPrintable(error));
    QCOMPARE(layer->crs().authid(), QStringLiteral("EPSG:3857"));
    std::unique_ptr<QgsRasterBlock> red(layer->dataProvider()->block(1, layer->extent(), 1, 1));
    QVERIFY(red && red->isValid());
    QCOMPARE(red->value(0, 0), 120.0);
  }

  void projectSelectionPreservesGeologyAndRoundTrips()
  {
    QgsProject project;
    project.setCrs(QgsCoordinateReferenceSystem(QStringLiteral("EPSG:3857")));
    auto *geology = new QgsVectorLayer(QStringLiteral("Point?crs=EPSG:3857"),
                                       QStringLiteral("geology"), QStringLiteral("memory"));
    auto *offline = new QgsVectorLayer(QStringLiteral("Polygon?crs=EPSG:3857"),
                                       QStringLiteral("offline"), QStringLiteral("memory"));
    offline->setCustomProperty(QStringLiteral("paleoBasemap"), true);
    project.addMapLayer(geology);
    project.addMapLayer(offline);
    auto *root = project.layerTreeRoot();
    root->setHasCustomLayerOrder(true);
    root->setCustomLayerOrder(QList<QgsMapLayer *>{geology, offline});
    QString error;
    auto *first = ob::addPresetToProject(&project, QStringLiteral("nasa_blue_marble"), &error);
    QVERIFY2(first, qPrintable(error));
    QCOMPARE(root->customLayerOrder().last(), first);
    QVERIFY(root->findLayer(geology->id())->isVisible());
    QVERIFY(!root->findLayer(offline->id())->isVisible());
    QCOMPARE(ob::addPresetToProject(&project, QStringLiteral("nasa_blue_marble")), first);
    QCOMPARE(project.mapLayers().size(), 3);
    auto *second = ob::addPresetToProject(&project, QStringLiteral("nasa_aster_gray"), &error);
    QVERIFY2(second, qPrintable(error));
    QVERIFY(!root->findLayer(first->id())->isVisible());
    QVERIFY(root->findLayer(second->id())->isVisible());
    QVERIFY(root->findLayer(geology->id())->isVisible());
    QCOMPARE(root->customLayerOrder().last(), second);
    QTemporaryDir temp;
    QVERIFY(temp.isValid());
    const auto path = temp.filePath(QStringLiteral("maps.qgz"));
    QVERIFY(project.write(path));
    QgsProject restored;
    QVERIFY(restored.read(path));
    auto *saved = restored.mapLayer(second->id());
    QVERIFY(saved);
    QCOMPARE(saved->customProperty(QStringLiteral("paleoOnlineBasemap")).toString(),
             QStringLiteral("nasa_aster_gray"));
    QVERIFY(restored.layerTreeRoot()->findLayer(saved->id())->isVisible());
    QVERIFY(!saved->customProperty(QStringLiteral("paleoBasemapAttribution")).toString().isEmpty());
  }

  void engineeringCrsAndUnknownPresetDoNotChangeProject()
  {
    QgsProject project;
    project.setCrs(QgsCoordinateReferenceSystem::fromWkt(QStringLiteral(
        "ENGCRS[\"local\",EDATUM[\"local\"],CS[Cartesian,2],AXIS[\"x\",east,ORDER[1],LENGTHUNIT[\"metre\",1]],AXIS[\"y\",north,ORDER[2],LENGTHUNIT[\"metre\",1]]]")));
    QString error;
    QVERIFY(!ob::addPresetToProject(&project, QStringLiteral("osm"), &error));
    QVERIFY(!error.isEmpty());
    QVERIFY(project.mapLayers().isEmpty());
    project.setCrs(QgsCoordinateReferenceSystem(QStringLiteral("EPSG:3857")));
    QVERIFY(!ob::addPresetToProject(&project, QStringLiteral("missing"), &error));
    QVERIFY(project.mapLayers().isEmpty());
  }
};

int main(int argc, char **argv)
{
  qputenv("QT_QPA_PLATFORM", "offscreen");
  QTemporaryDir settings;
  qputenv("GDAL_DEFAULT_WMS_CACHE_PATH", settings.filePath(QStringLiteral("tiles")).toUtf8());
  QSettings::setDefaultFormat(QSettings::IniFormat);
  QSettings::setPath(QSettings::IniFormat, QSettings::UserScope, settings.path());
  QgsApplication app(argc, argv, true);
  QgsApplication::setPrefixPath(qEnvironmentVariable("QGIS_PREFIX_PATH"), true);
  QgsApplication::initQgis();
  TestOnlineBasemap test;
  const int result = QTest::qExec(&test, argc, argv);
  QgsApplication::exitQgis();
  return result;
}

#include "tst_onlinebasemap.moc"
