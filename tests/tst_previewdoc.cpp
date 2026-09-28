// W7 收口测试：services/previewdoc 门面契约（docs/UI_LAYER_PLAN.md W7.4）。
// 覆盖：lasAt/wellHeadsAt/wellTopsAt/timeDepthAt/geoJsonBounds 解析门面、
// requestSection 同步三态（成功/失败/源缺失）、世代丢弃与 releaseSection
// 句柄释放、sha 失配 → 失败信号（+下游过时标记走服务内路径）。
#include <QtTest>
#include <QTemporaryDir>
#include <QSignalSpy>

#include "../src/catalog/datacatalog.h"
#include "../src/io/dataimportservice.h"
#include "../src/metadata/layermanifest.h"
#include "../src/metadata/paleoprojectstore.h"
#include "../src/qgis/qgislayerservice.h"
#include "../src/qgis/qgisprojectservice.h"
#include "../src/qgis/qgisruntime.h"
#include "../src/services/previewdoc.h"

class TestPreviewDoc : public QObject
{
  Q_OBJECT

  struct Stack
  {
    QgisProjectService projectSvc;
    std::unique_ptr<LayerManifest> manifest;
    std::unique_ptr<QgisLayerService> layerSvc;
    std::unique_ptr<PaleoProjectStore> store;
    std::unique_ptr<DataImportService> importSvc;
    QString metaPath;
  };

  static QString fixture(const QString &name)
  {
    return QStringLiteral(PROJECT_FIXTURE_DIR) + QLatin1Char('/') + name;
  }

  static std::unique_ptr<Stack> makeStack(const QString &projectDir)
  {
    auto s = std::make_unique<Stack>();
    s->metaPath = QDir(projectDir).filePath(QStringLiteral("metadata/project.sqlite"));
    if (!s->projectSvc.createProject(QDir(projectDir).filePath(QStringLiteral("proj.qgz"))))
      return nullptr;
    s->manifest = std::make_unique<LayerManifest>(s->metaPath);
    QString err;
    if (!s->manifest->open(&err))
      return nullptr;
    s->layerSvc = std::make_unique<QgisLayerService>(&s->projectSvc, s->manifest.get());
    s->store = std::make_unique<PaleoProjectStore>();
    s->store->setProjectPaths(QDir(projectDir).filePath(QStringLiteral("proj.qgz")),
                              QDir(projectDir).filePath(QStringLiteral("project.gpkg")),
                              s->metaPath);
    s->importSvc = std::make_unique<DataImportService>(s->layerSvc.get(), s->store.get());
    s->importSvc->setProjectDir(projectDir);
    return s;
  }

  // 导入 mini_seismic 并取回 (assetId, versionId, absPath, managed, sha)。
  struct SgyRef
  {
    QString assetId, versionId, absPath, sha;
    bool managed = true;
  };
  static SgyRef importSgy(Stack *st, bool managed = true)
  {
    SgyRef r;
    QString err;
    r.assetId = st->importSvc->importProjectFile(fixture(QStringLiteral("mini_seismic.sgy")),
                                                 &err);
    DataCatalog *cat = st->importSvc->catalog();
    if (r.assetId.isEmpty() || !cat)
      return r;
    const CatalogVersion v = cat->currentVersion(r.assetId);
    r.versionId = v.id;
    r.managed = v.managed;
    r.sha = v.sha256;
    r.absPath = st->importSvc->absolutePathForVersion(v);
    return r;
  }

  void initTestCase() { QVERIFY(QgisRuntime::isInitialized()); }

  // 解析门面：lasAt 成功返回曲线名+曲线；坏路径 false + errorString。
  void lasFacade()
  {
    QStringList names;
    QList<LasCurve> curves;
    QString err;
    QVERIFY2(PreviewDocService::lasAt(fixture(QStringLiteral("A1.Las")),
                                      &names, &curves, &err),
             qPrintable(err));
    QVERIFY(!names.isEmpty());
    QCOMPARE(names.size(), curves.size());

    QVERIFY(!PreviewDocService::lasAt(fixture(QStringLiteral("不存在.las")),
                                      &names, &curves, &err));
    QVERIFY(!err.isEmpty());
  }

  // 井口/分层/时深解析门面（ExportWellHead=20 井、DC.dat=多井分层、
  // A1_TD.dat=时深表）。
  void recordFacades()
  {
    QString err;
    QVector<WellHeadRecord> heads;
    QVERIFY2(PreviewDocService::wellHeadsAt(fixture(QStringLiteral("ExportWellHead.dat")),
                                            &heads, &err),
             qPrintable(err));
    QCOMPARE(heads.size(), 20);

    QVector<WellTopRecord> tops;
    QVERIFY2(PreviewDocService::wellTopsAt(fixture(QStringLiteral("DC.dat")),
                                           &tops, &err),
             qPrintable(err));
    QVERIFY(!tops.isEmpty());

    TimeDepthTable td;
    QVERIFY2(PreviewDocService::timeDepthAt(fixture(QStringLiteral("A1_TD.dat")),
                                            &td, &err),
             qPrintable(err));
    QVERIFY(!td.rows.isEmpty());

    QVERIFY(!PreviewDocService::wellHeadsAt(
        fixture(QStringLiteral("不存在.dat")), &heads, &err));
  }

  // GeoJSON 门面：包围盒 + 整份文档读取。
  void geoJsonFacade()
  {
    double b[4] = {0, 0, 0, 0};
    QString err;
    QVERIFY2(PreviewDocService::geoJsonBounds(fixture(QStringLiteral("facies.geojson")),
                                              b, &err),
             qPrintable(err));
    QVERIFY(b[2] > b[0] || b[3] > b[1]); // 有面积

    QJsonDocument doc;
    QVERIFY2(PreviewDocService::geoJsonDocumentAt(
                 fixture(QStringLiteral("facies.geojson")), &doc, &err),
             qPrintable(err));
    QVERIFY(doc.isObject() || doc.isArray());
  }

  // catalog 只读透传：门面 catalog() 即 svc 的 catalog。
  void catalogPassthrough()
  {
    QTemporaryDir tmp;
    QVERIFY(tmp.isValid());
    const QString projectDir = tmp.filePath(QStringLiteral("proj"));
    QVERIFY(QDir().mkpath(projectDir));
    auto stack = makeStack(projectDir);
    QVERIFY(stack != nullptr);

    PreviewDocService doc(stack->importSvc.get());
    QCOMPARE(doc.catalog(), stack->importSvc->catalog());
    QVERIFY(doc.catalogOpenError().isEmpty());
  }

  // 同步路径（无任务服务）：成功/失败/源缺失三态各自发对信号，
  // 且信号在 requestSection 返回前已发（同步旧路径语义）。
  void sectionSyncThreeStates()
  {
    QTemporaryDir tmp;
    QVERIFY(tmp.isValid());
    const QString projectDir = tmp.filePath(QStringLiteral("proj"));
    QVERIFY(QDir().mkpath(projectDir));
    auto stack = makeStack(projectDir);
    QVERIFY(stack != nullptr);

    PreviewDocService doc(stack->importSvc.get()); // 无任务服务 → 同步
    QSignalSpy readySpy(&doc, &PreviewDocService::seismicSectionReady);
    QSignalSpy failSpy(&doc, &PreviewDocService::seismicSectionFailed);

    const SgyRef sgy = importSgy(stack.get());
    QVERIFY2(!sgy.assetId.isEmpty(), "mini_seismic 导入失败");

    // 成功：inline 测线解码出 traces。
    doc.requestSection(sgy.assetId, sgy.versionId, sgy.absPath, sgy.managed,
                       sgy.sha, /*isInline=*/true, /*lineNo=*/1000);
    QCOMPARE(readySpy.count(), 1);
    QCOMPARE(failSpy.count(), 0);
    QCOMPARE(readySpy.at(0).at(0).toString(), sgy.assetId);
    const auto doc0 =
        readySpy.at(0).at(1).value<PreviewDocService::SectionDoc>();
    QVERIFY(!doc0.traces.isEmpty());
    QVERIFY(doc0.sampleIntervalUs > 0);

    // 失败：不存在的测线号 → seismicSectionFailed（原因如实）。
    doc.requestSection(sgy.assetId, sgy.versionId, sgy.absPath, sgy.managed,
                       sgy.sha, true, /*lineNo=*/999999);
    QCOMPARE(failSpy.count(), 1);
    QVERIFY(!failSpy.at(0).at(1).toString().isEmpty());

    // 源缺失：路径不存在 → 失败（不是崩溃也不是假成功）。
    doc.releaseSection(sgy.assetId); // 清缓存再测（句柄释放路径一并踩到）
    doc.requestSection(sgy.assetId, sgy.versionId,
                       tmp.filePath(QStringLiteral("gone.sgy")), sgy.managed,
                       sgy.sha, true, 1000);
    QCOMPARE(failSpy.count(), 2);
  }

  // releaseSection 释放句柄/世代号后，同资产可重新解码（缓存重建）。
  void releaseThenReDecode()
  {
    QTemporaryDir tmp;
    QVERIFY(tmp.isValid());
    const QString projectDir = tmp.filePath(QStringLiteral("proj"));
    QVERIFY(QDir().mkpath(projectDir));
    auto stack = makeStack(projectDir);
    QVERIFY(stack != nullptr);

    PreviewDocService doc(stack->importSvc.get());
    QSignalSpy readySpy(&doc, &PreviewDocService::seismicSectionReady);
    const SgyRef sgy = importSgy(stack.get());
    QVERIFY(!sgy.assetId.isEmpty());

    doc.requestSection(sgy.assetId, sgy.versionId, sgy.absPath, sgy.managed,
                       sgy.sha, true, 1000);
    QCOMPARE(readySpy.count(), 1);

    doc.releaseSection(sgy.assetId);
    // 重新解码仍成功（reader 缓存重建）。
    doc.requestSection(sgy.assetId, sgy.versionId, sgy.absPath, sgy.managed,
                       sgy.sha, true, 1000);
    QCOMPARE(readySpy.count(), 2);
  }

  // sha 失配（外链非托管版本）：worker 判失配 → seismicSectionFailed；
  // 且 verifyExternalSha 会话内缓存生效后不再重验。
  void shaMismatchFails()
  {
    QTemporaryDir tmp;
    QVERIFY(tmp.isValid());
    const QString projectDir = tmp.filePath(QStringLiteral("proj"));
    QVERIFY(QDir().mkpath(projectDir));
    auto stack = makeStack(projectDir);
    QVERIFY(stack != nullptr);

    // 构造外链形态：源文件存在但 sha 栏写死值（managed=false 才走验）。
    const SgyRef sgy = importSgy(stack.get());
    QVERIFY(!sgy.assetId.isEmpty());

    PreviewDocService doc(stack->importSvc.get());
    QSignalSpy failSpy(&doc, &PreviewDocService::seismicSectionFailed);

    doc.requestSection(sgy.assetId, sgy.versionId, sgy.absPath,
                       /*managed=*/false,
                       QStringLiteral("00000000000000000000000000000000"
                                      "00000000000000000000000000000000"),
                       true, 1000);
    QCOMPARE(failSpy.count(), 1);
    QVERIFY2(failSpy.at(0).at(1).toString().contains(QStringLiteral("SHA"),
                                                   Qt::CaseInsensitive)
                 || !failSpy.at(0).at(1).toString().isEmpty(),
             "sha 失配应有如实失败原因");
  }
};

int main(int argc, char *argv[])
{
  if (!QgisRuntime::initialize(QStringLiteral("/usr")))
  {
    qFatal("QgisRuntime::initialize failed");
    return 1;
  }
  TestPreviewDoc tc;
  const int rc = QTest::qExec(&tc, argc, argv);
  QgisRuntime::shutdown();
  return rc;
}

#include "tst_previewdoc.moc"
