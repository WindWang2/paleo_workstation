// 层：测试壳
#include <QtTest>
#include <QSettings>
#include <QTemporaryDir>
#include <QImage>
#include <QUrl>
#include <QBuffer>
#include <QLabel>
#include <QScopeGuard>
#include <QTcpServer>
#include <QTcpSocket>
#include <QThread>
#include <atomic>
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
#include <qgsmapcanvas.h>
#include <qgslabelingresults.h>
#include <qgsmarkersymbol.h>
#include <qgspallabeling.h>
#include <qgssinglesymbolrenderer.h>
#include <qgstextformat.h>
#include <qgsvectordataprovider.h>
#include <qgsvectorlayerlabeling.h>

namespace ob = paleo::onlinebasemap;

namespace {
// 独立事件循环：GDAL 在建层时也可能探测 alpha 波段，不能让测试 HTTP 服务
// 和客户端共用 GUI 线程，否则会把夹具自身的阻塞误当成渲染问题。
class DelayedTiles
{
public:
  DelayedTiles()
  {
    QImage tile(256, 256, QImage::Format_RGB32);
    tile.fill(QColor(180, 200, 160));
    QBuffer buffer(&m_png);
    buffer.open(QIODevice::WriteOnly);
    tile.save(&buffer, "PNG");
    m_server = new QTcpServer;
    m_server->moveToThread(&m_thread);
    QObject::connect(&m_thread, &QThread::finished, m_server, &QObject::deleteLater);
    m_thread.start();
    QMetaObject::invokeMethod(m_server, [this] {
      m_server->listen(QHostAddress::LocalHost, 0);
      m_port = m_server->serverPort();
      QObject::connect(m_server, &QTcpServer::newConnection, m_server, [this] {
        while (auto *socket = m_server->nextPendingConnection()) {
          QObject::connect(socket, &QTcpSocket::readyRead, m_server, [this, socket] {
            socket->readAll();
            if (socket->property("handled").toBool()) return;
            socket->setProperty("handled", true);
            if (m_hold) {
              if (!m_pending.contains(socket)) { m_pending.append(socket); ++held; }
            } else respond(socket);
          });
        }
      });
    }, Qt::BlockingQueuedConnection);
  }
  ~DelayedTiles()
  {
    release(true);
    m_thread.quit();
    m_thread.wait();
  }
  quint16 port() const { return m_port; }
  void hold() { QMetaObject::invokeMethod(m_server, [this] { m_hold = true; }, Qt::BlockingQueuedConnection); }
  void release(bool fail)
  {
    QMetaObject::invokeMethod(m_server, [this, fail] {
      m_hold = false; m_fail = fail;
      for (const auto &socket : std::as_const(m_pending)) if (socket) respond(socket);
      m_pending.clear();
    }, Qt::BlockingQueuedConnection);
  }
  std::atomic<int> held{0};
private:
  void respond(QTcpSocket *socket)
  {
    const QByteArray body = m_fail ? QByteArray("unavailable") : m_png;
    socket->write((m_fail ? QByteArray("HTTP/1.1 503 Service Unavailable\r\n") : QByteArray("HTTP/1.1 200 OK\r\n")) +
                  "Content-Type: image/png\r\nConnection: close\r\nContent-Length: " + QByteArray::number(body.size()) + "\r\n\r\n" + body);
    socket->flush();
    socket->disconnectFromHost();
  }
  QThread m_thread;
  QTcpServer *m_server = nullptr;
  QList<QPointer<QTcpSocket>> m_pending;
  QByteArray m_png;
  quint16 m_port = 0;
  bool m_hold = false, m_fail = false;
};
} // namespace

class TestOnlineBasemap : public QObject
{
  Q_OBJECT
private slots:
  void wellsStayVisibleWhileTilesArePending_data()
  {
    QTest::addColumn<bool>("failTiles");
    QTest::newRow("delayed-success") << false;
    QTest::newRow("delayed-failure") << true;
  }

  void wellsStayVisibleWhileTilesArePending()
  {
    QFETCH(bool, failTiles);
    DelayedTiles server;
    QVERIFY(server.port());
    QgsVectorLayer wells(QStringLiteral("Point?crs=EPSG:3857&field=name:string"), QStringLiteral("wells"), QStringLiteral("memory"));
    QgsFeature well(wells.fields());
    well.setGeometry(QgsGeometry::fromPointXY(QgsPointXY(0, 0)));
    well.setAttribute(QStringLiteral("name"), QStringLiteral("A1"));
    QgsFeatureList features{well};
    QVERIFY(wells.dataProvider()->addFeatures(features));
    wells.updateExtents();
    auto symbol = QgsMarkerSymbol::createSimple({});
    symbol->setColor(QColor(20, 60, 200));
    symbol->setSizeUnit(Qgis::RenderUnit::Pixels);
    symbol->setSize(8);
    wells.setRenderer(new QgsSingleSymbolRenderer(symbol.release()));
    QgsPalLayerSettings labels;
    labels.fieldName = QStringLiteral("name");
    labels.dist = 8;
    labels.distUnits = Qgis::RenderUnit::Pixels;
    QgsTextFormat format;
    format.setFont(QFont(QStringLiteral("Arial"), 12));
    format.setSize(12);
    labels.setFormat(format);
    wells.setLabeling(new QgsVectorLayerSimpleLabeling(labels));
    wells.setLabelsEnabled(true);
    const ob::Source source{QStringLiteral("delayed-tiles"), QStringLiteral("delayed"),
        QStringLiteral("http://127.0.0.1:%1/{z}/{x}/{y}.png").arg(server.port()), {}, 4};
    std::unique_ptr<QgsMapLayer> base(ob::createLayer(source));
    QVERIFY(base);
    server.hold();
    QgsMapCanvas canvas;
    ob::enableResponsiveForeground(&canvas);
    canvas.resize(400, 400);
    canvas.setDestinationCrs(wells.crs());
    canvas.setLayers({&wells, base.get()});
    canvas.setExtent(QgsRectangle(-100000, -100000, 100000, 100000));
    auto finish = qScopeGuard([&] {
      server.release(true);
      canvas.stopRendering();
    });
    canvas.show();
    canvas.refresh();
    QTRY_VERIFY_WITH_TIMEOUT(server.held.load() > 0, 5000);
    QVERIFY(canvas.isDrawing());
    auto *overlay = canvas.findChild<QLabel *>(QStringLiteral("responsiveForegroundOverlay"));
    QVERIFY(overlay);
    QTRY_VERIFY_WITH_TIMEOUT(overlay->isVisible(), 3000);
    const auto foreground = overlay->pixmap().toImage();
    QVERIFY(!foreground.isNull());
    const int cx = foreground.width() / 2, cy = foreground.height() / 2;
    QVERIFY(foreground.pixelColor(cx, cy).alpha() > 0); // 井点
    int labelPixels = 0;
    for (int y = cy - 70; y <= cy + 70; ++y)
      for (int x = cx - 70; x <= cx + 70; ++x)
        if ((std::abs(x - cx) > 12 || std::abs(y - cy) > 12) && foreground.pixelColor(x, y).alpha() > 0)
          ++labelPixels;
    QVERIFY(labelPixels > 10); // 点符号以外的井名像素
    canvas.setExtent(QgsRectangle(-50000, -100000, 150000, 100000));
    QVERIFY(!overlay->isVisible());
    canvas.refresh();
    QTRY_VERIFY_WITH_TIMEOUT(overlay->isVisible(), 3000);
    const auto moved = overlay->pixmap().toImage();
    QVERIFY(moved.pixelColor(moved.width() / 4, moved.height() / 2).alpha() > 0);
    QCOMPARE(moved.pixelColor(cx, cy).alpha(), 0); // 旧视野中心不能残留井点。
    server.release(failTiles);
    QTRY_VERIFY_WITH_TIMEOUT(!canvas.isDrawing(), 5000);
    QVERIFY(!overlay->isVisible());
    QVERIFY(canvas.labelingResults());
    const auto renderedLabels = canvas.labelingResults()->allLabels();
    QVERIFY(std::any_of(renderedLabels.cbegin(), renderedLabels.cend(), [](const auto &label) { return label.labelText == QLatin1String("A1"); }));
  }

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
