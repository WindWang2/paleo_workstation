// 层：测试壳
#include <QtTest>
#include <QDir>
#include <QFile>
#include <QFileInfo>
#include <QTemporaryDir>

#include "catalog/datacatalog.h"
#include "io/dataimportservice.h"
#include "metadata/paleoprojectstore.h"
#include "services/projectdata.h"
#include "workflow/folderimport.h"
#include "workflow/importledger.h"
#include "workflow/sectionworkbench.h"

namespace
{
QString fixture(const QString &name)
{
  return QFileInfo(QString::fromUtf8(__FILE__)).dir().filePath(
      QStringLiteral("fixtures/io_robustness/") + name);
}
bool write(const QString &path, const QByteArray &bytes)
{
  QDir().mkpath(QFileInfo(path).absolutePath());
  QFile f(path);
  return f.open(QIODevice::WriteOnly) && f.write(bytes) == bytes.size();
}
}

class IoImportLedgerTests : public QObject
{
  Q_OBJECT
private slots:
  void rejectedRowsReachLedgerAndAcceptedTops();
  void allInvalidRowsAreFailureWithReasons();
  void unitFilesReachSectionWorkbench_data();
  void unitFilesReachSectionWorkbench();
};

void IoImportLedgerTests::rejectedRowsReachLedgerAndAcceptedTops()
{
  QTemporaryDir dir;
  QVERIFY(dir.isValid());
  const QString src = dir.filePath(QStringLiteral("src"));
  QVERIFY(write(QDir(src).filePath(QStringLiteral("ExportWellHead.dat")), "W1 10 20 0 500\n"));
  QVERIFY(QDir().mkpath(QDir(src).filePath(QStringLiteral("井分层"))));
  const QString topsPath = QDir(src).filePath(QStringLiteral("井分层/DC.dat"));
  QVERIFY(QFile::copy(fixture(QStringLiteral("tops_nulls.tsv")), topsPath));
  PaleoProjectStore store;
  DataImportService importer(&store);
  const QString project = dir.filePath(QStringLiteral("project"));
  importer.setProjectDir(project);
  QVERIFY2(importer.catalogWritable(), qPrintable(importer.catalogOpenError()));
  FolderImportWorkflow workflow(&importer, nullptr);
  QVector<FolderRowResult> rows;
  QString error;
  workflow.importFolder(src, {}, [&](const auto &r, const QString &e) { rows = r; error = e; });
  QVERIFY2(error.isEmpty(), qPrintable(error));
  QCOMPARE(rows.size(), 2);
  const FolderRowResult *tops = nullptr;
  for (const auto &row : rows)
    if (row.path == topsPath) tops = &row;
  QVERIFY(tops);
  QCOMPARE(tops->outcome, FolderRowResult::Outcome::Imported);
  QVERIFY2(tops->message.contains(QStringLiteral("拒收 6 行")), qPrintable(tops->message));
  QVERIFY(tops->message.contains(QStringLiteral("哨兵 3")));
  QVERIFY(tops->message.contains(QStringLiteral("第 5 行")));
  QVERIFY(tops->message.contains(QStringLiteral("MD")));
  paleo::imports::ImportLedger ledger;
  ledger.load(importer.catalog());
  QCOMPARE(ledger.count(), 1);
  bool found = false;
  for (const auto &row : ledger.batches()[0].rows)
    if (row.path == topsPath)
    {
      QCOMPARE(row.message, tops->message);
      found = true;
    }
  QVERIFY(found);
  const auto matches = importer.catalog()->wellsMatchingName(QStringLiteral("W1"));
  QCOMPARE(matches.size(), 1);
  ProjectDataFacade data;
  data.setCatalog(importer.catalog(), project);
  const auto accepted = data.topsFor(matches[0]);
  QCOMPARE(accepted.size(), 1);
  QCOMPARE(accepted[0].horizon, QStringLiteral("GOOD"));
  QCOMPARE(accepted[0].md, 100.0);
  const auto versions = importer.catalog()->versionsForAsset(importer.assets(QStringLiteral("well_stratification")).value(0));
  QVERIFY(!versions.isEmpty());
  const auto report = versions[0].extra.value(QStringLiteral("wellParseReport")).toMap();
  QCOMPARE(report.value(QStringLiteral("sentinelHits")).toInt(), 3);
  // 同字节重导入，报告仍回到本次导入台账。
  rows.clear();
  workflow.importFolder(src, {}, [&](const auto &r, const QString &e) { rows = r; error = e; });
  QVERIFY2(error.isEmpty(), qPrintable(error));
  bool reportedAgain = false;
  for (const auto &row : rows)
    if (row.path == topsPath) reportedAgain = row.message.contains(QStringLiteral("哨兵 3"));
  QVERIFY(reportedAgain);
  rows.clear();
  workflow.importFolder(src, {}, {topsPath}, [&](const auto &r, const QString &e) { rows = r; error = e; });
  QVERIFY2(error.isEmpty(), qPrintable(error));
  reportedAgain = false;
  for (const auto &row : rows)
    if (row.path == topsPath) reportedAgain = row.message.contains(QStringLiteral("哨兵 3"));
  QVERIFY(reportedAgain); // 显式重导入经当前字节重新解析，仍有本次报告。
}

void IoImportLedgerTests::allInvalidRowsAreFailureWithReasons()
{
  QTemporaryDir dir;
  QVERIFY(dir.isValid());
  const QString src = dir.filePath(QStringLiteral("src/井分层/DC.dat"));
  QVERIFY(write(src, "W1 D61 -999.25\nW1 D62 -99999\nW1 D63\n"));
  PaleoProjectStore store;
  DataImportService importer(&store);
  importer.setProjectDir(dir.filePath(QStringLiteral("project")));
  QVERIFY(importer.catalogWritable());
  FolderImportWorkflow workflow(&importer, nullptr);
  QVector<FolderRowResult> rows;
  workflow.importFolder(dir.filePath(QStringLiteral("src")), {}, [&](const auto &r, const QString &) { rows = r; });
  QCOMPARE(rows.size(), 1);
  QCOMPARE(rows[0].outcome, FolderRowResult::Outcome::Failed);
  QVERIFY2(rows[0].message.contains(QStringLiteral("拒收 3 行")), qPrintable(rows[0].message));
  QVERIFY(rows[0].message.contains(QStringLiteral("哨兵 2")));
  QVERIFY(rows[0].message.contains(QStringLiteral("MD")));
  paleo::imports::ImportLedger ledger;
  ledger.load(importer.catalog());
  QCOMPARE(ledger.count(), 1);
  QCOMPARE(ledger.batches()[0].failed, 1);
  QCOMPARE(ledger.batches()[0].rows[0].message, rows[0].message);
  QVERIFY(importer.catalog()->assets().isEmpty());
}

void IoImportLedgerTests::unitFilesReachSectionWorkbench_data()
{
  QTest::addColumn<QString>("file");
  QTest::addColumn<double>("scale");
  for (const char *name : {"m_upper", "meter", "meters", "m_lower", "metre"})
    QTest::newRow(name) << QString::fromLatin1(name) << 1.0;
  for (const char *name : {"ft", "feet"})
    QTest::newRow(name) << QString::fromLatin1(name) << 0.3048;
  QTest::newRow("unknown") << QStringLiteral("unknown") << -1.0;
}

void IoImportLedgerTests::unitFilesReachSectionWorkbench()
{
  QFETCH(QString, file);
  QFETCH(double, scale);
  QTemporaryDir dir;
  QVERIFY(dir.isValid());
  DataCatalog cat;
  QVERIFY(cat.open(dir.path()));
  CatalogEntity well;
  well.id = QStringLiteral("well-W1");
  well.entityType = QStringLiteral("well");
  well.name = QStringLiteral("W1");
  well.hasSurface = true;
  well.coordinateStatus = QStringLiteral("untransformed");
  well.surfaceX = 10; well.surfaceY = 20;
  QVERIFY(cat.addEntity(well));
  CatalogAsset asset;
  asset.id = QStringLiteral("log"); asset.type = QStringLiteral("well_log");
  asset.format = QStringLiteral("las");
  QVERIFY(cat.addAsset(asset));
  CatalogVersion version;
  version.id = QStringLiteral("version"); version.assetId = asset.id;
  version.stage = QStringLiteral("RAW"); version.managed = false;
  version.path = fixture(QStringLiteral("unit_") + file + QStringLiteral(".las"));
  QVERIFY(cat.addVersion(version));
  EntityAssetLink link;
  link.entityType = QStringLiteral("well"); link.entityId = well.id;
  link.assetId = asset.id; link.role = QStringLiteral("well_log"); link.isPrimary = true;
  QVERIFY(cat.addLink(link));
  SectionWorkbench bench(&cat);
  QString error;
  QVERIFY2(bench.setCalibration(well.id, true, 2000, 0, &error), qPrintable(error));
  const auto wells = bench.sectionWells();
  QCOMPARE(wells.size(), size_t(1));
  if (scale < 0)
  {
    QVERIFY(wells[0].curves.empty());
    QVERIFY(wells[0].alignmentStatus.contains(QStringLiteral("深度单位未知")));
    QVERIFY(wells[0].alignmentStatus.contains(QStringLiteral("UNKNOWN")));
  }
  else
  {
    QCOMPARE(wells[0].curves.size(), size_t(1));
    const auto &curve = wells[0].curves[0];
    QCOMPARE(curve.depthsM.size(), size_t(2));
    QCOMPARE(curve.depthsM[0], 100 * scale);
    QCOMPARE(curve.depthsM[1], 200 * scale);
    QCOMPARE(curve.values[0], 10.0f);
    QCOMPARE(curve.values[1], 20.0f);
    QCOMPARE(curve.twtMs[0], 100 * scale);
  }
}

QTEST_GUILESS_MAIN(IoImportLedgerTests)
#include "tst_io_importledger.moc"
