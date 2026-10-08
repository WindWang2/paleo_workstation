#include <QtTest>

#include "../src/qgis/previewidentify.h"
#include "../src/qgis/qgisruntime.h"

#include <qgsmapcanvas.h>
#include <qgsrasterlayer.h>
#include <qgsvectorlayer.h>

#include <gdal.h>

#include <QTemporaryDir>
#include <memory>

// P2 D7/D6.4 PreviewIdentifyCore 契约：矢量点选/框选（空间索引缓存复用）、
// 栅格点取值（最近邻 + 双线性插值 D7.4）、层析构索引清理、无命中如实空。
class TestPreviewIdentify : public QObject
{
  Q_OBJECT

  private slots:
    void initTestCase()
    {
      QVERIFY(QgisRuntime::isInitialized());
      GDALAllRegister();
    }

    void pointIdentifyHitsVectorFeature();
    void pointIdentifyMissIsEmpty();
    void pointIdentifyRespectsTolerance();
    void rectIdentifyReturnsMultiple();
    void rectIdentifyCapsPerLayer();
    void spatialIndexIsCachedPerLayer();
    void indexClearedWhenLayerDestroyed();
    void indexClearedOnFeatureEdit();
    void rasterPointGivesNearestAndBilinear();
    void rasterPointOutsideExtentIsMiss();
    void pointIdentifyAcrossMixedLayers();
    void rectIdentifyEmptyRectIsEmpty();

  private:
    QgsVectorLayer *makeGridLayer(int n);
    QgsRasterLayer *makeTinyRaster();

    PreviewIdentifyCore m_core;
};

QgsVectorLayer *TestPreviewIdentify::makeGridLayer(int n)
{
  auto *vl = new QgsVectorLayer(
      QStringLiteral("Point?crs=EPSG:3857&field=name:string"), QStringLiteral("grid"),
      QStringLiteral("memory"));
  for (int i = 0; i < n; ++i)
    for (int j = 0; j < n; ++j)
    {
      QgsFeature f(vl->fields());
      f.setAttribute(QStringLiteral("name"), QStringLiteral("p%1_%2").arg(i).arg(j));
      f.setGeometry(QgsGeometry::fromPointXY(QgsPointXY(100.0 * i, 100.0 * j)));
      vl->dataProvider()->addFeature(f);
    }
  vl->updateExtents();
  return vl; // 调用方持有（析构测试依赖它）
}

QgsRasterLayer *TestPreviewIdentify::makeTinyRaster()
{
  // 4×4 float GeoTIFF：值 = 列号（x 向），行号自上而下。像素 100m。
  static QTemporaryDir dir;
  const QString path = dir.filePath(QStringLiteral("tiny_identify.tif"));
  if (QFile::exists(path))
    QFile::remove(path);
  GDALDriverH drv = GDALGetDriverByName("GTiff");
  if (!drv)
    return nullptr;
  GDALDatasetH ds =
      GDALCreate(drv, path.toUtf8().constData(), 4, 4, 1, GDT_Float64, nullptr);
  if (!ds)
    return nullptr;
  double geo[6] = {0.0, 100.0, 0.0, 400.0, 0.0, -100.0};
  GDALSetGeoTransform(ds, geo);
  double vals[16];
  for (int r = 0; r < 4; ++r)
    for (int c = 0; c < 4; ++c)
      vals[r * 4 + c] = double(c); // 列号即值
  GDALRasterIO(GDALGetRasterBand(ds, 1), GF_Write, 0, 0, 4, 4, vals, 4, 4, GDT_Float64,
               0, 0);
  GDALClose(ds);
  auto *rl = new QgsRasterLayer(path, QStringLiteral("tiny"), QStringLiteral("gdal"));
  return rl;
}

void TestPreviewIdentify::pointIdentifyHitsVectorFeature()
{
  std::unique_ptr<QgsVectorLayer> vl(makeGridLayer(4));
  const auto results = m_core.identifyPoint({vl.get()}, QgsPointXY(200.0, 300.0), 0.5);
  QCOMPARE(results.size(), 1);
  QCOMPARE(results.at(0).layerName, QStringLiteral("grid"));
  QCOMPARE(results.at(0).attributes.value(QStringLiteral("name")).toString(),
           QStringLiteral("p2_3"));
}

void TestPreviewIdentify::pointIdentifyMissIsEmpty()
{
  std::unique_ptr<QgsVectorLayer> vl(makeGridLayer(2));
  const auto results = m_core.identifyPoint({vl.get()}, QgsPointXY(9999.0, 9999.0), 1.0);
  QVERIFY(results.isEmpty()); // D7.6 空命中如实空
}

void TestPreviewIdentify::pointIdentifyRespectsTolerance()
{
  std::unique_ptr<QgsVectorLayer> vl(makeGridLayer(4));
  // 距点 30m：容差 10m 未命中，容差 50m 命中。
  QVERIFY(m_core.identifyPoint({vl.get()}, QgsPointXY(230.0, 100.0), 10.0).isEmpty());
  const auto hit = m_core.identifyPoint({vl.get()}, QgsPointXY(230.0, 100.0), 50.0);
  QCOMPARE(hit.size(), 1);
}

void TestPreviewIdentify::rectIdentifyReturnsMultiple()
{
  std::unique_ptr<QgsVectorLayer> vl(makeGridLayer(4));
  const auto results =
      m_core.identifyRect({vl.get()}, QgsRectangle(0, 0, 250, 250), 100);
  QCOMPARE(results.size(), 9); // 3×3 个格点
}

void TestPreviewIdentify::rectIdentifyCapsPerLayer()
{
  std::unique_ptr<QgsVectorLayer> vl(makeGridLayer(4));
  const auto results =
      m_core.identifyRect({vl.get()}, QgsRectangle(0, 0, 350, 350), 5);
  QCOMPARE(results.size(), 5); // maxPerLayer 截断
}

void TestPreviewIdentify::spatialIndexIsCachedPerLayer()
{
  std::unique_ptr<QgsVectorLayer> vl(makeGridLayer(8));
  m_core.identifyPoint({vl.get()}, QgsPointXY(0, 0), 1.0);
  QCOMPARE(m_core.indexCacheSize(), 1);
  // 二次查询复用（不新增缓存行）。
  m_core.identifyRect({vl.get()}, QgsRectangle(0, 0, 100, 100), 10);
  QCOMPARE(m_core.indexCacheSize(), 1); // D6.4
  m_core.dropIndex(vl.get());
  QCOMPARE(m_core.indexCacheSize(), 0);
}

void TestPreviewIdentify::indexClearedWhenLayerDestroyed()
{
  {
    QgsVectorLayer *vl = makeGridLayer(4);
    m_core.identifyPoint({vl}, QgsPointXY(0, 0), 1.0);
    QCOMPARE(m_core.indexCacheSize(), 1);
    delete vl; // 层析构 → 缓存行自动清（防悬空键）
  }
  QCOMPARE(m_core.indexCacheSize(), 0);
}

void TestPreviewIdentify::indexClearedOnFeatureEdit()
{
  std::unique_ptr<QgsVectorLayer> vl(makeGridLayer(2));
  m_core.identifyPoint({vl.get()}, QgsPointXY(0, 0), 1.0);
  QCOMPARE(m_core.indexCacheSize(), 1);

  // #235：编辑缓冲加要素 → 缓存索引失效（陈旧索引会让新要素漏报）。
  vl->startEditing();
  QgsFeature added(vl->fields());
  added.setAttribute(QStringLiteral("name"), QStringLiteral("late"));
  added.setGeometry(QgsGeometry::fromPointXY(QgsPointXY(500.0, 500.0)));
  QVERIFY(vl->addFeature(added));
  QCOMPARE(m_core.indexCacheSize(), 0);
  // 再查即重建索引，新要素如实命中（回归：靠陈旧索引会漏报）。
  QCOMPARE(m_core.identifyPoint({vl.get()}, QgsPointXY(500.0, 500.0), 1.0).size(), 1);
  QCOMPARE(m_core.indexCacheSize(), 1);

  // 提交变更 → 同样失效。
  QVERIFY(vl->commitChanges());
  QCOMPARE(m_core.indexCacheSize(), 0);
  m_core.identifyPoint({vl.get()}, QgsPointXY(0, 0), 1.0);
  QCOMPARE(m_core.indexCacheSize(), 1);

  // 删除路径：编辑缓冲删要素 → 失效。
  const QgsFeatureIds ids = vl->allFeatureIds();
  QVERIFY(!ids.isEmpty());
  vl->startEditing();
  QVERIFY(vl->deleteFeature(*ids.constBegin()));
  QCOMPARE(m_core.indexCacheSize(), 0);
}

void TestPreviewIdentify::rasterPointGivesNearestAndBilinear()
{
  std::unique_ptr<QgsRasterLayer> rl(makeTinyRaster());
  QVERIFY2(rl && rl->isValid(), "tiny raster fixture");
  // 像素中心 (150, 350)：最近邻 = 列 1；双线性在列 1/2 之间 → 1.5。
  double nearest = -1.0;
  double bilinear = -1.0;
  QVERIFY(PreviewIdentifyCore::sampleRaster(rl.get(), QgsPointXY(150.0, 350.0), 1,
                                            &nearest, &bilinear));
  QCOMPARE(nearest, 1.0);
  QCOMPARE(bilinear, 1.0); // 正中心时插值=最近邻
  // 亚像元：x=175 → 双线性 1.25。
  QVERIFY(PreviewIdentifyCore::sampleRaster(rl.get(), QgsPointXY(175.0, 350.0), 1,
                                            &nearest, &bilinear));
  QCOMPARE(nearest, 1.0);
  QVERIFY(qAbs(bilinear - 1.25) < 0.01);
}

void TestPreviewIdentify::rasterPointOutsideExtentIsMiss()
{
  std::unique_ptr<QgsRasterLayer> rl(makeTinyRaster());
  QVERIFY(rl && rl->isValid());
  double nearest = 0.0;
  double bilinear = 0.0;
  QVERIFY(!PreviewIdentifyCore::sampleRaster(rl.get(), QgsPointXY(9999.0, -9999.0), 1,
                                             &nearest, &bilinear));
  // identifyPoint 路径也不产假值。
  const auto results = m_core.identifyPoint({rl.get()}, QgsPointXY(9999.0, -9999.0), 1.0);
  QVERIFY(results.isEmpty());
}

void TestPreviewIdentify::pointIdentifyAcrossMixedLayers()
{
  // 混合层（矢量在上、栅格在下）：点查同时命中矢量要素与栅格取值，
  // 结果序 = 层序（视顶到视底）。
  std::unique_ptr<QgsVectorLayer> vl(makeGridLayer(4));
  std::unique_ptr<QgsRasterLayer> rl(makeTinyRaster());
  QVERIFY(rl && rl->isValid());
  // 矢量点 (200,300)=p2_3 落在栅格范围内（0..400）：双命中。
  const auto results = m_core.identifyPoint({vl.get(), rl.get()}, QgsPointXY(200.0, 300.0), 0.5);
  QCOMPARE(results.size(), 2);
  QVERIFY(!results.at(0).isRaster); // 矢量在前
  QCOMPARE(results.at(0).attributes.value(QStringLiteral("name")).toString(),
           QStringLiteral("p2_3"));
  QVERIFY(results.at(1).isRaster); // 栅格在后
  QCOMPARE(results.at(1).rasterValueNearest, 2.0);
}

void TestPreviewIdentify::rectIdentifyEmptyRectIsEmpty()
{
  std::unique_ptr<QgsVectorLayer> vl(makeGridLayer(4));
  // 空矩形（零宽/零高）不产任何命中——不回退成全表。
  QVERIFY(m_core.identifyRect({vl.get()}, QgsRectangle(100, 100, 100, 100), 10).isEmpty());
  QVERIFY(m_core.identifyRect({vl.get()}, QgsRectangle(), 10).isEmpty());
}

int main(int argc, char *argv[])
{
  if (!QgisRuntime::initialize(QStringLiteral("/usr")))
  {
    qFatal("QgisRuntime::initialize failed");
    return 1;
  }
  TestPreviewIdentify tc;
  const int rc = QTest::qExec(&tc, argc, argv);
  QgisRuntime::shutdown();
  return rc;
}

#include "tst_previewmap_identify.moc"
