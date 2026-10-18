#include <QtTest>
#include <QSignalSpy>
#include <QTemporaryDir>

#include <gdal.h>
#include <cpl_error.h>

#include "../src/qgis/mappingartifactwriter.h"
#include "../src/qgis/previewmapcanvas.h"
#include "../src/qgis/previewrendercache.h"
#include "../src/qgis/qgisruntime.h"
#include "../src/qgis/projectmapreference.h"
#include "../src/qgis/mappingartifactwriter.h"
#include "../src/catalog/datacatalog.h"
#include <ogr_api.h>
#include <ogr_srs_api.h>
#include <qgscoordinatereferencesystem.h>
#include <qgscoordinatetransform.h>

#include <qgsmapcanvas.h>
#include <qgsmaplayer.h>
#include <qgsvectorlayer.h>
#include <qgsrasterlayer.h>
#include <qgsproject.h>
#include <qgsmapsettings.h>
#include <qgsmaprenderersequentialjob.h>
#include <qgsrectangle.h>

#include <algorithm>
#include <cmath>
#include <memory>

// P2 D1.1 PreviewMapCanvas 契约测试：私有层容器（不进 QgsProject）、CRS 钉死、
// 视图历史栈、渲染状态、渐进快照 overlay、键盘缩放平移、渲染缓存（D6.2/D6.6）。
class TestPreviewMapCanvas : public QObject
{
  Q_OBJECT

  private slots:
    void initTestCase() { QVERIFY(QgisRuntime::isInitialized()); }
    void init()
    {
      m_canvas = std::make_unique<PreviewMapCanvas>();
      m_canvas->resize(600, 400);
    }

    void defaultsToLocalGridCrs();
    void canvasIsChildWidget();
    void addLayerGoesOnTop();
    void removeAndMoveLayers();
    void layerDestroyedDropsAllBookkeeping();
    void setLayersReplacementDropsReplacedBookkeeping();
    void restoreRasterCrsHandlesProviderOptions();
    void visibilityGatesCanvasLayerList();
    void opacityAndBlendApplyToLayer();
    void layersNeverRegisteredInProject();
    void zoomHistoryBackForward();
    void historyDeduplicatesIdenticalExtents();
    void fullExtentCombinesVisibleLayers();
    void zoomToLayerGrowsSlightly();
    void renderSnapshotProducesImage();
    void overlayShowHideCycle();
    void keyboardPlusMinusZero();
    void keyboardArrowPan();
    void estimateElementsCountsVectorFeatures();
    void cacheKeyIgnoresSmallJitter();
    void cacheLruEviction();
    void cacheDiskRoundTrip();

    void setViewExtentIsExact();
    void cancelRenderingWithoutJobIsSafe();
    void localGridRasterRendersThroughGeoreference();
    void downgradedFaciesPolygonsRegainLocalGrid();
    void renderSignalLifecycleFires();
    void overlayFollowsCanvasResize();

  private:
    QgsVectorLayer *makePointLayer(int points, const QString &name);

    std::unique_ptr<PreviewMapCanvas> m_canvas;
};

QgsVectorLayer *TestPreviewMapCanvas::makePointLayer(int points, const QString &name)
{
  // 契约：预览层与画布共享工程局部网格 CRS（fullExtent 不做跨 CRS 变换）。
  auto *vl = new QgsVectorLayer(
      QStringLiteral("Point?crs=WKT:%1&field=id:int")
          .arg(DataCatalog::localGridCrsWkt()),
      name, QStringLiteral("memory"));
  for (int i = 0; i < points; ++i)
  {
    QgsFeature f(vl->fields());
    f.setAttribute(QStringLiteral("id"), i);
    f.setGeometry(QgsGeometry::fromPointXY(QgsPointXY(100.0 + i * 10, 200.0 + i * 5)));
    vl->dataProvider()->addFeature(f);
  }
  vl->updateExtents();
  vl->setParent(m_canvas.get()); // 画布销毁随测清层（与产品同构：宿主持有层）
  return vl;
}

void TestPreviewMapCanvas::defaultsToLocalGridCrs()
{
  const QgsCoordinateReferenceSystem crs = m_canvas->crs();
  QVERIFY(crs.isValid());
  // datum-free 局部工程网格：非经纬度（EPSG:4326）而是自定义米制网格。
  QVERIFY(crs != QgsCoordinateReferenceSystem::fromEpsgId(4326));
  QCOMPARE(m_canvas->canvas()->mapSettings().destinationCrs().authid(), crs.authid());
}

void TestPreviewMapCanvas::canvasIsChildWidget()
{
  QVERIFY(m_canvas->canvas() != nullptr);
  QCOMPARE(m_canvas->canvas()->parentWidget(), m_canvas.get());
}

void TestPreviewMapCanvas::addLayerGoesOnTop()
{
  auto *a = makePointLayer(1, QStringLiteral("a"));
  auto *b = makePointLayer(1, QStringLiteral("b"));
  m_canvas->addLayer(a);
  m_canvas->addLayer(b);
  QCOMPARE(m_canvas->layerCount(), 2);
  QCOMPARE(m_canvas->layers().first(), b); // 新层在顶
  QCOMPARE(m_canvas->layers().last(), a);
  QCOMPARE(m_canvas->indexOfLayer(b), 0);
  // 幂等：同一层重复 add 不重复。
  m_canvas->addLayer(b);
  QCOMPARE(m_canvas->layerCount(), 2);
}

void TestPreviewMapCanvas::removeAndMoveLayers()
{
  auto *a = makePointLayer(1, QStringLiteral("a"));
  auto *b = makePointLayer(1, QStringLiteral("b"));
  auto *c = makePointLayer(1, QStringLiteral("c"));
  for (auto *l : {a, b, c})
    m_canvas->addLayer(l);
  QCOMPARE(m_canvas->layers(), QList<QgsMapLayer *>({c, b, a}));
  m_canvas->moveLayer(0, 2); // c 移到底
  QCOMPARE(m_canvas->layers(), QList<QgsMapLayer *>({b, a, c}));
  m_canvas->removeLayer(c);
  QCOMPARE(m_canvas->layerCount(), 2);
  QCOMPARE(m_canvas->indexOfLayer(c), -1);
}

void TestPreviewMapCanvas::layerDestroyedDropsAllBookkeeping()
{
  // #483：层所有权归调用方，外部直接销毁（未经 removeLayer）后，容器/三层
  // 记账（可见/透明/混合）/画布层表不得残留悬垂指针——destroyed 钩子自动摘除。
  auto *a = makePointLayer(1, QStringLiteral("a"));
  auto *b = makePointLayer(2, QStringLiteral("b"));
  m_canvas->addLayer(a);
  m_canvas->addLayer(b);
  m_canvas->setLayerOpacity(b, 0.5);
  m_canvas->setLayerBlendMode(b, QPainter::CompositionMode_Multiply);
  m_canvas->setLayerVisible(a, false);
  QCOMPARE(m_canvas->layerCount(), 2);

  delete b; // 外部销毁（调用方持有所有权的一条路径）
  QTRY_COMPARE(m_canvas->layerCount(), 1);
  QCOMPARE(m_canvas->layers(), QList<QgsMapLayer *>({a}));
  QVERIFY(m_canvas->canvas()->layers().isEmpty()); // a 隐藏中，b 已摘除
  m_canvas->setLayerVisible(a, true);
  QCOMPARE(m_canvas->canvas()->layers(), QList<QgsMapLayer *>({a}));
  // 残亡层不再可查询记账（哈希键已摘）。
  QCOMPARE(m_canvas->layerOpacity(b), 1.0);
  QCOMPARE(m_canvas->layerBlendMode(b), QPainter::CompositionMode_SourceOver);

  delete a;
  QTRY_COMPARE(m_canvas->layerCount(), 0);
  QVERIFY(m_canvas->canvas()->layers().isEmpty());
}

// FD-003：setLayers 整表置换必须摘除被置换层的三层记账——旧实现只加不减，
// 地址复用时新层经 !contains 守卫继承陈旧可见性/透明度（TOC 显示可见、
// 画布不画）；仍在新表中的层不重置（重排不清用户显式设置）。
void TestPreviewMapCanvas::setLayersReplacementDropsReplacedBookkeeping()
{
  auto *a = makePointLayer(1, QStringLiteral("replace-a"));
  auto *b = makePointLayer(1, QStringLiteral("replace-b"));
  m_canvas->setLayers({a});
  QCOMPARE(m_canvas->isLayerVisible(a), true);
  m_canvas->setLayerVisible(a, false);
  m_canvas->setLayerOpacity(a, 0.3);
  // a 置换出去（记账应随之摘除）……
  m_canvas->setLayers({b});
  // ……再回表：默认可见/不透明，不继承置换前的隐藏+0.3。
  m_canvas->setLayers({a});
  QCOMPARE(m_canvas->isLayerVisible(a), true);
  QCOMPARE(m_canvas->layerOpacity(a), 1.0);
  // b 置换后回表同样取默认值。
  m_canvas->setLayers({b});
  QCOMPARE(m_canvas->isLayerVisible(b), true);
  QCOMPARE(m_canvas->layerOpacity(b), 1.0);
  // 重排不清用户显式设置：a、b 同表时对 b 隐藏，重排（同集合异序）后保持。
  m_canvas->setLayers({b, a});
  m_canvas->setLayerVisible(b, false);
  m_canvas->setLayers({a, b});
  QCOMPARE(m_canvas->isLayerVisible(b), false);
}

void TestPreviewMapCanvas::restoreRasterCrsHandlesProviderOptions()
{
  // #483：restoreRasterCrs 两处加固的红绿面——
  //  a) source 带 provider 选项串（"?iro=..."，QGIS 栅格 URI 惯例）时不得
  //     静默失效（修复前 GDALOpen 直接打不开，CRS 恢复悄悄跳过）；
  //  b) 同层只探测一次（幂等标记），重复触发不再重复磁盘打开。
  QTemporaryDir tmp;
  const QString tif = tmp.filePath(QStringLiteral("crsprobe.tif"));
  // 手写 tif：投影给 EPSG:4326、PALEO_CRS_WKT 内嵌工程局部网格——
  // 「恢复发生」可判别（CRS 从 4326 翻成局部网格）。
  GDALDatasetH ds = GDALCreate(GDALGetDriverByName("GTiff"),
                               tif.toUtf8().constData(), 4, 4, 1, GDT_Byte,
                               nullptr);
  QVERIFY(ds != nullptr);
  double gt[6] = {0, 25, 0, 100, 0, -25};
  QCOMPARE(GDALSetGeoTransform(ds, gt), CE_None);
  QCOMPARE(GDALSetProjection(
               ds, QgsCoordinateReferenceSystem::fromEpsgId(4326)
                       .toWkt()
                       .toUtf8()
                       .constData()),
           CE_None);
  QCOMPARE(GDALSetMetadataItem(ds, "PALEO_CRS_WKT",
                               DataCatalog::localGridCrsWkt().toUtf8().constData(),
                               nullptr),
           CE_None);
  GDALClose(ds);

  const QgsCoordinateReferenceSystem localFromWkt =
      QgsCoordinateReferenceSystem::fromWkt(DataCatalog::localGridCrsWkt());
  QVERIFY(localFromWkt.isValid());

  { // 纯路径 source：基线恢复语义不回归。
    QgsRasterLayer rl(tif, QStringLiteral("plain"), QStringLiteral("gdal"));
    QVERIFY(rl.isValid());
    MappingArtifactWriter::restoreRasterCrs(&rl);
    QCOMPARE(rl.crs().toWkt(), localFromWkt.toWkt());
  }
  { // 带 provider 选项串：先建有效层再换上选项串 source（该选项串会让
    // provider 拒载，但这不是跳过 CRS 恢复的理由——路径本身可打开）。
    // 修复前 GDALOpen 直接吃 "path?opt" 整串 → 静默失败 → CRS 停在 4326。
    QgsRasterLayer rl(tif, QStringLiteral("opt"), QStringLiteral("gdal"));
    QVERIFY(rl.isValid());
    rl.setDataSource(tif + QStringLiteral("?iro=nearest"), QStringLiteral("opt"),
                     QStringLiteral("gdal"));
    QVERIFY(rl.source().endsWith(QLatin1String("?iro=nearest")));
    MappingArtifactWriter::restoreRasterCrs(&rl);
    QCOMPARE(rl.crs().toWkt(), localFromWkt.toWkt());
  }
  { // 幂等：首探后挂标记，重复调用短路，CRS 稳定。
    QgsRasterLayer rl(tif, QStringLiteral("idem"), QStringLiteral("gdal"));
    MappingArtifactWriter::restoreRasterCrs(&rl);
    QVERIFY(rl.property("paleoRasterCrsProbed").toBool());
    MappingArtifactWriter::restoreRasterCrs(&rl);
    QCOMPARE(rl.crs().toWkt(), localFromWkt.toWkt());
  }
}

void TestPreviewMapCanvas::visibilityGatesCanvasLayerList()
{
  auto *a = makePointLayer(1, QStringLiteral("a"));
  auto *b = makePointLayer(1, QStringLiteral("b"));
  m_canvas->addLayer(a);
  m_canvas->addLayer(b);
  QVERIFY(m_canvas->isLayerVisible(a));
  m_canvas->setLayerVisible(a, false);
  QVERIFY(!m_canvas->isLayerVisible(a));
  // 不可见层不进画布 setLayers。
  QCOMPARE(m_canvas->canvas()->layers().size(), 1);
  QCOMPARE(m_canvas->canvas()->layers().first(), b);
  m_canvas->setLayerVisible(a, true);
  QCOMPARE(m_canvas->canvas()->layers().size(), 2);
}

void TestPreviewMapCanvas::opacityAndBlendApplyToLayer()
{
  auto *a = makePointLayer(1, QStringLiteral("a"));
  m_canvas->addLayer(a);
  m_canvas->setLayerOpacity(a, 0.4);
  QCOMPARE(m_canvas->layerOpacity(a), 0.4);
  QCOMPARE(a->opacity(), 0.4);
  m_canvas->setLayerOpacity(a, 1.7); // 越界夹取
  QCOMPARE(m_canvas->layerOpacity(a), 1.0);
  m_canvas->setLayerBlendMode(a, QPainter::CompositionMode_Multiply);
  QCOMPARE(m_canvas->layerBlendMode(a), QPainter::CompositionMode_Multiply);
  QCOMPARE(a->blendMode(), QPainter::CompositionMode_Multiply);
}

void TestPreviewMapCanvas::layersNeverRegisteredInProject()
{
  auto *a = makePointLayer(1, QStringLiteral("a"));
  m_canvas->addLayer(a);
  QgsProject *proj = QgsProject::instance();
  QVERIFY(proj);
  QVERIFY(!proj->mapLayers().contains(a->id())); // 私有层约定
}

void TestPreviewMapCanvas::zoomHistoryBackForward()
{
  auto *a = makePointLayer(4, QStringLiteral("a"));
  m_canvas->addLayer(a);
  m_canvas->zoomToFullExtent();
  const QgsRectangle first = m_canvas->currentExtent();
  QVERIFY(!first.isEmpty());
  m_canvas->zoomToRect(QgsRectangle(105, 205, 115, 215));
  const QgsRectangle second = m_canvas->currentExtent();
  auto isNearExtent = [](const QgsRectangle &a, const QgsRectangle &b) {
    const double tol = std::max(std::abs(a.width()), 1.0) * 1e-6;
    return std::abs(a.xMinimum() - b.xMinimum()) < tol &&
           std::abs(a.xMaximum() - b.xMaximum()) < tol &&
           std::abs(a.yMinimum() - b.yMinimum()) < tol &&
           std::abs(a.yMaximum() - b.yMaximum()) < tol;
  };
  QVERIFY(m_canvas->canZoomBack());
  m_canvas->zoomBack();
  QVERIFY(isNearExtent(m_canvas->currentExtent(), first));
  QVERIFY(m_canvas->canZoomForward());
  m_canvas->zoomForward();
  QVERIFY(isNearExtent(m_canvas->currentExtent(), second));
  m_canvas->clearHistory();
  QVERIFY(!m_canvas->canZoomBack());
  QVERIFY(!m_canvas->canZoomForward());
}

void TestPreviewMapCanvas::historyDeduplicatesIdenticalExtents()
{
  auto *a = makePointLayer(4, QStringLiteral("a"));
  m_canvas->addLayer(a);
  m_canvas->zoomToFullExtent();
  const QgsRectangle ext = m_canvas->currentExtent();
  m_canvas->zoomToRect(ext); // 同范围再设一次——不双记
  m_canvas->zoomBack();      // 回到上一格（应仍是同范围前的状态）
  // 前后栈语义：重复设置不产生新历史项。
  m_canvas->zoomToFullExtent();
  QVERIFY(m_canvas->canZoomBack());
}

void TestPreviewMapCanvas::fullExtentCombinesVisibleLayers()
{
  auto *a = makePointLayer(1, QStringLiteral("a"));
  auto *b = new QgsVectorLayer(
      QStringLiteral("Point?crs=WKT:%1&field=id:int")
          .arg(DataCatalog::localGridCrsWkt()),
      QStringLiteral("b"), QStringLiteral("memory"));
  QgsFeature f(b->fields());
  f.setGeometry(QgsGeometry::fromPointXY(QgsPointXY(5000.0, 5000.0)));
  b->dataProvider()->addFeature(f);
  b->updateExtents();
  b->setParent(m_canvas.get());
  m_canvas->addLayer(a);
  m_canvas->addLayer(b);
  const QgsRectangle combined = m_canvas->fullExtent();
  QVERIFY(combined.contains(QgsPointXY(100, 200)));
  QVERIFY(combined.contains(QgsPointXY(5000, 5000)));
  // 隐藏层退出联合范围。
  m_canvas->setLayerVisible(b, false);
  const QgsRectangle onlyA = m_canvas->fullExtent();
  QVERIFY(onlyA.width() < combined.width());
}

void TestPreviewMapCanvas::zoomToLayerGrowsSlightly()
{
  auto *a = makePointLayer(4, QStringLiteral("a"));
  m_canvas->addLayer(a);
  m_canvas->zoomToLayer(a);
  const QgsRectangle ext = m_canvas->currentExtent();
  QVERIFY(ext.width() > a->extent().width()); // 1.08 边距
}

void TestPreviewMapCanvas::renderSnapshotProducesImage()
{
  auto *a = makePointLayer(8, QStringLiteral("a"));
  m_canvas->addLayer(a);
  m_canvas->zoomToFullExtent();
  const QImage img = m_canvas->renderSnapshot(320);
  QVERIFY(!img.isNull());
  QVERIFY(img.width() <= 320);
  QVERIFY(img.width() > 0 && img.height() > 0);
}

void TestPreviewMapCanvas::overlayShowHideCycle()
{
  m_canvas->show();
  QTest::qWaitForWindowExposed(m_canvas.get());
  QImage img(64, 48, QImage::Format_ARGB32);
  img.fill(Qt::red);
  QVERIFY(!m_canvas->overlayVisible());
  m_canvas->showPreviewOverlay(img);
  QVERIFY(m_canvas->overlayVisible());
  m_canvas->hidePreviewOverlay();
  QVERIFY(!m_canvas->overlayVisible());
}

void TestPreviewMapCanvas::keyboardPlusMinusZero()
{
  auto *a = makePointLayer(8, QStringLiteral("a"));
  m_canvas->addLayer(a);
  m_canvas->zoomToFullExtent();
  m_canvas->show();
  QTest::qWaitForWindowExposed(m_canvas.get());
  const QgsRectangle before = m_canvas->currentExtent();
  QTest::keyClick(m_canvas.get(), Qt::Key_Plus);
  const QgsRectangle zoomed = m_canvas->currentExtent();
  QVERIFY(zoomed.width() < before.width());
  QTest::keyClick(m_canvas.get(), Qt::Key_Minus);
  QVERIFY(m_canvas->currentExtent().width() > zoomed.width());
  QTest::keyClick(m_canvas.get(), Qt::Key_0); // 复位
  const QgsRectangle reset = m_canvas->currentExtent();
  QVERIFY(qAbs(reset.width() - before.width()) < before.width() * 0.01);
}

void TestPreviewMapCanvas::keyboardArrowPan()
{
  auto *a = makePointLayer(8, QStringLiteral("a"));
  m_canvas->addLayer(a);
  m_canvas->zoomToFullExtent();
  m_canvas->show();
  QTest::qWaitForWindowExposed(m_canvas.get());
  const QgsRectangle before = m_canvas->currentExtent();
  QTest::keyClick(m_canvas.get(), Qt::Key_Right);
  const QgsRectangle after = m_canvas->currentExtent();
  QCOMPARE(after.width(), before.width()); // 平移不改宽度
  QVERIFY(after.xMinimum() > before.xMinimum());
}

void TestPreviewMapCanvas::estimateElementsCountsVectorFeatures()
{
  auto *a = makePointLayer(7, QStringLiteral("a"));
  auto *b = makePointLayer(3, QStringLiteral("b"));
  QCOMPARE(PreviewMapCanvas::estimateElements({a, b}), qint64(10));
  QCOMPARE(PreviewMapCanvas::estimateElements({}), qint64(0));
}

void TestPreviewMapCanvas::cacheKeyIgnoresSmallJitter()
{
  const QgsRectangle a(100.0001, 200.0002, 300.0004, 400.0003);
  const QgsRectangle b(100.0002, 200.0001, 300.0003, 400.0004);
  const QString k1 = PreviewRenderCache::makeKey(QStringLiteral("asset"), QStringLiteral("v1"), a, 512, 384);
  const QString k2 = PreviewRenderCache::makeKey(QStringLiteral("asset"), QStringLiteral("v1"), b, 512, 384);
  QCOMPARE(k1, k2); // 亚毫米抖动共享键
  const QString k3 = PreviewRenderCache::makeKey(QStringLiteral("asset"), QStringLiteral("v1"), a, 514, 384);
  QCOMPARE(k1, k3); // 8px 粒度（514/8*8=512）
  const QString k4a = PreviewRenderCache::makeKey(QStringLiteral("asset"), QStringLiteral("v1"), a, 520, 384);
  QVERIFY(k1 != k4a); // 跨桶必不同
  const QString k4 = PreviewRenderCache::makeKey(QStringLiteral("asset"), QStringLiteral("v2"), a, 512, 384);
  QVERIFY(k1 != k4); // 版本不同必不同
  QVERIFY(k1 != k4a);
}

void TestPreviewMapCanvas::cacheLruEviction()
{
  PreviewRenderCache::instance().clearMemoryForTesting();
  QImage img(4, 4, QImage::Format_ARGB32);
  img.fill(Qt::blue);
  for (int i = 0; i < 20; ++i)
    PreviewRenderCache::instance().store(QStringLiteral("k%1").arg(i), img);
  QCOMPARE(PreviewRenderCache::instance().memoryEntryCount(),
           PreviewRenderCache::instance().memoryLimit()); // D6.6 逐最远
  // 两段式：内存被逐但磁盘 PNG 仍可回填（非空 = 命中磁盘层，D6.2 语义）。
  QVERIFY(!PreviewRenderCache::instance().lookup(QStringLiteral("k0")).isNull());
  // 回填后仍是 limit 张，且最近一条必在内存。
  QCOMPARE(PreviewRenderCache::instance().memoryEntryCount(),
           PreviewRenderCache::instance().memoryLimit());
}

void TestPreviewMapCanvas::cacheDiskRoundTrip()
{
  PreviewRenderCache::instance().clearMemoryForTesting();
  QImage img(8, 6, QImage::Format_ARGB32);
  img.fill(Qt::green);
  const QString key = QStringLiteral("diskrt");
  PreviewRenderCache::instance().store(key, img);
  PreviewRenderCache::instance().clearMemoryForTesting(); // 清内存逼走磁盘层
  const QImage back = PreviewRenderCache::instance().lookup(key);
  QVERIFY(!back.isNull());
  QCOMPARE(back.size(), img.size());
}

void TestPreviewMapCanvas::setViewExtentIsExact()
{
  auto *a = makePointLayer(4, QStringLiteral("a"));
  m_canvas->addLayer(a);
  m_canvas->show(); // 画布几何要 show 后布局才生效（隐藏时 resize 事件延迟）
  QTest::qWaitForWindowExposed(m_canvas.get());
  m_canvas->zoomToFullExtent();
  // 目标框宽高比 = 视口（600×400）——避开 QgsMapCanvas 的 aspect 补边，
  // 隔离「无额外 1.05/1.08 放大」这一被测契约。
  const QgsRectangle target(10.0, 20.0, 130.0, 100.0); // 120×80 = 视口 1.5 宽高比
  m_canvas->setViewExtent(target);
  // 精确复位：不加 1.05/1.08 边距（书签跳转语义 D3.7）。宽高按相对容差
  //（QgsMapCanvas 刷新时可做视口 aspect 微调）。
  QVERIFY(qAbs(m_canvas->currentExtent().width() - target.width()) < target.width() * 0.01);
  QVERIFY(qAbs(m_canvas->currentExtent().height() - target.height()) < target.height() * 0.01);
  QVERIFY(qAbs(m_canvas->currentExtent().xMinimum() - target.xMinimum()) < 1.0);
}

void TestPreviewMapCanvas::cancelRenderingWithoutJobIsSafe()
{
  auto *a = makePointLayer(2, QStringLiteral("a"));
  m_canvas->addLayer(a);
  m_canvas->cancelRendering(); // 无在飞 job：幂等不崩
  QVERIFY(!m_canvas->isRendering());
}

void TestPreviewMapCanvas::localGridRasterRendersThroughGeoreference()
{
  // 地震相产品路径回归：ENGCRS 网格栅格 + similarity 配准 + 3857 画布。
  // 「图层勾选但画布不可见」排查的渲染期环节——验证本地网格栅格经
  // 工程注册的仿射操作能落到投影画布并出像素。
  QTemporaryDir tmp;
  QVERIFY(tmp.isValid());
  const QString tif = tmp.filePath(QStringLiteral("pred.tif"));
  {
    GDALDriverH drv = GDALGetDriverByName("GTiff");
    QVERIFY(drv);
    GDALDatasetH ds = GDALCreate(drv, tif.toUtf8().constData(), 100, 80, 1,
                                 GDT_Byte, nullptr);
    QVERIFY(ds);
    double gt[6] = {0.0, 40.0, 0.0, 4000.0, 0.0, -40.0};
    QCOMPARE(GDALSetGeoTransform(ds, gt), CE_None);
    QCOMPARE(GDALSetProjection(ds, DataCatalog::localGridCrsWkt()
                                        .toUtf8()
                                        .constData()),
             CE_None);
    GDALRasterBandH band = GDALGetRasterBand(ds, 1);
    QVERIFY(band);
    // 1/2/3 变化的相编码——常数栅格在默认灰度拉伸下映射成纯黑，测不出。
    QByteArray row(100, 0);
    for (int x = 0; x < 100; ++x)
      row[x] = char(1 + x % 3);
    for (int y = 0; y < 80; ++y)
      QCOMPARE(GDALRasterIO(band, GF_Write, 0, y, 100, 1, row.data(), 100,
                            1, GDT_Byte, 0, 0),
               CE_None);
    GDALClose(ds);
  }

  QgsProject project;
  PaleoGeoreference g;
  g.targetCrs = QStringLiteral("EPSG:4326");
  g.anchorLonDeg = 108.05;
  g.anchorLatDeg = 36.1;
  g.metersPerDegLon = 90049.687955;
  g.metersPerDegLat = 110960.830261;
  g.a = 1.000896238;
  g.b = 0.030275703;
  g.tE = -6182.244744;
  g.tN = -9871.264523;
  QString cfgErr;
  QVERIFY2(paleo::mapreference::configure(
               &project, g, QStringLiteral("EPSG:3857"), &cfgErr),
           qPrintable(cfgErr));

  auto *raster = new QgsRasterLayer(tif, QStringLiteral("pred"),
                                    QStringLiteral("gdal"));
  QVERIFY(raster->isValid());
  // 等价于 restoreRasterCrs：tif 内嵌 ENGCRS 与工程局部网格同构
  raster->setCrs(
      QgsCoordinateReferenceSystem::fromWkt(DataCatalog::localGridCrsWkt()));
  QVERIFY(raster->crs().isValid());
  project.addMapLayer(raster);

  // 目标范围：层范围经同一 context 变换
  QgsCoordinateTransform tr(
      raster->crs(), QgsCoordinateReferenceSystem(QStringLiteral("EPSG:3857")),
      project.transformContext());
  QVERIFY2(tr.isValid(), "local→3857 变换不可用");
  const QgsRectangle dest = tr.transformBoundingBox(raster->extent());
  QVERIFY2(!dest.isEmpty() && std::isfinite(dest.width()),
           qPrintable(QStringLiteral("dest=%1,%2 %3x%4")
                          .arg(dest.xMinimum()).arg(dest.yMinimum())
                          .arg(dest.width()).arg(dest.height())));
  // 落点应在 108°E 附近的 Web Mercator 米域（配准锚点 lon 108.05）
  QVERIFY2(std::abs(dest.center().x() - 12028000.0) < 30000.0,
           qPrintable(QStringLiteral("dest center x=%1 不在配准目标带")
                          .arg(dest.center().x())));

  QgsMapSettings ms;
  ms.setDestinationCrs(QgsCoordinateReferenceSystem(QStringLiteral("EPSG:3857")));
  ms.setTransformContext(project.transformContext());
  ms.setLayers({raster});
  ms.setExtent(dest);
  ms.setOutputSize(QSize(640, 480));
  ms.setBackgroundColor(QColor(0, 0, 0, 255));

  QgsMapRendererSequentialJob job(ms);
  job.start();
  job.waitForFinished();
  const QImage img = job.renderedImage();
  QCOMPARE(img.size(), QSize(640, 480));

  int nonBg = 0;
  for (int y = 0; y < img.height(); ++y)
    for (int x = 0; x < img.width(); ++x)
      if (img.pixelColor(x, y) != QColor(0, 0, 0))
        ++nonBg;
  QVERIFY2(nonBg > 1000,
           qPrintable(QStringLiteral("栅格在 3857 画布上不可见 nonBg=%1")
                          .arg(nonBg)));
}

void TestPreviewMapCanvas::downgradedFaciesPolygonsRegainLocalGrid()
{
  QTemporaryDir dir;
  QVERIFY(dir.isValid());
  const QString path = dir.filePath(QStringLiteral("facies.gpkg"));
  GDALAllRegister();
  GDALDriverH driver = GDALGetDriverByName("GPKG");
  QVERIFY(driver);
  GDALDatasetH ds = GDALCreate(driver, path.toUtf8().constData(), 0, 0, 0,
                               GDT_Unknown, nullptr);
  QVERIFY(ds);
  OGRSpatialReferenceH srs = OSRNewSpatialReference(nullptr);
  QByteArray local(
      "LOCAL_CS[\"Paleo local engineering grid\","
      "UNIT[\"metre\",1,AUTHORITY[\"EPSG\",\"9001\"]],"
      "AXIS[\"Easting\",EAST],AXIS[\"Northing\",NORTH]]");
  char *wktPtr = local.data();
  QVERIFY(OSRImportFromWkt(srs, &wktPtr) == OGRERR_NONE);
  OSRSetAxisMappingStrategy(srs, OAMS_TRADITIONAL_GIS_ORDER);
  OGRLayerH ogrLayer =
      GDALDatasetCreateLayer(ds, "facies_polygons", srs, wkbPolygon, nullptr);
  OSRDestroySpatialReference(srs);
  QVERIFY(ogrLayer);
  OGRFeatureH feature = OGR_F_Create(OGR_L_GetLayerDefn(ogrLayer));
  QByteArray ring(
      "POLYGON ((1000 1000, 2000 1000, 2000 2000, 1000 2000, 1000 1000))");
  char *ringPtr = ring.data();
  OGRGeometryH geometry = nullptr;
  QVERIFY(OGR_G_CreateFromWkt(&ringPtr, nullptr, &geometry) == OGRERR_NONE);
  QVERIFY(OGR_F_SetGeometry(feature, geometry) == OGRERR_NONE);
  OGR_G_DestroyGeometry(geometry);
  QVERIFY(OGR_L_CreateFeature(ogrLayer, feature) == OGRERR_NONE);
  OGR_F_Destroy(feature);
  GDALClose(ds);

  QgsVectorLayer layer(path + QStringLiteral("|layername=facies_polygons"),
                       QStringLiteral("facies"), QStringLiteral("ogr"));
  QVERIFY(layer.isValid());
  QVERIFY2(!layer.crs().toWkt().contains(
               QStringLiteral("Local engineering datum")),
           qPrintable(layer.crs().toWkt()));
  MappingArtifactWriter::restoreRasterCrs(&layer);
  const QgsCoordinateReferenceSystem canonical =
      QgsCoordinateReferenceSystem::fromWkt(DataCatalog::localGridCrsWkt());
  QVERIFY2(layer.crs().isValid() && layer.crs() == canonical,
           qPrintable(layer.crs().toWkt()));

  QgsProject project;
  PaleoGeoreference reference;
  reference.targetCrs = QStringLiteral("EPSG:4326");
  reference.anchorLonDeg = 108.05;
  reference.anchorLatDeg = 36.1;
  reference.metersPerDegLon = 90049.687955;
  reference.metersPerDegLat = 110960.830261;
  reference.a = 1.000896238;
  reference.b = 0.030275703;
  reference.tE = -6182.244744;
  reference.tN = -9871.264523;
  QString error;
  QVERIFY2(paleo::mapreference::configure(
               &project, reference, QStringLiteral("EPSG:3857"), &error),
           qPrintable(error));
  QgsCoordinateTransform transform(
      layer.crs(), QgsCoordinateReferenceSystem(QStringLiteral("EPSG:3857")),
      project.transformContext());
  QVERIFY2(transform.isValid(), "相面降级 CRS 仍不能投到地图");
  const QgsPointXY placed = transform.transform(QgsPointXY(1500, 1500));
  QVERIFY2(std::abs(placed.x() - 12028000.0) < 80000.0,
           qPrintable(QString::number(placed.x())));
}

void TestPreviewMapCanvas::renderSignalLifecycleFires()
{
  auto *a = makePointLayer(4, QStringLiteral("a"));
  m_canvas->addLayer(a);
  m_canvas->show();
  QTest::qWaitForWindowExposed(m_canvas.get());
  QSignalSpy startedSpy(m_canvas.get(), &PreviewMapCanvas::renderStarted);
  QSignalSpy doneSpy(m_canvas.get(), &PreviewMapCanvas::renderCompleted);
  m_canvas->zoomToFullExtent();
  QTRY_COMPARE_WITH_TIMEOUT(doneSpy.count(), 1, 5000);
  QVERIFY(startedSpy.count() >= 1);
  QCOMPARE(doneSpy.at(0).at(2).toLongLong(), qint64(4)); // 图元数 = 要素数
}

void TestPreviewMapCanvas::overlayFollowsCanvasResize()
{
  m_canvas->show();
  QTest::qWaitForWindowExposed(m_canvas.get());
  QImage img(64, 48, QImage::Format_ARGB32);
  img.fill(Qt::red);
  m_canvas->showPreviewOverlay(img);
  QVERIFY(m_canvas->overlayVisible());
  const QRect before = m_canvas->findChild<QWidget *>(
                          QStringLiteral("previewSnapshotOverlay") )
                          ->geometry();
  m_canvas->resize(800, 600);
  QTest::qWait(50); // Resize 事件经事件循环送达
  const QRect after = m_canvas->findChild<QWidget *>(
                          QStringLiteral("previewSnapshotOverlay") )
                          ->geometry();
  QVERIFY(after.width() > before.width()); // 覆盖层跟随画布尺寸
}

int main(int argc, char *argv[])
{
  if (!QgisRuntime::initialize(QStringLiteral("/usr")))
  {
    qFatal("QgisRuntime::initialize failed");
    return 1;
  }
  TestPreviewMapCanvas tc;
  const int rc = QTest::qExec(&tc, argc, argv);
  QgisRuntime::shutdown();
  return rc;
}

#include "tst_previewmap_canvas.moc"
