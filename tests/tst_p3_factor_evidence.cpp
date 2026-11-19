// 层：测试壳
// P3/W5–W7：证据确认完成状态、失败注入、定向恢复与后台重复投影幂等。
// 验收矩阵：Catalog 成功而 DS 失败 → 表达已提交但证据未完成（持久 pending），
// 旧成果可读、受管文件不被误删；重试/重复 sync → 绑定按稳定 id 幂等，
// 无影子事实源；agent_result 只精确识别 Host 核验的 constraint.idw 产物。
#include <QtTest>
#include "catalog/datacatalog.h"
#include "io/areamodel.h"
#include "paleo_data/service.h"
#include "services/subintervalfactors.h"
#include "workflow/analysisresultsync.h"
#include "workflow/workflows.h"
#include "services/stratigraphicsubintervals.h"
#include "workflow/factorevidenceconfirmation.h"
#include "workflow/derivedassets.h"

#include <QCryptographicHash>
#include <QFile>
#include <QTemporaryDir>
#include <cmath>

using namespace paleo;

namespace {
struct EvidenceFixture
{
  QTemporaryDir dir;
  DataCatalog catalog;
  paleo::strat::UnitSelection unit;
  QString schemeId;

  bool init()
  {
    if (!dir.isValid() || !catalog.open(dir.path()))
      return false;
    std::string detail;
    paleo::data::DataService ds;
    const auto root = area_model::directory(dir.path()).toStdString();
    if (!ds.createProject(root, "evidence-test", detail))
      return false;
    paleo::data::ParsedFile parsed;
    parsed.kind = "test";
    parsed.status = "parsed";
    paleo::data::WellDataset well;
    well.wellInfo.wellName = "W-1";
    well.wellInfo.wellheadX = 0;
    well.wellInfo.wellheadY = 0;
    well.defaultDepthInfo.depthType = "MD";
    well.defaultDepthInfo.depthUnit = "m";
    parsed.wells.push_back(well);
    const auto sha = QCryptographicHash::hash(QByteArray("evidence"), QCryptographicHash::Sha256).toHex().toStdString();
    long long sourceId = 0;
    if (!ds.importPreparedFile("x", "x", "x", sha, parsed, sourceId, detail))
      return false;
    std::vector<paleo::data::WellSummary> wells;
    if (!ds.listWells(wells, detail) || wells.empty())
      return false;
    paleo::data::StratigraphicScheme scheme;
    std::vector<paleo::data::FrameworkIssue> issues;
    auto draft = paleo::strat::draftScheme(
        "scheme", "scheme", {"TOP", "BASE"}, {{wells.front().id, {{"TOP", 100.}, {"BASE", 200.}}}});
    if (!ds.commitStratigraphicScheme(draft, {}, scheme, issues, detail))
      return false;
    unit = paleo::strat::makeSelection(scheme, QString::fromStdString(scheme.intervals.front().id.value), 1);
    schemeId = unit.schemeId;
    return unit.isValid();
  }

  // 提交一个携带冻结单元身份的单因素栅格版本（模拟正式发布产物）。
  QString commitFactorVersion(const QString &assetType, const QVariantMap &extra)
  {
    DerivedAssetRegistrar registrar(&catalog, dir.path());
    QString error;
    const auto st = registrar.stage(assetType, QStringLiteral("f"), QStringLiteral("f.tif"), &error);
    if (!st.isValid())
      return {};
    {
      QFile file(st.absolutePath);
      if (!file.open(QIODevice::WriteOnly))
        return {};
      file.write("factor-raster");
      file.close();
    }
    DerivedFileFingerprint fp;
    fp.sha256 = QStringLiteral("aa");
    DerivedOutputSetReceipt receipt;
    if (!registrar.commit(st, {}, QStringLiteral(""), extra, &error, &fp))
      return {};
    return st.versionId;
  }

  int bindingCount(const QString &versionId)
  {
    paleo::data::DataService ds;
    std::string detail;
    if (!ds.openProjectReadOnly(area_model::directory(dir.path()).toStdString(), detail))
      return -1;
    std::vector<paleo::data::SubIntervalResultBinding> bindings;
    if (!ds.listSubIntervalResults(schemeId.toStdString(), {}, bindings, detail))
      return -1;
    const auto id = ("catalog." + versionId).toStdString();
    return static_cast<int>(std::count_if(bindings.begin(), bindings.end(),
                                          [&](const auto &b) { return b.id == id; }));
  }
};
} // namespace

class P3FactorEvidenceTest : public QObject
{
  Q_OBJECT
private slots:
  // W6：提交成功 + DS 失败 → 「已提交但证据未确认」：持久 pending、精确原因、
  // 旧成果可读；恢复后重试收敛且不重复。
  void catalogCommitWithDsFailureKeepsRecoverablePendingState()
  {
    EvidenceFixture f;
    QVERIFY(f.init());
    QVariantMap extra;
    extra.insert(QStringLiteral("unitSelection"), paleo::strat::toMap(f.unit));
    extra.insert(QStringLiteral("horizon"), paleo::strat::analysisLayerKey(f.unit));
    extra.insert(QStringLiteral("manifest_layer_id"),
                 QStringLiteral("factor.%1.sandthick").arg(paleo::strat::analysisLayerKey(f.unit)));
    const QString versionId = f.commitFactorVersion(QStringLiteral("single_factor_raster"), extra);
    QVERIFY(!versionId.isEmpty());

    // 注入 DS 故障：挪走 area model 的 data.sqlite。
    const auto sqlite = area_model::directory(f.dir.path()) + QStringLiteral("/data.sqlite");
    const auto parked = f.dir.filePath(QStringLiteral("data.sqlite.parked"));
    QVERIFY(QFile::rename(sqlite, parked));

    const auto pending = paleo::factor_evidence::confirmVersionEvidence(&f.catalog, f.dir.path(), versionId, f.unit);
    QVERIFY(pending.catalogCommitted);
    QVERIFY(!pending.evidenceConfirmed);
    QVERIFY(!pending.pendingReason.isEmpty());
    QCOMPARE(pending.bindingId, QStringLiteral("catalog.") + versionId);
    // 未完成步骤沿既有扩展记录接缝持久化（版本仍可读、受管文件不动）。
    const auto marked = f.catalog.versionById(versionId);
    QCOMPARE(marked.id, versionId);
    QVERIFY(marked.extra.contains(QStringLiteral("evidence_pending")));
    QVERIFY(marked.extra.value(QStringLiteral("evidence_pending")).toMap()
                .value(QStringLiteral("step")).toString() == QStringLiteral("ds_binding"));
    QVERIFY(QFile::exists(f.dir.path() + QLatin1Char('/') + marked.path));

    // 恢复 DS → 定向重试只补确认：绑定出现、标记清除、版本数不变。
    QVERIFY(QFile::rename(parked, sqlite));
    QString error;
    QCOMPARE(paleo::factor_evidence::retryPendingEvidence(&f.catalog, f.dir.path(), &error), 0);
    QCOMPARE(f.bindingCount(versionId), 1);
    QVERIFY(!f.catalog.versionById(versionId).extra.contains(QStringLiteral("evidence_pending")));

    // 重复重试与后台重复投影：绑定数量稳定，无影子事实源。
    QCOMPARE(paleo::factor_evidence::retryPendingEvidence(&f.catalog, f.dir.path(), &error), 0);
    QVERIFY(syncAnalysisResults(&f.catalog, f.dir.path()).isEmpty());
    QVERIFY(syncAnalysisResults(&f.catalog, f.dir.path()).isEmpty());
    QCOMPARE(f.bindingCount(versionId), 1);
    QCOMPARE(f.catalog.versions().size(), 1);
  }

  // W6：部分确认——一个可确认版本 + 一个无单元历史版本（只读兼容，不虚构身份）。
  void partialConfirmationLeavesLegacyVersionsPendingWithoutForgingIdentity()
  {
    EvidenceFixture f;
    QVERIFY(f.init());
    QVariantMap withUnit;
    withUnit.insert(QStringLiteral("unitSelection"), paleo::strat::toMap(f.unit));
    const QString v1 = f.commitFactorVersion(QStringLiteral("single_factor_raster"), withUnit);
    // 无单元的历史产物（legacy）：先人为标 pending（模拟历史失败），重试不得补登记。
    QVariantMap legacy;
    legacy.insert(QStringLiteral("horizon"), QStringLiteral("T70"));
    const QString v2 = f.commitFactorVersion(QStringLiteral("single_factor_raster"), legacy);
    QVERIFY(!v1.isEmpty() && !v2.isEmpty());
    QString error;
    QVERIFY(f.catalog.updateVersionExtra(v2, QStringLiteral("evidence_pending"),
                                         QVariantMap{{QStringLiteral("step"), QStringLiteral("ds_binding")}}, &error));

    QCOMPARE(paleo::factor_evidence::retryPendingEvidence(&f.catalog, f.dir.path(), &error), 1);
    QCOMPARE(f.bindingCount(v1), 1);      // 可确认的已确认
    QCOMPARE(f.bindingCount(v2), 0);      // 无单元不产生绑定（不虚构身份）
    QVERIFY(!f.catalog.versionById(v1).extra.contains(QStringLiteral("evidence_pending")));
    QVERIFY(f.catalog.versionById(v2).extra.contains(QStringLiteral("evidence_pending"))); // 如实保留
  }

  // W5：agent_result 只精确识别 Host 核验的 constraint.idw 产物。
  void agentResultRecognitionRequiresHostVerifiedIdentity()
  {
    EvidenceFixture f;
    QVERIFY(f.init());
    // 带宿主核验标记 + 冻结单元 + 受信算法契约 → single_factor 绑定。
    QVariantMap verified;
    verified.insert(QStringLiteral("subinterval_verified"), true);
    verified.insert(QStringLiteral("algorithm_id"), QStringLiteral("paleo:paleo_constraint_idw"));
    verified.insert(QStringLiteral("unitSelection"), paleo::strat::toMap(f.unit));
    verified.insert(QStringLiteral("subIntervalId"), f.unit.subIntervalId);
    verified.insert(QStringLiteral("kind"), QStringLiteral("single_factor_raster"));
    const QString v1 = f.commitFactorVersion(QStringLiteral("agent_result"), verified);
    // 无核验标记的普通 agent_result → 不冒充 single_factor。
    QVariantMap unverified;
    unverified.insert(QStringLiteral("kind"), QStringLiteral("single_factor_raster"));
    unverified.insert(QStringLiteral("unitSelection"), paleo::strat::toMap(f.unit));
    const QString v2 = f.commitFactorVersion(QStringLiteral("agent_result"), unverified);
    QVERIFY(!v1.isEmpty() && !v2.isEmpty());

    QVERIFY(syncAnalysisResults(&f.catalog, f.dir.path()).isEmpty());
    QCOMPARE(f.bindingCount(v1), 1);
    QCOMPARE(f.bindingCount(v2), 0);
  }

  // W7：workflow 入口的定向恢复（现有任务入口；不重算、不新建版本）。
  void workflowRetryEntryIsIdempotent()
  {
    EvidenceFixture f;
    QVERIFY(f.init());
    QVariantMap extra;
    extra.insert(QStringLiteral("unitSelection"), paleo::strat::toMap(f.unit));
    const QString versionId = f.commitFactorVersion(QStringLiteral("single_factor_raster"), extra);
    QVERIFY(!versionId.isEmpty());
    ConstraintWorkflow workflow(nullptr, nullptr);
    workflow.setCatalog(&f.catalog, f.dir.path());
    QString error;
    QCOMPARE(workflow.retryPendingFactorEvidence(&error), 0);
    const auto versions = f.catalog.versions().size();
    QCOMPARE(workflow.retryPendingFactorEvidence(&error), 0);
    QCOMPARE(f.catalog.versions().size(), versions);
    QCOMPARE(f.bindingCount(versionId), 1);
  }
};

QTEST_GUILESS_MAIN(P3FactorEvidenceTest)
#include "tst_p3_factor_evidence.moc"
