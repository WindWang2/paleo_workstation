#include <QtTest>
#include <QTemporaryDir>

#include "../src/catalog/datacatalog.h"
#include "../src/io/dataimportservice.h"
#include "../src/metadata/layermanifest.h"
#include "../src/metadata/paleoprojectstore.h"
#include "../src/qgis/qgislayerservice.h"
#include "../src/qgis/qgisprojectservice.h"
#include "../src/qgis/qgisruntime.h"
#include "../src/ui/datapreview/datapreviewtabs.h"
#include "../src/workflow/mapversioncontroller.h"

#include <QLabel>
#include <QTabWidget>

// p5b data/staleness：staleness-lite 的两端接线——
//   1. 预览侧产标：外链 sha 复验失败 ⇒ markDownstreamStale 落
//      extra["stale"]（reason=「上游外链版本 sha 校验失败」），幂等不空涨 revision；
//   2. 发布门 advisory：stale DERIVED 计数与文案（只读、不阻断、可见）；
//   3. 数据页徽标：stale 资产预览标签标题带「过时」，catalog 变更后随刷新。
// catalog 内部语义（闭包/supersede/标记原子性）由 tst_entityview 覆盖，
// 此处只测 UI/workflow 接线 + 重开 catalog 后的标记存活。
class TestStaleness : public QObject
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
    DataCatalog *cat = nullptr;
  };

  // 与 tst_datapreview 同构的最小栈（刻意不接任务服务——外链 sha 复验走
  // buildContent 的同步门，正是本包要接 markDownstreamStale 的路径）。
  static std::unique_ptr<Stack> makeStack(const QString &projectDir)
  {
    if (!QDir().mkpath(projectDir))
      return nullptr;
    auto s = std::make_unique<Stack>();
    if (!s->projectSvc.createProject(
            QDir(projectDir).filePath(QStringLiteral("proj.qgz"))))
      return nullptr;
    s->manifest = std::make_unique<LayerManifest>(
        QDir(projectDir).filePath(QStringLiteral("metadata/project.sqlite")));
    if (!s->manifest->open())
      return nullptr;
    s->layerSvc = std::make_unique<QgisLayerService>(&s->projectSvc, s->manifest.get());
    s->store = std::make_unique<PaleoProjectStore>();
    s->importSvc = std::make_unique<DataImportService>(s->layerSvc.get(), s->store.get());
    s->importSvc->setProjectDir(projectDir);
    s->cat = s->importSvc->catalog();
    s->preview = std::make_unique<DataPreviewTabs>();
    s->preview->setImportService(s->importSvc.get());
    return s;
  }

  static QString fixture(const QString &name)
  {
    return QStringLiteral(PROJECT_FIXTURE_DIR) + QLatin1Char('/') + name;
  }

  static CatalogAsset makeAsset(const QString &id)
  {
    CatalogAsset a;
    a.id = id;
    a.type = QStringLiteral("generic");
    a.format = QStringLiteral("dat");
    a.displayName = id + QStringLiteral(".dat");
    return a;
  }

  static CatalogVersion makeVersion(const QString &id, const QString &assetId,
                                    const QString &stage = QStringLiteral("RAW"),
                                    const QStringList &parents = {})
  {
    CatalogVersion v;
    v.id = id;
    v.assetId = assetId;
    v.stage = stage;
    v.versionNumber = 1;
    v.parentVersionIds = parents;
    v.fileName = id + QStringLiteral(".dat");
    return v;
  }

private slots:
  void initTestCase() { QVERIFY(QgisRuntime::isInitialized()); }

  void previewShaMismatchMarksDerivedStale();
  void previewShaMismatchIsIdempotentNoRevisionGrowth();
  void publishAdvisoryCountsStaleDerived();
  void publishAdvisorySurvivesCatalogReopen();
  void staleBadgeMarksPreviewTabTitle();
};

// 外链源被改字节 ⇒ 预览同步 sha 门拒解码，同时把下游闭包里的 DERIVED
// 版本如实标 stale（reason 用发布规格定的中文一致文案），种子自身不标。
void TestStaleness::previewShaMismatchMarksDerivedStale()
{
  QTemporaryDir tmp;
  auto st = makeStack(tmp.filePath(QStringLiteral("proj")));
  QVERIFY(st != nullptr);
  const QString sgy = tmp.filePath(QStringLiteral("vol.sgy"));
  QVERIFY(QFile::copy(fixture(QStringLiteral("mini_seismic.sgy")), sgy));
  QString err;
  const QString assetId = st->importSvc->importProjectFile(sgy, &err);
  QVERIFY2(!assetId.isEmpty(), qPrintable(err));
  const CatalogVersion ext = st->cat->currentVersion(assetId);
  QVERIFY(!ext.managed);
  QVERIFY(!ext.sha256.isEmpty());

  // 下游 DERIVED：以被篡改的外链版本为唯一父。
  QVERIFY(st->cat->addAsset(makeAsset(QStringLiteral("ast-d"))));
  QVERIFY(st->cat->addVersion(makeVersion(QStringLiteral("ver-d"),
                                          QStringLiteral("ast-d"),
                                          QStringLiteral("DERIVED"),
                                          {ext.id})));

  QFile f(sgy);
  QVERIFY(f.open(QIODevice::Append));
  QCOMPARE(f.write("X"), 1);
  f.close();

  st->preview->openAsset(assetId);
  auto *tabs = st->preview->findChild<QTabWidget *>(QStringLiteral("dataPreviewTabs"));
  QVERIFY(tabs);
  QWidget *page = tabs->widget(tabs->currentIndex());
  QVERIFY(page);
  auto *state = page->findChild<QLabel *>(QStringLiteral("stateText"));
  QVERIFY2(state, "sha-mismatch tab must show the honest state line");
  QCOMPARE(state->text(), QStringLiteral("源文件与入库时的 SHA-256 不一致"));

  // 接线断言：下游 DERIVED 已标过时，原因与规格文案一致；外链种子不标。
  const CatalogVersion d = st->cat->versionById(QStringLiteral("ver-d"));
  QVERIFY2(d.extra.value(QStringLiteral("stale")).toBool(),
           "sha mismatch must mark downstream DERIVED stale");
  QCOMPARE(d.extra.value(QStringLiteral("staleReason")).toString(),
           QStringLiteral("上游外链版本 sha 校验失败"));
  QVERIFY(!st->cat->versionById(ext.id)
               .extra.value(QStringLiteral("stale"))
               .toBool());
}

// 幂等：同一失配重复触发（关标签重开 = 复验再次失败再次标记）不空涨
// revision——datacatalog 已保证同标记不写盘，此处断言预览路径如实受益。
void TestStaleness::previewShaMismatchIsIdempotentNoRevisionGrowth()
{
  QTemporaryDir tmp;
  auto st = makeStack(tmp.filePath(QStringLiteral("proj")));
  QVERIFY(st != nullptr);
  const QString sgy = tmp.filePath(QStringLiteral("vol.sgy"));
  QVERIFY(QFile::copy(fixture(QStringLiteral("mini_seismic.sgy")), sgy));
  QString err;
  const QString assetId = st->importSvc->importProjectFile(sgy, &err);
  QVERIFY2(!assetId.isEmpty(), qPrintable(err));
  const CatalogVersion ext = st->cat->currentVersion(assetId);
  QVERIFY(st->cat->addAsset(makeAsset(QStringLiteral("ast-d"))));
  QVERIFY(st->cat->addVersion(makeVersion(QStringLiteral("ver-d"),
                                          QStringLiteral("ast-d"),
                                          QStringLiteral("DERIVED"),
                                          {ext.id})));

  QFile f(sgy);
  QVERIFY(f.open(QIODevice::Append));
  QCOMPARE(f.write("X"), 1);
  f.close();

  st->preview->openAsset(assetId);
  QVERIFY(st->cat->versionById(QStringLiteral("ver-d"))
              .extra.value(QStringLiteral("stale"))
              .toBool());
  const int revAfterFirst = st->cat->catalogRevision();

  // 关标签（清 m_shaVerified 会话缓存）再开：复验再次失败、标记再次触发。
  st->preview->closeAssetTab(assetId);
  st->preview->openAsset(assetId);
  QVERIFY(st->cat->versionById(QStringLiteral("ver-d"))
              .extra.value(QStringLiteral("stale"))
              .toBool());
  QCOMPARE(st->cat->catalogRevision(), revAfterFirst); // 幂等：不空涨
}

// 发布门 advisory 的数据口径：只数带 extra["stale"] 的 DERIVED 版本；
// RAW/INTERMEDIATE/未标的不计。文案如实带计数且声明不阻断；零 stale →
// 空文案（确认对话不加行）。
void TestStaleness::publishAdvisoryCountsStaleDerived()
{
  QTemporaryDir dir;
  QVERIFY(dir.isValid());
  DataCatalog cat;
  QVERIFY(cat.open(dir.path()));
  for (const QString &id : {QStringLiteral("ast-raw"), QStringLiteral("ast-d1"),
                            QStringLiteral("ast-d2"), QStringLiteral("ast-d3")})
    QVERIFY(cat.addAsset(makeAsset(id)));
  QVERIFY(cat.addVersion(makeVersion(QStringLiteral("ver-raw"),
                                     QStringLiteral("ast-raw"))));
  QVERIFY(cat.addVersion(makeVersion(QStringLiteral("ver-d1"),
                                     QStringLiteral("ast-d1"),
                                     QStringLiteral("DERIVED"),
                                     {QStringLiteral("ver-raw")})));
  QVERIFY(cat.addVersion(makeVersion(QStringLiteral("ver-d2"),
                                     QStringLiteral("ast-d2"),
                                     QStringLiteral("DERIVED"),
                                     {QStringLiteral("ver-raw")})));
  QVERIFY(cat.addVersion(makeVersion(QStringLiteral("ver-d3"),
                                     QStringLiteral("ast-d3"),
                                     QStringLiteral("DERIVED"),
                                     {QStringLiteral("ver-raw")})));
  QVERIFY(cat.addVersion(makeVersion(QStringLiteral("ver-i1"),
                                     QStringLiteral("ast-raw"),
                                     QStringLiteral("INTERMEDIATE"),
                                     {QStringLiteral("ver-raw")})));

  QCOMPARE(MapVersionController::staleDerivedCount(&cat), 0);
  QVERIFY(MapVersionController::stalePublishAdvisory(&cat).isEmpty());

  QVERIFY(cat.markDownstreamStale(QStringLiteral("ver-raw"),
                                  QStringLiteral("上游外链版本 sha 校验失败")));
  QCOMPARE(MapVersionController::staleDerivedCount(&cat), 3); // 只数 DERIVED

  const QString advisory = MapVersionController::stalePublishAdvisory(&cat);
  QVERIFY2(!advisory.isEmpty(), "stale downstream present → advisory must be visible");
  QVERIFY(advisory.contains(QStringLiteral("存在过时下游产物（3 个）")));
  QVERIFY(advisory.contains(QStringLiteral("不阻断"))); // advisory 语义如实

  // 空 catalog 指针（未打开工程）：0 + 空文案，不假装有评估。
  QCOMPARE(MapVersionController::staleDerivedCount(nullptr), 0);
  QVERIFY(MapVersionController::stalePublishAdvisory(nullptr).isEmpty());
}

// 重开工程：stale 标记随 catalog.json 持久化，发布门 advisory 重开后仍
// 如实计数（卸载/重开不丢产标）。
void TestStaleness::publishAdvisorySurvivesCatalogReopen()
{
  QTemporaryDir dir;
  QVERIFY(dir.isValid());
  QString reason;
  {
    DataCatalog cat;
    QVERIFY(cat.open(dir.path()));
    QVERIFY(cat.addAsset(makeAsset(QStringLiteral("ast-raw"))));
    QVERIFY(cat.addAsset(makeAsset(QStringLiteral("ast-d"))));
    QVERIFY(cat.addVersion(makeVersion(QStringLiteral("ver-raw"),
                                       QStringLiteral("ast-raw"))));
    QVERIFY(cat.addVersion(makeVersion(QStringLiteral("ver-d"),
                                       QStringLiteral("ast-d"),
                                       QStringLiteral("DERIVED"),
                                       {QStringLiteral("ver-raw")})));
    QVERIFY(cat.markDownstreamStale(QStringLiteral("ver-raw"),
                                    QStringLiteral("上游外链版本 sha 校验失败")));
    reason = cat.versionById(QStringLiteral("ver-d"))
                 .extra.value(QStringLiteral("staleReason"))
                 .toString();
    QCOMPARE(reason, QStringLiteral("上游外链版本 sha 校验失败"));
  } // catalog 析构 = 卸载工程

  DataCatalog reopened;
  QVERIFY(reopened.open(dir.path()));
  QVERIFY(reopened.versionById(QStringLiteral("ver-d"))
              .extra.value(QStringLiteral("stale"))
              .toBool());
  QCOMPARE(reopened.versionById(QStringLiteral("ver-d"))
               .extra.value(QStringLiteral("staleReason"))
               .toString(),
           reason);
  QCOMPARE(MapVersionController::staleDerivedCount(&reopened), 1);
}

// 数据页徽标：stale 资产（当前版本被标）的预览标签标题带「过时」；
// 标记落盘时 catalog changed() → 已开标签标题随刷新（不必重建标签）。
void TestStaleness::staleBadgeMarksPreviewTabTitle()
{
  QTemporaryDir tmp;
  auto st = makeStack(tmp.filePath(QStringLiteral("proj")));
  QVERIFY(st != nullptr);
  QVERIFY(st->cat->addAsset(makeAsset(QStringLiteral("ast-raw"))));
  QVERIFY(st->cat->addAsset(makeAsset(QStringLiteral("ast-d"))));
  QVERIFY(st->cat->addVersion(makeVersion(QStringLiteral("ver-raw"),
                                          QStringLiteral("ast-raw"))));
  QVERIFY(st->cat->addVersion(makeVersion(QStringLiteral("ver-d"),
                                          QStringLiteral("ast-d"),
                                          QStringLiteral("DERIVED"),
                                          {QStringLiteral("ver-raw")})));

  st->preview->openAsset(QStringLiteral("ast-d"));
  auto *tabs = st->preview->findChild<QTabWidget *>(QStringLiteral("dataPreviewTabs"));
  QVERIFY(tabs);
  const int idx = tabs->currentIndex();
  QCOMPARE(tabs->tabText(idx), QStringLiteral("ast-d.dat")); // 未标 → 无徽标

  // 上游失效（如 sha 失配/被取代）→ DERIVED 被标 → 已开标签标题即时带「过时」。
  QVERIFY(st->cat->markDownstreamStale(QStringLiteral("ver-raw"),
                                       QStringLiteral("上游外链版本 sha 校验失败")));
  QVERIFY(tabs->tabText(idx).endsWith(QStringLiteral("过时")));

  // 新开标签同样带徽标；未标资产永不带。
  st->preview->openAsset(QStringLiteral("ast-raw"));
  QVERIFY(!tabs->tabText(tabs->currentIndex())
               .contains(QStringLiteral("过时")));
}

int main(int argc, char *argv[])
{
  if (!QgisRuntime::initialize(QStringLiteral("/usr")))
  {
    qFatal("QgisRuntime::initialize failed");
    return 1;
  }
  TestStaleness tc;
  const int rc = QTest::qExec(&tc, argc, argv);
  QgisRuntime::shutdown();
  return rc;
}

#include "tst_staleness.moc"
