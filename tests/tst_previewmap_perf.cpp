#include <QtTest>
#include <QTemporaryDir>

#include "../src/catalog/datacatalog.h"
#include "../src/io/dataimportservice.h"
#include "../src/metadata/layermanifest.h"
#include "../src/metadata/paleoprojectstore.h"
#include "../src/qgis/previewidentify.h"
#include "../src/qgis/previewrasteranalysis.h"
#include "../src/qgis/previewrendercache.h"
#include "../src/qgis/qgisruntime.h"
#include "../src/qgis/qgislayerservice.h"
#include "../src/qgis/qgisprojectservice.h"
#include "../src/ui/datapreview/datapreviewtabs.h"
#include "../src/ui/datapreview/previewmappage.h"

#include <qgsmapcanvas.h>
#include <qgsrasterlayer.h>
#include <qgsvectorlayer.h>
#include <QElapsedTimer>
#include <QTabWidget>

// P2 D6.x 性能预算：首次打开 <300ms（fixture 数据，D6.7）、渲染缓存二开
// 命中（D6.2）、低清快照预算（D6.1）、identify 空间索引复用（D6.4）。
// 墙钟断言按 fixture 量级放宽（CI 共享机负载抖动），测量值用 qInfo 落盘
// 记录（selfcheck 口径：能看到数字，而不是只有红绿）。
class TestPreviewMapPerf : public QObject
{
  Q_OBJECT

  struct Stack
  {
    QgisProjectService projectSvc;
    std::unique_ptr<LayerManifest> manifest;
    std::unique_ptr<QgisLayerService> layerSvc;
    std::unique_ptr<PaleoProjectStore> store;
    std::unique_ptr<DataImportService> importSvc;
    std::unique_ptr<DataPreviewTabs> preview;
  };

  private slots:
    void initTestCase() { QVERIFY(QgisRuntime::isInitialized()); }

    void firstOpenUnderBudget();
    void renderCacheHitOnReopen();
    void snapshotRenderFast();
    void identifyIndexReused();
    void profileSamplingUnderBudget();

  private:
    static std::unique_ptr<Stack> makeStack(const QString &projectDir);
    static QString fixture(const QString &name);
    static QString stage(const QTemporaryDir &tmp, const QString &dir, const QString &name,
                         const QString &asName = QString());
};

std::unique_ptr<TestPreviewMapPerf::Stack> TestPreviewMapPerf::makeStack(const QString &projectDir)
{
  if (!QDir().mkpath(projectDir))
    return nullptr;
  auto s = std::make_unique<Stack>();
  const QString metaPath = QDir(projectDir).filePath(QStringLiteral("metadata/project.sqlite"));
  if (!s->projectSvc.createProject(QDir(projectDir).filePath(QStringLiteral("proj.qgz"))))
    return nullptr;
  s->manifest = std::make_unique<LayerManifest>(metaPath);
  if (!s->manifest->open())
    return nullptr;
  s->layerSvc = std::make_unique<QgisLayerService>(&s->projectSvc, s->manifest.get());
  s->store = std::make_unique<PaleoProjectStore>();
  s->importSvc = std::make_unique<DataImportService>(s->layerSvc.get(), s->store.get());
  s->importSvc->setProjectDir(projectDir);
  s->preview = std::make_unique<DataPreviewTabs>();
  s->preview->setImportService(s->importSvc.get());
  return s;
}

QString TestPreviewMapPerf::fixture(const QString &name)
{
  return QStringLiteral(PROJECT_FIXTURE_DIR) + QLatin1Char('/') + name;
}

QString TestPreviewMapPerf::stage(const QTemporaryDir &tmp, const QString &dir,
                                  const QString &name, const QString &asName)
{
  const QString d = tmp.filePath(dir);
  if (!QDir().mkpath(d))
    return QString();
  const QString dst = QDir(d).filePath(asName.isEmpty() ? name : asName);
  return QFile::copy(fixture(name), dst) ? dst : QString();
}

void TestPreviewMapPerf::firstOpenUnderBudget()
{
  QTemporaryDir tmp;
  auto st = makeStack(tmp.filePath(QStringLiteral("proj")));
  QVERIFY(st);
  QString err;
  const QString d61Path = stage(tmp, QString::fromUtf8("层位"),
                                QStringLiteral("D61_sample.dat"), QStringLiteral("D61.dat"));
  const QString d61 = st->importSvc->importProjectFile(d61Path, &err);
  QVERIFY(!d61.isEmpty());

  // D6.7：可地图化资产首开墙钟（建页+栅格+渲染出口+等值线，不含帧渲完）。
  QElapsedTimer timer;
  timer.start();
  st->preview->openAsset(d61);
  const qint64 openMs = timer.elapsed();
  qInfo() << "PERF first-open(ms):" << openMs;
  QVERIFY(st->preview->findChild<QWidget *>(QStringLiteral("horizonPreviewPage")));
  // 预算 300ms；共享机负载放宽到 3 倍（fixture 量级 <50ms 常态）。
  QVERIFY2(openMs < 300 * 3,
           qPrintable(QStringLiteral("first open %1 ms exceeds budget").arg(openMs)));
}

void TestPreviewMapPerf::renderCacheHitOnReopen()
{
  QTemporaryDir tmp;
  auto st = makeStack(tmp.filePath(QStringLiteral("proj")));
  QVERIFY(st);
  QString err;
  const QString d61Path = stage(tmp, QString::fromUtf8("层位"),
                                QStringLiteral("D61_sample.dat"), QStringLiteral("D61.dat"));
  const QString d61 = st->importSvc->importProjectFile(d61Path, &err);
  QVERIFY(!d61.isEmpty());
  PreviewRenderCache::instance().clearMemoryForTesting();

  // 第一次开：渲染完成后落缓存。
  st->preview->openAsset(d61);
  auto *page = st->preview->findChild<QWidget *>(QStringLiteral("horizonPreviewPage"));
  QVERIFY(page);
  st->preview->resize(1100, 700);
  st->preview->show();
  QTest::qWaitForWindowExposed(st->preview.get());
  QTest::qWait(400); // 等 singleShot 缩放 + 首帧
  auto *canvas = page->findChild<QgsMapCanvas *>(QStringLiteral("horizonMapCanvas"));
  QVERIFY(canvas);
  const QSize sz = canvas->size();
  QVERIFY(!sz.isEmpty());
  // 与 horizon 分支同源：预览选中最新 DERIVED 版本（identity 的 versionId）。
  QString derivedId;
  for (const CatalogVersion &cv : st->importSvc->catalog()->versionsForAsset(d61))
    if (cv.stage == QLatin1String("DERIVED"))
      derivedId = cv.id; // versionsForAsset 按 versionNumber 升序——最后即最新
  QVERIFY(!derivedId.isEmpty());
  const QString key = PreviewRenderCache::makeKey(d61, derivedId, canvas->extent(),
                                                  sz.width(), sz.height());
  // 二开同资产同范围：缓存命中给非空图（D6.2）。
  const QImage cached = PreviewRenderCache::instance().lookup(key);
  qInfo() << "PERF cache-hit:" << !cached.isNull() << key.left(40);
  QVERIFY(!cached.isNull());
}

void TestPreviewMapPerf::snapshotRenderFast()
{
  QTemporaryDir tmp;
  auto st = makeStack(tmp.filePath(QStringLiteral("proj")));
  QVERIFY(st);
  QString err;
  const QString d61Path = stage(tmp, QString::fromUtf8("层位"),
                                QStringLiteral("D61_sample.dat"), QStringLiteral("D61.dat"));
  const QString d61 = st->importSvc->importProjectFile(d61Path, &err);
  QVERIFY(!d61.isEmpty());
  st->preview->openAsset(d61);
  auto *page = st->preview->findChild<PreviewMapPage *>(QStringLiteral("horizonPreviewPage"));
  QVERIFY(page);
  st->preview->resize(1100, 700);
  st->preview->show();
  QTest::qWaitForWindowExposed(st->preview.get());
  page->mapCanvas()->zoomToFullExtent();
  QTest::qWait(50);
  // D6.1：低清整图同步渲一张 <100ms（320px 宽量级）。
  QElapsedTimer timer;
  timer.start();
  const QImage img = page->mapCanvas()->renderSnapshot(320);
  const qint64 ms = timer.elapsed();
  qInfo() << "PERF snapshot(ms):" << ms;
  QVERIFY(!img.isNull());
  QVERIFY2(ms < 100 * 3, qPrintable(QStringLiteral("snapshot %1 ms").arg(ms)));
}

void TestPreviewMapPerf::identifyIndexReused()
{
  // D6.4：同一矢量层二次 identify 复用空间索引（缓存行不增、结果一致）。
  QTemporaryDir tmp;
  auto st = makeStack(tmp.filePath(QStringLiteral("proj")));
  QVERIFY(st);
  QString err;
  const QString geo = st->importSvc->importProjectFile(
      fixture(QStringLiteral("facies.geojson")), &err);
  QVERIFY(!geo.isEmpty());
  st->preview->openAsset(geo);
  auto *page = st->preview->findChild<PreviewMapPage *>(QStringLiteral("faciesPreviewPage"));
  QVERIFY(page);
  auto *canvas = page->findChild<QgsMapCanvas *>(QStringLiteral("faciesMapCanvas"));
  QVERIFY(canvas);
  auto *vl = qobject_cast<QgsVectorLayer *>(canvas->layers().first());
  QVERIFY(vl);

  PreviewIdentifyCore core;
  const QgsRectangle ext = vl->extent();
  const QgsPointXY center(ext.center());
  const auto r1 = core.identifyPoint({vl}, center, ext.width() * 0.05);
  QCOMPARE(core.indexCacheSize(), 1);
  QElapsedTimer timer;
  timer.start();
  const auto r2 = core.identifyPoint({vl}, center, ext.width() * 0.05);
  const qint64 secondMs = timer.elapsed();
  qInfo() << "PERF identify-2nd(ms):" << secondMs << "hits:" << r1.size() << r2.size();
  QCOMPARE(r2.size(), r1.size()); // 索引路径结果一致
  QCOMPARE(core.indexCacheSize(), 1); // 无新增缓存行（复用）
}

void TestPreviewMapPerf::profileSamplingUnderBudget()
{
  // D5.1：200 点沿线采样预算（fixture 栅格，provider->sample 路径）。
  QTemporaryDir tmp;
  auto st = makeStack(tmp.filePath(QStringLiteral("proj")));
  QVERIFY(st);
  QString err;
  const QString d61Path = stage(tmp, QString::fromUtf8("层位"),
                                QStringLiteral("D61_sample.dat"), QStringLiteral("D61.dat"));
  const QString d61 = st->importSvc->importProjectFile(d61Path, &err);
  QVERIFY(!d61.isEmpty());
  st->preview->openAsset(d61);
  auto *page = st->preview->findChild<PreviewMapPage *>(QStringLiteral("horizonPreviewPage"));
  QVERIFY(page);
  auto *canvas = page->findChild<QgsMapCanvas *>(QStringLiteral("horizonMapCanvas"));
  QVERIFY(canvas);
  auto *raster = qobject_cast<QgsRasterLayer *>(canvas->layers().value(1));
  QVERIFY(raster);
  const QgsRectangle ext = raster->extent();
  QElapsedTimer timer;
  timer.start();
  const auto samples = PreviewRasterAnalysis::sampleProfile(
      raster, QgsPointXY(ext.xMinimum(), ext.yMinimum()),
      QgsPointXY(ext.xMaximum(), ext.yMaximum()), 200);
  const qint64 ms = timer.elapsed();
  qInfo() << "PERF profile-sampling(ms):" << ms << "samples:" << samples.size();
  QCOMPARE(samples.size(), 200);
  QVERIFY2(ms < 100, qPrintable(QStringLiteral("sampling %1 ms").arg(ms)));
}

int main(int argc, char *argv[])
{
  if (!QgisRuntime::initialize(QStringLiteral("/usr")))
  {
    qFatal("QgisRuntime::initialize failed");
    return 1;
  }
  TestPreviewMapPerf tc;
  const int rc = QTest::qExec(&tc, argc, argv);
  QgisRuntime::shutdown();
  return rc;
}

#include "tst_previewmap_perf.moc"
