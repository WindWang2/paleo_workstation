// 层：测试壳
#include <QtTest>
#include <QSettings>
#include <QTemporaryDir>
#include <QUrl>
#include <QVBoxLayout>
#include <qgsapplication.h>
#include <qgsmapcanvas.h>
#include <qgsmaprenderersequentialjob.h>
#include <qgsproject.h>
#include <qgsrasterlayer.h>
#include <qgsvectorlayer.h>
#include "qgis/onlinebasemap.h"
#include "services/paleotaskservice.h"
#include "ui/datapreview/datapreviewtabs.h"
#include "ui/datapreview/previewbasemapcontrol.h"
#include "ui/datapreview/previewmappage.h"

class TestBasemapRendering : public QObject
{
  Q_OBJECT
private slots:
  void selectionReplacesOpaqueBackgroundAndRestoresVisibility()
  {
    QTemporaryDir dir;
    QVERIFY(QDir(dir.path()).mkpath(QStringLiteral("0/0")));
    QImage tile(256, 256, QImage::Format_RGB32);
    tile.fill(QColor(220, 30, 30));
    QVERIFY(tile.save(dir.filePath(QStringLiteral("0/0/0.png"))));
    QVERIFY(QDir(dir.path()).mkpath(QStringLiteral("background/0/0")));
    tile.fill(QColor(40, 80, 160));
    QVERIFY(tile.save(dir.filePath(QStringLiteral("background/0/0/0.png"))));
    PaleoTaskService tasks;
    QSignalSpy added(&tasks, &PaleoTaskService::taskAdded);
    PreviewMapPage page;
    page.setTaskService(&tasks);
    auto *view = page.mapCanvas();
    view->setOverrideCrs(QgsCoordinateReferenceSystem(QStringLiteral("EPSG:3857")));
    paleo::onlinebasemap::Source background{QStringLiteral("fixture"), QStringLiteral("background"),
        QUrl::fromLocalFile(dir.filePath(QStringLiteral("background"))).toString() + QStringLiteral("/{z}/{x}/{y}.png"), {}, 0};
    auto *offline = paleo::onlinebasemap::createLayer(background, &page);
    QVERIFY(offline);
    // 工程离线背景没有在线选择标识，预览原本会把它留在新来源上方。
    offline->removeCustomProperty(QStringLiteral("paleoOnlineBasemap"));
    page.addMapLayer(offline, QStringLiteral("offline"), {});
    QCOMPARE(added.count(), 0); // 底图入 TOC 不得启动全图统计任务。
    auto *hidden = new QgsVectorLayer(QStringLiteral("Polygon?crs=EPSG:3857"), QStringLiteral("hidden"), QStringLiteral("memory"));
    hidden->setParent(&page);
    hidden->setCustomProperty(QStringLiteral("paleoBasemap"), true);
    page.addMapLayer(hidden, QStringLiteral("hidden"), {});
    view->setLayerVisible(hidden, false);
    auto *geology = new QgsVectorLayer(QStringLiteral("Point?crs=EPSG:3857"), QStringLiteral("geology"), QStringLiteral("memory"));
    geology->setParent(&page);
    page.addMapLayer(geology, QStringLiteral("geology"), {});
    PreviewBasemapControl control(&page, nullptr, QStringLiteral("test"));
    const auto choice = QStringLiteral("xyz:") + QUrl::fromLocalFile(dir.path()).toString() + QStringLiteral("/{z}/{x}/{y}.png");
    QVERIFY2(control.applyChoice(choice, false), qPrintable(control.statusText()));
    QCOMPARE(added.count(), 0);
    QVERIFY(!view->isLayerVisible(offline));
    QVERIFY(!view->isLayerVisible(hidden));
    QVERIFY(view->isLayerVisible(geology));
    auto settings = view->canvas()->mapSettings();
    const double edge = 20037508.342789244;
    settings.setExtent(QgsRectangle(-edge, -edge, edge, edge));
    settings.setOutputSize(QSize(256, 256));
    QgsMapRendererSequentialJob job(settings);
    job.start();
    job.waitForFinished();
    QVERIFY(job.errors().isEmpty());
    const auto color = job.renderedImage().pixelColor(128, 128);
    QCOMPARE(color, QColor(220, 30, 30));
    // 再选一次不能把已暂时隐藏的背景记成原来就隐藏。
    QVERIFY(control.applyChoice(choice, false));
    auto *selected = control.basemapLayer();
    QVERIFY(!control.applyChoice(QStringLiteral("file:/missing-basemap.tif"), false));
    QCOMPARE(control.basemapLayer(), selected);
    QCOMPARE(control.currentChoice(), choice);
    QVERIFY(control.applyChoice(QStringLiteral("none"), false));
    QVERIFY(view->isLayerVisible(offline));
    QVERIFY(!view->isLayerVisible(hidden));
    QVERIFY(view->isLayerVisible(geology));
  }

  void menuSelectionTargetsOnlyVisiblePreview()
  {
    QTemporaryDir dir;
    QVERIFY(QDir(dir.path()).mkpath(QStringLiteral("0/0")));
    QImage tile(256, 256, QImage::Format_RGB32);
    tile.fill(Qt::red);
    QVERIFY(tile.save(dir.filePath(QStringLiteral("0/0/0.png"))));
    const auto choice = QStringLiteral("xyz:") + QUrl::fromLocalFile(dir.path()).toString() + QStringLiteral("/{z}/{x}/{y}.png");
    DataPreviewTabs tabs;
    auto *content = new QWidget;
    auto *layout = new QVBoxLayout(content);
    auto *page = new PreviewMapPage(content);
    layout->addWidget(page);
    page->mapCanvas()->setOverrideCrs(QgsCoordinateReferenceSystem(QStringLiteral("EPSG:3857")));
    auto *control = attachPreviewBasemap(page, nullptr, QStringLiteral("test"));
    tabs.openWorkspace(content, QStringLiteral("map"));
    tabs.resize(900, 700);
    tabs.show();
    QTest::qWait(20);
    QVERIFY(tabs.applyBasemapToCurrentMap(choice));
    QCOMPARE(control->currentChoice(), choice);
    tabs.hide();
    QVERIFY(!tabs.applyBasemapToCurrentMap(QStringLiteral("none")));
    QCOMPARE(control->currentChoice(), choice);
  }
};

int main(int argc, char **argv)
{
  qputenv("QT_QPA_PLATFORM", "offscreen");
  QTemporaryDir settings;
  QSettings::setDefaultFormat(QSettings::IniFormat);
  QSettings::setPath(QSettings::IniFormat, QSettings::UserScope, settings.path());
  qputenv("GDAL_DEFAULT_WMS_CACHE_PATH", settings.filePath(QStringLiteral("tiles")).toUtf8());
  QgsApplication app(argc, argv, true);
  QgsApplication::setPrefixPath(qEnvironmentVariable("QGIS_PREFIX_PATH"), true);
  QgsApplication::initQgis();
  TestBasemapRendering test;
  const int result = QTest::qExec(&test, argc, argv);
  QgsApplication::exitQgis();
  return result;
}

#include "tst_basemap_rendering.moc"
