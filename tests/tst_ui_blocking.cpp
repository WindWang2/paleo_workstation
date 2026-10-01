// tst_ui_blocking — goal/perf-systematize 簇2：UI 线程阻塞清扫的动态验证。
// 三修复的判别性测试（比率门，机器无关）：
//   F1 well_log 预览两段式（lasHeaderAt 秒铺 + requestLas 池内解析）
//   F2 综合柱状图 XML 两段式（loadComprehensiveXmlAsync）
//   F3 GeoJSON 统计流式单遍 + 异步（geoJsonSummaryAt + 任务池）
// 门形：页面构建/受理耗时 < 0.5 × 同文件整份解析耗时。若任一修复回退成
// 同步整解析，构建耗时 ≈ 解析耗时 → 比率→1 必红（判别力）。
// 探活：解析等待期内事件循环 processEvents 分片 ≥2 轮——解析若回到 UI
// 线程，第一片就要阻塞到解析结束（1 轮即红）。
#include <QtTest>

#include "../src/catalog/datacatalog.h"
#include "../src/domain/wellcompositemodel.h"
#include "../src/io/dataimportservice.h"
#include "../src/io/perffixtures.h"
#include "../src/io/wellcompositexml.h"
#include "../src/metadata/layermanifest.h"
#include "../src/metadata/paleoprojectstore.h"
#include "../src/qgis/qgislayerservice.h"
#include "../src/qgis/qgisprojectservice.h"
#include "../src/qgis/qgisruntime.h"
#include "../src/services/paleotaskservice.h"
#include "../src/services/previewdoc.h"
#include "../src/ui/datapreview/datapreviewtabs.h"
#include "../src/ui/pages/entitypanel.h"
#include "../src/ui/wellcomposite/wellcompositepanel.h"

#include <QComboBox>
#include <QDeadlineTimer>
#include <QDir>
#include <QElapsedTimer>
#include <QFile>
#include <QFileInfo>
#include <QLabel>
#include <QSignalSpy>
#include <QTemporaryDir>
#include <QToolButton>
#include <functional>

namespace
{
// 等待谓词成立：20ms 分片排事件循环，返回分片轮数（探活面——解析在池内
// 时循环每片都能进出；解析若占住 UI 线程，第一片即吞掉整段等待）。
int waitUntil(const std::function<bool()> &done, int timeoutMs)
{
  QDeadlineTimer t(timeoutMs);
  int laps = 0;
  while (!done() && !t.hasExpired())
  {
    QApplication::processEvents(QEventLoop::AllEvents, 20);
    ++laps;
  }
  QApplication::processEvents(QEventLoop::AllEvents, 20);
  return laps;
}

// 大 LAS：740k 行 × 5 曲线 ≈ 59MB（B1 实测冷解析 350-440ms，头部 <2ms）。
QString makeBigLas(const QString &dir)
{
  const QString las = QDir(dir).filePath(QStringLiteral("big.las"));
  if (!PerfFixtures::makeSyntheticLas(las, 740000))
    return QString();
  return las;
}

// 大 GeoJSON：150k 点要素（≈20MB）——DOM 双解析曾 >300ms，流式 ~60ms。
QString makeBigGeoJson(const QString &dir)
{
  const QString path = QDir(dir).filePath(QStringLiteral("big.geojson"));
  QFile f(path);
  if (!f.open(QIODevice::WriteOnly))
    return QString();
  f.write("{\"type\":\"FeatureCollection\",\"features\":[");
  for (int i = 0; i < 150000; ++i)
  {
    if (i)
      f.write(",");
    f.write(QStringLiteral(
                R"({"type":"Feature","geometry":{"type":"Point","coordinates":[%1,%2]},)"
                R"("properties":{"id":%3,"facies":"f%4","name":"N%5"}})")
                .arg(110.0 + (i % 1000) * 0.01, 0, 'f', 2)
                .arg(30.0 + (i % 500) * 0.01, 0, 'f', 2)
                .arg(i)
                .arg(i % 7)
                .arg(i)
                .toUtf8());
  }
  f.write("]}");
  f.close();
  return path;
}

// 大综合柱状图 XML：300k 岩性段（SpreadsheetML ≈ 数十 MB）。
QString makeBigCompositeXml(const QString &dir)
{
  WellComposite::ComprehensiveWellData data;
  data.wellName = QStringLiteral("BIG1");
  data.minDepth = 0.0;
  data.maxDepth = 300000.0;
  data.lithologyIntervals.reserve(300000);
  for (int i = 0; i < 300000; ++i)
  {
    WellComposite::LithologyInterval li;
    li.topDepth = float(i);
    li.bottomDepth = float(i + 1);
    li.lithoName = QStringLiteral("灰色泥岩");
    li.lithoCode = QStringLiteral("MUD");
    li.patternType = QStringLiteral("mudstone");
    data.lithologyIntervals.append(li);
  }
  const QString path = QDir(dir).filePath(QStringLiteral("big.wc.xml"));
  QString err;
  if (!WellComposite::writeComprehensiveWellXmlFile(data, path, {}, &err))
  {
    qWarning("writeComprehensiveWellXmlFile: %s", qPrintable(err));
    return QString();
  }
  return path;
}
} // namespace

class TestUiBlocking : public QObject
{
  Q_OBJECT

  private slots:
    void geoJsonSummaryMatchesDomReference();
    void wellLogPreviewBuildsHeaderOnlyWhileParsingInPool();
    void comprehensiveXmlSubmitIsInstantWhileParsingInPool();
    void entityPanelGeoJsonStatsStreamOffTheUiThread();

  private:
    struct Stack
    {
      QgisProjectService projectSvc;
      std::unique_ptr<LayerManifest> manifest;
      std::unique_ptr<QgisLayerService> layerSvc;
      std::unique_ptr<PaleoProjectStore> store;
      std::unique_ptr<DataImportService> importSvc;
      QString projectDir;
    };
    static std::unique_ptr<Stack> makeStack(const QString &projectDir);
};

std::unique_ptr<TestUiBlocking::Stack> TestUiBlocking::makeStack(const QString &projectDir)
{
  if (!QDir().mkpath(projectDir))
    return nullptr;
  auto s = std::make_unique<Stack>();
  if (!s->projectSvc.createProject(QDir(projectDir).filePath(QStringLiteral("proj.qgz"))))
    return nullptr;
  const QString metaPath = QDir(projectDir).filePath(QStringLiteral("metadata/project.sqlite"));
  s->manifest = std::make_unique<LayerManifest>(metaPath);
  if (!s->manifest->open())
    return nullptr;
  s->layerSvc = std::make_unique<QgisLayerService>(&s->projectSvc, s->manifest.get());
  s->store = std::make_unique<PaleoProjectStore>();
  s->importSvc = std::make_unique<DataImportService>(s->store.get());
  QObject::connect(s->importSvc.get(), &DataImportService::layerDeclared,
                   s->layerSvc.get(), [layerSvc = s->layerSvc.get()](const LayerDeclaration &decl) {
                     QString err;
                     layerSvc->declare(decl, &err);
                   });
  s->importSvc->setProjectDir(projectDir);
  s->projectDir = projectDir;
  return s;
}

// F3 服务面等价性：流式 summary vs 旧 DOM 口径（bounds/计数/键集合）。
void TestUiBlocking::geoJsonSummaryMatchesDomReference()
{
  QTemporaryDir dir;
  const QString small = QDir(dir.path()).filePath(QStringLiteral("small.geojson"));
  {
    QFile f(small);
    QVERIFY(f.open(QIODevice::WriteOnly));
    f.write(R"({"type":"FeatureCollection","features":[)"
            R"({"type":"Feature","geometry":{"type":"Point","coordinates":[120.0,35.8]},"properties":{"facies":"prodelta","id":1}},)"
            R"({"type":"Feature","geometry":{"type":"LineString","coordinates":[[110.5,30.0],[121.0,31.5]]},"properties":{"name":"L1"}})"
            R"(]})");
  }
  PreviewDocService::GeoJsonSummary sum;
  QString err;
  QVERIFY2(PreviewDocService::geoJsonSummaryAt(small, &sum, &err), qPrintable(err));
  QCOMPARE(sum.featureCount, qint64(2));
  QVERIFY(sum.hasBounds);
  QCOMPARE(sum.bounds[0], 110.5); // minX
  QCOMPARE(sum.bounds[1], 30.0);  // minY
  QCOMPARE(sum.bounds[2], 121.0); // maxX
  QCOMPARE(sum.bounds[3], 35.8);  // maxY
  const QStringList expectedKeys{QStringLiteral("facies"), QStringLiteral("id"),
                                 QStringLiteral("name")};
  QCOMPARE(sum.propKeys, expectedKeys);

  // 旧 DOM 门面同口径对照（bounds 一致；键集合 = 排序去重）。
  double b[4] = {0, 0, 0, 0};
  QVERIFY(PreviewDocService::geoJsonBounds(small, b, nullptr));
  QCOMPARE(b[0], sum.bounds[0]);
  QCOMPARE(b[1], sum.bounds[1]);
  QCOMPARE(b[2], sum.bounds[2]);
  QCOMPARE(b[3], sum.bounds[3]);
}

// F1：openAsset 只付头部解析（构建耗时 ≪ 整份解析）；解析期间事件循环分片
// 进出 ≥2 轮（UI 线程未被占用）；数据到达后占位隐藏、单位 tooltip 就位。
void TestUiBlocking::wellLogPreviewBuildsHeaderOnlyWhileParsingInPool()
{
  QTemporaryDir dir;
  const QString las = makeBigLas(dir.path());
  QVERIFY2(!las.isEmpty(), "合成大 LAS 失败");
  auto st = makeStack(QDir(dir.path()).filePath(QStringLiteral("proj")));
  QVERIFY(st);
  QString err;
  const QString assetId = st->importSvc->importProjectFile(las, &err);
  QVERIFY2(!assetId.isEmpty(), qPrintable(err));

  DataPreviewTabs tabs;
  tabs.setImportService(st->importSvc.get());
  PaleoTaskService taskSvc;
  tabs.setTaskService(&taskSvc);

  QElapsedTimer t;
  t.start();
  tabs.openAsset(assetId);
  const double buildMs = double(t.nsecsElapsed()) / 1.0e6;

  auto *hint = tabs.findChild<QLabel *>(QStringLiteral("lasPendingHint"));
  QVERIFY2(hint, "两段式占位提示缺失（同步路径不会建它）");
  QVERIFY2(!hint->isHidden(), "openAsset 返回后解析仍在途——占位应可见");

  // 等整份解析完成（占位隐藏即数据已装）。
  const double t0 = double(t.nsecsElapsed()) / 1.0e6;
  const int laps = waitUntil([&hint] { return hint->isHidden(); }, 60000);
  const double doneAtMs = double(t.nsecsElapsed()) / 1.0e6;
  const double parseMs = doneAtMs - t0;

  QVERIFY2(parseMs > 30.0,
           qPrintable(QStringLiteral("整份解析仅 %1ms——夹具不足以判别（需 ≥30ms）")
                          .arg(parseMs, 0, 'f', 1)));
  QVERIFY2(buildMs < 0.5 * parseMs,
           qPrintable(QStringLiteral("页构建 %1ms ≥ 0.5×解析 %2ms——疑似回退同步整解析")
                          .arg(buildMs, 0, 'f', 1)
                          .arg(parseMs, 0, 'f', 1)));
  QVERIFY2(laps >= 2,
           qPrintable(QStringLiteral("等待期事件循环仅 %1 片——解析疑似占住 UI 线程")
                          .arg(laps)));

  // 数据确已装：chip tooltip 带单位（仅 fill 会写）。
  bool unitSeen = false;
  const auto chips = tabs.findChildren<QToolButton *>();
  for (QToolButton *c : chips)
    if (c->toolTip().contains(QLatin1String(" (")))
      unitSeen = true;
  QVERIFY2(unitSeen, "曲线单位 tooltip 未补——数据未真正装填");
  tabs.closeAssetTab(assetId);
}

// F2：loadComprehensiveXmlAsync 受理即返回（受理耗时 ≪ 解析耗时），解析
// 期间事件循环分片进出；完成后数据完整（300k 段）。
void TestUiBlocking::comprehensiveXmlSubmitIsInstantWhileParsingInPool()
{
  QTemporaryDir dir;
  const QString xml = makeBigCompositeXml(dir.path());
  QVERIFY2(!xml.isEmpty(), "合成大综合柱状图 XML 失败");
  QVERIFY2(QFileInfo(xml).size() > 1000000, "XML 夹具太小，判别力不足");

  WellComposite::WellCompositePanel panel;
  PaleoTaskService taskSvc;
  QSignalSpy loaded(&panel, &WellComposite::WellCompositePanel::comprehensiveXmlLoaded);

  QElapsedTimer t;
  t.start();
  QVERIFY(panel.loadComprehensiveXmlAsync(xml, &taskSvc));
  const double submitMs = double(t.nsecsElapsed()) / 1.0e6;

  const int laps = waitUntil([&loaded] { return loaded.count() > 0; }, 60000);
  const double parseMs = double(t.nsecsElapsed()) / 1.0e6 - submitMs;

  QCOMPARE(loaded.at(0).at(0).toBool(), true);
  QVERIFY2(parseMs > 30.0,
           qPrintable(QStringLiteral("解析仅 %1ms——夹具不足以判别").arg(parseMs, 0, 'f', 1)));
  QVERIFY2(submitMs < 0.5 * parseMs,
           qPrintable(QStringLiteral("受理 %1ms ≥ 0.5×解析 %2ms——疑似回退同步解析")
                          .arg(submitMs, 0, 'f', 1)
                          .arg(parseMs, 0, 'f', 1)));
  QVERIFY2(laps >= 2,
           qPrintable(QStringLiteral("等待期事件循环仅 %1 片——解析疑似占住 UI 线程")
                          .arg(laps)));
  QCOMPARE(panel.currentData().lithologyIntervals.size(), 300000);

  // 无任务服务：同步旧路径（失败语义=返回值）。
  WellComposite::WellCompositePanel syncPanel;
  QSignalSpy syncLoaded(&syncPanel, &WellComposite::WellCompositePanel::comprehensiveXmlLoaded);
  QVERIFY(syncPanel.loadComprehensiveXmlAsync(xml, nullptr));
  QCOMPARE(syncLoaded.count(), 1); // 同步执行、返回前信号已发
  QCOMPARE(syncPanel.currentData().lithologyIntervals.size(), 300000);
  QVERIFY(!syncPanel.loadComprehensiveXmlAsync(QDir(dir.path()).filePath(QStringLiteral("nope.xml")), nullptr));
}

// F3：EntityPanel refresh 只铺占位（refresh 耗时 ≪ 流式统计耗时），统计
// 期间事件循环分片进出；到达后标签/详情为真实统计值。
void TestUiBlocking::entityPanelGeoJsonStatsStreamOffTheUiThread()
{
  QTemporaryDir dir;
  const QString geo = makeBigGeoJson(dir.path());
  QVERIFY2(!geo.isEmpty(), "合成大 GeoJSON 失败");
  auto st = makeStack(QDir(dir.path()).filePath(QStringLiteral("proj")));
  QVERIFY(st);

  DataCatalog *cat = st->importSvc->catalog();
  QVERIFY(cat && cat->isOpen());
  CatalogAsset a;
  a.id = QStringLiteral("ast-geo-big");
  a.type = QStringLiteral("geojson");
  a.displayName = QStringLiteral("big.geojson");
  QVERIFY(cat->addAsset(a));
  CatalogVersion v;
  v.id = QStringLiteral("ver-geo-big");
  v.assetId = a.id;
  v.stage = QStringLiteral("RAW");
  v.versionNumber = 1;
  v.managed = false;
  v.path = geo;
  QString addErr;
  QVERIFY2(cat->addVersion(v, &addErr), qPrintable(addErr));

  PreviewDocService doc(st->importSvc.get());
  PaleoTaskService taskSvc;
  doc.setTaskService(&taskSvc);

  EntityPanel panel;
  panel.setDocService(&doc);
  panel.setContext(QString(), a.id);

  QElapsedTimer t;
  t.start();
  panel.refresh();
  const double refreshMs = double(t.nsecsElapsed()) / 1.0e6;

  auto *grid = panel.findChild<QLabel *>(QStringLiteral("propGrid"));
  QVERIFY(grid);
  QVERIFY2(grid->text().contains(QString::fromUtf8("统计中")),
           "refresh 后应先铺占位（异步路径）");

  const double t0 = double(t.nsecsElapsed()) / 1.0e6;
  const int laps = waitUntil(
      [&grid] { return !grid->text().contains(QString::fromUtf8("统计中")); }, 60000);
  const double parseMs = double(t.nsecsElapsed()) / 1.0e6 - t0;

  QVERIFY2(parseMs > 30.0,
           qPrintable(QStringLiteral("统计仅 %1ms——夹具不足以判别").arg(parseMs, 0, 'f', 1)));
  QVERIFY2(refreshMs < 0.5 * parseMs,
           qPrintable(QStringLiteral("refresh %1ms ≥ 0.5×统计 %2ms——疑似回退同步解析")
                          .arg(refreshMs, 0, 'f', 1)
                          .arg(parseMs, 0, 'f', 1)));
  QVERIFY2(laps >= 2,
           qPrintable(QStringLiteral("等待期事件循环仅 %1 片——统计疑似占住 UI 线程")
                          .arg(laps)));
  QCOMPARE(grid->text(), QStringLiteral("要素个数：150000"));
  auto *coord = panel.findChild<QLabel *>(QStringLiteral("propCoord"));
  QVERIFY(coord);
  QVERIFY2(coord->text().startsWith(QStringLiteral("X ")),
           qPrintable(QStringLiteral("bounds 未装填：%1").arg(coord->text())));
  auto *details = panel.findChild<QLabel *>(QStringLiteral("propDetailsText"));
  QVERIFY(details);
  QVERIFY(details->text().contains(QStringLiteral("facies")));
  QVERIFY(!details->text().contains(QString::fromUtf8("统计中")));
}

int main(int argc, char *argv[])
{
  if (qgetenv("QT_QPA_PLATFORM").isEmpty())
    qputenv("QT_QPA_PLATFORM", "offscreen");
  if (!QgisRuntime::initialize(
          qEnvironmentVariable("QGIS_PREFIX_PATH", QgisRuntime::defaultPrefixPath())))
  {
    qFatal("QgisRuntime::initialize failed");
    return 1;
  }
  TestUiBlocking tc;
  const int rc = QTest::qExec(&tc, argc, argv);
  QgisRuntime::shutdown();
  return rc;
}

#include "tst_ui_blocking.moc"
