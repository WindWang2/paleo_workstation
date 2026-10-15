// 层：测试壳
#include <QtTest>
#include <QDir>
#include <QFile>
#include <QFileInfo>
#include <QCryptographicHash>
#include <QElapsedTimer>
#include <QTemporaryDir>
#include <QTimeZone>

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
  void corruptLedgerPreservedBeforeOverwrite();
  void corruptLedgerUnbackupableRefusesWrite();
  void ledgerScaleRoundTrip();
  void unitFilesReachSectionWorkbench_data();
  void unitFilesReachSectionWorkbench();
};

namespace
{
// 确定性大批次（opt-01 P2）：固定 id/时间戳，A/B 两套实现落盘字节可直接比对。
paleo::imports::LedgerBatch syntheticBatch(int n, int seq)
{
  paleo::imports::LedgerBatch b;
  b.id = QStringLiteral("batch-%1").arg(1700000000000LL + seq);
  b.dir = QStringLiteral("/data/工区/批次%1").arg(seq);
  b.startedAt = QDateTime(QDate(2026, 10, 16), QTime(8, 0), QTimeZone::UTC).addSecs(seq * 60);
  b.finishedAt = b.startedAt.addSecs(30);
  static const char *outcomes[] = {"imported", "unresolved", "failed", "skipped"};
  b.rows.reserve(n);
  for (int i = 0; i < n; ++i)
  {
    paleo::imports::LedgerRow r;
    r.path = QStringLiteral("/data/工区/well_%1/log_%2.las").arg(i / 8, 6, 10, QLatin1Char('0')).arg(i % 8);
    r.type = (i % 3) ? QStringLiteral("las") : QStringLiteral("well_tops");
    r.entity = QStringLiteral("W%1").arg(i / 8);
    r.outcome = QLatin1String(outcomes[i % 4]);
    r.message = (i % 4 == 2) ? QStringLiteral("第 %1 行深度非单调，已拒收").arg(i) : QString();
    switch (i % 4)
    {
    case 0: ++b.imported; break;
    case 1: ++b.unresolved; break;
    case 2: ++b.failed; break;
    default: ++b.skipped; break;
    }
    b.rows.append(r);
  }
  return b;
}
bool sameBatch(const paleo::imports::LedgerBatch &a, const paleo::imports::LedgerBatch &b)
{
  if (a.id != b.id || a.dir != b.dir || a.startedAt != b.startedAt ||
      a.finishedAt != b.finishedAt || a.imported != b.imported || a.unresolved != b.unresolved ||
      a.failed != b.failed || a.skipped != b.skipped || a.error != b.error ||
      a.rows.size() != b.rows.size())
    return false;
  for (qsizetype i = 0; i < a.rows.size(); ++i)
  {
    const auto &x = a.rows[i], &y = b.rows[i];
    if (x.path != y.path || x.type != y.type || x.entity != y.entity ||
        x.outcome != y.outcome || x.message != y.message)
      return false;
  }
  return true;
}
} // namespace

// opt-01 P2：台账在 GUI 线程上 load+record（FolderImportWorkflow::recordLedger）。
// 有界规模：默认只跑 1k 行（ctest 常驻），PALEO_OPT01_LEDGER_SCALE=1000,10000,100000
// 打开大规模 A/B；每档 3 次「重新 load + record 一批」取中位数，并以落盘字节
// sha256 + 逐字段回读作正确性交叉校验。
void IoImportLedgerTests::ledgerScaleRoundTrip()
{
  QList<int> scales{1000};
  const QByteArray env = qgetenv("PALEO_OPT01_LEDGER_SCALE");
  if (!env.isEmpty())
  {
    scales.clear();
    for (const QByteArray &part : env.split(','))
      if (part.toInt() > 0)
        scales << part.toInt();
  }
  for (const int n : scales)
  {
    QTemporaryDir dir;
    QVERIFY(dir.isValid());
    DataCatalog cat;
    QVERIFY(cat.open(dir.path(), nullptr));
    QList<qint64> loadMs, recordMs;
    for (int rep = 0; rep < 3; ++rep)
    {
      paleo::imports::ImportLedger ledger;
      QElapsedTimer t;
      t.start();
      ledger.load(&cat);
      loadMs << t.elapsed();
      QCOMPARE(ledger.count(), rep);
      QVERIFY(ledger.loadWarning().isEmpty());
      const auto batch = syntheticBatch(n, rep);
      t.restart();
      QString err;
      QVERIFY2(ledger.record(batch, &err), qPrintable(err));
      recordMs << t.elapsed();
    }
    paleo::imports::ImportLedger reread;
    QElapsedTimer t;
    t.start();
    reread.load(&cat);
    const qint64 finalLoad = t.elapsed();
    QCOMPARE(reread.count(), 3);
    for (int rep = 0; rep < 3; ++rep)
      QVERIFY2(sameBatch(reread.batches()[rep], syntheticBatch(n, rep)), "台账回读与写入不一致");
    QFile f(reread.filePath());
    QVERIFY(f.open(QIODevice::ReadOnly));
    const QByteArray bytes = f.readAll();
    std::sort(loadMs.begin(), loadMs.end());
    std::sort(recordMs.begin(), recordMs.end());
    qInfo().noquote() << QStringLiteral("OPT01_PERF ledger rows=%1 load_median_ms=%2 "
                                        "record_median_ms=%3 final_load_ms=%4 bytes=%5 sha256=%6")
                             .arg(n)
                             .arg(loadMs[1])
                             .arg(recordMs[1])
                             .arg(finalLoad)
                             .arg(bytes.size())
                             .arg(QString::fromLatin1(
                                 QCryptographicHash::hash(bytes, QCryptographicHash::Sha256).toHex()));
  }
}

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

// opt-01 U2：台账是跨会话审计面。损坏件（半截 JSON/误编辑）读出空历史后，
// 旧实现下一批 record 直接用「仅本批」覆盖——全部历史静默蒸发。新口径：
// load 给出可解释原因；record 先把原字节另存 .corrupt-<时间戳> 再写新台账。
void IoImportLedgerTests::corruptLedgerPreservedBeforeOverwrite()
{
  QTemporaryDir dir;
  QVERIFY(dir.isValid());
  const QString src = dir.filePath(QStringLiteral("src"));
  QVERIFY(write(QDir(src).filePath(QStringLiteral("ExportWellHead.dat")), "W1 10 20 0 500\n"));
  PaleoProjectStore store;
  DataImportService importer(&store);
  importer.setProjectDir(dir.filePath(QStringLiteral("project")));
  QVERIFY2(importer.catalogWritable(), qPrintable(importer.catalogOpenError()));

  paleo::imports::ImportLedger probe;
  probe.load(importer.catalog());
  const QString ledgerPath = probe.filePath();
  QVERIFY(!ledgerPath.isEmpty());
  QVERIFY(probe.loadWarning().isEmpty()); // 缺文件 = 正常空历史
  const QByteArray corrupt = "{\"batches\":[{\"id\":\"batch-1\",\"dir\":\"/old\",\"rows\":[";
  QVERIFY(write(ledgerPath, corrupt));
  probe.load(importer.catalog());
  QCOMPARE(probe.count(), 0);
  QVERIFY2(probe.loadWarning().contains(QStringLiteral("损坏")), qPrintable(probe.loadWarning()));

  FolderImportWorkflow workflow(&importer, nullptr);
  QString error;
  workflow.importFolder(src, {}, [&](const auto &, const QString &e) { error = e; });
  QVERIFY2(error.isEmpty(), qPrintable(error));

  // 原损坏字节被逐字保全（恰一份备份），新台账只含本批且可正常读回。
  const QFileInfo li(ledgerPath);
  const QStringList backups = li.dir().entryList(
      QStringList{li.fileName() + QStringLiteral(".corrupt-*")}, QDir::Files);
  QCOMPARE(backups.size(), 1);
  QFile b(li.dir().filePath(backups.first()));
  QVERIFY(b.open(QIODevice::ReadOnly));
  QCOMPARE(b.readAll(), corrupt);
  paleo::imports::ImportLedger after;
  after.load(importer.catalog());
  QVERIFY(after.loadWarning().isEmpty());
  QCOMPARE(after.count(), 1);
  QCOMPARE(after.batches()[0].dir, src);
  // 再写一批：不再重复备份（损坏件只保全一次）。
  workflow.importFolder(src, {}, [&](const auto &, const QString &e) { error = e; });
  QCOMPARE(li.dir().entryList(QStringList{li.fileName() + QStringLiteral(".corrupt-*")},
                              QDir::Files).size(), 1);
  after.load(importer.catalog());
  QCOMPARE(after.count(), 2);
}

// 备份不可得（目录只读）→ record 拒写并给出原因；原损坏件字节不动。
void IoImportLedgerTests::corruptLedgerUnbackupableRefusesWrite()
{
#ifdef Q_OS_WIN
  QSKIP("POSIX 目录权限语义");
#else
  QTemporaryDir dir;
  QVERIFY(dir.isValid());
  PaleoProjectStore store;
  DataImportService importer(&store);
  importer.setProjectDir(dir.filePath(QStringLiteral("project")));
  QVERIFY(importer.catalogWritable());
  paleo::imports::ImportLedger ledger;
  ledger.load(importer.catalog());
  const QString ledgerPath = ledger.filePath();
  const QByteArray corrupt = "not json at all";
  QVERIFY(write(ledgerPath, corrupt));
  ledger.load(importer.catalog());
  QVERIFY(!ledger.loadWarning().isEmpty());
  const QString sidecarDir = QFileInfo(ledgerPath).absolutePath();
  const auto perms = QFile::permissions(sidecarDir);
  QVERIFY(QFile::setPermissions(sidecarDir, QFileDevice::ReadOwner | QFileDevice::ExeOwner));
  if (QFileInfo(sidecarDir).isWritable())
  {
    QFile::setPermissions(sidecarDir, perms);
    QSKIP("以特权运行：目录只读不生效");
  }
  paleo::imports::LedgerBatch batch;
  batch.id = QStringLiteral("batch-x");
  batch.dir = QStringLiteral("/x");
  QString error;
  const bool ok = ledger.record(batch, &error);
  QVERIFY(QFile::setPermissions(sidecarDir, perms));
  QVERIFY(!ok);
  QVERIFY2(error.contains(QStringLiteral("损坏")), qPrintable(error));
  QFile f(ledgerPath);
  QVERIFY(f.open(QIODevice::ReadOnly));
  QCOMPARE(f.readAll(), corrupt);
#endif
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
