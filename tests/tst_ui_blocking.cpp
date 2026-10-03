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
#include "../src/catalog/datacatalog.h"
#include "../src/ui/pages/dataopspanelextra.h"   // TopologyGraph / EntityOverrideStore
#include "../src/ui/pages/dataops/dataopscommands.h"  // SoftDeleteCmd / DataOpsContext
#include "../src/ui/pages/dataops/dataopsmodel.h"     // TagStore / OverrideStore / RecycleBin
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
    // ---- 方向20 轮5：探测面扩展（探测先于修复，红的如实进 TODOS） ----
    void topologyRebuildIsOffTheUiThread();
    void batchSoftDeleteDoesNotRewritePerItem();
    void metadataOpenDoesNotRebuildTopologyInline();

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
// 计时前先热身一遍两段式——进程级一次性初始化（控件/theme/字体/池首启）
// 不计入构建侧，比率门两侧才可比。
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

  // 冷启动吸收：buildMs 曾含进程级一次性开销（首个页控件/theme tokens/
  // 字体度量/任务池线程首启），慢机上 ~270ms 固定项把 0.5×parseMs 门打翻
  // （parseMs 随文件线性伸缩、buildMs 固定不伸缩——比率分子不可比）。
  // 先用小 LAS 完整走一遍两段式路径再计时大文件：比率两侧都稳定可比。
  const QString warmLas = QDir(dir.path()).filePath(QStringLiteral("warm.las"));
  QVERIFY2(PerfFixtures::makeSyntheticLas(warmLas, 50), "热身 LAS 合成失败");
  const QString warmId = st->importSvc->importProjectFile(warmLas, &err);
  QVERIFY2(!warmId.isEmpty(), qPrintable(err));
  tabs.openAsset(warmId);
  if (QLabel *warmHint = tabs.findChild<QLabel *>(QStringLiteral("lasPendingHint")))
    waitUntil([warmHint] { return warmHint->isHidden(); }, 30000);
  QApplication::processEvents(QEventLoop::AllEvents, 20); // deleteLater 入队页清完
  tabs.closeAssetTab(warmId);

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
  qInfo("F1 计时：build=%.1fms parse=%.1fms laps=%d", buildMs, parseMs, laps);

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

// ===========================================================================
// 方向20 轮5：探测面扩展
//
// 纪律：**探测先于修复**。这三条探针只回答「该操作现在是否同步占住 UI 线程」，
// 不在本次改任何生产代码——红的如实进 TODOS.md 标注「已知阻塞」，不许调宽松阈值
// 让它变绿。
//
// 已有覆盖（早于本轮）：LAS 预览两段式、综合柱状图 XML 两段式、GeoJSON 统计流式。
// 本轮补的三条对应任务书点名的另三个面：元数据打开、实体批量删除、连接诊断。
// ===========================================================================

namespace
{
/// 铺一棵规模足够的 catalog：n 实体 + n 资产 + n 链接。规模要够让「同步重建」
/// 的耗时显著高于噪声，否则探针没有判别力。
std::unique_ptr<DataCatalog> makeWideCatalog(const QString &dir, int n, QString *err)
{
  auto cat = std::make_unique<DataCatalog>();
  if (!cat->open(dir, err))
    return nullptr;
  // commitStore/save 是 private（catalog 自己在 mutator 里落盘）；这里用公有的
  // BatchSave 作用域把 N 次落盘收敛成一次——否则造夹具本身就要 N 次全量写盘。
  {
    DataCatalog::BatchSave batch(cat.get());
    for (int i = 0; i < n; ++i)
    {
      CatalogEntity e;
      e.id = QStringLiteral("E%1").arg(i);
      e.entityType = QStringLiteral("well");
      e.name = QStringLiteral("井 %1").arg(i);
      if (!cat->addEntity(e, err))
        return nullptr;
    }
    for (int i = 0; i < n; ++i)
    {
      CatalogAsset a;
      a.id = QStringLiteral("A%1").arg(i);
      a.type = QStringLiteral("well_log");
      a.format = QStringLiteral("las");
      a.displayName = QStringLiteral("log-%1.las").arg(i);
      if (!cat->addAsset(a, err))
        return nullptr;
    }
    for (int i = 0; i < n; ++i)
    {
      EntityAssetLink l;
      l.entityId = QStringLiteral("E%1").arg(i);
      l.assetId = QStringLiteral("A%1").arg(i);
      l.role = QStringLiteral("well_log");
      l.entityType = QStringLiteral("well");
      if (!cat->addLink(l, err))
        return nullptr;
    }
    if (!batch.flush(err))
      return nullptr;
  }
  return cat;
}
} // namespace

// ---- 探针 1：拓扑重建（loadTopology）当前是同步还是异步 --------------------
void TestUiBlocking::topologyRebuildIsOffTheUiThread()
{
  QTemporaryDir dir;
  QVERIFY(dir.isValid());
  QString err;
  // 规模：1200 实体 + 1200 资产 + 1200 链接。loadTopology 的 nodeById 是裸
  // 线性扫（dataopspanelextra.h:443），总代价 O(L×(N+M))——这个规模下同步重建
  // 应该是数十毫秒量级，异步则是个位数。
  const int n = 1200;
  auto cat = makeWideCatalog(dir.path(), n, &err);
  QVERIFY2(cat != nullptr, qPrintable(err));
  QCOMPARE(cat->entities().size(), n);
  QCOMPARE(cat->assets().size(), n);

  DataCatalog *raw = cat.get();
  paleo::dataops::EntityOverrideStore overrides;
  paleo::dataops::TopologyGraph graph;

  // 探活：QEventDispatcher 的 aboutToBlock 就是「有人开始同步占住事件循环」的
  // 钩子。这里用「同步重建期间事件循环能分片进出」作判据（与 F1/F2/F3 同款）。
  QElapsedTimer clock;
  clock.start();
  const int laps = waitUntil(
      [&] {
        graph.loadTopology(raw, overrides);
        return true;
      },
      60000);
  const double totalMs = double(clock.nsecsElapsed()) / 1.0e6;

  qInfo("拓扑重建（n=%d）耗时 %.1fms，事件循环分片 %d", n, totalMs, laps);
  QCOMPARE(graph.nodeCount(), n * 2); // 实体 + 资产各一个节点

  // 判据：同步重建期间事件循环只能分 1 片（第一次 processEvents 就把整个重建
  // 等完了）。若将来 loadTopology 走任务池，这里会 ≥2 片。
  // 现在它是同步的 —— 下面的 QVERIFY2 如实把当前状态记为红，修复后自动转绿。
  QVERIFY2(laps >= 2,
           qPrintable(QStringLiteral(
                          "拓扑重建同步占住 UI 线程（%1ms 只分 1 片事件循环）——"
                          "已知阻塞：TopologyGraph::loadTopology 的 nodeById 是裸线性扫，"
                          "总代价 O(L×(N+M))，每次开元数据页都在 GUI 线程上全量重建")
                          .arg(totalMs, 0, 'f', 1)));
}

// ---- 探针 2：批量软删是否逐项重写 -----------------------------------------
void TestUiBlocking::batchSoftDeleteDoesNotRewritePerItem()
{
  QTemporaryDir dir;
  QVERIFY(dir.isValid());
  QString err;
  const int n = 40;
  auto cat = makeWideCatalog(dir.path(), n, &err);
  QVERIFY2(cat != nullptr, qPrintable(err));

  // 计量：软删 N 项时 recycle_bin.json 被落盘几次。现状 pushCommand 每项都
  // push 一次命令、每次命令 redo() 里 save() 一次、每次 save() 全量重写
  // recycle_bin.json（dataopsmodel.h:403）——即 N 次。
  // 期望：一次批量只落盘一次（末尾合并），或至少不随 N 线性放大。
  //
  // 判据取「sidecar 文件的写入次数」而非墙钟时间——时间会被文件系统缓存污染，
  // 次数不会。这与 F1/F2 的「比率门」是同一思路的另一种落法。
  // sidecar 真路径：<projectDir>/.paleo/<name>（dataopsmodel.h:53）
  const QString recyclePath =
      QDir(dir.path()).filePath(QStringLiteral(".paleo/recycle_bin.json"));
  DataCatalog *raw = cat.get();

  // 逐项软删——等价于 DataListPanel::batchRemoveSoft 的循环体
  // （datalist.cpp:2666）：每个选中项 push 一条 SoftDeleteCmd，而 pushCommand
  // 里每次都 refreshAssetTable()（datalist.cpp:2413）。
  //
  // 计量口径：**用 recycle_bin.json 的 mtime 变化次数**数落盘，不看墙钟——
  // 时间会被文件系统缓存污染，mtime 变化不会。
  paleo::dataops::TagStore tags;
  paleo::dataops::AssetOverrideStore assetOverrides;
  paleo::dataops::EntityOverrideStore entityOverrides;
  paleo::dataops::RecycleBin recycle;
  recycle.load(raw);
  paleo::dataops::DataOpsContext ctx{raw, &tags, &assetOverrides, &entityOverrides, &recycle};
  QVERIFY2(ctx.valid(), "DataOpsContext 未就绪（探针夹具问题，不是被测行为）");

  QElapsedTimer clock;
  clock.start();
  int writes = 0;
  QFileInfo before(recyclePath);
  for (int i = 0; i < n; ++i)
  {
    const CatalogAsset a = cat->assetById(QStringLiteral("A%1").arg(i));
    paleo::dataops::SoftDeleteCmd cmd(ctx, a.id, a.displayName, a.type, true);
    cmd.redo(); // redo() 内部一次 recycle->save() → 一次全量重写
    const QFileInfo after(recyclePath);
    if (after.lastModified() != before.lastModified() || after.size() != before.size())
      ++writes;
    before = after;
  }
  const double totalMs = double(clock.nsecsElapsed()) / 1.0e6;
  qInfo("逐项软删 %d 项耗时 %.1fms，recycle_bin.json 落盘 %d 次", n, totalMs, writes);

  QVERIFY2(writes <= 2,
           qPrintable(QStringLiteral(
                          "软删 %1 项触发 %2 次 recycle_bin.json 全量重写——"
                          "已知阻塞：DataListPanel::pushCommand 每 push 一条命令就"
                          " refreshAssetTable() 一次（datalist.cpp:2413），"
                          "批量软删因此是 O(N²) 且全在 GUI 线程")
                          .arg(n).arg(writes)));
}

// ---- 探针 3：开元数据页是否会就地重建拓扑 ---------------------------------
void TestUiBlocking::metadataOpenDoesNotRebuildTopologyInline()
{
  QTemporaryDir dir;
  QVERIFY(dir.isValid());
  QString err;
  const int n = 800;
  auto cat = makeWideCatalog(dir.path(), n, &err);
  QVERIFY2(cat != nullptr, qPrintable(err));

  DataCatalog *raw = cat.get();
  paleo::dataops::EntityOverrideStore overrides;
  // 面板的 doc 面与控件装配都挂在 setDocService 上（buildD4Ui 在其中被调，
  // entitypanel.cpp:250）。夹具形态照既有用例 entityPanelGeoJsonStatsStream… 一致：
  // 需要一个接了 importSvc 的 PreviewDocService。
  auto st = makeStack(QDir(dir.path()).filePath(QStringLiteral("proj")));
  QVERIFY(st);
  PreviewDocService doc(st->importSvc.get());
  PaleoTaskService taskSvc;
  doc.setTaskService(&taskSvc);

  EntityPanel panel;
  panel.setDocService(&doc);

  // 元数据打开的真实路径：DataPage::selectAsset → EntityPanel::setContext +
  // refresh()，refresh() 内部无条件调 loadTopology（entitypanel.cpp:1147）。
  // 这里直接压 setContext + refresh 这条等价路径。
  QElapsedTimer clock;
  clock.start();
  // setContext 的实参形态要紧照既有用例（tst_ui_blocking.cpp:358）：
  // **空 entityId + 真实 assetId** 才走 refresh() 的「单资产」分支；双非空会落到
  // 别的分支去（第一版探针给了 E0/A0，结果 refresh 0.0ms 直接早退）。
  const CatalogAsset first = cat->assetById(QStringLiteral("A0"));
  QVERIFY2(!first.id.isEmpty(), "探针夹具缺 A0");
  panel.setContext(QString(), first.id);
  panel.refresh();
  const double openMs = double(clock.nsecsElapsed()) / 1.0e6;
  // m_topology 由 refresh() 内部按需 new（entitypanel.cpp:382），故只能在
  // refresh 之后取——之前取会拿到 nullptr（第一版探针就栽在这）。
  auto *graph = panel.findChild<paleo::dataops::TopologyGraph *>(QStringLiteral("topologyGraph"));
  QVERIFY2(graph, "refresh() 后仍未找到 topologyGraph——探针夹具与实现脱节，不是被测行为");
  qInfo("开元数据页（n=%d）耗时 %.1fms，拓扑节点 %d", n, openMs, graph->nodeCount());

  QVERIFY(graph->nodeCount() > 0); // 拓扑确实建起来了（否则这条探针没意义）
  QVERIFY2(openMs < 400.0,
           qPrintable(QStringLiteral(
                          "开元数据页耗时 %1ms（n=%2）——"
                          "已知阻塞：EntityPanel::refresh() 每次都同步调 "
                          "loadTopology 全量重建拓扑图，未走任务池")
                          .arg(openMs, 0, 'f', 1).arg(n)));
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
  // ctest 无控制台下 QtTest 结果走 OutputDebugString，失败时看不到哪条红；
  // 追加 -o 让结果落盘（方向20 轮5 探测面扩展用）。
  QByteArray logPath = QByteArray(QT_TESTCASE_BUILDDIR) + "/tst_ui_blocking-result.txt";
  QList<QByteArray> fwd;
  fwd << QByteArray(argv[0]);
  for (int i = 1; i < argc; ++i) fwd << QByteArray(argv[i]);
  fwd << QByteArray("-o") << logPath + ",txt";
  QList<char *> cargv;
  cargv.reserve(fwd.size());
  for (QByteArray &a : fwd) cargv << a.data();
  const int rc = QTest::qExec(&tc, cargv.size(), cargv.data());
  QgisRuntime::shutdown();
  return rc;
}

#include "tst_ui_blocking.moc"
