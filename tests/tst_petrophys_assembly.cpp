// 层：数据（测试壳位于 tests/，被测对象为 ui 装配缝 + services 任务面）
// #276 装配缝回归：连井面板井集是资产 id（ast-N，refreshCorrelationWells 从
// catalog well_log 资产灌入），编排须映射成井实体 id（well-N）再
// resolveWellLas，成果曲线按映射键并回同一行井。修前每次计算恒报
// 「井实体不存在」，整条测井计算链在生产中不可达。本套件经真实
// PaleoMainWindow + attachWorkflows + PaleoTaskService 驱动端到端形状。
#include <QtTest>
#include <QApplication>
#include <QDir>
#include <QElapsedTimer>
#include <QFile>
#include <QFileInfo>
#include <QLabel>
#include <QTemporaryDir>
#include <QTextStream>

#include <functional>

#include "../src/catalog/datacatalog.h"
#include "../src/io/dataimportservice.h"
#include "../src/linkage/selectioncontext.h"
#include "../src/services/paleotaskservice.h"
#include "../src/services/petrophyscomputeservice.h"
#include "../src/ui/correlation/curvebrowser.h"
#include "../src/ui/correlation/petrophyspanel.h"
#include "../src/ui/correlationpanel.h"
#include "../src/ui/paleomainwindow.h"

using paleo::petrophys::PetroPhysTaskService;
using Formula = PetroPhysTaskService::Formula;

class TestPetrophysAssembly : public QObject
{
  Q_OBJECT

private:
  // 手工 LAS（DEPT + GR，tst_correlation 同格式；GR 线性上升供 VSH 断言）。
  static bool writeGrLas(const QString &path)
  {
    QFile f(path);
    if (!f.open(QIODevice::WriteOnly | QIODevice::Text))
      return false;
    QTextStream ts(&f);
    ts << "~VERSION INFORMATION\n"
          "VERS.  2.0   : CWLS LOG ASCII STANDARD - VERSION 2.0\n"
          "WRAP.  NO    : ONE LINE PER DEPTH STEP\n"
          "~WELL INFORMATION\n"
          "STRT.M 1000.0 : START DEPTH\n"
          "STOP.M 1004.0 : STOP DEPTH\n"
          "STEP.M 1.0    : STEP\n"
          "NULL.  -999.25 : NULL VALUE\n"
          "~CURVE INFORMATION\n"
          "DEPT.M   : DEPTH\n"
          "GR.GAPI  : GAMMA RAY\n"
          "~A  DEPTH       GR\n"
          "1000 20.0\n"
          "1001 28.0\n"
          "1002 36.0\n"
          "1003 44.0\n"
          "1004 52.0\n";
    return true;
  }

  static bool waitFor(const std::function<bool()> &pred, int timeoutMs = 60000)
  {
    QElapsedTimer clock;
    clock.start();
    while (!pred())
    {
      if (clock.elapsed() > timeoutMs)
        return false;
      QTest::qWait(10);
    }
    return true;
  }

private slots:

  // #276：面板资产 id（ast-N）→ 实体 id 映射 → resolveWellLas → 成果曲线
  // 并回同一行井（面板键 = 资产 id）。
  void petrophysComputeMergesCurvesBackToAssetRow()
  {
    QTemporaryDir dir;
    QVERIFY(dir.isValid());
    DataImportService svc(nullptr, nullptr);
    svc.setProjectDir(dir.path());
    DataCatalog *cat = svc.catalog();
    QVERIFY(cat && cat->isOpen());

    const QString lasPath = QDir(dir.path()).filePath(QStringLiteral("r1.las"));
    QVERIFY(writeGrLas(lasPath));

    QString err;
    CatalogEntity e;
    e.id = QStringLiteral("well-R1");
    e.entityType = QStringLiteral("well");
    e.name = QStringLiteral("R1");
    QVERIFY(cat->addEntity(e, &err));
    const QString assetId = cat->nextAssetId();
    QVERIFY(assetId.startsWith(QStringLiteral("ast-")));
    CatalogAsset a;
    a.id = assetId;
    a.type = QStringLiteral("well_log");
    a.format = QStringLiteral("las");
    a.displayName = QStringLiteral("r1.las");
    QVERIFY(cat->addAsset(a, &err));
    CatalogVersion v;
    v.id = cat->nextVersionId();
    v.assetId = assetId;
    v.stage = QStringLiteral("RAW");
    v.versionNumber = 1;
    v.managed = false;
    v.path = QFileInfo(lasPath).absoluteFilePath();
    v.fileName = QStringLiteral("r1.las");
    QVERIFY(cat->addVersion(v, &err));
    EntityAssetLink l;
    l.entityType = QStringLiteral("well");
    l.entityId = e.id;
    l.assetId = assetId;
    l.role = QStringLiteral("well_log");
    l.isPrimary = true;
    QVERIFY(cat->addLink(l, &err));

    PaleoTaskService taskSvc;
    SelectionContext selection;
    PaleoMainWindow win(nullptr, nullptr, nullptr, nullptr, &selection);
    win.attachWorkflows(nullptr, nullptr, nullptr, nullptr, &svc, nullptr, nullptr, nullptr,
                        nullptr, nullptr, &taskSvc);

    // 生产路径：井集由 refreshCorrelationWells 从 catalog 的 well_log 资产灌入
    //（默认不同步加载 LAS，列占位即可承载计算曲线回填）。
    win.refreshCorrelationWells();
    auto *corrPanel = win.findChild<WellCorrelationPanel *>(QStringLiteral("correlationPanel"));
    auto *petroPanel =
        win.findChild<paleo::petrophys::PetroPhysPanel *>(QStringLiteral("petrophysPanel"));
    QVERIFY(corrPanel && petroPanel);
    QCOMPARE(corrPanel->wellCount(), 1);
    QCOMPARE(corrPanel->wellAt(0), assetId);

    PetroPhysTaskService::BatchRequest req;
    req.formula = Formula::VshGrLinear;
    req.params.grAutoBaseline = false;
    req.params.grMin = 20.0;
    req.params.grMax = 100.0;
    req.outputMnemonic = QStringLiteral("VSH");
    req.outputUnit = QStringLiteral("v/v");
    req.writeProduct = true;
    emit petroPanel->computeRequested(req);

    auto *browser = corrPanel->findChild<CurveBrowser *>();
    QVERIFY(browser);
    QVERIFY2(waitFor([&] { return browser->mnemonics().contains(QStringLiteral("VSH")); }),
             "计算曲线未并入面板行（超时）");

    // 曲线并回同一行井——面板键是资产 id，不是井实体 id。
    QCOMPARE(browser->wellId(), assetId);
    const QString summary =
        petroPanel->findChild<QLabel *>(QStringLiteral("petrophysStatus"))->text();
    QVERIFY2(!summary.contains(QStringLiteral("井实体不存在")), qPrintable(summary));
    QVERIFY2(summary.contains(QStringLiteral("1/1")), qPrintable(summary));

    // 产物落盘 + catalog DERIVED 登记（资产 id 按公式+助记符+井实体命名）。
    const QString derivedAssetId = QStringLiteral("petrophys_vsh_lin_vsh_well-R1");
    QVERIFY(!cat->assetById(derivedAssetId).id.isEmpty());
    bool derived = false;
    for (const CatalogVersion &dv : cat->versionsForAsset(derivedAssetId))
      if (dv.stage == QStringLiteral("DERIVED") && !dv.sha256.isEmpty() &&
          !dv.parentVersionIds.isEmpty())
        derived = true;
    QVERIFY2(derived, "测井产物应登记 DERIVED 版本（父版本 = 源 RAW 版本）");
  }
};

int main(int argc, char *argv[])
{
  if (qgetenv("QT_QPA_PLATFORM").isEmpty())
    qputenv("QT_QPA_PLATFORM", "offscreen");
  QApplication app(argc, argv);
  TestPetrophysAssembly tc;
  return QTest::qExec(&tc, argc, argv);
}

#include "tst_petrophys_assembly.moc"
