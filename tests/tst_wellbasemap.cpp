// #14：所有井位可视化页面都能加载 GIS 底图。
// 全部用本地夹具（GDAL 现造 GeoTIFF、file:// XYZ 瓦片目录、内存矢量层），
// 不访问任何外部瓦片服务。
#include <QtTest>
#include <QComboBox>
#include <QImage>
#include <QTemporaryDir>

#include "helpers/previewfixture.h"
#include "../src/catalog/datacatalog.h"
#include "../src/qgis/basemaplayer.h"
#include "../src/qgis/onlinebasemap.h"
#include "../src/qgis/qgisruntime.h"
#include "../src/ui/datapreview/datapreviewtabs.h"
#include "../src/ui/datapreview/previewbasemapcontrol.h"
#include "../src/ui/datapreview/previewmappage.h"

#include <cpl_conv.h>
#include <gdal.h>
#include <ogr_srs_api.h>

#include <qgscoordinatereferencesystem.h>
#include <qgscoordinatetransformcontext.h>
#include <qgsmapcanvas.h>
#include <qgsmaprenderersequentialjob.h>
#include <qgsmapsettings.h>
#include <qgsproject.h>
#include <qgsrasterlayer.h>
#include <qgsvectorlayer.h>

using namespace paleo::tests::preview;
namespace bm = paleo::basemap;

namespace {
// 16×16 单波段 GeoTIFF；epsg<=0 不写坐标系。
QString makeTiff(const QString &path, double x0, double y0, double size, int epsg)
{
  GDALAllRegister();
  GDALDriverH driver = GDALGetDriverByName("GTiff");
  if (!driver) return {};
  GDALDatasetH ds = GDALCreate(driver, path.toUtf8().constData(), 16, 16, 1, GDT_Byte, nullptr);
  if (!ds) return {};
  double gt[6] = {x0, size / 16.0, 0, y0 + size, 0, -size / 16.0};
  GDALSetGeoTransform(ds, gt);
  if (epsg > 0) {
    OGRSpatialReferenceH srs = OSRNewSpatialReference(nullptr);
    OSRImportFromEPSG(srs, epsg);
    char *wkt = nullptr;
    OSRExportToWkt(srs, &wkt);
    GDALSetProjection(ds, wkt);
    CPLFree(wkt);
    OSRDestroySpatialReference(srs);
  }
  std::vector<unsigned char> px(256, 200);
  if (GDALRasterIO(GDALGetRasterBand(ds, 1), GF_Write, 0, 0, 16, 16, px.data(), 16, 16, GDT_Byte, 0, 0) != CE_None) {
    GDALClose(ds);
    return {};
  }
  GDALClose(ds);
  return path;
}

// 多井资产：选中 A1 才建井位图（同 tst_previewmap_assets 口径）。
void pickWell(DataPreviewTabs *preview)
{
  auto *tabs = preview->findChild<QTabWidget *>(QStringLiteral("dataPreviewTabs"));
  auto *combo = tabs ? tabs->widget(tabs->currentIndex())->findChild<QComboBox *>(QStringLiteral("wellCombo")) : nullptr;
  if (combo) combo->setCurrentIndex(combo->findText(QStringLiteral("A1")));
  QTest::qWait(50);
}

PreviewBasemapControl *controlOf(QWidget *root, const QString &pageName)
{
  auto *page = root->findChild<PreviewMapPage *>(pageName);
  return page ? page->findChild<PreviewBasemapControl *>() : nullptr;
}
} // namespace

class TestWellBasemap : public QObject
{
  Q_OBJECT
private slots:
  void initTestCase() { QVERIFY(QgisRuntime::isInitialized()); }

  void choiceEncodingRoundTrips()
  {
    for (const QString spec : {QStringLiteral("osm"), QStringLiteral("xyz:https://t.example/{z}/{x}/{y}.png"),
                               QStringLiteral("file:/data/base.tif"), QStringLiteral("layer:abc_123")})
      QCOMPARE(bm::encode(bm::decode(spec)), spec);
    QCOMPARE(bm::decode(QStringLiteral("none")).kind, bm::Kind::None);
    QCOMPARE(bm::decode(QString()).kind, bm::Kind::None);
    QCOMPARE(bm::decode(QStringLiteral("file:/a.tif")).kind, bm::Kind::File);
  }

  void xyzUrlValidationAndPrivacy()
  {
    QString why;
    QVERIFY(bm::isValidXyzUrl(QStringLiteral("https://t.example/{z}/{x}/{y}.png"), &why));
    QVERIFY(bm::isValidXyzUrl(QStringLiteral("file:///tiles/{z}/{x}/{y}.png")));
    QVERIFY(!bm::isValidXyzUrl(QStringLiteral("ftp://t.example/{z}/{x}/{y}.png"), &why));
    QVERIFY(why.contains(QStringLiteral("http")));
    QVERIFY(!bm::isValidXyzUrl(QStringLiteral("https://t.example/{x}/{y}.png"), &why));
    QVERIFY(why.contains(QStringLiteral("{z}")));
    QVERIFY(!bm::createXyz(QStringLiteral("not a url"), nullptr));
    // 显示名只露主机：查询串里的访问令牌不进界面。
    const QString shown = bm::displayName({bm::Kind::Xyz, QStringLiteral("https://tiles.example.com/{z}/{x}/{y}?tk=SECRET")});
    QVERIFY(shown.contains(QStringLiteral("tiles.example.com")));
    QVERIFY(!shown.contains(QStringLiteral("SECRET")));
  }

  void crsRules()
  {
    QTemporaryDir tmp;
    const auto eng = QgsCoordinateReferenceSystem::fromWkt(DataCatalog::localGridCrsWkt());
    QVERIFY(eng.isValid());
    const auto wgs84 = QgsCoordinateReferenceSystem(QStringLiteral("EPSG:4326"));
    const auto merc = QgsCoordinateReferenceSystem(QStringLiteral("EPSG:3857"));
    const QgsCoordinateTransformContext ctx;
    QString msg;

    // 无坐标系底图：按画布坐标系叠加 + 明确警告。
    std::unique_ptr<QgsMapLayer> bare(bm::createFromFile(makeTiff(tmp.filePath("bare.tif"), 0, 0, 1000, 0), nullptr, &msg));
    QVERIFY2(bare, qPrintable(msg));
    QVERIFY(bm::checkCrs(bare.get(), eng, ctx, &msg));
    QVERIFY(msg.contains(QStringLiteral("没有坐标系")));
    QCOMPARE(bare->crs(), eng);

    // 地理坐标系底图 → Web 墨卡托画布：实时重投影。
    std::unique_ptr<QgsMapLayer> geo(bm::createFromFile(makeTiff(tmp.filePath("geo.tif"), 110, 30, 1, 4326), nullptr, &msg));
    QVERIFY2(geo, qPrintable(msg));
    QCOMPARE(geo->crs(), wgs84);
    QVERIFY(bm::checkCrs(geo.get(), merc, ctx, &msg));
    QVERIFY(msg.contains(QStringLiteral("重投影")));
    QVERIFY(bm::checkCrs(geo.get(), wgs84, ctx, &msg));
    QVERIFY(msg.isEmpty());
    // 未配准的工程坐标系画布：无法转换 → 拒绝并说明。
    QVERIFY(!bm::checkCrs(geo.get(), eng, ctx, &msg));
    QVERIFY(msg.contains(QStringLiteral("无法转换")));

    // XYZ 瓦片只叠加到真实坐标系画布。
    std::unique_ptr<QgsMapLayer> xyz(bm::createXyz(QStringLiteral("file:///nonexistent/{z}/{x}/{y}.png"), nullptr, &msg));
    QVERIFY2(xyz, qPrintable(msg));
    QVERIFY(xyz->customProperty("paleoBasemap").toBool());
    QVERIFY(!bm::checkCrs(xyz.get(), eng, ctx, &msg));
    QVERIFY(msg.contains(QStringLiteral("工程坐标系")));
    QVERIFY(bm::checkCrs(xyz.get(), merc, ctx, &msg));

    // 非 GIS 文件：明确报错，不建层。
    QFile junk(tmp.filePath("junk.tif"));
    QVERIFY(junk.open(QIODevice::WriteOnly));
    junk.write("not a raster");
    junk.close();
    QVERIFY(!bm::createFromFile(junk.fileName(), nullptr, &msg));
    QVERIFY(msg.contains(QStringLiteral("无法读取")));
  }

  // 井位类页面都挂了底图控件：测区全景、井位表、分层落图、井相/GeoJSON 成果。
  void wellViewsCarryBasemapControl()
  {
    QTemporaryDir tmp;
    auto st = makeStack(tmp.filePath(QStringLiteral("proj")));
    QVERIFY(st);
    const Imported ids = importAll(*st, tmp);
    st->preview->setProject(st->projectSvc.project());
    st->preview->openSurveyArea();
    QVERIFY(controlOf(st->preview.get(), QStringLiteral("surveyPreviewPage")));
    st->preview->openAsset(ids.wellHead);
    pickWell(st->preview.get());
    QVERIFY(controlOf(st->preview.get(), QStringLiteral("wellHeadPreviewPage")));
    st->preview->openAsset(ids.tops);
    pickWell(st->preview.get());
    QVERIFY(controlOf(st->preview.get(), QStringLiteral("topsPreviewPage")));
    st->preview->openAsset(ids.geojson);
    auto *facies = controlOf(st->preview.get(), QStringLiteral("faciesPreviewPage"));
    QVERIFY(facies);
    QVERIFY(facies->findChild<QComboBox *>(QStringLiteral("faciesOnlineBasemap")));
    QVERIFY(facies->findChild<QWidget *>(QStringLiteral("faciesBasemapMore")));
    // 每页只挂一个控件（重复挂载会叠两个底图）。
    for (auto *page : st->preview->findChildren<PreviewMapPage *>())
      QVERIFY(page->findChildren<PreviewBasemapControl *>().size() <= 1);
  }

  // 井位表页：本地文件底图压在井位层下面，选择按工程记忆并在其它井位页复用；
  // 工程图层作底图是克隆，不改动工程。
  void wellHeadLocalFileAndProjectLayer()
  {
    QTemporaryDir tmp;
    auto st = makeStack(tmp.filePath(QStringLiteral("proj")));
    QVERIFY(st);
    const Imported ids = importAll(*st, tmp);
    auto *project = st->projectSvc.project();
    project->setCrs(QgsCoordinateReferenceSystem::fromWkt(DataCatalog::localGridCrsWkt()));
    st->preview->setProject(project);
    st->preview->openAsset(ids.wellHead);
    pickWell(st->preview.get());
    auto *page = st->preview->findChild<PreviewMapPage *>(QStringLiteral("wellHeadPreviewPage"));
    QVERIFY(page);
    auto *control = page->findChild<PreviewBasemapControl *>();
    QVERIFY(control);
    // 工程坐标系：在线瓦片下拉禁用并说明原因。
    QVERIFY(!control->comboBox()->isEnabled());
    QVERIFY(control->comboBox()->toolTip().contains(QStringLiteral("工程坐标系")));
    const int before = page->mapCanvas()->layers().size();

    // 无坐标系的本地栅格：叠加 + 警告；最底层；井位层仍在。
    const QString tif = makeTiff(tmp.filePath("local.tif"), 0, 0, 100000, 0);
    QVERIFY(!tif.isEmpty());
    QVERIFY2(control->applyChoice(QStringLiteral("file:") + tif), qPrintable(control->statusText()));
    QVERIFY(control->basemapLayer());
    QCOMPARE(page->mapCanvas()->layers().size(), before + 1);
    QVERIFY(static_cast<QObject *>(page->mapCanvas()->layers().last()) == control->basemapLayer());
    QVERIFY(control->statusText().contains(QStringLiteral("没有坐标系")));
    QCOMPARE(paleo::onlinebasemap::savedChoice(project), QStringLiteral("file:") + tif);

    // 未配准的工程坐标系无法转换 → 拒绝，保留当前可用底图与选择。
    auto *previous = control->basemapLayer();
    const QString geo = makeTiff(tmp.filePath("geo.tif"), 110, 30, 1, 4326);
    QVERIFY(!control->applyChoice(QStringLiteral("file:") + geo));
    QCOMPARE(control->basemapLayer(), previous);
    QVERIFY(control->statusText().contains(QStringLiteral("无法转换")));
    QCOMPARE(control->currentChoice(), QStringLiteral("file:") + tif);
    QCOMPARE(page->mapCanvas()->layers().size(), before + 1);
    QCOMPARE(paleo::onlinebasemap::savedChoice(project), QStringLiteral("file:") + tif); // 失败不覆盖记忆

    // 记忆的选择在其它井位页自动恢复（分层落图页）。
    st->preview->openAsset(ids.tops);
    pickWell(st->preview.get());
    auto *tops = controlOf(st->preview.get(), QStringLiteral("topsPreviewPage"));
    QVERIFY(tops);
    QVERIFY(tops->basemapLayer());
    QCOMPARE(tops->currentChoice(), QStringLiteral("file:") + tif);

    // 工程已有图层（同坐标系内存层）作底图：克隆进预览，工程图层数不变。
    auto *vl = new QgsVectorLayer(QStringLiteral("Polygon"), QStringLiteral("区域边界"), QStringLiteral("memory"));
    vl->setCrs(project->crs());
    QVERIFY(vl->isValid());
    project->addMapLayer(vl);
    const int projectLayers = project->count();
    QVERIFY(bm::projectCandidates(project).contains(vl));
    QVERIFY2(control->applyChoice(QStringLiteral("layer:") + vl->id()), qPrintable(control->statusText()));
    QVERIFY(control->basemapLayer());
    QVERIFY(control->basemapLayer() != static_cast<QObject *>(vl));
    QCOMPARE(project->count(), projectLayers);
    QVERIFY(control->comboBox()->currentText().contains(QStringLiteral("区域边界")));
    QVERIFY(control->applyChoice(QStringLiteral("none")));
    QVERIFY(!control->basemapLayer());
    QCOMPARE(page->mapCanvas()->layers().size(), before);
  }

  // 自定义 XYZ：file:// 本地瓦片目录真实出图（不联网）；URL 不写进工程文件。
  void customXyzRendersLocalTiles()
  {
    QTemporaryDir tmp;
    QVERIFY(QDir().mkpath(tmp.filePath("tiles/0/0")));
    QImage tile(256, 256, QImage::Format_ARGB32);
    tile.fill(QColor(220, 30, 30));
    QVERIFY(tile.save(tmp.filePath("tiles/0/0/0.png")));
    auto st = makeStack(tmp.filePath(QStringLiteral("proj")));
    QVERIFY(st);
    auto *project = st->projectSvc.project();
    project->setCrs(QgsCoordinateReferenceSystem(QStringLiteral("EPSG:3857")));
    st->preview->setProject(project);
    st->preview->openSurveyArea();
    auto *control = controlOf(st->preview.get(), QStringLiteral("surveyPreviewPage"));
    QVERIFY(control);
    QVERIFY(control->comboBox()->isEnabled());
    const QString url = QUrl::fromLocalFile(tmp.filePath("tiles")).toString() + QStringLiteral("/{z}/{x}/{y}.png?tk=SECRET");
    QVERIFY2(control->applyChoice(QStringLiteral("xyz:") + url), qPrintable(control->statusText()));
    auto *layer = qobject_cast<QgsMapLayer *>(control->basemapLayer());
    QVERIFY(layer);
    QVERIFY(layer->customProperty("paleoBasemap").toBool());
    QVERIFY(!control->statusText().contains(QStringLiteral("SECRET")));
    QVERIFY(!control->comboBox()->currentText().contains(QStringLiteral("SECRET")));
    // 工程文件只留标记，地址（可能带密钥）只在本机设置。
    QCOMPARE(project->readEntry(QStringLiteral("paleo"), QStringLiteral("survey/onlineBasemap")), QStringLiteral("xyz"));
    QCOMPARE(paleo::onlinebasemap::savedChoice(project), QStringLiteral("xyz:") + url);

    // 出图：z0 整幅世界 256×256，中心像素应是瓦片红色。
    // 去掉查询串的同目录模板再建一次，确保 file:// 取瓦片真实可读。
    std::unique_ptr<QgsMapLayer> plain(bm::createXyz(QUrl::fromLocalFile(tmp.filePath("tiles")).toString() + QStringLiteral("/{z}/{x}/{y}.png"), nullptr));
    QVERIFY(plain);
    QgsMapSettings ms;
    ms.setDestinationCrs(QgsCoordinateReferenceSystem(QStringLiteral("EPSG:3857")));
    ms.setLayers({plain.get()});
    const double w = 20037508.342789244;
    ms.setExtent(QgsRectangle(-w, -w, w, w));
    ms.setOutputSize(QSize(256, 256));
    ms.setBackgroundColor(Qt::white);
    QgsMapRendererSequentialJob job(ms);
    job.start();
    job.waitForFinished();
    const QColor c = job.renderedImage().pixelColor(128, 128);
    QVERIFY2(c.red() > 200 && c.green() < 60 && c.blue() < 60, qPrintable(c.name()));
  }
};

int main(int argc, char *argv[])
{
  if (!QgisRuntime::initialize(QStringLiteral("/usr"))) {
    qFatal("QgisRuntime::initialize failed");
    return 1;
  }
  TestWellBasemap tc;
  const int rc = QTest::qExec(&tc, argc, argv);
  QgisRuntime::shutdown();
  return rc;
}

#include "tst_wellbasemap.moc"
