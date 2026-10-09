// 层：测试壳
// tst_mkprojectfixture — mkproject 夹具工厂消费面（方向 78：manifest 参数化
// + 测试消费闭环）。共享一次 paleo_mkproject --manifest mini 全链产物，四个
// 断言面：
//   1) 导入全链：逐实体类型/角色断言 catalog 登记（真跑生产导入路径——io
//      解析 → catalog 登记 → 工程文件落盘，synthetic 夹具盖不到的面）；
//   2) 工程重开 round-trip：qgz+project.paleo 双件在，重开摘要一致；
//   3) 地理配准：georeference 节回读，控制点残差 ≤ 阈值（口径见方向 78
//      ledger D5：mini 生成器微扰 3.0m，断言 ≤5.0m 上限 + maxResidualM 精确值）；
//   4) 夹具可复用：两个连续实例（沙箱监狱下 QTemporaryDir/子进程可用性）。
#include "fixtures/mkprojectfixture.h"

#include "../src/catalog/datacatalog.h"
#include "../src/metadata/paleoprojectfile.h"
#include "../src/qgis/qgisprojectservice.h"
#include "../src/qgis/qgisruntime.h"

#include <QDir>
#include <QFile>
#include <QTemporaryDir>
#include <QTest>

#include <memory>

using MkProjectFixture::CatalogSummary;
using MkProjectFixture::RunResult;

namespace
{
// mini manifest 的确定性期望（生成器 tools/reference/mkproject/mini/
// generate.py 同源口径；计数改动 = 生成器与这里同步改）。
constexpr int kExpectedWells = 3;
constexpr int kExpectedImports = 17; // manifest imports 条数（= 资产数）
constexpr double kResidualThresholdM = 5.0;
constexpr double kExpectedMaxResidualM = 3.0; // 生成器 A3 微扰
} // namespace

class TstMkProjectFixture : public QObject
{
  Q_OBJECT
private slots:
  void initTestCase()
  {
    m_shared = std::make_unique<QTemporaryDir>();
    m_run = MkProjectFixture::buildMiniProject(m_shared->path());
  }

  void mkprojectRunsClean();
  void importFullChain();
  void projectRoundTrip();
  void georeferenceResiduals();
  void twoConsecutiveInstances();

private:
  std::unique_ptr<QTemporaryDir> m_shared;
  RunResult m_run;
};

void TstMkProjectFixture::mkprojectRunsClean()
{
  QVERIFY2(m_run.ran, qPrintable(m_run.errorText));
  // 退出码 = fails?1:0——0 即「全部通过」的机器口径（stdout 中文经
  // qPrintable=本地 8 位，跨 codepage 不做字节断言；FAIL 是 ASCII 字面）。
  QCOMPARE(m_run.exitCode, 0);
  QVERIFY2(!m_run.standardOutput.contains(QLatin1String("FAIL")),
           qPrintable(m_run.standardOutput));
}

void TstMkProjectFixture::importFullChain()
{
  QVERIFY2(m_run.ran && m_run.exitCode == 0,
           qPrintable(m_run.errorText.isEmpty() ? m_run.standardOutput
                                                : m_run.errorText));
  const CatalogSummary s =
      MkProjectFixture::openCatalogSummary(m_run.projectDir);
  QVERIFY2(s.opened, qPrintable(s.openError));
  QCOMPARE(s.wells, kExpectedWells);
  QCOMPARE(s.wellNames,
           QStringList({QStringLiteral("A1"), QStringLiteral("A2"),
                        QStringLiteral("A3")}));
  QCOMPARE(s.entitiesByType.value(QStringLiteral("well")), kExpectedWells);
  QCOMPARE(s.entitiesByType.value(QStringLiteral("seismic_survey")), 1);
  QCOMPARE(s.unresolvedLinks, 0);
  QCOMPARE(s.assets, kExpectedImports);

  // 逐井角色面（真跑 DataImportService 的井建齐 → 挂链接契约）。
  const auto wellRole = [&s](const QString &well, const QString &role) {
    return s.wellRoles.value(well).value(role);
  };
  for (const QString &w : s.wellNames)
  {
    QCOMPARE(wellRole(w, QStringLiteral("well_head")), 1);
    QCOMPARE(wellRole(w, QStringLiteral("tops")), 2); // 双分层源
    QCOMPARE(wellRole(w, QStringLiteral("cuttings")), 1);
  }
  QCOMPARE(wellRole(QStringLiteral("A1"), QStringLiteral("well_log")), 1);
  QCOMPARE(wellRole(QStringLiteral("A2"), QStringLiteral("well_log")), 1);
  QCOMPARE(wellRole(QStringLiteral("A3"), QStringLiteral("well_log")), 2); // 多文件井
  QCOMPARE(wellRole(QStringLiteral("A1"), QStringLiteral("core")), 1);
  QCOMPARE(wellRole(QStringLiteral("A2"), QStringLiteral("core")), 1);
  QCOMPARE(wellRole(QStringLiteral("A3"), QStringLiteral("core")), 1);
  QCOMPARE(wellRole(QStringLiteral("A3"), QStringLiteral("lab_analysis")), 1); // 薄片

  // 角色总面（含井域 reference 等）——well_log 4 份（3 井 + A3 双份）；
  // well_head 3 = 多井单文件契约（井位坐标.dat 一件挂 3 口井各一条）。
  QCOMPARE(s.linkRoles.value(QStringLiteral("well_log")), 4);
  QCOMPARE(s.linkRoles.value(QStringLiteral("tops")), kExpectedWells * 2);
  QCOMPARE(s.linkRoles.value(QStringLiteral("well_head")), kExpectedWells);
  QCOMPARE(s.linkRoles.value(QStringLiteral("core")), 3);
  QCOMPARE(s.linkRoles.value(QStringLiteral("cuttings")), 3);
  QCOMPARE(s.linkRoles.value(QStringLiteral("lab_analysis")), 1);

  // 版本面：每个资产一个受管 RAW 版本（外链 sgy/xml 记外链版本）。
  DataCatalog cat;
  QString err;
  QVERIFY2(cat.open(m_run.projectDir, &err), qPrintable(err));
  QCOMPARE(int(cat.versions().size()), kExpectedImports);
}

void TstMkProjectFixture::projectRoundTrip()
{
  QVERIFY2(m_run.ran && m_run.exitCode == 0,
           qPrintable(m_run.errorText.isEmpty() ? m_run.standardOutput
                                                : m_run.errorText));
  // 双件在盘。
  QVERIFY(QFile::exists(m_run.qgzPath));
  QVERIFY(QFile::exists(m_run.paleoPath));
  // project.paleo 直读：sourceArea 溯源 + sourceStats。
  bool ok = false;
  QString rerr;
  const PaleoProjectFile pf = readProjectFile(m_run.paleoPath, &ok, &rerr);
  QVERIFY2(ok, qPrintable(rerr));
  QCOMPARE(pf.sourceAreaRoot, QDir(MkProjectFixture::miniDir()).absolutePath());
  QCOMPARE(pf.sourceStats.value(QStringLiteral("files")).toInt(),
           kExpectedImports);
  QVERIFY(pf.georeference.has_value());

  // 重开（生产 openProject 面）+ georeference 解析。
  QgisProjectService reopen;
  QVERIFY2(reopen.openProject(m_run.paleoPath),
           qPrintable(reopen.lastErrors().join(QLatin1Char(';'))));
  QVERIFY(reopen.georeference().has_value());
  QCOMPARE(reopen.georeference()->maxResidualM, kExpectedMaxResidualM);

  // catalog 摘要与导入侧一致（重开无漂移：另一个 DataCatalog 实例同口径）。
  const CatalogSummary s2 =
      MkProjectFixture::openCatalogSummary(m_run.projectDir);
  QVERIFY2(s2.opened, qPrintable(s2.openError));
  QCOMPARE(s2.wells, kExpectedWells);
  QCOMPARE(s2.entitiesByType.value(QStringLiteral("seismic_survey")), 1);
  QCOMPARE(s2.unresolvedLinks, 0);
  QCOMPARE(s2.linkRoles.value(QStringLiteral("well_log")), 4);
}

void TstMkProjectFixture::georeferenceResiduals()
{
  QVERIFY2(m_run.ran && m_run.exitCode == 0,
           qPrintable(m_run.errorText.isEmpty() ? m_run.standardOutput
                                                : m_run.errorText));
  bool ok = false;
  QString rerr;
  const PaleoProjectFile pf = readProjectFile(m_run.paleoPath, &ok, &rerr);
  QVERIFY2(ok, qPrintable(rerr));
  QVERIFY(pf.georeference.has_value());
  const PaleoGeoreference &g = *pf.georeference;
  QCOMPARE(g.targetCrs, QStringLiteral("EPSG:4326"));
  QCOMPARE(int(g.controlPoints.size()), kExpectedWells);
  // 断言口径（ledger D5）：逐点 residualM ≤ 5.0m + maxResidualM == 3.0
  //（生成器 A3 纬度微扰 3.0m——残差非零才有物理意义）。
  for (const auto &cp : g.controlPoints)
    QVERIFY2(cp.residualM <= kResidualThresholdM,
             qPrintable(QStringLiteral("%1 残差 %2").arg(cp.well).arg(cp.residualM)));
  QCOMPARE(g.maxResidualM, kExpectedMaxResidualM);

  // 配准应用面：井 coordinateStatus=ok（well_head 导入时相似变换生效）。
  const CatalogSummary s =
      MkProjectFixture::openCatalogSummary(m_run.projectDir);
  QVERIFY2(s.opened, qPrintable(s.openError));
  for (const QString &w : s.wellNames)
    QCOMPARE(s.wellCoordinateStatus.value(w), QStringLiteral("ok"));
}

void TstMkProjectFixture::twoConsecutiveInstances()
{
  // 沙箱监狱口径（方向 72 后 TEMP 树内化）：QTemporaryDir + QProcess 子进程
  // 连续两个实例都成功——夹具可复用性（Oracle 2）。
  QTemporaryDir second;
  QVERIFY(second.isValid());
  const RunResult r2 = MkProjectFixture::buildMiniProject(second.path());
  QVERIFY2(r2.ran, qPrintable(r2.errorText));
  QCOMPARE(r2.exitCode, 0);
  QVERIFY(QFile::exists(r2.paleoPath));
  const CatalogSummary s2 = MkProjectFixture::openCatalogSummary(second.path());
  QVERIFY2(s2.opened, qPrintable(s2.openError));
  QCOMPARE(s2.wells, kExpectedWells);
}

// QGIS 初始化必须在任何 QCoreApplication 之前（QgisRuntime 契约）——
// 自定义 main 对齐 tst_projectsvc 先例；prefix 由 QGIS_PREFIX_PATH env
//（沙箱注入）优先，"/usr" 仅兜底。
int main(int argc, char *argv[])
{
  qputenv("QT_QPA_PLATFORM", "offscreen");
  if (!QgisRuntime::initialize(QStringLiteral("/usr")))
    qFatal("QgisRuntime::initialize failed");
  TstMkProjectFixture tc;
  const int rc = QTest::qExec(&tc, argc, argv);
  QgisRuntime::shutdown();
  return rc;
}
#include "tst_mkprojectfixture.moc"
