#include <QtTest>
#include <QDirIterator>
#include <QSignalSpy>
#include <QTemporaryDir>
#include <cmath>
#include <memory>
#include <algorithm>

#include "../src/catalog/datacatalog.h"
#include "../src/io/dataimportservice.h"
#include "../src/io/importclassification.h"
#include "../src/io/importrecovery.h"
#include "../src/io/ingestplan.h"
#include "../src/io/shacache.h"
#include "../src/io/welllogread.h"
#include "../src/metadata/paleoprojectstore.h"
#include "../src/services/welllogset.h"
#include "importuxfixtures.h"

namespace
{
using Outcome = DataImportService::ImportOutcome;
using Commit = DataImportService::CommitStatus;
using RowOutcome = FolderRowResult::Outcome;

struct Harness
{
  QTemporaryDir temporary;
  QString project = temporary.filePath(QStringLiteral("project"));
  PaleoProjectStore store;
  std::unique_ptr<DataImportService> service;

  Harness()
  {
    QDir().mkpath(project);
    store.setProjectPaths(project + QStringLiteral("/project.qgz"),
                          project + QStringLiteral("/project.gpkg"),
                          project + QStringLiteral("/metadata/project.sqlite"));
    reopen();
  }
  void reopen()
  {
    service.reset();
    service = std::make_unique<DataImportService>(&store);
    service->setProjectDir(project);
  }
  DataCatalog *catalog() const { return service->catalog(); }
  QString source(const QString &name, const QByteArray &content) const
  {
    const QString path = temporary.filePath(QStringLiteral("source/") + name);
    return ImportUxFixtures::write(path, content) ? path : QString();
  }
  QString wellId(const QString &name = QStringLiteral("A1")) const
  {
    for (const auto &well : catalog()->entities(QStringLiteral("well")))
      if (well.name == name) return well.id;
    return {};
  }
  QVector<WellCurveRef> curves(const QString &well = QStringLiteral("A1")) const
  { return WellLogSet::wellCurveIndex(catalog(), project, wellId(well)); }
};

WellCurveRef findCurve(const QVector<WellCurveRef> &curves, const QString &name)
{
  for (const auto &curve : curves)
    if (curve.mnemonic == name) return curve;
  return {};
}

QByteArray bytesAt(const QString &path)
{
  QFile file(path);
  return file.open(QIODevice::ReadOnly) ? file.readAll() : QByteArray();
}

QStringList rawFiles(const QString &project)
{
  QStringList files;
  QDirIterator iterator(project + QStringLiteral("/artifacts/raw"), QDir::Files,
                        QDirIterator::Subdirectories);
  while (iterator.hasNext()) files << iterator.next();
  return files;
}

// Simulate power loss between filesystem rename and catalog transaction. Keep
// the real intent and real managed byte layout rather than inventing JSON.
bool interruptAfterMove(ImportSession &session, QStringList &moved, QString *error)
{
  if (!paleo::dataimport_detail::writeImportIntent(session, error) || !session.intentPrepared)
    return false;
  // Preparing a receipt is still cancellable production; only the owner that
  // starts final moves preserves the receipt after an interruption.
  session.recoveryPending = true;
  for (const auto &version : session.cat->versions())
  {
    if (!session.cat->isStagedVersion(version.id) || !version.managed) continue;
    const QString from = session.cat->versionFilePath(version);
    const QString to = DataCatalog::resolvedVersionPath(session.projectDir, version);
    if (to.isEmpty() || !QDir().mkpath(QFileInfo(to).absolutePath()) || !QFile::rename(from, to))
      return false;
    moved << to;
  }
  return !moved.isEmpty();
}
}

// Service-level regression: no GUI or QGIS project is required to prove the
// persisted well/asset/link contract and the business curve read entry point.
class TestImportUx : public QObject
{
  Q_OBJECT
private slots:
  void completeXmlCreatesOneWellAndThreeReadableCurves()
  {
    Harness h;
    QVERIFY2(h.service->catalogWritable(), qPrintable(h.service->catalogOpenError()));
    const QByteArray source = ImportUxFixtures::xml();
    const QString path = h.source(QStringLiteral("unrelated-name.xml"), source);
    QVERIFY(!path.isEmpty());
    QString error;
    const auto result = h.service->importProjectFileEx(path, &error);
    QVERIFY2(result.outcome == Outcome::Imported, qPrintable(error));
    QCOMPARE(h.catalog()->entities(QStringLiteral("well")).size(), 1);
    QCOMPARE(h.catalog()->assetById(result.assetId).type, QStringLiteral("well_log"));
    QCOMPARE(h.curves().size(), 3);
    const auto links = h.catalog()->linksForAsset(result.assetId);
    QCOMPARE(links.size(), 1);
    QCOMPARE(links.first().entityId, h.wellId());
    QVERIFY(!links.first().unresolved);
    QVERIFY(!h.catalog()->entityById(h.wellId()).hasSurface);
    WellLogSet::WellCurveTvdSamples samples;
    QVERIFY2(WellLogSet::readCurveTvd(findCurve(h.curves(), QStringLiteral("GR")), nullptr,
                                     &samples, &error), qPrintable(error));
    QCOMPARE(samples.md, QVector<double>({1000, 1000.5, 1001}));
    QCOMPARE(samples.values, QVector<double>({40, 45, 50}));
    QCOMPARE(bytesAt(h.service->absolutePath(result.assetId)), source);
    QCOMPARE(bytesAt(path), source);
  }

  void worksheetNameIsContentEvidence()
  {
    Harness h;
    const QString path = h.source(QStringLiteral("unknown.xml"), ImportUxFixtures::xml(QStringLiteral("A1"), false));
    QString error;
    const auto preview = h.service->previewFiles({path}, &error);
    QCOMPARE(preview.size(), 1);
    QVERIFY2(!preview.first().importBlocked, qPrintable(preview.first().issues.join('\n')));
    QCOMPARE(preview.first().proposedWellName, QStringLiteral("A1"));
    QVERIFY(!preview.first().needsWellChoice);
    QVERIFY2(!h.service->importProjectFile(path, &error).isEmpty(), qPrintable(error));
    QCOMPARE(h.curves().size(), 3);
  }

  void largeWorkbookClassificationScansBeyondFirstBlock_data()
  {
    QTest::addColumn<QString>("sheet");
    QTest::addColumn<QString>("type");
    QTest::newRow("late-curves") << QStringLiteral("测井曲线-A1井") << QStringLiteral("well_log");
    QTest::newRow("late-deviation") << QStringLiteral("井斜-A1井") << QStringLiteral("well_deviation");
    QTest::newRow("ordinary-workbook") << QStringLiteral("普通资料") << QStringLiteral("unknown");
  }

  void largeWorkbookClassificationScansBeyondFirstBlock()
  {
    QFETCH(QString, sheet);
    QFETCH(QString, type);
    Harness h;
    const QByteArray bytes = ImportUxFixtures::paddedXml(sheet);
    QVERIFY(bytes.indexOf(sheet.toUtf8()) > 65536);
    const QString path = h.source(QStringLiteral("large.xml"), bytes);
    QCOMPARE(classifyImportFile(path).type, type);
    QString error;
    const auto preview = h.service->previewFiles({path}, &error);
    QCOMPARE(preview.size(), 1);
    QCOMPARE(preview.first().classifiedType, type);
    QVERIFY(h.catalog()->entities().isEmpty());
    if (type == QLatin1String("well_log"))
    {
      QVERIFY2(!preview.first().importBlocked, qPrintable(preview.first().issues.join('\n')));
      const auto imported = h.service->importProjectFileEx(path, &error);
      QVERIFY2(imported.outcome == Outcome::Imported, qPrintable(error));
      QCOMPARE(h.catalog()->assetById(imported.assetId).type, type);
      QCOMPARE(h.curves().size(), 3);
    }
  }

  void quotedCsvAndWellColumnBeforeIndexUseUnifiedClassifier_data()
  {
    QTest::addColumn<QByteArray>("bytes");
    QTest::newRow("quoted-well-first") << QByteArray(
        "\"WELL\",\"MD[m]\",\"RES[ohm.m]\"\n\"A1\",\"1000\",\"10\"\n\"A1\",\"1001\",\"20\"\n");
    QTest::newRow("unquoted-well-first") << QByteArray("WELL,MD[m],RES[ohm.m]\nA1,1000,10\nA1,1001,20\n");
  }

  void quotedCsvAndWellColumnBeforeIndexUseUnifiedClassifier()
  {
    QFETCH(QByteArray, bytes);
    Harness h;
    const QString path = h.source(QStringLiteral("curve.csv"), bytes);
    QCOMPARE(classifyImportFile(path).type, QStringLiteral("well_log"));
    QString error;
    const auto preview = h.service->previewFiles({path}, &error);
    QCOMPARE(preview.size(), 1);
    QCOMPARE(preview.first().classifiedType, QStringLiteral("well_log"));
    QCOMPARE(preview.first().proposedWellName, QStringLiteral("A1"));
    QVERIFY2(!preview.first().importBlocked, qPrintable(preview.first().issues.join('\n')));
    QVERIFY2(!h.service->importProjectFile(path, &error).isEmpty(), qPrintable(error));
    QCOMPARE(h.curves().size(), 1);
    WellLogSet::WellCurveTvdSamples samples;
    QVERIFY2(WellLogSet::readCurveTvd(h.curves().first(), nullptr, &samples, &error), qPrintable(error));
    QCOMPARE(samples.md, QVector<double>({1000, 1001}));
    QCOMPARE(samples.values, QVector<double>({10, 20}));
  }

  void managedXmlHeaderMetadataMatchesReadableCurveSources()
  {
    Harness h;
    QString error;
    const QString path = h.source(QStringLiteral("complete.xml"), ImportUxFixtures::xml());
    const QString asset = h.service->importProjectFile(path, &error);
    QVERIFY2(!asset.isEmpty(), qPrintable(error));
    const auto version = h.catalog()->currentVersion(asset);
    LasHeaderInfo stored, parsed;
    QVERIFY(WellLogSet::storedHeader(version, &stored));
    QVERIFY2(WellLogRead::parseHeader(h.service->absolutePath(asset), parsed, &error), qPrintable(error));
    QCOMPARE(stored.curveNames, parsed.curveNames);
    QCOMPARE(stored.wellName, parsed.wellName);
    QCOMPARE(stored.indexBasis, parsed.indexBasis);
    QCOMPARE(stored.curveNames, QStringList({"MD", "GR", "RHOB", "NPHI"}));
    QCOMPARE(version.extra.value(QStringLiteral("logCurveNames")).toStringList(), stored.curveNames);
    QCOMPARE(version.extra.value(QStringLiteral("logCurves")).toStringList(), stored.curveNames.mid(1));
    const auto files = WellLogSet::wellLogFiles(h.catalog(), h.project, h.wellId());
    QCOMPARE(files.size(), 1);
    QCOMPARE(files.first().curveNames, stored.curveNames);
    QCOMPARE(files.first().indexBasis, stored.indexBasis);
    for (const auto &ref : h.curves())
    {
      QCOMPARE(ref.sourceVersionId, version.id);
      QCOMPARE(ref.path, h.service->absolutePath(asset));
      QCOMPARE(stored.curveNames.value(ref.column), ref.mnemonic);
    }
    CatalogVersion legacy = version;
    legacy.extra.clear();
    QVERIFY(!WellLogSet::storedHeader(legacy, &stored));
    CatalogVersion external = version;
    external.managed = false;
    QVERIFY(!WellLogSet::storedHeader(external, &stored));
  }

  void singleCsvCreatesReadableCurve()
  {
    Harness h;
    const QString path = h.source(QStringLiteral("RES.csv"), ImportUxFixtures::csv());
    QString error;
    QVERIFY2(!h.service->importProjectFile(path, &error).isEmpty(), qPrintable(error));
    QCOMPARE(h.curves().size(), 1);
    WellLogSet::WellCurveTvdSamples samples;
    QVERIFY2(WellLogSet::readCurveTvd(h.curves().first(), nullptr, &samples, &error), qPrintable(error));
    QCOMPARE(samples.md, QVector<double>({1000, 1001}));
    QCOMPARE(samples.values, QVector<double>({10, 20}));
  }

  void mixedBatchAndLaterAdditionShareExistingWell()
  {
    Harness h;
    const QString xml = h.source(QStringLiteral("complete.xml"), ImportUxFixtures::xml());
    const QString csv = h.source(QStringLiteral("RES.csv"), ImportUxFixtures::csv());
    QString error;
    const auto rows = h.service->importFiles({xml, csv}, &error);
    QVERIFY2(error.isEmpty(), qPrintable(error));
    QCOMPARE(rows.size(), 2);
    for (const auto &row : rows) QCOMPARE(row.outcome, RowOutcome::Imported);
    QCOMPARE(h.catalog()->entities(QStringLiteral("well")).size(), 1);
    const QString originalWell = h.wellId();
    QCOMPARE(h.curves().size(), 4);
    const QString sp = h.source(QStringLiteral("SP.csv"), ImportUxFixtures::csv("A1", "SP[mV]"));
    QVERIFY2(!h.service->importProjectFile(sp, &error).isEmpty(), qPrintable(error));
    QCOMPARE(h.wellId(), originalWell);
    QCOMPARE(h.curves().size(), 5);
    QCOMPARE(h.catalog()->linksForEntity(originalWell).size(), 3);
  }

  void duplicateShaAcrossNamesDoesNotGrowEntitiesOrVersions()
  {
    Harness h;
    const QString first = h.source(QStringLiteral("one.csv"), ImportUxFixtures::csv());
    const QString copy = h.source(QStringLiteral("renamed.csv"), ImportUxFixtures::csv());
    QString error;
    const auto imported = h.service->importProjectFileEx(first, &error);
    QVERIFY2(imported.outcome == Outcome::Imported, qPrintable(error));
    const quint64 seq = h.catalog()->mutationSeq();
    const auto repeated = h.service->importProjectFileEx(copy, &error);
    QCOMPARE(repeated.outcome, Outcome::AlreadyStored);
    QCOMPARE(repeated.assetId, imported.assetId);
    QCOMPARE(h.catalog()->assets().size(), 1);
    QCOMPARE(h.catalog()->versions().size(), 1);
    QCOMPARE(h.catalog()->entities(QStringLiteral("well")).size(), 1);
    QCOMPARE(h.catalog()->links().size(), 1);
    QCOMPARE(h.catalog()->mutationSeq(), seq);
    QCOMPARE(rawFiles(h.project).size(), 1);
  }

  void duplicateShaCannotSilentlyIgnoreDifferentTarget_data()
  {
    QTest::addColumn<bool>("createNew");
    QTest::newRow("existing-other-well") << false;
    QTest::newRow("new-other-well") << true;
  }

  void duplicateShaCannotSilentlyIgnoreDifferentTarget()
  {
    QFETCH(bool, createNew);
    Harness h;
    const QString path = h.source(QStringLiteral("one.csv"), ImportUxFixtures::csv());
    QString error;
    const QString asset = h.service->importProjectFile(path, &error);
    QVERIFY2(!asset.isEmpty(), qPrintable(error));
    const QString original = h.wellId();
    const QString otherId = QStringLiteral("well-other");
    if (!createNew)
    {
      CatalogEntity other;
      other.id = otherId;
      other.entityType = QStringLiteral("well");
      other.name = QStringLiteral("B1");
      QVERIFY2(h.catalog()->addEntity(other, &error), qPrintable(error));
    }
    const auto entityCount = h.catalog()->entities().size();
    const quint64 seq = h.catalog()->mutationSeq();
    const QString target = createNew ? QStringLiteral("new:B1") : otherId;
    DataImportService::ImportOptions options;
    options.targetWellId = target;
    const auto rejected = h.service->importProjectFileEx(path, options, &error);
    QCOMPARE(rejected.outcome, Outcome::Failed);
    QVERIFY2(error.contains(QStringLiteral("所属井")), qPrintable(error));
    QCOMPARE(h.catalog()->mutationSeq(), seq);
    QCOMPARE(h.catalog()->entities().size(), entityCount);
    QCOMPARE(h.catalog()->assets().size(), 1);
    QCOMPARE(h.catalog()->versions().size(), 1);
    QCOMPARE(h.catalog()->links().size(), 1);
    QCOMPARE(h.catalog()->linksForAsset(asset).first().entityId, original);
    // The batch default skip decision must validate the selected target too.
    auto session = h.service->beginImport();
    session->wellTargets.insert(path, target);
    DataImportService::produceFiles(*session, {path}, {}, {});
    QCOMPARE(session->rows.size(), 1);
    QCOMPARE(session->rows.first().outcome, RowOutcome::Failed);
    QVERIFY2(h.service->commitImport(*session, &error) == Commit::Committed, qPrintable(error));
    QCOMPARE(h.catalog()->mutationSeq(), seq);
    QCOMPARE(h.catalog()->entities().size(), entityCount);
    options.targetWellId = original;
    const auto repeated = h.service->importProjectFileEx(path, options, &error);
    QVERIFY2(repeated.outcome == Outcome::AlreadyStored, qPrintable(error));
    QCOMPARE(repeated.assetId, asset);
    QCOMPARE(h.catalog()->mutationSeq(), seq);
    QCOMPARE(h.catalog()->links().size(), 1);
    QCOMPARE(h.curves().size(), 1);
  }

  void previewDigestRejectsChangedSourceBeforeAnyEntityIsCreated_data()
  {
    QTest::addColumn<bool>("preserveMetadata");
    QTest::newRow("different-size") << false;
    QTest::newRow("same-size-and-mtime") << true;
  }

  void previewDigestRejectsChangedSourceBeforeAnyEntityIsCreated()
  {
    QFETCH(bool, preserveMetadata);
    Harness h;
    const QByteArray original = ImportUxFixtures::csv();
    const QString path = h.source(QStringLiteral("changing.csv"), original);
    QString error;
    const auto preview = h.service->previewFiles({path}, &error);
    QCOMPARE(preview.size(), 1);
    QCOMPARE(preview.first().proposedWellName, QStringLiteral("A1"));
    const QString expected = preview.first().sourceSha256;
    QCOMPARE(expected.size(), 64);
    QCOMPARE(expected, DataCatalog::sha256FileHex(path, &error));
    const QDateTime modified = QFileInfo(path).lastModified();
    const QByteArray replacement = preserveMetadata ? ImportUxFixtures::csv("B1")
        : ImportUxFixtures::csv("B1", "RES[ohm.m]", "1000,123456\n1001,20\n");
    QVERIFY(ImportUxFixtures::write(path, replacement));
    if (preserveMetadata)
    {
      QFile changed(path);
      QVERIFY(changed.open(QIODevice::ReadWrite));
      QVERIFY(changed.setFileTime(modified, QFileDevice::FileModificationTime));
      changed.close();
      QCOMPARE(QFileInfo(path).lastModified(), modified);
      QCOMPARE(QFileInfo(path).size(), qint64(original.size()));
    }
    QVERIFY(DataCatalog::sha256FileHex(path, &error) != expected);
    auto session = h.service->beginImport();
    session->expectedSourceHashes.insert(path, expected);
    session->wellTargets.insert(path, QStringLiteral("new:A1"));
    DataImportService::produceFiles(*session, {path}, {}, {});
    QCOMPARE(session->rows.size(), 1);
    QCOMPARE(session->rows.first().outcome, RowOutcome::Failed);
    QVERIFY(session->rows.first().message.contains(QStringLiteral("预览")));
    QVERIFY2(h.service->commitImport(*session, &error) == Commit::Committed, qPrintable(error));
    QVERIFY(h.catalog()->entities().isEmpty());
    QVERIFY(h.catalog()->assets().isEmpty());
    QVERIFY(h.catalog()->versions().isEmpty());
    QVERIFY(h.catalog()->links().isEmpty());
    QVERIFY(rawFiles(h.project).isEmpty());
    QCOMPARE(bytesAt(path), replacement);
  }

  // F1（review-security）：预览确认后、执行前新落盘的文件不得随批入库——
  // 执行期目录重扫与预览路径集差分，多出一律 Skipped 并提示重新预览。
  void lateArrivingFilesAreSkippedNotImported()
  {
    Harness h;
    const QString dir = h.temporary.filePath(QStringLiteral("late"));
    QVERIFY(QDir().mkpath(dir));
    const QString first = dir + QStringLiteral("/first.csv");
    QVERIFY(ImportUxFixtures::write(first, ImportUxFixtures::csv("A1")));
    QString error;
    const auto preview = h.service->previewFolder(dir, &error);
    QVERIFY2(error.isEmpty(), qPrintable(error));
    QCOMPARE(preview.size(), 1);
    QCOMPARE(preview.first().path, first);
    // 用户审阅窗口内，另一进程向源目录写入新文件。
    const QString late = dir + QStringLiteral("/late.csv");
    QVERIFY(ImportUxFixtures::write(late, ImportUxFixtures::csv("B1", "GR[API]")));

    auto session = h.service->beginImport();
    for (const auto &row : preview)
      session->previewedPaths << row.path; // 确认表传入的「所见」全集
    DataImportService::produceFolder(*session, dir, {}, {});
    QCOMPARE(session->rows.size(), 2);
    const auto firstRow = std::find_if(session->rows.begin(), session->rows.end(),
                                       [&](const auto &r) { return r.path == first; });
    const auto lateRow = std::find_if(session->rows.begin(), session->rows.end(),
                                      [&](const auto &r) { return r.path == late; });
    QVERIFY(firstRow != session->rows.end() && lateRow != session->rows.end());
    QCOMPARE(firstRow->outcome, RowOutcome::Imported);
    QCOMPARE(lateRow->outcome, RowOutcome::Skipped);
    QVERIFY(lateRow->message.contains(QStringLiteral("预览后才出现")));
    QVERIFY2(h.service->commitImport(*session, &error) == Commit::Committed,
             qPrintable(error));
    // 晚到文件零痕迹：B1 井不存在、资产/raw 只有 first 的份。
    QVERIFY(h.wellId(QStringLiteral("B1")).isEmpty());
    QCOMPARE(h.catalog()->assets().size(), 1);
    QCOMPARE(rawFiles(h.project).size(), 1);
    QCOMPARE(h.curves(QStringLiteral("A1")).size(), 1);
  }

  // F2（review-security）：同尺寸换内容 + 保 mtime（同步工具做法）——dedup
  // 决策不信 ShaCache 缓存指纹，按真哈希导入为新数据，不静默报「已在库」。
  void staleCacheDoesNotSuppressChangedBytes()
  {
    Harness h;
    const QByteArray original = ImportUxFixtures::csv("A1", "RES[ohm.m]", "1000,10\n1001,20\n");
    const QString path = h.source(QStringLiteral("a1.csv"), original);
    QVERIFY(!path.isEmpty());
    QString error;
    const auto first = h.service->importProjectFileEx(path, &error);
    QVERIFY2(first.outcome == Outcome::Imported, qPrintable(error));
    QCOMPARE(h.catalog()->assets().size(), 1);
    QCOMPARE(h.curves(QStringLiteral("A1")).size(), 1);

    // 污染源缓存：换同尺寸内容并把 mtime 拨回原值——指纹（mtime|size）不变，
    // ShaCache 仍会返回旧内容的哈希（bug 场景的真实构造）。
    ShaCache::shared().invalidate(path);
    QString primeErr;
    const QString cachedOriginal =
        ShaCache::shared().sha256Hex(path, &primeErr, ShaCache::Fingerprint::MtimeSizeInode);
    QCOMPARE(cachedOriginal, DataCatalog::sha256FileHex(path));
    const QDateTime modified = QFileInfo(path).lastModified();
    const QByteArray swapped = ImportUxFixtures::csv("A1", "RES[ohm.m]", "1000,10\n1001,21\n");
    QCOMPARE(swapped.size(), original.size());
    QVERIFY(swapped != original);
    QVERIFY(ImportUxFixtures::write(path, swapped));
    QFile touched(path);
    QVERIFY(touched.open(QIODevice::ReadWrite));
    QVERIFY(touched.setFileTime(modified, QFileDevice::FileModificationTime));
    touched.close();
    QString staleErr;
    const QString stale = ShaCache::shared().sha256Hex(path, &staleErr,
                                                       ShaCache::Fingerprint::MtimeSizeInode);
    QCOMPARE(stale, cachedOriginal); // 缓存确实陈旧（返回旧内容的哈希）
    QVERIFY(stale != DataCatalog::sha256FileHex(path)); // 真哈希已变

    // 重导：不得 AlreadyStored——新字节必须如实成为新数据来源。
    const auto second = h.service->importProjectFileEx(path, &error);
    QVERIFY2(second.outcome == Outcome::Imported, qPrintable(error));
    QVERIFY(second.assetId != first.assetId);
    QCOMPARE(h.catalog()->assets().size(), 2);
    // 同名曲线两来源并存（别名机制），互不覆盖。
    QCOMPARE(h.curves(QStringLiteral("A1")).size(), 2);
  }

  // F4（review-security）：显式「新建井：X」撞上已有井 X——拒绝并入，行如实
  // 失败并提示改选已有井；已有井与库零改动。
  void explicitNewWellCollidingExistingNameIsRejected()
  {
    Harness h;
    const QString first = h.source(QStringLiteral("a1.csv"), ImportUxFixtures::csv("A1"));
    QVERIFY(!first.isEmpty());
    QString error;
    QVERIFY2(h.service->importProjectFileEx(first, &error).outcome == Outcome::Imported,
             qPrintable(error));
    const QString existing = h.wellId(QStringLiteral("A1"));
    QVERIFY(!existing.isEmpty());
    QCOMPARE(h.catalog()->assets().size(), 1);

    // 第二份文件内容井名是 B1，但用户显式选「新建井：A1」（与既有井撞名）。
    const QString second =
        h.source(QStringLiteral("b1.csv"), ImportUxFixtures::csv("B1", "GR[API]"));
    QVERIFY(!second.isEmpty());
    auto session = h.service->beginImport();
    session->wellTargets.insert(second, QStringLiteral("new:A1"));
    DataImportService::produceFiles(*session, {second}, {}, {});
    QCOMPARE(session->rows.size(), 1);
    QCOMPARE(session->rows.first().outcome, RowOutcome::Failed);
    QVERIFY(session->rows.first().message.contains(QStringLiteral("已存在")));
    QVERIFY2(h.service->commitImport(*session, &error) == Commit::Committed,
             qPrintable(error));
    // 零并入：A1 仍一口井、资产仍 1 个、B1 未建。
    QCOMPARE(h.catalog()->entities(QStringLiteral("well")).size(), 1);
    QCOMPARE(h.catalog()->assets().size(), 1);
    QVERIFY(h.wellId(QStringLiteral("B1")).isEmpty());
    QCOMPARE(rawFiles(h.project).size(), 1);
  }

  // F5（review-security）：测井先建的井无坐标；井位表命中该井时回填坐标与
  // 状态（updateEntity 经 staging/journal），重开工程后坐标仍在。
  void wellheadImportBackfillsLogCreatedWellCoordinates()
  {
    Harness h;
    const QString logPath = h.source(QStringLiteral("a1.csv"), ImportUxFixtures::csv("A1"));
    QVERIFY(!logPath.isEmpty());
    QString error;
    QVERIFY2(h.service->importProjectFileEx(logPath, &error).outcome == Outcome::Imported,
             qPrintable(error));
    const QString wid = h.wellId(QStringLiteral("A1"));
    QVERIFY(!wid.isEmpty());
    const auto before = h.catalog()->entityById(wid);
    QCOMPARE(before.coordinateStatus, QStringLiteral("unknown"));
    QVERIFY(!before.hasSurface);

    // 井位表（路径段「井位」触发 well_head 分类）带来 A1 坐标。
    const QString headsPath = h.source(
        QStringLiteral("井位/heads.dat"),
        QByteArrayLiteral("#WellHead File From SMI\n#Name X Y KB TD\n"
                          "A1 3.5 4.5 10.0 2100.0\n"));
    QVERIFY(!headsPath.isEmpty());
    QVERIFY2(h.service->importProjectFileEx(headsPath, &error).outcome == Outcome::Imported,
             qPrintable(error));
    const auto after = h.catalog()->entityById(wid);
    QVERIFY(after.hasSurface);
    QCOMPARE(after.surfaceX, 3.5);
    QCOMPARE(after.surfaceY, 4.5);
    QCOMPARE(after.kb, 10.0);
    QCOMPARE(after.td, 2100.0);
    QVERIFY(after.coordinateStatus != QStringLiteral("unknown"));
    // journal 重放进持久化：重开后坐标不丢。
    h.reopen();
    const auto reopened = h.catalog()->entityById(wid);
    QVERIFY(reopened.hasSurface);
    QCOMPARE(reopened.surfaceX, 3.5);
    QCOMPARE(reopened.surfaceY, 4.5);
  }

  void sameMnemonicDifferentValuesRetainsBothSources()
  {
    Harness h;
    QString error;
    const QString first = h.source(QStringLiteral("original.csv"), ImportUxFixtures::csv());
    const QString second = h.source(QStringLiteral("revised.csv"), ImportUxFixtures::csv("A1", "RES[ohm.m]", "1000,90\n1001,99\n"));
    QVERIFY2(!h.service->importProjectFile(first, &error).isEmpty(), qPrintable(error));
    const auto preview = h.service->previewFiles({second}, &error);
    QCOMPARE(preview.size(), 1);
    QVERIFY(preview.first().issues.join('\n').contains(QStringLiteral("同名")));
    QVERIFY2(!h.service->importProjectFile(second, &error).isEmpty(), qPrintable(error));
    const auto index = h.curves();
    QCOMPARE(index.size(), 2);
    QVERIFY(index[0].mnemonic != index[1].mnemonic);
    QVERIFY(index[0].sourceVersionId != index[1].sourceVersionId);
    QVector<double> beginnings;
    for (const auto &ref : index)
    {
      WellLogSet::WellCurveTvdSamples samples;
      QVERIFY2(WellLogSet::readCurveTvd(ref, nullptr, &samples, &error), qPrintable(error));
      beginnings << samples.values.first();
    }
    QVERIFY(beginnings.contains(10));
    QVERIFY(beginnings.contains(90));
    QCOMPARE(bytesAt(first), ImportUxFixtures::csv());
  }

  void similarWellNamesRequireExplicitSelection()
  {
    Harness h;
    QString error;
    const QString first = h.source(QStringLiteral("original.csv"), ImportUxFixtures::csv());
    QVERIFY2(!h.service->importProjectFile(first, &error).isEmpty(), qPrintable(error));
    const QString second = h.source(QStringLiteral("similar.csv"), ImportUxFixtures::csv("A-1", "GR[API]"));
    const auto preview = h.service->previewFiles({second}, &error);
    QCOMPARE(preview.size(), 1);
    QVERIFY(preview.first().needsWellChoice);
    QVERIFY(preview.first().suggestedWellId.isEmpty());
    const auto rejected = h.service->importProjectFileEx(second, &error);
    QCOMPARE(rejected.outcome, Outcome::Failed);
    QVERIFY(!error.isEmpty());
    QCOMPARE(h.catalog()->assets().size(), 1);
    QCOMPARE(h.catalog()->entities(QStringLiteral("well")).size(), 1);
    QCOMPARE(h.curves().size(), 1);
    DataImportService::ImportOptions options;
    options.targetWellId = h.wellId();
    QVERIFY2(!h.service->importProjectFile(second, options, &error).isEmpty(), qPrintable(error));
    QCOMPARE(h.curves().size(), 2);
  }

  void internalWellNameWinsOverMisleadingFilename()
  {
    Harness h;
    QString error;
    QVERIFY2(!h.service->importProjectFile(h.source(QStringLiteral("seed.csv"), ImportUxFixtures::csv()), &error).isEmpty(), qPrintable(error));
    const QString misleading = h.source(QStringLiteral("A1.csv"), ImportUxFixtures::csv("B1", "GR[API]"));
    const auto result = h.service->importProjectFileEx(misleading, &error);
    QVERIFY2(result.outcome == Outcome::Imported, qPrintable(error));
    QCOMPARE(h.catalog()->linksForAsset(result.assetId).first().entityId, h.wellId(QStringLiteral("B1")));
    QCOMPARE(h.curves().size(), 1);
    QCOMPARE(h.curves(QStringLiteral("B1")).size(), 1);
  }

  void namelessFilenameCannotAssignWellButExplicitTargetCan()
  {
    Harness h;
    QString error;
    QVERIFY2(!h.service->importProjectFile(h.source(QStringLiteral("seed.csv"), ImportUxFixtures::csv()), &error).isEmpty(), qPrintable(error));
    const QString nameless = h.source(QStringLiteral("A1.csv"), ImportUxFixtures::csv({}, "GR[API]"));
    const auto preview = h.service->previewFiles({nameless}, &error);
    QCOMPARE(preview.size(), 1);
    QVERIFY(preview.first().needsWellChoice);
    QCOMPARE(h.service->importProjectFileEx(nameless, &error).outcome, Outcome::Failed);
    QCOMPARE(h.catalog()->assets().size(), 1);
    DataImportService::ImportOptions options;
    options.targetWellId = h.wellId();
    const auto result = h.service->importProjectFileEx(nameless, options, &error);
    QVERIFY2(result.outcome == Outcome::Imported, qPrintable(error));
    QCOMPARE(h.catalog()->linksForAsset(result.assetId).first().entityId, options.targetWellId);
    QCOMPARE(h.curves().size(), 2);
  }

  void previewSelectionsReachBatchSession()
  {
    Harness h;
    QString error;
    const QString path = h.source(QStringLiteral("nameless.csv"), ImportUxFixtures::csv({}));
    auto session = h.service->beginImport();
    QVERIFY(session);
    session->wellTargets.insert(path, QStringLiteral("new:Chosen"));
    DataImportService::produceFiles(*session, {path}, {}, {});
    QCOMPARE(session->rows.size(), 1);
    QCOMPARE(session->rows.first().outcome, RowOutcome::Imported);
    QCOMPARE(h.catalog()->assets().size(), 0);
    QVERIFY2(h.service->commitImport(*session, &error) == Commit::Committed, qPrintable(error));
    QCOMPARE(h.curves(QStringLiteral("Chosen")).size(), 1);
    const QString next = h.source(QStringLiteral("other.csv"), ImportUxFixtures::csv({}, "GR[API]"));
    session = h.service->beginImport();
    session->wellTargets.insert(next, h.wellId(QStringLiteral("Chosen")));
    DataImportService::produceFiles(*session, {next}, {}, {});
    QVERIFY2(h.service->commitImport(*session, &error) == Commit::Committed, qPrintable(error));
    QCOMPARE(h.curves(QStringLiteral("Chosen")).size(), 2);
    QCOMPARE(h.catalog()->entities(QStringLiteral("well")).size(), 1);
  }

  void previewIsReadOnlyAndDiscardedProductionLeavesNoEntities()
  {
    Harness h;
    const QString path = h.source(QStringLiteral("complete.xml"), ImportUxFixtures::xml());
    const quint64 before = h.catalog()->mutationSeq();
    QString error;
    const auto preview = h.service->previewFiles({path}, &error);
    QCOMPARE(preview.size(), 1);
    QVERIFY2(error.isEmpty(), qPrintable(error));
    QVERIFY(preview.first().summary.contains(QStringLiteral("3")));
    QVERIFY(preview.first().dataPreview.contains(QStringLiteral("GR")));
    QCOMPARE(h.catalog()->mutationSeq(), before);
    QVERIFY(h.catalog()->entities().isEmpty());
    QVERIFY(h.catalog()->assets().isEmpty());
    QVERIFY(rawFiles(h.project).isEmpty());
    QString staging;
    {
      auto session = h.service->beginImport();
      DataImportService::produceFiles(*session, {path}, {}, {});
      staging = session->stagingRoot;
      QVERIFY(QDir(staging).exists());
      QCOMPARE(h.catalog()->mutationSeq(), before);
    }
    QVERIFY(!QDir(staging).exists());
    QVERIFY(h.catalog()->entities().isEmpty());
    QVERIFY(rawFiles(h.project).isEmpty());
  }

  void preparedIntentBeforeFinalMoveIsDiscardedWithSession()
  {
    Harness h;
    const QString path = h.source(QStringLiteral("one.csv"), ImportUxFixtures::csv());
    auto session = h.service->beginImport();
    DataImportService::produceFiles(*session, {path}, {}, {});
    QCOMPARE(session->rows.size(), 1);
    QCOMPARE(session->rows.first().outcome, RowOutcome::Imported);
    QString error;
    QVERIFY2(paleo::dataimport_detail::writeImportIntent(*session, &error), qPrintable(error));
    QVERIFY(session->intentPrepared);
    QVERIFY(!session->recoveryPending);
    const QString staging = session->stagingRoot;
    QVERIFY(QFile::exists(staging + QStringLiteral("/commit-intent.json")));
    session.reset();
    QVERIFY(!QDir(staging).exists());
    QVERIFY(h.catalog()->entities().isEmpty());
    QVERIFY(h.catalog()->assets().isEmpty());
    QVERIFY(rawFiles(h.project).isEmpty());
    h.reopen();
    QVERIFY2(h.service->catalogWritable(), qPrintable(h.service->catalogOpenError()));
  }

  void unitsAndSamplingRemainPerSource()
  {
    Harness h;
    QString error;
    const QString xml = h.source(QStringLiteral("metric.xml"), ImportUxFixtures::xml());
    const QString ft = h.source(QStringLiteral("feet.csv"), ImportUxFixtures::csv("A1", "RES[ohm.m]", "3280,10\n3282,20\n", "MD[ft]"));
    const auto rows = h.service->importFiles({xml, ft}, &error);
    QCOMPARE(rows.size(), 2);
    for (const auto &row : rows) QCOMPARE(row.outcome, RowOutcome::Imported);
    const auto index = h.curves();
    WellLogSet::WellCurveTvdSamples metric, feet;
    const auto res = findCurve(index, QStringLiteral("RES"));
    QVERIFY2(WellLogSet::readCurveTvd(res, nullptr, &feet, &error), qPrintable(error));
    QVERIFY2(WellLogSet::readCurveTvd(findCurve(index, QStringLiteral("GR")), nullptr, &metric, &error), qPrintable(error));
    QCOMPARE(metric.md.size(), 3);
    QCOMPARE(feet.md.size(), 2);
    QVERIFY(std::abs(feet.md[0] - 3280 * 0.3048) < 1e-9);
    QVERIFY(std::abs(feet.md[1] - 3282 * 0.3048) < 1e-9);
    QCOMPARE(feet.tvd, feet.md);
    LasHeaderInfo header;
    QList<LasCurve> values;
    QVERIFY2(WellLogRead::parse(res.path, header, values, &error), qPrintable(error));
    QCOMPARE(header.indexBasis, QStringLiteral("MD"));
    QCOMPARE(values.first().unit, QStringLiteral("ft"));
    QCOMPARE(values.first().values, QVector<double>({3280, 3282}));
  }

  void missingSamplesRemainNan()
  {
    Harness h;
    QString error;
    const QString path = h.source(QStringLiteral("missing.csv"), ImportUxFixtures::csv("A1", "RES[ohm.m]", "1000,\n1001,NaN\n1002,-999.25\n1003,12\n"));
    QVERIFY2(!h.service->importProjectFile(path, &error).isEmpty(), qPrintable(error));
    WellLogSet::WellCurveTvdSamples samples;
    QVERIFY2(WellLogSet::readCurveTvd(h.curves().first(), nullptr, &samples, &error), qPrintable(error));
    QCOMPARE(samples.values.size(), 4);
    for (int i = 0; i < 3; ++i) QVERIFY(std::isnan(samples.values[i]));
    QCOMPARE(samples.values[3], 12.0);
  }

  void invalidIndicesAndSamplesAreRejected_data()
  {
    QTest::addColumn<QByteArray>("rows");
    QTest::newRow("invalid-depth") << QByteArray("oops,10\n1001,20\n");
    QTest::newRow("missing-depth") << QByteArray(",10\n1001,20\n");
    QTest::newRow("duplicate-depth") << QByteArray("1000,10\n1000,20\n");
    QTest::newRow("reversed-depth") << QByteArray("1001,10\n1000,20\n");
    QTest::newRow("illegal-value") << QByteArray("1000,bad\n1001,20\n");
    QTest::newRow("row-width") << QByteArray("1000,10,extra\n1001,20\n");
    QTest::newRow("missing-separator") << QByteArray("1000\n1001,20\n");
  }
  void invalidIndicesAndSamplesAreRejected()
  {
    QFETCH(QByteArray, rows);
    Harness h;
    const QString path = h.source(QStringLiteral("broken.csv"), ImportUxFixtures::csv("A1", "RES[ohm.m]", rows));
    QString error;
    const auto preview = h.service->previewFiles({path}, &error);
    QCOMPARE(preview.size(), 1);
    QVERIFY(preview.first().importBlocked);
    QVERIFY(preview.first().issues.join('\n').contains(QStringLiteral("broken.csv")));
    QCOMPARE(h.service->importProjectFileEx(path, &error).outcome, Outcome::Failed);
    QVERIFY2(error.contains(QStringLiteral("broken.csv")), qPrintable(error));
    QVERIFY2(error.contains(QStringLiteral("行")), qPrintable(error));
    QVERIFY(h.catalog()->entities().isEmpty());
    QVERIFY(h.catalog()->assets().isEmpty());
    QVERIFY(h.catalog()->versions().isEmpty());
    QVERIFY(rawFiles(h.project).isEmpty());
  }

  void malformedXmlIsBlockedWithoutLosingValidBatchMember()
  {
    Harness h;
    QByteArray broken = ImportUxFixtures::xml();
    broken.chop(20);
    const QString bad = h.source(QStringLiteral("broken.xml"), broken);
    const QString good = h.source(QStringLiteral("good.csv"), ImportUxFixtures::csv());
    QString error;
    DataImportService::ImportOptions options;
    options.forceType = QStringLiteral("well_log");
    QCOMPARE(h.service->importProjectFileEx(bad, options, &error).outcome, Outcome::Failed);
    QVERIFY(!error.isEmpty());
    QVERIFY(h.catalog()->entities().isEmpty());
    const auto rows = h.service->importFiles({bad, good}, &error, {{bad, QStringLiteral("well_log")}});
    QCOMPARE(rows.size(), 2);
    int imported = 0, failed = 0;
    for (const auto &row : rows)
    {
      if (row.path == bad) { QCOMPARE(row.outcome, RowOutcome::Failed); QVERIFY(!row.message.isEmpty()); ++failed; }
      if (row.path == good) { QCOMPARE(row.outcome, RowOutcome::Imported); ++imported; }
    }
    QCOMPARE(imported, 1);
    QCOMPARE(failed, 1);
    QCOMPARE(h.catalog()->assets().size(), 1);
    QCOMPARE(h.curves().size(), 1);
  }

  void cancelledSessionAndPlanCommitNothing()
  {
    Harness h;
    const QString first = h.source(QStringLiteral("one.csv"), ImportUxFixtures::csv());
    const QString second = h.source(QStringLiteral("two.csv"), ImportUxFixtures::csv("B1"));
    QString error;
    auto session = h.service->beginImport();
    DataImportService::produceFiles(*session, {first, second}, {}, {});
    QCOMPARE(session->rows.size(), 2);
    session->cancelled = true;
    QCOMPARE(h.service->commitImport(*session, &error), Commit::Failed);
    QVERIFY(error.contains(QStringLiteral("取消")));
    QVERIFY(h.catalog()->entities().isEmpty());
    QVERIFY(rawFiles(h.project).isEmpty());
    const auto plan = buildIngestPlanForFiles({first, second}, *h.catalog());
    int progressCalls = 0;
    const auto rows = executeIngestPlan(plan, *h.service,
        [&progressCalls](int, int, const QString &) { ++progressCalls; return false; }, &error);
    QCOMPARE(progressCalls, 1);
    QVERIFY2(error.contains(QStringLiteral("取消")), qPrintable(error));
    for (const auto &row : rows) QVERIFY(row.outcome != RowOutcome::Imported);
    QVERIFY(h.catalog()->entities().isEmpty());
    QVERIFY(h.catalog()->assets().isEmpty());
    QVERIFY(rawFiles(h.project).isEmpty());
  }

  void destinationCollisionRollsBackAndCanRetry()
  {
    Harness h;
    const QString path = h.source(QStringLiteral("one.csv"), ImportUxFixtures::csv());
    auto session = h.service->beginImport();
    DataImportService::produceFiles(*session, {path}, {}, {});
    QCOMPARE(session->rows.size(), 1);
    QCOMPARE(session->rows.first().outcome, RowOutcome::Imported);
    const auto versions = session->cat->versions();
    QCOMPARE(versions.size(), 1);
    const QString destination = DataCatalog::resolvedVersionPath(h.project, versions.first());
    QVERIFY(ImportUxFixtures::write(destination, "protected existing bytes"));
    QSignalSpy imported(h.service.get(), &DataImportService::imported);
    QString error;
    QCOMPARE(h.service->commitImport(*session, &error), Commit::Failed);
    QVERIFY2(error.contains(QStringLiteral("拒绝覆盖")), qPrintable(error));
    QCOMPARE(imported.count(), 0);
    QVERIFY(h.catalog()->entities().isEmpty());
    QVERIFY(h.catalog()->assets().isEmpty());
    QVERIFY(h.catalog()->versions().isEmpty());
    QCOMPARE(bytesAt(destination), QByteArray("protected existing bytes"));
    QVERIFY(QFile::remove(destination));
    QVERIFY2(!h.service->importProjectFile(path, &error).isEmpty(), qPrintable(error));
    QCOMPARE(h.curves().size(), 1);
    QCOMPARE(rawFiles(h.project).size(), 1);
  }

  void catalogMutationConflictUsesFreshSession()
  {
    Harness h;
    const QString path = h.source(QStringLiteral("one.csv"), ImportUxFixtures::csv());
    auto session = h.service->beginImport();
    DataImportService::produceFiles(*session, {path}, {}, {});
    CatalogEntity other;
    other.id = QStringLiteral("well-other");
    other.entityType = QStringLiteral("well");
    other.name = QStringLiteral("Other");
    QString error;
    QVERIFY2(h.catalog()->addEntity(other, &error), qPrintable(error));
    QCOMPARE(h.service->commitImport(*session, &error, true), Commit::Conflict);
    QVERIFY(!error.isEmpty());
    QVERIFY(h.catalog()->assets().isEmpty());
    QVERIFY(rawFiles(h.project).isEmpty());
    session.reset();
    session = h.service->beginImport();
    DataImportService::produceFiles(*session, {path}, {}, {});
    QVERIFY2(h.service->commitImport(*session, &error) == Commit::Committed, qPrintable(error));
    QVERIFY(h.catalog()->hasEntity(other.id));
    QCOMPARE(h.catalog()->entities(QStringLiteral("well")).size(), 2);
    QCOMPARE(h.curves().size(), 1);
  }

  void partialMoveFailureRestoresEarlierFilesAndProtectsCollision()
  {
    Harness h;
    const QString first = h.source(QStringLiteral("one.csv"), ImportUxFixtures::csv("A1"));
    const QString second = h.source(QStringLiteral("two.csv"), ImportUxFixtures::csv("B1"));
    auto session = h.service->beginImport();
    DataImportService::produceFiles(*session, {first, second}, {}, {});
    QCOMPARE(session->rows.size(), 2);
    QStringList destinations;
    for (const auto &version : session->cat->versions())
      destinations << DataCatalog::resolvedVersionPath(h.project, version);
    QCOMPARE(destinations.size(), 2);
    destinations.sort(); // Same ordering as moveStagedFiles.
    const QByteArray protectedBytes("existing collision must survive partial rollback");
    QVERIFY(ImportUxFixtures::write(destinations.last(), protectedBytes));
    QString error;
    QCOMPARE(h.service->commitImport(*session, &error), Commit::Failed);
    QVERIFY2(error.contains(QStringLiteral("拒绝覆盖")), qPrintable(error));
    QVERIFY(!QFile::exists(destinations.first()));
    QCOMPARE(bytesAt(destinations.last()), protectedBytes);
    QVERIFY(h.catalog()->entities().isEmpty());
    QVERIFY(h.catalog()->assets().isEmpty());
    QVERIFY(h.catalog()->versions().isEmpty());
    QVERIFY(QFile::remove(destinations.last()));
    const auto retried = h.service->importFiles({first, second}, &error);
    QCOMPARE(retried.size(), 2);
    for (const auto &row : retried) QCOMPARE(row.outcome, RowOutcome::Imported);
    QCOMPARE(h.catalog()->entities(QStringLiteral("well")).size(), 2);
    QCOMPARE(rawFiles(h.project).size(), 2);
  }

  void interruptedPartialMoveCleansOnlyThisSessionsUnregisteredBytes()
  {
    Harness h;
    QString error;
    const QByteArray original = ImportUxFixtures::csv("A1");
    const QString first = h.source(QStringLiteral("one.csv"), original);
    const QString second = h.source(QStringLiteral("two.csv"), ImportUxFixtures::csv("B1"));
    auto session = h.service->beginImport();
    DataImportService::produceFiles(*session, {first, second}, {}, {});
    const auto versions = session->cat->versions();
    QCOMPARE(versions.size(), 2);
    const QString staged = DataCatalog::resolvedVersionPath(session->stagingRoot, versions.first());
    const QString moved = DataCatalog::resolvedVersionPath(h.project, versions.first());
    QVERIFY(QDir().mkpath(QFileInfo(moved).absolutePath()));
    QVERIFY(QFile::rename(staged, moved));
    session->recoveryPending = true; // Crash after the first atomic move, before journal commit.
    const QString intent = session->stagingRoot + QStringLiteral("/commit-intent.json");
    QVERIFY(QFile::exists(intent));
    session.reset();
    h.reopen();
    QVERIFY2(h.service->catalogWritable(), qPrintable(h.service->catalogOpenError()));
    QVERIFY(!QFile::exists(moved));
    QVERIFY(!QFile::exists(intent));
    QCOMPARE(bytesAt(first), original);
    QVERIFY(h.catalog()->entities().isEmpty());
    QVERIFY(h.catalog()->versions().isEmpty());
    const auto retried = h.service->importFiles({first, second}, &error);
    QCOMPARE(retried.size(), 2);
    for (const auto &row : retried) QCOMPARE(row.outcome, RowOutcome::Imported);
    QCOMPARE(h.catalog()->entities(QStringLiteral("well")).size(), 2);
  }

  void catalogWriteFailureRollsBackMovedBytesAndCanRetry()
  {
    Harness h;
    const QString path = h.source(QStringLiteral("one.csv"), ImportUxFixtures::csv());
    auto session = h.service->beginImport();
    DataImportService::produceFiles(*session, {path}, {}, {});
    QCOMPARE(session->rows.size(), 1);
    QCOMPARE(session->rows.first().outcome, RowOutcome::Imported);
    h.catalog()->debugAbortJournalAfter(1);
    QString error;
    QCOMPARE(h.service->commitImport(*session, &error), Commit::Failed);
    QVERIFY(!error.isEmpty());
    QVERIFY(h.catalog()->entities().isEmpty());
    QVERIFY(h.catalog()->assets().isEmpty());
    QVERIFY(h.catalog()->versions().isEmpty());
    QVERIFY(rawFiles(h.project).isEmpty());
    h.catalog()->debugAbortJournalAfter(0);
    QVERIFY2(!h.service->importProjectFile(path, &error).isEmpty(), qPrintable(error));
    QCOMPARE(h.curves().size(), 1);
  }

  void interruptedUnregisteredBytesAreRemovedOnReopen()
  {
    Harness h;
    const QString path = h.source(QStringLiteral("one.csv"), ImportUxFixtures::csv());
    auto session = h.service->beginImport();
    DataImportService::produceFiles(*session, {path}, {}, {});
    QStringList moved;
    QString error;
    QVERIFY2(interruptAfterMove(*session, moved, &error), qPrintable(error));
    const QString intent = session->stagingRoot + QStringLiteral("/commit-intent.json");
    session.reset();
    QVERIFY(QFile::exists(intent));
    QVERIFY(QFile::exists(moved.first()));
    h.reopen();
    QVERIFY2(h.service->catalogWritable(), qPrintable(h.service->catalogOpenError()));
    QVERIFY(!QFile::exists(intent));
    for (const auto &file : moved) QVERIFY(!QFile::exists(file));
    QVERIFY(h.catalog()->entities().isEmpty());
    QVERIFY(h.catalog()->assets().isEmpty());
    QVERIFY2(!h.service->importProjectFile(path, &error).isEmpty(), qPrintable(error));
    QCOMPARE(h.curves().size(), 1);
  }

  void interruptedRegisteredBytesSurviveReopen()
  {
    Harness h;
    const QByteArray source = ImportUxFixtures::csv();
    const QString path = h.source(QStringLiteral("one.csv"), source);
    auto session = h.service->beginImport();
    DataImportService::produceFiles(*session, {path}, {}, {});
    QStringList moved;
    QString error;
    QVERIFY2(interruptAfterMove(*session, moved, &error), qPrintable(error));
    QVERIFY2(h.catalog()->applyJournal(session->cat->journal(), &error), qPrintable(error));
    const QString originalWell = h.wellId();
    const QString originalVersion = h.catalog()->versions().first().id;
    session.reset();
    h.reopen();
    QVERIFY2(h.service->catalogWritable(), qPrintable(h.service->catalogOpenError()));
    QCOMPARE(bytesAt(moved.first()), source);
    QCOMPARE(h.wellId(), originalWell);
    QCOMPARE(h.catalog()->versions().first().id, originalVersion);
    QCOMPARE(h.curves().size(), 1);
  }

  void recoveryRefusesTamperedBytesAndPreservesEvidence()
  {
    Harness h;
    const QString path = h.source(QStringLiteral("one.csv"), ImportUxFixtures::csv());
    auto session = h.service->beginImport();
    DataImportService::produceFiles(*session, {path}, {}, {});
    QStringList moved;
    QString error;
    QVERIFY2(interruptAfterMove(*session, moved, &error), qPrintable(error));
    const QString intent = session->stagingRoot + QStringLiteral("/commit-intent.json");
    const QByteArray intentBytes = bytesAt(intent);
    QVERIFY(QFile::setPermissions(moved.first(), QFile::permissions(moved.first()) | QFileDevice::WriteOwner));
    QVERIFY(ImportUxFixtures::write(moved.first(), "unexpected replacement bytes"));
    session.reset();
    h.reopen();
    QVERIFY(!h.service->catalogWritable());
    QVERIFY(!h.service->catalogOpenError().isEmpty());
    QCOMPARE(bytesAt(intent), intentBytes);
    QCOMPARE(bytesAt(moved.first()), QByteArray("unexpected replacement bytes"));
    QVERIFY(h.catalog()->assets().isEmpty());
    QVERIFY(h.catalog()->entities().isEmpty());
    QCOMPARE(h.service->importProjectFileEx(path, &error).outcome, Outcome::Failed);
  }

  void reopenKeepsIdentityAndCatalogRemovalRefreshesBusinessIndex()
  {
    Harness h;
    QString error;
    const QString xml = h.source(QStringLiteral("complete.xml"), ImportUxFixtures::xml());
    const QString csv = h.source(QStringLiteral("RES.csv"), ImportUxFixtures::csv());
    const QString xmlId = h.service->importProjectFile(xml, &error);
    QVERIFY2(!xmlId.isEmpty(), qPrintable(error));
    const QString csvId = h.service->importProjectFile(csv, &error);
    QVERIFY2(!csvId.isEmpty(), qPrintable(error));
    const QString well = h.wellId();
    const QString originalVersion = h.catalog()->currentVersion(csvId).id;
    h.reopen();
    QVERIFY2(h.service->catalogWritable(), qPrintable(h.service->catalogOpenError()));
    QCOMPARE(h.wellId(), well);
    QCOMPARE(h.catalog()->currentVersion(csvId).id, originalVersion);
    QCOMPARE(h.curves().size(), 4);
    QSignalSpy changed(h.catalog(), &DataCatalog::changed);
    QVERIFY2(h.catalog()->removeAsset(csvId, &error), qPrintable(error));
    QVERIFY(changed.count() > 0);
    QCOMPARE(h.curves().size(), 3);
    QVERIFY(findCurve(h.curves(), QStringLiteral("RES")).path.isEmpty());
    QVERIFY(!findCurve(h.curves(), QStringLiteral("GR")).path.isEmpty());
    h.reopen();
    QCOMPARE(h.curves().size(), 3);
    QCOMPARE(h.wellId(), well);
  }

  void retypedReferenceCannotAttachLegacyLogByFilename()
  {
    Harness h;
    const QString path = h.source(QStringLiteral("A1.las"), QByteArray(
        "~Version Information\nVERS. 2.0:\nWRAP. NO:\n~Well\nWELL. B1 : WELL\n"
        "~Curve\nDEPT.M :\nGR.API :\n~ASCII\n1000 30\n1001 40\n"));
    QVERIFY(!path.isEmpty());
    CatalogEntity well;
    well.id = QStringLiteral("legacy-well");
    well.entityType = QStringLiteral("well");
    well.name = QStringLiteral("A1");
    CatalogAsset asset;
    asset.id = QStringLiteral("legacy-log");
    asset.type = QStringLiteral("well_log");
    asset.format = QStringLiteral("las");
    CatalogVersion version;
    version.id = QStringLiteral("legacy-version");
    version.assetId = asset.id;
    version.stage = QStringLiteral("RAW");
    version.path = path;
    version.managed = false;
    version.sourceUri = path;
    QString error;
    version.sha256 = DataCatalog::sha256FileHex(path, &error);
    EntityAssetLink link;
    link.assetId = asset.id;
    link.entityType = QStringLiteral("well");
    link.role = QStringLiteral("well_log");
    link.unresolved = true;
    QVERIFY2(h.catalog()->addEntity(well, &error), qPrintable(error));
    QVERIFY2(h.catalog()->addAsset(asset, &error), qPrintable(error));
    QVERIFY2(h.catalog()->addVersion(version, &error), qPrintable(error));
    QVERIFY2(h.catalog()->addLink(link, &error), qPrintable(error));
    DataImportService::ImportOptions options;
    options.forceType = QStringLiteral("reference");
    const auto result = h.service->importProjectFileEx(path, options, &error);
    QCOMPARE(result.outcome, Outcome::AlreadyStored);
    QVERIFY(!result.linkAttached);
    const auto links = h.catalog()->linksForAsset(asset.id);
    QCOMPARE(links.size(), 1);
    QVERIFY(links.first().unresolved);
    QVERIFY(links.first().entityId.isEmpty());
    QCOMPARE(h.catalog()->entities(QStringLiteral("well")).size(), 1);
  }

  void oldLasProjectWithoutNewMetadataRemainsReadable()
  {
    Harness h;
    const QByteArray legacyBytes(
        "~Version Information\nVERS. 2.0:\nWRAP. NO:\n~Well\n"
        "STRT.M 1000:\nSTOP.M 1001:\nSTEP.M 1:\nNULL. -999.25:\nWELL. A1 : WELL\n"
        "~Curve\nDEPT.M :\nGR.API :\n~ASCII\n1000 30\n1001 40\n");
    const QString las = h.source(QStringLiteral("legacy.las"), legacyBytes);
    QVERIFY(!las.isEmpty());
    CatalogEntity well;
    well.id = QStringLiteral("legacy-well");
    well.entityType = QStringLiteral("well");
    well.name = QStringLiteral("A1");
    CatalogAsset asset;
    asset.id = QStringLiteral("ast-40");
    asset.type = QStringLiteral("well_log");
    asset.format = QStringLiteral("las");
    asset.displayName = QStringLiteral("legacy");
    CatalogVersion version;
    version.id = QStringLiteral("ver-40");
    version.assetId = asset.id;
    version.stage = QStringLiteral("RAW");
    version.managed = false;
    version.path = las;
    version.fileName = QStringLiteral("legacy.las");
    EntityAssetLink link;
    link.entityType = QStringLiteral("well");
    link.entityId = well.id;
    link.assetId = asset.id;
    link.role = QStringLiteral("well_log");
    QString error;
    QVERIFY2(h.catalog()->addEntity(well, &error), qPrintable(error));
    QVERIFY2(h.catalog()->addAsset(asset, &error), qPrintable(error));
    QVERIFY2(h.catalog()->addVersion(version, &error), qPrintable(error));
    QVERIFY2(h.catalog()->addLink(link, &error), qPrintable(error));
    h.reopen();
    QVERIFY2(h.service->catalogWritable(), qPrintable(h.service->catalogOpenError()));
    QCOMPARE(h.wellId(), well.id);
    QCOMPARE(h.curves().size(), 1);
    QCOMPARE(h.catalog()->currentVersion(asset.id).extra.size(), 0);
    WellLogSet::WellCurveTvdSamples samples;
    QVERIFY2(WellLogSet::readCurveTvd(h.curves().first(), nullptr, &samples, &error), qPrintable(error));
    QCOMPARE(samples.values, QVector<double>({30, 40}));
    const QString csv = h.source(QStringLiteral("RES.csv"), ImportUxFixtures::csv());
    QVERIFY2(!h.service->importProjectFile(csv, &error).isEmpty(), qPrintable(error));
    QCOMPARE(h.wellId(), well.id);
    QCOMPARE(h.catalog()->entities(QStringLiteral("well")).size(), 1);
    QCOMPARE(h.curves().size(), 2);
    QCOMPARE(h.catalog()->currentVersion(asset.id).id, version.id);
    QCOMPARE(bytesAt(las), legacyBytes);
  }
};

QTEST_GUILESS_MAIN(TestImportUx)
#include "tst_import_ux.moc"
