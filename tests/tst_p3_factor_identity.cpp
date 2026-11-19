// 层：测试壳
// P3/W1–W3：单因素/插值引擎入口的稳定单元身份、同一身份贯穿与父版本核验。
// 夹具：真实 Data Service 方案（井+顶底）+ 声明式井点层（direct 因子值），
// 覆盖验收矩阵：无单元/伪造 subinterval/错修订/Boundary/aggregate 拒绝；
// S1/S2 同顶界不碰撞；伪造 prepared/父版本被核验；显示名不改身份。
#include <QtTest>
#include <gdal.h>
#include <qgsapplication.h>
#include <qgsvectorlayer.h>
#include <paleo_data/service.h>

#include "catalog/datacatalog.h"
#include "metadata/layermanifest.h"
#include "metadata/paleoprojectstore.h"
#include "qgis/qgislayerservice.h"
#include "qgis/qgisprocessingservice.h"
#include "qgis/qgisprojectservice.h"
#include "services/analysisunitcontext.h"
#include "services/subintervalfactors.h"
#include "io/areamodel.h"
#include "workflow/constraintanalysisidentity.h"
#include "workflow/workflows.h"
#include "workflow/analysisresultsync.h"
#include "workflow/derivedassets.h"
#include "services/stratigraphicsubintervals.h"

#include <QTemporaryDir>
#include <QCryptographicHash>
#include <QFile>
#include <cmath>

using namespace paleo;

namespace {

// 与 tst_factorworkflow 同款 Data Service 方案夹具：一口井 + TOP/BASE 分层。
struct SchemeFixture
{
  QTemporaryDir dir;
  DataCatalog catalog;
  QgisProjectService projectSvc;
  PaleoProjectStore store;
  LayerManifest manifest{dir.filePath(QStringLiteral("project.sqlite"))};
  QgisLayerService layers{&projectSvc, &manifest};
  QgisProcessingService proc{&store};
  paleo::strat::UnitSelection s1, s2;
  QString schemeId, revision, intervalId;

  bool init()
  {
    if (!dir.isValid() || !catalog.open(dir.path()) ||
        !projectSvc.createProject(dir.filePath(QStringLiteral("proj.qgz"))) ||
        !manifest.open())
      return false;
    std::string detail;
    paleo::data::DataService ds;
    const auto root = area_model::directory(dir.path()).toStdString();
    if (!ds.createProject(root, "identity-test", detail))
      return false;
    const auto raw = dir.filePath(QStringLiteral("source.txt"));
    {
      QFile file(raw);
      if (!file.open(QIODevice::WriteOnly))
        return false;
      file.write("identity");
      file.close();
    }
    paleo::data::ParsedFile parsed;
    parsed.path = raw.toStdString();
    parsed.kind = "test";
    parsed.status = "parsed";
    paleo::data::WellDataset well;
    well.wellInfo.wellName = "W-1";
    well.wellInfo.wellheadX = 0;
    well.wellInfo.wellheadY = 0;
    well.defaultDepthInfo.depthType = "MD";
    well.defaultDepthInfo.depthUnit = "m";
    paleo::data::IntervalTrack track; // 岩性道：S 窗内有砂有泥，保证 direct/auto 都可用
    track.trackName = "岩性";
    track.trackType = "岩性";
    track.depthInfo = well.defaultDepthInfo;
    for (const auto &v : {std::tuple{80., 100., "泥岩"}, std::tuple{100., 130., "砂岩"},
                          std::tuple{130., 200., "泥岩"}}) {
      paleo::data::GeologicalInterval row;
      row.topDepth = std::get<0>(v);
      row.bottomDepth = std::get<1>(v);
      row.intervalName = std::get<2>(v);
      track.intervals.push_back(row);
    }
    well.wellbore.intervalTracks.push_back(track);
    parsed.wells.push_back(well);
    long long sourceId = 0;
    const auto sha = QCryptographicHash::hash(QByteArray("identity"), QCryptographicHash::Sha256).toHex().toStdString();
    if (!ds.importPreparedFile(raw.toStdString(), raw.toStdString(), raw.toStdString(), sha, parsed, sourceId, detail))
      return false;
    std::vector<paleo::data::WellSummary> wells;
    if (!ds.listWells(wells, detail) || wells.empty())
      return false;
    CatalogEntity entity;
    entity.id = "well-1";
    entity.name = "W-1";
    entity.entityType = "well";
    entity.hasSurface = true;
    entity.coordinateStatus = "untransformed";
    entity.surfaceX = 0;
    entity.surfaceY = 0;
    entity.extra.insert("areaModelWellId", wells.front().id);
    QString error;
    if (!catalog.addEntity(entity, &error))
      return false;
    paleo::data::StratigraphicScheme scheme;
    std::vector<paleo::data::FrameworkIssue> issues;
    auto draft = paleo::strat::draftScheme(
        "scheme", "scheme", {"TOP", "BASE"},
        {{wells.front().id, {{"TOP", 100.}, {"BASE", 200.}}}});
    if (!ds.commitStratigraphicScheme(draft, {}, scheme, issues, detail))
      return false;
    ds.closeProject();
    schemeId = QString::fromStdString(scheme.id.value);
    revision = QString::fromStdString(scheme.revision.value);
    intervalId = QString::fromStdString(scheme.intervals.front().id.value);
    s1 = paleo::strat::makeSelection(scheme, intervalId, 1);
    s2 = paleo::strat::makeSelection(scheme, intervalId, 2);
    return s1.isValid() && s2.isValid() && QFile::remove(raw);
  }

  // 声明带 z 值的井点层（direct 提取的真实输入层）。
  QString declareWells(const QString &layerId, const QString &horizon)
  {
    const auto path = dir.filePath(QStringLiteral("wells-%1.geojson").arg(layerId));
    {
      QFile f(path);
      if (!f.open(QIODevice::WriteOnly))
        return {};
      f.write("{\"type\":\"FeatureCollection\",\"crs\":{\"type\":\"name\",\"properties\":{\"name\":\"EPSG:3857\"}},\"features\":["
              "{\"type\":\"Feature\",\"properties\":{\"well_id\":\"W-1\",\"z\":42.0},\"geometry\":{\"type\":\"Point\",\"coordinates\":[0.0,0.0]}},"
              "{\"type\":\"Feature\",\"properties\":{\"well_id\":\"W-2\",\"z\":7.0},\"geometry\":{\"type\":\"Point\",\"coordinates\":[4.0,0.0]}},"
              "{\"type\":\"Feature\",\"properties\":{\"well_id\":\"W-3\",\"z\":20.0},\"geometry\":{\"type\":\"Point\",\"coordinates\":[0.0,4.0]}}"
              "]}");
      f.close();
    }
    LayerDeclaration d;
    d.layerId = layerId;
    d.horizon = horizon;
    d.type = QStringLiteral("vector");
    d.source = path;
    if (!layers.declare(d))
      return {};
    return d.layerId;
  }
};

QVariantMap runParams(const paleo::strat::UnitSelection &unit)
{
  return QVariantMap{{QStringLiteral("factorMode"), QStringLiteral("direct")},
                     {QStringLiteral("valueField"), QStringLiteral("z")},
                     {QStringLiteral("unitSelection"), paleo::strat::toMap(unit)},
                     {QStringLiteral("analysisLayerKey"), paleo::strat::analysisLayerKey(unit)},
                     {QStringLiteral("constraintHorizon"), QStringLiteral("TOP")},
                     {QStringLiteral("method"), QStringLiteral("idw")},
                     {QStringLiteral("cellSize"), 1.0}};
}

} // namespace

class P3FactorIdentityTest : public QObject
{
  Q_OBJECT
private slots:
  // W1：工程已有方案时，无单元的正式分析在数值/暂存前被拒绝，且无孤儿产物。
  void unitlessFormalAnalysisRejectedWhenSchemeExists()
  {
    SchemeFixture f;
    QVERIFY(f.init());
    f.declareWells(QStringLiteral("wells.main"), QStringLiteral("TOP"));
    ConstraintWorkflow workflow(&f.proc, &f.layers);
    workflow.setCatalog(&f.catalog, f.dir.path());
    const auto versionsBefore = f.catalog.versions().size();
    QString error;
    QVERIFY(!workflow.generateFactor(QStringLiteral("TOP"), QStringLiteral("sandthick"), {}, &error));
    QVERIFY(error.contains(QStringLiteral("子层段")));
    QVERIFY(!workflow.extractWellFactors(QStringLiteral("TOP"), QStringLiteral("sandthick"), {}, &error));
    QVERIFY(error.contains(QStringLiteral("子层段")));
    QVERIFY(!workflow.runConstraintIDW(QStringLiteral("TOP"), QStringLiteral("wells.main"),
                                       QStringLiteral("z"), 1.0, &error));
    QVERIFY(error.contains(QStringLiteral("子层段")));
    QCOMPARE(f.catalog.versions().size(), versionsBefore); // 无孤儿新产物
  }

  // W1：伪造 subinterval / 错 scheme/revision / Boundary 冒充 / aggregate 成图全部拒绝。
  void forgedAndInvalidUnitsRejected()
  {
    SchemeFixture f;
    QVERIFY(f.init());
    f.declareWells(QStringLiteral("wells.main"), QStringLiteral("TOP"));
    ConstraintWorkflow workflow(&f.proc, &f.layers);
    workflow.setCatalog(&f.catalog, f.dir.path());
    const auto versionsBefore = f.catalog.versions().size();
    QString error;
    auto params = runParams(f.s1);
    // 伪造 subinterval（不在方案内）
    auto forged = params;
    auto unit = f.s1;
    unit.subIntervalId = QStringLiteral("T70");
    forged.insert(QStringLiteral("unitSelection"), paleo::strat::toMap(unit));
    QVERIFY(!workflow.generateFactor(paleo::strat::analysisLayerKey(unit), QStringLiteral("sandthick"), forged, &error));
    QVERIFY(!error.isEmpty());
    // 错修订
    auto stale = params;
    unit = f.s1;
    unit.revision = QStringLiteral("deadbeef");
    stale.insert(QStringLiteral("unitSelection"), paleo::strat::toMap(unit));
    QVERIFY(!workflow.generateFactor(paleo::strat::analysisLayerKey(f.s1), QStringLiteral("sandthick"), stale, &error));
    QVERIFY(!error.isEmpty());
    // 错 scheme
    auto wrongScheme = params;
    unit = f.s1;
    unit.schemeId = QStringLiteral("no-such-scheme");
    wrongScheme.insert(QStringLiteral("unitSelection"), paleo::strat::toMap(unit));
    QVERIFY(!workflow.generateFactor(paleo::strat::analysisLayerKey(f.s1), QStringLiteral("sandthick"), wrongScheme, &error));
    QVERIFY(!error.isEmpty());
    // aggregate 冒充成图单元（正式成图按 S1–S4 运行）
    auto agg = params;
    paleo::strat::UnitSelection aggregate;
    aggregate.schemeId = f.schemeId;
    aggregate.revision = f.revision;
    aggregate.intervalId = f.intervalId;
    aggregate.aggregate = true;
    agg.insert(QStringLiteral("unitSelection"), paleo::strat::toMap(aggregate));
    QVERIFY(!workflow.generateFactor(QStringLiteral("agg-%1").arg(f.intervalId), QStringLiteral("sandthick"), agg, &error));
    QVERIFY(error.contains(QStringLiteral("S1–S4")) || !error.isEmpty());
    QCOMPARE(f.catalog.versions().size(), versionsBefore);
  }

  // W2：同 T70–T71（同顶界 TOP）的 S1/S2 各自成图——层键/版本归属/父版本不碰撞。
  void siblingSubIntervalsShareTopWithoutCollision()
  {
    SchemeFixture f;
    QVERIFY(f.init());
    f.declareWells(QStringLiteral("wells.main"), QStringLiteral("TOP"));
    ConstraintWorkflow workflow(&f.proc, &f.layers);
    workflow.setCatalog(&f.catalog, f.dir.path());
    QString error;
    const QString key1 = paleo::strat::analysisLayerKey(f.s1);
    const QString key2 = paleo::strat::analysisLayerKey(f.s2);
    QVERIFY(key1 != key2);

    QVERIFY(workflow.extractWellFactors(QStringLiteral("TOP"), QStringLiteral("sandthick"), runParams(f.s1), &error, nullptr));
    QString points1 = workflow.preparedWellFactorVersion(QStringLiteral("TOP"), QStringLiteral("sandthick"), runParams(f.s1));
    QVERIFY(!points1.isEmpty());
    QVERIFY(workflow.generateFactor(key1, QStringLiteral("sandthick"), runParams(f.s1), &error));

    QVERIFY(workflow.extractWellFactors(QStringLiteral("TOP"), QStringLiteral("sandthick"), runParams(f.s2), &error, nullptr));
    QString points2 = workflow.preparedWellFactorVersion(QStringLiteral("TOP"), QStringLiteral("sandthick"), runParams(f.s2));
    QVERIFY(!points2.isEmpty());
    QVERIFY(points1 != points2);
    QVERIFY(workflow.generateFactor(key2, QStringLiteral("sandthick"), runParams(f.s2), &error));

    // 各自的主栅格层键与版本互不碰撞，extra.horizon 指向各自稳定键。
    CatalogVersion v1, v2;
    for (const auto &v : f.catalog.versions()) {
      if (v.extra.value(QStringLiteral("manifest_layer_id")).toString() ==
          QStringLiteral("factor.%1.sandthick").arg(key1))
        v1 = v;
      if (v.extra.value(QStringLiteral("manifest_layer_id")).toString() ==
          QStringLiteral("factor.%2.sandthick").arg(key2))
        v2 = v;
    }
    QVERIFY(!v1.id.isEmpty() && !v2.id.isEmpty() && v1.id != v2.id);
    QCOMPARE(v1.extra.value(QStringLiteral("horizon")).toString(), key1);
    QCOMPARE(v2.extra.value(QStringLiteral("horizon")).toString(), key2);
    // 父版本各自指向自己单元的井点快照版本；绑定身份各自成立。
    QVERIFY(v1.parentVersionIds.contains(points1));
    QVERIFY(v2.parentVersionIds.contains(points2));
    QVERIFY(!v1.parentVersionIds.contains(points2));
    QVERIFY(!v2.parentVersionIds.contains(points1));
    QCOMPARE(paleo::strat::fromMap(v1.extra.value(QStringLiteral("unitSelection")).toMap()), f.s1);
    QCOMPARE(paleo::strat::fromMap(v2.extra.value(QStringLiteral("unitSelection")).toMap()), f.s2);
    // 样点层键不共用（S1/S2 同顶界 TOP 也不再碰撞）。
    QVERIFY(v1.extra.value(QStringLiteral("extraction")).toMap()
                .value(QStringLiteral("pointsLayerId")).toString() !=
            v2.extra.value(QStringLiteral("extraction")).toMap()
                .value(QStringLiteral("pointsLayerId")).toString());
    // 投影后各自获得指向真实单元的 single_factor 绑定。
    QVERIFY2(syncAnalysisResults(&f.catalog, f.dir.path()).isEmpty(), "projection must succeed");
    paleo::data::DataService ds;
    std::string detail;
    QVERIFY(ds.openProjectReadOnly(area_model::directory(f.dir.path()).toStdString(), detail));
    for (const auto &unit : {f.s1, f.s2}) {
      std::vector<paleo::data::SubIntervalResultBinding> bindings;
      QVERIFY(ds.listSubIntervalResults(f.schemeId.toStdString(), {unit.subIntervalId.toStdString()}, bindings, detail));
      const auto version = unit == f.s1 ? v1 : v2;
      QVERIFY(std::any_of(bindings.begin(), bindings.end(), [&](const auto &b) {
        return b.kind == "single_factor" && b.id == "catalog." + version.id.toStdString();
      }));
    }
  }

  // W3：调用方伪造 factorInputPrepared / parentVersionIds 不能绕过引擎核验。
  void forgedPreparedFlagAndParentsAreReverified()
  {
    SchemeFixture f;
    QVERIFY(f.init());
    f.declareWells(QStringLiteral("wells.main"), QStringLiteral("TOP"));
    ConstraintWorkflow workflow(&f.proc, &f.layers);
    workflow.setCatalog(&f.catalog, f.dir.path());
    QString error;
    // 伪造 prepared 标志：无真实提取快照时不得短路——要么真实提取，要么明确失败。
    auto forged = runParams(f.s1);
    forged.insert(QStringLiteral("factorInputPrepared"), true);
    const auto versionsBefore = f.catalog.versions().size();
    QVERIFY(workflow.generateFactor(paleo::strat::analysisLayerKey(f.s1), QStringLiteral("sandthick"), forged, &error));
    // 引擎自行完成提取：结果版本成为父版本，伪造标志没有省掉任何核验。
    CatalogVersion raster;
    for (const auto &v : f.catalog.versions())
      if (v.extra.value(QStringLiteral("manifest_layer_id")).toString() ==
          QStringLiteral("factor.%1.sandthick").arg(paleo::strat::analysisLayerKey(f.s1)))
        raster = v;
    QVERIFY(!raster.id.isEmpty());
    QVERIFY(!raster.parentVersionIds.isEmpty());
    // 伪造父版本 id：数值前拒绝，不进入发布血缘。
    auto withFakeParents = runParams(f.s1);
    withFakeParents.insert(QStringLiteral("parentVersionIds"),
                           QStringList{QStringLiteral("not-a-version")});
    QVERIFY(!workflow.generateFactor(paleo::strat::analysisLayerKey(f.s1), QStringLiteral("sandthick"), withFakeParents, &error));
    QVERIFY(error.contains(QStringLiteral("父版本")));
    QCOMPARE(f.catalog.versions().size(), versionsBefore + 1); // 只留第一次合法产物
  }

  // W2：显示名/资产重命名不改变稳定身份（token/extra.unitSelection 不受影响）。
  void displayNameDoesNotChangeIdentity()
  {
    SchemeFixture f;
    QVERIFY(f.init());
    auto renamed = f.s1;
    renamed.displayName = QStringLiteral("T7 混淆名 T70");
    QVERIFY(paleo::strat::analysisLayerKey(renamed) == paleo::strat::analysisLayerKey(f.s1));
    const auto map = paleo::strat::toMap(renamed);
    QCOMPARE(map.value(QStringLiteral("subIntervalId")).toString(), f.s1.subIntervalId);
    paleo::constraint_identity::FrozenAnalysisIdentity identity;
    QString error;
    QVERIFY(paleo::constraint_identity::resolveAnalysisIdentity(
        f.dir.path(), QStringLiteral("TOP"),
        QVariantMap{{QStringLiteral("unitSelection"), map}},
        paleo::constraint_identity::AggregatePolicy::Reject, identity, &error));
    QCOMPARE(identity.analysisLayerKey, paleo::strat::analysisLayerKey(f.s1));
    QCOMPARE(identity.constraintHorizon, QStringLiteral("TOP"));
  }

  // W1 兼容口径：无任何已提交方案的工程（无可选单元）保持 legacy 键路径。
  void legacyProjectWithoutSchemeKeepsLegacyKeys()
  {
    QTemporaryDir dir;
    DataCatalog catalog;
    QgisProjectService projectSvc;
    PaleoProjectStore store;
    LayerManifest manifest{dir.filePath(QStringLiteral("project.sqlite"))};
    QgisLayerService layers{&projectSvc, &manifest};
    QgisProcessingService proc{&store};
    QVERIFY(catalog.open(dir.path()) && projectSvc.createProject(dir.filePath(QStringLiteral("proj.qgz"))) &&
            manifest.open());
    QVERIFY(!paleo::constraint_identity::projectHasAnalysisScheme(dir.path()));
    paleo::constraint_identity::FrozenAnalysisIdentity identity;
    QString error;
    QVERIFY(paleo::constraint_identity::resolveAnalysisIdentity(
        dir.path(), QStringLiteral("T1"), {},
        paleo::constraint_identity::AggregatePolicy::Reject, identity, &error));
    QVERIFY(!identity.hasUnit);
    QCOMPARE(identity.analysisLayerKey, QStringLiteral("T1"));
    QCOMPARE(identity.constraintHorizon, QStringLiteral("T1"));
  }

  // W4：宿主 constraint.idw 冻结——单元核验、subinterval 匹配、引用解析为父版本。
  void hostConstraintIdwFreezeResolvesUnitAndParents()
  {
    SchemeFixture f;
    QVERIFY(f.init());
    // 受管井点版本（供 points_ref 解析）
    DerivedAssetRegistrar registrar(&f.catalog, f.dir.path());
    QString regError;
    const auto st = registrar.stage(QStringLiteral("well_factor_points"),
                                    QStringLiteral("井点"), QStringLiteral("wf.gpkg"), &regError);
    QVERIFY(st.isValid());
    {
      QFile file(st.absolutePath);
      QVERIFY(file.open(QIODevice::WriteOnly));
      file.write("stub");
      file.close();
    }
    DerivedFileFingerprint fp;
    fp.sha256 = QStringLiteral("ab");
    DerivedOutputSetReceipt receipt;
    QVariantMap extra;
    extra.insert(QStringLiteral("manifest_layer_id"), QStringLiteral("wellfactor.sample.sandthick"));
    QVERIFY(registrar.commit(st, {}, QStringLiteral(""), extra, &regError, &fp));

    paleo::constraint_identity::ConstraintIdwFrozenContext ctx;
    QString code = paleo::constraint_identity::freezeConstraintIdwContext(
        f.dir.path(), &f.catalog, f.s1, f.s1.subIntervalId,
        QStringLiteral("catalog:") + st.versionId, {}, ctx);
    QCOMPARE(code, QString());
    QCOMPARE(ctx.unit.subIntervalId, f.s1.subIntervalId);
    QCOMPARE(ctx.parentVersionIds, QStringList{st.versionId});

    // 引用解析失败（不存在的版本）→ 不臆造父版本，也不崩溃
    code = paleo::constraint_identity::freezeConstraintIdwContext(
        f.dir.path(), &f.catalog, f.s1, f.s1.subIntervalId,
        QStringLiteral("catalog:not-a-version"), {}, ctx);
    QCOMPARE(code, QString());
    QVERIFY(ctx.parentVersionIds.isEmpty());

    // 层键引用同样解析
    code = paleo::constraint_identity::freezeConstraintIdwContext(
        f.dir.path(), &f.catalog, f.s1, {}, QStringLiteral("wellfactor.sample.sandthick"), {}, ctx);
    QCOMPARE(code, QString());
    QCOMPARE(ctx.parentVersionIds, QStringList{st.versionId});

    // subinterval 不匹配 → 明确拒绝
    code = paleo::constraint_identity::freezeConstraintIdwContext(
        f.dir.path(), &f.catalog, f.s1, QStringLiteral("S9"), {}, {}, ctx);
    QCOMPARE(code, QStringLiteral("subinterval_mismatch"));

    // 无有效单元 → 拒绝
    code = paleo::constraint_identity::freezeConstraintIdwContext(
        f.dir.path(), &f.catalog, paleo::strat::UnitSelection{}, {}, {}, {}, ctx);
    QCOMPARE(code, QStringLiteral("constraint_idw_requires_unit"));

    // aggregate 冒充 → 拒绝（冻结实体核验）
    paleo::strat::UnitSelection agg;
    agg.schemeId = f.schemeId;
    agg.revision = f.revision;
    agg.intervalId = f.intervalId;
    agg.aggregate = true;
    code = paleo::constraint_identity::freezeConstraintIdwContext(
        f.dir.path(), &f.catalog, agg, {}, {}, {}, ctx);
    QCOMPARE(code, QStringLiteral("constraint_idw_unit_invalid"));
  }
};

int main(int argc, char *argv[])
{
  QgsApplication app(argc, argv, false);
  app.setPrefixPath(qEnvironmentVariable("QGIS_PREFIX_PATH"), true);
  app.initQgis();
  QgsApplication::processingRegistry();
  GDALAllRegister();
  P3FactorIdentityTest tc;
  QByteArray logPath = QByteArray(QT_TESTCASE_BUILDDIR) + "/tst_p3_factor_identity-result.txt";
  QList<QByteArray> forwarded;
  forwarded << QByteArray(argv[0]);
  for (int i = 1; i < argc; ++i)
    forwarded << QByteArray(argv[i]);
  forwarded << QByteArray("-o") << logPath + ",txt";
  QList<char *> cargv;
  cargv.reserve(forwarded.size());
  for (QByteArray &a : forwarded)
    cargv << a.data();
  const int rc = QTest::qExec(&tc, cargv.size(), cargv.data());
  QgsApplication::exitQgis();
  return rc;
}

#include "tst_p3_factor_identity.moc"
