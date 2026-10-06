#include <QtTest>
#include <QFont>
#include <QImage>

#include <qgsapplication.h>
#include <qgscoordinatereferencesystem.h>
#include <qgsfeature.h>
#include <qgsfillsymbol.h>
#include <qgsgeometry.h>
#include <qgsmaplayer.h>
#include <qgsmaprendererjob.h> // QGIS_PALEO_LABELS_WITH_LAYER（vendored 补丁探针）
#include <qgspointxy.h>
#include <qgsmaprendererparalleljob.h>
#include <qgsmaprenderersequentialjob.h>
#include <qgsmapsettings.h>
#include <qgspallabeling.h>
#include <qgsproject.h>
#include <qgssinglesymbolrenderer.h>
#include <qgstextbackgroundsettings.h>
#include <qgstextformat.h>
#include <qgsvectorlayer.h>
#include <qgsvectorlayerlabeling.h>

#include "../src/qgis/qgislabelzorder.h"

// 标注随图层 z 序：QgisLabelZOrder 给工程内每个图层钉
// rendering/labelsWithLayer（vendored QGIS 补丁消费该属性，标注随本层
// 渲染目标出图），并清旧实现写过的 rendering/renderAboveLabels 残留。
// 像素回归：底层标注被中间不透明面盖住、顶层标注照常画出、未覆盖处
// 标注可见；属性全关时回到 QGIS 原生「标注永远置顶」。

static const QString kFlag = QStringLiteral("rendering/labelsWithLayer");
static const QString kLegacy = QStringLiteral("rendering/renderAboveLabels");

static QgsVectorLayer *mkPointLayer(QgsProject *proj, const QString &name,
                                    const QList<QPointF> &pts)
{
  auto *vl = new QgsVectorLayer(
      QStringLiteral("Point?crs=EPSG:4326&field=name:string"), name,
      QStringLiteral("memory"));
  QgsFeatureList feats;
  for (const QPointF &p : pts)
  {
    QgsFeature f(vl->fields());
    f.setAttribute(QStringLiteral("name"), QStringLiteral("XXXX"));
    f.setGeometry(QgsGeometry::fromPointXY(QgsPointXY(p)));
    feats << f;
  }
  vl->dataProvider()->addFeatures(feats);
  vl->updateExtents();
  if (proj)
    proj->addMapLayer(vl);
  return vl;
}

static QgsVectorLayer *mkRectLayer(QgsProject *proj, const QString &name,
                                   const QgsRectangle &rect)
{
  auto *vl = new QgsVectorLayer(
      QStringLiteral("Polygon?crs=EPSG:4326&field=id:int"), name,
      QStringLiteral("memory"));
  QgsFeature f(vl->fields());
  f.setAttribute(QStringLiteral("id"), 1);
  f.setGeometry(QgsGeometry::fromRect(rect));
  QgsFeatureList feats;
  feats << f;
  vl->dataProvider()->addFeatures(feats);
  vl->updateExtents();
  auto *sym = new QgsFillSymbol();
  sym->setColor(Qt::red); // 不透明红——压在下层标注之上即遮罩
  vl->setRenderer(new QgsSingleSymbolRenderer(sym));
  if (proj)
    proj->addMapLayer(vl);
  return vl;
}

// 大字号黑底矩形标注：OrderedPositionsAroundPoint 固定 BottomRight，
// 标注盒左上角锚在点位（像素坐标向右下展开），盒内全黑。
// #138：标注盒宽度随字体变——"Sans" 解析到 DejaVu Sans（CI runner 只有它）时
// 字更宽，A1 标注盒（img x 从 60 起）与 C 标注盒（x 从 120 起）交叠。补丁路径
// 每层独立标注引擎、互不碰撞；原生路径共享一个 PAL 引擎，两枚冲突时 PAL
// 丢掉 A1 → nativeLabelsStayOnTop 的 A1 探区无黑（CI 恒红的根因；本机用
// DejaVu-only fontconfig + QGIS 4.2.3 deb 闭包复现，有 Noto 时字窄、不交叠、绿）。
// 显式 AllowOverlapAtNoCost：两条路径都不做标注间避让，像素断言只测 z 序，
// 不再依赖字体宽度与 PAL 冲突裁决。
static void enableBigLabel(QgsVectorLayer *vl)
{
  QgsPalLayerSettings s;
  s.fieldName = QStringLiteral("name");
  s.placement = Qgis::LabelPlacement::OrderedPositionsAroundPoint;
  s.pointSettings().setPredefinedPositionOrder(
      {Qgis::LabelPredefinedPointPosition::BottomRight});
  s.placementSettings().setOverlapHandling(Qgis::LabelOverlapHandling::AllowOverlapAtNoCost);
  QgsTextFormat fmt;
  fmt.setFont(QFont(QStringLiteral("Sans")));
  fmt.setSize(24);
  fmt.setSizeUnit(Qgis::RenderUnit::Pixels);
  fmt.setColor(Qt::black);
  QgsTextBackgroundSettings bg;
  bg.setEnabled(true);
  bg.setType(QgsTextBackgroundSettings::ShapeRectangle);
  bg.setSizeType(QgsTextBackgroundSettings::SizeBuffer);
  bg.setSize(QSizeF(10, 10));
  bg.setSizeUnit(Qgis::RenderUnit::Pixels);
  bg.setFillColor(Qt::black);
  fmt.setBackground(bg);
  s.setFormat(fmt);
  vl->setLabeling(new QgsVectorLayerSimpleLabeling(s));
  vl->setLabelsEnabled(true);
}

class TestLabelZOrder : public QObject
{
    Q_OBJECT
  private slots:
    void flagsExistingLayers();
    void flagsAddedLater();
    void flagsAfterProjectRead();
    void staleLegacyFlagRemoved();
    void labelsStackWithLayerParallel();
    void labelsStackWithLayerSequential();
    void nativeLabelsStayOnTop();
    void supportProbeMatchesBuild();
    void unlabeledMiddleLayerNeverCoversTopGeometry();

  private:
    void runStackFixture(bool parallel, bool expectCovered);
};

void TestLabelZOrder::flagsExistingLayers()
{
  QgsProject proj;
  QgsVectorLayer *a = mkPointLayer(&proj, QStringLiteral("a"), {});
  QgsVectorLayer *b = mkPointLayer(&proj, QStringLiteral("b"), {});
  QVERIFY(!a->customProperty(kFlag).toBool());

  QgisLabelZOrder z(&proj);
  QVERIFY(a->customProperty(kFlag).toBool());
  QVERIFY(b->customProperty(kFlag).toBool());
}

void TestLabelZOrder::flagsAddedLater()
{
  QgsProject proj;
  QgisLabelZOrder z(&proj);
  // layersAdded 同步发射，无需事件循环
  QgsVectorLayer *a = mkPointLayer(&proj, QStringLiteral("a"), {});
  QVERIFY(a->customProperty(kFlag).toBool());
}

void TestLabelZOrder::flagsAfterProjectRead()
{
  QTemporaryDir dir;
  QVERIFY(dir.isValid());
  const QString geojson = dir.filePath(QStringLiteral("pts.geojson"));
  {
    QFile f(geojson);
    QVERIFY(f.open(QIODevice::WriteOnly));
    f.write("{\"type\":\"FeatureCollection\",\"features\":[{\"type\":\"Feature\","
            "\"properties\":{\"name\":\"x\"},\"geometry\":{\"type\":\"Point\","
            "\"coordinates\":[1,2]}}]}");
  }

  QgsProject src;
  auto *srcLayer = new QgsVectorLayer(geojson, QStringLiteral("pts"),
                                      QStringLiteral("ogr"));
  QVERIFY(srcLayer->isValid());
  // 把旧实现的属性写进工程文件——customproperties 随 .qgs 序列化，
  // 回读后由 readProject 触发的 refresh 清掉。
  srcLayer->setCustomProperty(kLegacy, true);
  src.addMapLayer(srcLayer);
  const QString projFile = dir.filePath(QStringLiteral("p.qgs"));
  QVERIFY(src.write(projFile));

  QgsProject proj;
  QgisLabelZOrder z(&proj);
  QVERIFY(proj.read(projFile));

  const QMap<QString, QgsMapLayer *> layers = proj.mapLayers();
  QCOMPARE(layers.size(), 1);
  QgsMapLayer *loaded = layers.first();
  QVERIFY(loaded->customProperty(kFlag).toBool());
  // 工程文件里若带回旧实现的属性，readProject 后必须被清掉
  QVERIFY(!loaded->customProperty(kLegacy).isValid());
}

void TestLabelZOrder::staleLegacyFlagRemoved()
{
  QgsProject proj;
  QgsVectorLayer *a = mkPointLayer(&proj, QStringLiteral("a"), {});
  a->setCustomProperty(kLegacy, true); // 模拟旧版本写过的残留
  QgisLabelZOrder z(&proj);
  QVERIFY(!a->customProperty(kLegacy).isValid());
  QVERIFY(a->customProperty(kFlag).toBool());
}

// 像素fixture：extent (0,0)-(200,200) → 1 图元 = 1 px（图 y 向下）。
//   A（底，点+标注）：p1 map(60,150)→img(60,50)，标注盒 img x[60,~135] y[50,~100]；
//                    p2 map(60,40)→img(60,160)，标注盒 img y[160,~195]。
//   B（中，不透明红面）：map x[30,170] y[80,180] → img x[30,170] y[20,120]，
//                    完整盖住 A 的第一枚标注，不碰第二枚。
//   C（顶，点+标注）：map(120,140)→img(120,60)，标注盒 img x[120,~195] y[60,~110]，
//                    整体落在 B 上。
// expectCovered=true（补丁生效）：A1 探区全红、A2 探区有黑、C 探区有黑。
// expectCovered=false（原生）：A1 探区也含黑（标注置顶穿透 B）。
void TestLabelZOrder::runStackFixture(bool parallel, bool expectCovered)
{
  QgsProject proj;
  QgsVectorLayer *a = mkPointLayer(&proj, QStringLiteral("A"),
                                   {QPointF(60, 150), QPointF(60, 40)});
  enableBigLabel(a);
  QgsVectorLayer *b = mkRectLayer(&proj, QStringLiteral("B"),
                                  QgsRectangle(30, 80, 170, 180));
  QgsVectorLayer *c = mkPointLayer(&proj, QStringLiteral("C"),
                                   {QPointF(120, 140)});
  enableBigLabel(c);

  std::unique_ptr<QgisLabelZOrder> z;
  if (expectCovered)
    z = std::make_unique<QgisLabelZOrder>(&proj); // 给三层都打上 labelsWithLayer
  else
    for (QgsMapLayer *l : proj.mapLayers())
      l->setCustomProperty(kFlag, false); // 显式关闭 → QGIS 原生路径

  QgsMapSettings ms;
  ms.setDestinationCrs(QgsCoordinateReferenceSystem(QStringLiteral("EPSG:4326")));
  ms.setExtent(QgsRectangle(0, 0, 200, 200));
  ms.setOutputSize(QSize(200, 200));
  ms.setOutputDpi(96);
  ms.setBackgroundColor(Qt::white);
  ms.setLayers({c, b, a}); // QgsMapSettings 图层序 = 顶在前
  ms.setFlag(Qgis::MapSettingsFlag::DrawLabeling, true);
  ms.setFlag(Qgis::MapSettingsFlag::Antialiasing, false);

  QImage img;
  if (parallel)
  {
    QgsMapRendererParallelJob job(ms);
    job.start();
    job.waitForFinished();
    img = job.renderedImage();
  }
  else
  {
    QgsMapRendererSequentialJob job(ms);
    job.start();
    job.waitForFinished();
    img = job.renderedImage();
  }
  QVERIFY2(!img.isNull(), "render job returned null image");

  const auto regionHasDark = [&img](int x0, int y0, int x1, int y1) {
    for (int y = y0; y <= y1; ++y)
      for (int x = x0; x <= x1; ++x)
        if (qGray(img.pixel(x, y)) < 80)
          return true;
    return false;
  };
  const auto regionHasNonRed = [&img](int x0, int y0, int x1, int y1) {
    for (int y = y0; y <= y1; ++y)
      for (int x = x0; x <= x1; ++x)
      {
        const QColor px(img.pixel(x, y));
        if (!(px.red() > 200 && px.green() < 80 && px.blue() < 80))
          return true;
      }
    return false;
  };

  // C 的标注压在 B 上（两种行为下都应有黑）
  QVERIFY2(regionHasDark(125, 65, 160, 95),
           qPrintable(QStringLiteral("top layer label missing (parallel=%1 covered=%2)")
                        .arg(parallel).arg(expectCovered)));
  // A 第二枚标注在 B 之外（两种行为下都应有黑）
  QVERIFY2(regionHasDark(65, 163, 100, 190),
           qPrintable(QStringLiteral("uncovered bottom label missing (parallel=%1 covered=%2)")
                        .arg(parallel).arg(expectCovered)));

  if (expectCovered)
  {
    QVERIFY2(!regionHasNonRed(64, 54, 96, 84),
             "patched build: bottom layer label leaked through covering layer");
  }
  else
  {
    QVERIFY2(regionHasDark(64, 54, 96, 84),
             "native build: label pass should draw on top of covering layer");
  }
}

void TestLabelZOrder::labelsStackWithLayerParallel()
{
#ifdef QGIS_PALEO_LABELS_WITH_LAYER
  runStackFixture(true, true);
#else
  QSKIP("vendored QGIS lacks labelsWithLayer patch (QGIS_PALEO_LABELS_WITH_LAYER)");
#endif
}

void TestLabelZOrder::labelsStackWithLayerSequential()
{
#ifdef QGIS_PALEO_LABELS_WITH_LAYER
  runStackFixture(false, true);
#else
  QSKIP("vendored QGIS lacks labelsWithLayer patch (QGIS_PALEO_LABELS_WITH_LAYER)");
#endif
}

void TestLabelZOrder::supportProbeMatchesBuild()
{
  // #138：壳层降级提示读的探针必须与编译期宏一致。
#ifdef QGIS_PALEO_LABELS_WITH_LAYER
  QVERIFY(QgisLabelZOrder::labelsWithLayerSupported());
#else
  QVERIFY(!QgisLabelZOrder::labelsWithLayerSupported());
#endif
}

void TestLabelZOrder::nativeLabelsStayOnTop()
{
  runStackFixture(true, false); // 属性全关 → 原生标注置顶，不受补丁与否影响
}

// #123：旧 renderAboveLabels 维护器会给「带标注层之上的无标注层」设该标志，
// QGIS 把这类层整张图像合成在所有普通层之后——中层不透明面盖住上层要素
// （原生复现：上层 645 个像素 → 0）。现实现只钉 labelsWithLayer、并清掉
// renderAboveLabels：不论 QGIS 是否打补丁，几何必须严格按图层树序合成。
void TestLabelZOrder::unlabeledMiddleLayerNeverCoversTopGeometry()
{
  QgsProject proj;
  QgsVectorLayer *bottom = mkPointLayer(&proj, QStringLiteral("bottom"), {QPointF(100, 100)});
  enableBigLabel(bottom);
  QgsVectorLayer *middle = mkRectLayer(&proj, QStringLiteral("middle"), QgsRectangle(20, 20, 180, 180));
  QgsVectorLayer *top = mkRectLayer(&proj, QStringLiteral("top"), QgsRectangle(40, 40, 80, 80));
  auto *blue = new QgsFillSymbol();
  blue->setColor(Qt::blue);
  top->setRenderer(new QgsSingleSymbolRenderer(blue));
  enableBigLabel(top); // 上层带标注、中层无标注、下层带标注——issue 的三层混合序
  // 旧工程残留：中层带着 renderAboveLabels 读进来，维护器必须清掉。
  middle->setCustomProperty(kLegacy, true);

  QgisLabelZOrder z(&proj);
  for (QgsMapLayer *l : proj.mapLayers())
    QVERIFY2(!l->customProperty(kLegacy).isValid(), qPrintable(l->name()));

  for (const bool parallel : {true, false})
  {
    QgsMapSettings ms;
    ms.setDestinationCrs(QgsCoordinateReferenceSystem(QStringLiteral("EPSG:4326")));
    ms.setExtent(QgsRectangle(0, 0, 200, 200));
    ms.setOutputSize(QSize(200, 200));
    ms.setOutputDpi(96);
    ms.setBackgroundColor(Qt::white);
    ms.setLayers({top, middle, bottom}); // 顶在前
    ms.setFlag(Qgis::MapSettingsFlag::DrawLabeling, true);
    ms.setFlag(Qgis::MapSettingsFlag::Antialiasing, false);
    QImage img;
    if (parallel)
    {
      QgsMapRendererParallelJob job(ms);
      job.start();
      job.waitForFinished();
      img = job.renderedImage();
    }
    else
    {
      QgsMapRendererSequentialJob job(ms);
      job.start();
      job.waitForFinished();
      img = job.renderedImage();
    }
    QVERIFY(!img.isNull());
    // 上层蓝面范围：地图 (40..80, 40..80) → 像素 x 40..80, y 120..160（y 轴翻转）。
    int bluePixels = 0;
    for (int y = 122; y <= 158; ++y)
      for (int x = 42; x <= 78; ++x)
      {
        const QColor px(img.pixel(x, y));
        if (px.blue() > 200 && px.red() < 80 && px.green() < 80)
          ++bluePixels;
      }
    QVERIFY2(bluePixels > 200,
             qPrintable(QStringLiteral("top geometry covered by unlabeled middle layer "
                                       "(parallel=%1 bluePixels=%2)").arg(parallel).arg(bluePixels)));
  }
}

int main(int argc, char *argv[])
{
  QgsApplication app(argc, argv, false);
  app.setPrefixPath(qEnvironmentVariable("QGIS_PREFIX_PATH", QStringLiteral("/usr")), true);
  app.initQgis();
  TestLabelZOrder tc;
  const int rc = QTest::qExec(&tc, argc, argv);
  QgsApplication::exitQgis();
  return rc;
}

#include "tst_labelzorder.moc"
