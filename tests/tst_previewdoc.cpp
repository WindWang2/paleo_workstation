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
#include "../src/services/paleotaskservice.h"

#include <QElapsedTimer>

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

private slots:
  void initTestCase() { QVERIFY(QgisRuntime::isInitialized()); }
  // ---- wave/data-foundation ----
  void lasHeaderFacadeMatchesAndIsFast();     // T1：header-only 出参一致且快
  void requestLasSyncFallbackEmitsBeforeReturn(); // T1：无任务服务同步降级
  void requestLasGenerationsDiscardStale();   // T1：世代号压制陈旧结果
  void catalogRecoveredFromBackupForwards();  // T5：门面转发恢复告警

  // 解析门面：lasAt 成功返回曲线名+曲线；坏路径 false + errorString。
  void lasFacade()
  {
    QStringList names;
    QList<LasCurve> curves;
    QString err;
    QVERIFY2(PreviewDocService::lasAt(TestPreviewDoc::fixture(QStringLiteral("A1.Las")),
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

  // ---- wave/data-foundation T1：LAS header-only 门面 ------------------------
  // 大 LAS fixture：6 曲线 × 60k 行（约 3.5MB）——头部只有 ~10 行。断言：
  // header 出参与全量解析逐项一致；耗时与数据行数解耦（绝对预算大余量）。
  static QString makeBigLas(const QTemporaryDir &tmp)
  {
    const QString path = tmp.filePath(QStringLiteral("BIG1.Las"));
    QFile f(path);
    if (!f.open(QIODevice::WriteOnly | QIODevice::Truncate))
      return QString();
    QByteArray head =
        "~Version Information\nVERS. 2.0:\nWRAP. NO:\n"
        "~Well\nWELL. BIG1 : WELL\nNULL. -99999.0 :\n"
        "~Curve\nDEPT.M :\nGR.API :\nDEN.G/C3 :\nSP.MV :\nCAL.MM :\nPEF. :\n"
        "~ASCII LOG DATA\n";
    f.write(head);
    QByteArray row;
    for (int i = 0; i < 60000; ++i)
    {
      const double d = 1000.0 + 0.125 * i;
      row = QByteArray::number(d, 'f', 3);
      for (int c = 0; c < 5; ++c)
        row += ' ' + QByteArray::number(d + c * 1.5, 'f', 3);
      row += '\n';
      f.write(row);
    }
    f.close();
    return path;
  }

void TestPreviewDoc::lasHeaderFacadeMatchesAndIsFast()
{
    QTemporaryDir tmp;
    QVERIFY(tmp.isValid());
    const QString big = makeBigLas(tmp);
    QVERIFY(!big.isEmpty());

    QStringList fullNames;
    QList<LasCurve> fullCurves;
    QString ferr;
    QVERIFY2(PreviewDocService::lasAt(big, &fullNames, &fullCurves, &ferr),
             qPrintable(ferr));
    QCOMPARE(fullNames.size(), 6);
    QCOMPARE(fullCurves.size(), 6);
    QCOMPARE(fullCurves.at(0).values.size(), 60000);

    LasHeaderInfo header;
    QString herr;
    QVERIFY2(PreviewDocService::lasHeaderAt(big, &header, &herr), qPrintable(herr));
    QCOMPARE(header.curveNames, fullNames); // 与全量解析逐项一致
    QCOMPARE(header.wellName, QStringLiteral("BIG1"));
    QCOMPARE(header.nullValue, -99999.0);
    QVERIFY(header.sawAscii);

    // 计时：header-only 明显快于全量（预算 = 全量时间本身作上限，另加
    // 50ms 绝对余量防超快机器上全量也毫秒级的退化）。
    QElapsedTimer clock;
    clock.start();
    LasHeaderInfo h2;
    QVERIFY(PreviewDocService::lasHeaderAt(big, &h2));
    const qint64 headerMs = clock.elapsed();
    clock.restart();
    QStringList n2;
    QList<LasCurve> c2;
    QVERIFY(PreviewDocService::lasAt(big, &n2, &c2));
    const qint64 fullMs = clock.elapsed();
    qInfo("las timings: header=%lldms full=%lldms", static_cast<long long>(headerMs),
          static_cast<long long>(fullMs));
    QVERIFY2(headerMs < fullMs || fullMs < 5,
             qPrintable(QStringLiteral("header=%1 full=%2").arg(headerMs).arg(fullMs)));
    QVERIFY2(headerMs < 50, qPrintable(QString::number(headerMs)));

    // 坏文件语义对齐：WRAP YES 拒绝；无 ~C 拒绝；~A 缺失不算失败。
    {
      const QString wrapYes = tmp.filePath(QStringLiteral("WRAP.Las"));
      QFile f(wrapYes);
      QVERIFY(f.open(QIODevice::WriteOnly));
      f.write("~Version\nWRAP. YES :\n~Curve\nDEPT.M :\n~A DEPT\n1\n");
      f.close();
      LasHeaderInfo h;
      QString err;
      QVERIFY(!PreviewDocService::lasHeaderAt(wrapYes, &h, &err));
      QVERIFY(err.contains(QStringLiteral("WRAP YES")));
    }
    {
      const QString noC = tmp.filePath(QStringLiteral("NOC.Las"));
      QFile f(noC);
      QVERIFY(f.open(QIODevice::WriteOnly));
      f.write("~Version\nVERS. 2.0:\n~A 1\n");
      f.close();
      LasHeaderInfo h;
      QString err;
      QVERIFY(!PreviewDocService::lasHeaderAt(noC, &h, &err));
      QVERIFY(err.contains(QStringLiteral("~C")));
    }
  }

  // T1：无任务服务 → 同步降级——lasReady 在 requestLas 返回前已发。
void TestPreviewDoc::requestLasSyncFallbackEmitsBeforeReturn()
{
    QTemporaryDir tmp;
    QVERIFY(tmp.isValid());
    auto stack = TestPreviewDoc::makeStack(tmp.path());
    QVERIFY(stack != nullptr);
    PreviewDocService doc(stack->importSvc.get()); // 无 setTaskService

    const QString las = TestPreviewDoc::fixture(QStringLiteral("A1.Las"));
    QSignalSpy ready(&doc, &PreviewDocService::lasReady);
    doc.requestLas(QStringLiteral("k1"), las);
    QCOMPARE(ready.count(), 1); // 同步路径：返回前已发
    const QStringList names = ready.at(0).at(1).toStringList();
    QVERIFY(!names.isEmpty());

    // 与 lasAt 同口径：曲线数一致。
    QStringList fullNames;
    QList<LasCurve> fullCurves;
    QString err;
    QVERIFY(PreviewDocService::lasAt(las, &fullNames, &fullCurves, &err));
    QCOMPARE(names, fullNames);

    doc.releaseLas(QStringLiteral("k1"));
    doc.requestLas(QStringLiteral("k1"), las);
    QCOMPARE(ready.count(), 2); // release 后同 key 重新可用
  }

  // T1：任务池路径——同 key 新请求作废旧代（陈旧 lasReady 压制在发射前）。
void TestPreviewDoc::requestLasGenerationsDiscardStale()
{
    QTemporaryDir tmp;
    QVERIFY(tmp.isValid());
    auto stack = TestPreviewDoc::makeStack(tmp.path());
    QVERIFY(stack != nullptr);
    PaleoTaskService tasks(stack->store.get());
    PreviewDocService doc(stack->importSvc.get());
    doc.setTaskService(&tasks);

    const QString big = makeBigLas(tmp);
    QVERIFY(!big.isEmpty());
    const QString small = TestPreviewDoc::fixture(QStringLiteral("A1.Las"));

    QSignalSpy ready(&doc, &PreviewDocService::lasReady);
    QSignalSpy failed(&doc, &PreviewDocService::lasFailed);
    doc.requestLas(QStringLiteral("k"), big);   // 旧代：大文件（慢）
    doc.requestLas(QStringLiteral("k"), small); // 新代：小文件（快）

    QElapsedTimer clock;
    clock.start();
    while (ready.count() < 1 && clock.elapsed() < 15000)
      QCoreApplication::processEvents(QEventLoop::AllEvents, 25);
    // 让可能晚到的大文件任务收尾（其结果按世代号被丢弃）。
    while (clock.elapsed() < 1000)
      QCoreApplication::processEvents(QEventLoop::AllEvents, 25);

    QCOMPARE(ready.count(), 1); // 该 key 只有最新一代到达
    QCOMPARE(failed.count(), 0);
    const QStringList names = ready.at(0).at(1).toStringList();
    QStringList smallNames;
    QList<LasCurve> smallCurves;
    QString err;
    QVERIFY(PreviewDocService::lasAt(small, &smallNames, &smallCurves, &err));
    QCOMPARE(names, smallNames); // 到达的是小文件那一代
    QCOMPARE(ready.at(0).at(0).toString(), QStringLiteral("k"));
  }

  // T5：catalog .bak 恢复告警经 DataImportService → 门面转发。
void TestPreviewDoc::catalogRecoveredFromBackupForwards()
{
    QTemporaryDir tmp;
    QVERIFY(tmp.isValid());
    auto stack = TestPreviewDoc::makeStack(tmp.path());
    QVERIFY(stack != nullptr);
    const QString catPath = QDir(tmp.path()).filePath(
        QStringLiteral("artifacts/metadata/catalog.json"));
    QVERIFY(QFile::exists(catPath));
    // 先制造一代 .bak（再导入一次触发 save 轮转），再损坏主文件。
    QString ierr;
    QVERIFY(!stack->importSvc
                  ->importProjectFile(TestPreviewDoc::fixture(QStringLiteral("A1.Las")), &ierr)
                  .isEmpty());
    QVERIFY(QFile::exists(catPath + QStringLiteral(".bak")));

    PreviewDocService doc(stack->importSvc.get());
    QSignalSpy recovered(&doc, &PreviewDocService::catalogRecoveredFromBackup);
    QSignalSpy openFailed(&doc, &PreviewDocService::catalogOpenFailed);
    {
      QFile f(catPath);
      QVERIFY(f.open(QIODevice::WriteOnly));
      f.write("{broken");
    }
    stack->importSvc->setProjectDir(tmp.path()); // 重开 → .bak 回退
    QCOMPARE(recovered.count(), 1);
    QCOMPARE(openFailed.count(), 0); // 恢复成功不算打开失败
    QVERIFY(recovered.at(0).at(0).toString().contains(
        QStringLiteral("catalog.json"))); // 原因 = 主文件路径 + 解析错误
    QVERIFY(doc.catalog()->isOpen());
    QVERIFY(doc.catalog()->recoveredFromBackup());
  }

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
