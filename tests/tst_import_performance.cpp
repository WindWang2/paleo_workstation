#include <QtTest>
#include <QElapsedTimer>
#include <QTemporaryDir>
#include <QThread>
#include <QTimer>
#include <atomic>
#include <cmath>
#include <memory>

#ifdef Q_OS_WIN
#ifndef NOMINMAX
#define NOMINMAX
#endif
#include <windows.h>
#include <psapi.h>
#endif

#include "../src/catalog/datacatalog.h"
#include "../src/io/dataimportservice.h"
#include "../src/io/welllogread.h"
#include "../src/metadata/paleoprojectstore.h"
#include "../src/services/paleotaskservice.h"
#include "../src/services/welllogset.h"
#include "../src/workflow/folderimport.h"
#include "importuxfixtures.h"
#include "perfbudget_relax.h"

namespace
{
constexpr int kXmlSamples = 20000;
constexpr int kXmlCurves = 8;
constexpr int kCsvFiles = 24;
constexpr int kCsvSamples = 500;

struct MemoryUsage
{
  // Optional platform observation: -1 means unavailable, not a test failure.
  qint64 residentKiB = -1;
  qint64 peakKiB = -1;
};

MemoryUsage processMemory()
{
  MemoryUsage result;
#ifdef Q_OS_WIN
  // Resolve the kernel32 entry point so this optional observation does not
  // introduce a new Psapi link requirement for a portable service test.
  using QueryMemory = BOOL(WINAPI *)(HANDLE, PPROCESS_MEMORY_COUNTERS, DWORD);
  const auto query = reinterpret_cast<QueryMemory>(
      GetProcAddress(GetModuleHandleW(L"kernel32.dll"), "K32GetProcessMemoryInfo"));
  PROCESS_MEMORY_COUNTERS counters{};
  counters.cb = sizeof(counters);
  if (query && query(GetCurrentProcess(), &counters, sizeof(counters)))
  {
    result.residentKiB = qint64(counters.WorkingSetSize) / 1024;
    result.peakKiB = qint64(counters.PeakWorkingSetSize) / 1024;
  }
#elif defined(Q_OS_LINUX)
  QFile status(QStringLiteral("/proc/self/status"));
  if (status.open(QIODevice::ReadOnly))
    for (const auto &line : status.readAll().split('\n'))
    {
      const auto fields = line.simplified().split(' ');
      if (fields.size() < 2) continue;
      if (fields.first() == "VmRSS:") result.residentKiB = fields[1].toLongLong();
      if (fields.first() == "VmHWM:") result.peakKiB = fields[1].toLongLong();
    }
#endif
  return result;
}

void report(const char *phase, qint64 milliseconds)
{
  const auto memory = processMemory();
  qInfo().nospace() << "import-perf phase=" << phase << " elapsed_ms=" << milliseconds
                    << " resident_KiB=" << memory.residentKiB << " peak_KiB=" << memory.peakKiB;
}

QByteArray cell(const QByteArray &text)
{
  return "<Cell><Data ss:Type=\"String\">" + text + "</Data></Cell>";
}

// Stream generation keeps fixture construction separate from the parser's
// memory footprint. This is a full, namespaced SpreadsheetML workbook.
bool writeRepresentativeXml(const QString &path)
{
  QFile file(path);
  if (!file.open(QIODevice::WriteOnly)) return false;
  const auto write = [&file](const QByteArray &bytes) { return file.write(bytes) == bytes.size(); };
  if (!write(QStringLiteral("<?xml version=\"1.0\" encoding=\"UTF-8\"?>\n"
      "<Workbook xmlns=\"urn:schemas-microsoft-com:office:spreadsheet\" "
      "xmlns:ss=\"urn:schemas-microsoft-com:office:spreadsheet\">"
      "<Worksheet ss:Name=\"说明\"><Table><Row><Cell><Data ss:Type=\"String\">"
      "公开可复现性能样例</Data></Cell></Row></Table></Worksheet>"
      "<Worksheet ss:Name=\"测井曲线-A1井\"><Table>\n<Row>").toUtf8())) return false;
  QByteArray headers = cell("MD[m]");
  for (int c = 1; c <= kXmlCurves; ++c) headers += cell("C" + QByteArray::number(c) + "[API]");
  if (!write(headers + "</Row>\n")) return false;
  for (int r = 0; r < kXmlSamples; ++r)
  {
    QByteArray row = "<Row>" + cell(QByteArray::number(1000.0 + r * 0.5, 'f', 1));
    for (int c = 1; c <= kXmlCurves; ++c)
      row += cell((r + c - 1) % 97 == 0 ? QByteArray("NaN")
          : QByteArray::number(c + r * 0.01, 'f', 2));
    if (!write(row + "</Row>\n")) return false;
  }
  return write("</Table></Worksheet></Workbook>\n") && file.flush();
}

QString writeCsv(const QString &directory, int index)
{
  const QString name = QStringLiteral("EXTRA%1").arg(index, 2, 10, QLatin1Char('0'));
  const QString path = QDir(directory).filePath(name + QStringLiteral(".csv"));
  QByteArray rows;
  for (int r = 0; r < kCsvSamples; ++r)
  {
    rows += QByteArray::number(1100.0 + r * 0.37, 'f', 2) + ',';
    rows += r % 41 == 0 ? QByteArray("NaN") : QByteArray::number(index + r * 0.1, 'f', 1);
    rows += '\n';
  }
  return ImportUxFixtures::write(path, ImportUxFixtures::csv("A1", name.toUtf8() + "[mV]", rows))
      ? path : QString();
}

QString onlyWell(const DataCatalog *catalog)
{
  const auto wells = catalog->entities(QStringLiteral("well"));
  return wells.size() == 1 ? wells.first().id : QString();
}

WellCurveRef named(const QVector<WellCurveRef> &refs, const QString &name)
{
  for (const auto &ref : refs) if (ref.mnemonic == name) return ref;
  return {};
}
}

class TestImportPerformance : public QObject
{
  Q_OBJECT
private slots:
  void representativeXmlBatchQueriesAndCancellation()
  {
    QElapsedTimer total;
    total.start();
    QTemporaryDir temporary;
    QVERIFY(temporary.isValid());
    const QString sourceDir = temporary.filePath(QStringLiteral("sources"));
    const QString project = temporary.filePath(QStringLiteral("project"));
    QVERIFY(QDir().mkpath(sourceDir));
    QVERIFY(QDir().mkpath(project));
    const QString xml = QDir(sourceDir).filePath(QStringLiteral("representative.xml"));
    QVERIFY(writeRepresentativeXml(xml));
    qInfo().nospace() << "import-perf fixture xml_bytes=" << QFileInfo(xml).size()
        << " xml_samples=" << kXmlSamples << " xml_value_curves=" << kXmlCurves
        << " csv_files=" << kCsvFiles << " csv_samples_each=" << kCsvSamples;
    report("baseline", 0);

    QElapsedTimer clock;
    QString error;
    LasHeaderInfo parsed;
    QList<LasCurve> curves;
    clock.start();
    QVERIFY2(WellLogRead::parse(xml, parsed, curves, &error), qPrintable(error));
    report("parse_xml", clock.elapsed());
    QCOMPARE(parsed.wellName, QStringLiteral("A1"));
    QCOMPARE(parsed.indexBasis, QStringLiteral("MD"));
    QCOMPARE(curves.size(), kXmlCurves + 1);
    for (const auto &curve : curves) QCOMPARE(curve.values.size(), kXmlSamples);
    QCOMPARE(curves.first().values.first(), 1000.0);
    QCOMPARE(curves.first().values.last(), 1000.0 + (kXmlSamples - 1) * 0.5);
    QVERIFY(std::isnan(curves[1].values[0]));
    QVERIFY(std::abs(curves[1].values[1] - 1.01) < 1e-12);
    QVERIFY(std::isnan(curves[1].values[97]));
    curves.clear();

    PaleoProjectStore store;
    store.setProjectPaths(project + QStringLiteral("/project.qgz"), project + QStringLiteral("/project.gpkg"),
                          project + QStringLiteral("/metadata/project.sqlite"));
    DataImportService imports(&store);
    imports.setProjectDir(project);
    QVERIFY2(imports.catalogWritable(), qPrintable(imports.catalogOpenError()));
    clock.restart();
    const auto preview = imports.previewFiles({xml}, &error);
    report("preview_xml", clock.elapsed());
    QCOMPARE(preview.size(), 1);
    QVERIFY2(!preview.first().importBlocked, qPrintable(preview.first().issues.join('\n')));
    QCOMPARE(preview.first().sourceSha256.size(), 64);
    QVERIFY(imports.catalog()->entities().isEmpty());
    QVERIFY(imports.catalog()->assets().isEmpty());
    clock.restart();
    const QString xmlAsset = imports.importProjectFile(xml, &error);
    report("import_xml", clock.elapsed());
    QVERIFY2(!xmlAsset.isEmpty(), qPrintable(error));
    const QString well = onlyWell(imports.catalog());
    QVERIFY(!well.isEmpty());
    auto refs = WellLogSet::wellCurveIndex(imports.catalog(), project, well);
    QCOMPARE(refs.size(), kXmlCurves);
    WellLogSet::WellCurveTvdSamples xmlSamples;
    QVERIFY2(WellLogSet::readCurveTvd(named(refs, QStringLiteral("C1")), nullptr, &xmlSamples, &error), qPrintable(error));
    QCOMPARE(xmlSamples.md.size(), kXmlSamples);
    QCOMPARE(xmlSamples.values.size(), kXmlSamples);
    QCOMPARE(xmlSamples.md[1] - xmlSamples.md[0], 0.5);
    QVERIFY(std::isnan(xmlSamples.values[97]));

    QStringList paths;
    for (int i = 0; i < kCsvFiles; ++i)
    {
      const QString path = writeCsv(sourceDir, i);
      QVERIFY(!path.isEmpty());
      paths << path;
    }
    clock.restart();
    const auto rows = imports.importFiles(paths, &error);
    report("import_csv_batch", clock.elapsed());
    QCOMPARE(rows.size(), kCsvFiles);
    for (const auto &row : rows)
      QVERIFY2(row.outcome == FolderRowResult::Outcome::Imported, qPrintable(row.path + ": " + row.message));
    QCOMPARE(imports.catalog()->assets().size(), kCsvFiles + 1);
    QCOMPARE(imports.catalog()->entities(QStringLiteral("well")).size(), 1);
    QCOMPARE(imports.catalog()->linksForEntity(well).size(), kCsvFiles + 1);
    refs = WellLogSet::wellCurveIndex(imports.catalog(), project, well);
    QCOMPARE(refs.size(), kXmlCurves + kCsvFiles);
    WellLogSet::WellCurveTvdSamples csvSamples;
    QVERIFY2(WellLogSet::readCurveTvd(named(refs, QStringLiteral("EXTRA00")), nullptr, &csvSamples, &error), qPrintable(error));
    QCOMPARE(csvSamples.md.size(), kCsvSamples);
    QCOMPARE(csvSamples.values.size(), kCsvSamples);
    QVERIFY(std::abs(csvSamples.md[1] - csvSamples.md[0] - 0.37) < 1e-9);
    QVERIFY(std::isnan(csvSamples.values[0]));
    QVERIFY(std::isnan(csvSamples.values[41]));
    // Source sampling is retained: the 500-point CSV is not expanded onto
    // the XML's 20,000-point grid during import or business reads.
    QCOMPARE(xmlSamples.md.size(), kXmlSamples);

    constexpr int queryCount = 25;
    clock.restart();
    for (int i = 0; i < queryCount; ++i)
      QCOMPARE(WellLogSet::wellCurveIndex(imports.catalog(), project, well).size(), kXmlCurves + kCsvFiles);
    report("curve_directory_25_queries", clock.elapsed());

    const auto version = imports.catalog()->currentVersion(xmlAsset);
    const QString managed = imports.absolutePath(xmlAsset);
    const QString unavailable = managed + QStringLiteral(".temporarily-unavailable");
    QVERIFY(QFile::rename(managed, unavailable));
    const bool bodyAbsent = !QFileInfo::exists(managed);
    LasHeaderInfo fromMetadata;
    clock.restart();
    const bool metadataRead = WellLogSet::storedHeader(version, &fromMetadata);
    const qint64 metadataMs = clock.elapsed();
    // Restore before assertions, so even a failed contract check leaves the
    // fixture's managed source in its original state for diagnostics.
    QVERIFY(QFile::rename(unavailable, managed));
    QVERIFY(bodyAbsent);
    QVERIFY(metadataRead);
    QCOMPARE(fromMetadata.curveNames, parsed.curveNames);
    QCOMPARE(fromMetadata.wellName, parsed.wellName);
    QCOMPARE(fromMetadata.indexBasis, parsed.indexBasis);
    report("stored_header_with_body_absent", metadataMs);

    // A fresh catalog makes this a real parse/copy/commit batch, rather than
    // a fast duplicate-skip run. A main-thread timer observes the actual
    // workflow + task-service path while its worker handles the same files.
    const QString asyncProject = temporary.filePath(QStringLiteral("async-project"));
    QVERIFY(QDir().mkpath(asyncProject));
    PaleoProjectStore asyncStore;
    asyncStore.setProjectPaths(asyncProject + QStringLiteral("/project.qgz"),
        asyncProject + QStringLiteral("/project.gpkg"), asyncProject + QStringLiteral("/metadata/project.sqlite"));
    DataImportService asyncImports(&asyncStore);
    asyncImports.setProjectDir(asyncProject);
    QVERIFY2(asyncImports.catalogWritable(), qPrintable(asyncImports.catalogOpenError()));
    QVERIFY(asyncImports.catalog()->assets().isEmpty());
    PaleoTaskService tasks(&asyncStore);
    FolderImportWorkflow workflow(&asyncImports, &tasks);
    struct AsyncObservation
    {
      QElapsedTimer clock;
      std::atomic_bool done{false};
      QVector<FolderRowResult> rows;
      QString error;
      bool callbackOnOwner = false;
      qint64 callbackNs = 0;
      int ticks = 0;
      qint64 lastTickNs = 0;
      qint64 maxGapNs = 0;
    };
    // Shared lifetime also keeps timeout diagnostics safe if the task service
    // must shut down after an assertion failure while a callback is pending.
    const auto observation = std::make_shared<AsyncObservation>();
    QThread *const owner = QThread::currentThread();
    QTimer pulse;
    pulse.setInterval(1);
    QObject::connect(&pulse, &QTimer::timeout, &workflow, [observation] {
      if (observation->done.load()) return;
      const qint64 now = observation->clock.nsecsElapsed();
      observation->maxGapNs = qMax(observation->maxGapNs, now - observation->lastTickNs);
      observation->lastTickNs = now;
      ++observation->ticks;
    });
    DataCatalog::resetThreadViolationCount();
    observation->clock.start();
    pulse.start();
    workflow.importFiles(QStringList{xml} + paths, {}, {},
        [observation, owner](const QVector<FolderRowResult> &result, const QString &message,
                          bool cancelled) {
          Q_UNUSED(cancelled)
          observation->callbackOnOwner = QThread::currentThread() == owner;
          observation->rows = result;
          observation->error = message;
          observation->callbackNs = observation->clock.nsecsElapsed();
          observation->done.store(true);
        });
    QVERIFY(!observation->done.load());
    QTRY_VERIFY_WITH_TIMEOUT(observation->done.load(), paleo::perfbudget::relaxedBudgetMs(60000));
    pulse.stop();
    observation->maxGapNs = qMax(observation->maxGapNs,
                                 observation->callbackNs - observation->lastTickNs);
    qInfo().nospace() << "import-perf phase=async_batch_owner_event_loop elapsed_ms="
        << observation->callbackNs / 1000000.0 << " timer_ticks=" << observation->ticks
        << " max_owner_gap_ms=" << observation->maxGapNs / 1000000.0;
    report("async_batch_xml_and_24_csv", observation->callbackNs / 1000000);
    QVERIFY(observation->callbackOnOwner);
    QVERIFY2(observation->ticks > 0, "the owner event loop did not process any timer ticks during the batch");
    QVERIFY2(observation->error.isEmpty(), qPrintable(observation->error));
    QCOMPARE(observation->rows.size(), kCsvFiles + 1);
    for (const auto &row : observation->rows)
      QVERIFY2(row.outcome == FolderRowResult::Outcome::Imported, qPrintable(row.path + ": " + row.message));
    QCOMPARE(asyncImports.catalog()->assets().size(), kCsvFiles + 1);
    QCOMPARE(asyncImports.catalog()->entities(QStringLiteral("well")).size(), 1);
    QCOMPARE(WellLogSet::wellCurveIndex(asyncImports.catalog(), asyncProject,
                                      onlyWell(asyncImports.catalog())).size(), kXmlCurves + kCsvFiles);
    QCOMPARE(DataCatalog::threadViolationCount(), 0);
    QVERIFY(tasks.shutdown(5000));

    const QString cancelProject = temporary.filePath(QStringLiteral("cancel-project"));
    QVERIFY(QDir().mkpath(cancelProject));
    PaleoProjectStore cancelStore;
    cancelStore.setProjectPaths(cancelProject + QStringLiteral("/project.qgz"),
        cancelProject + QStringLiteral("/project.gpkg"), cancelProject + QStringLiteral("/metadata/project.sqlite"));
    DataImportService cancelling(&cancelStore);
    cancelling.setProjectDir(cancelProject);
    QVERIFY2(cancelling.catalogWritable(), qPrintable(cancelling.catalogOpenError()));
    auto session = cancelling.beginImport();
    QVERIFY(session);
    clock.restart();
    DataImportService::produceFiles(*session, QStringList{xml} + paths.mid(0, 4), {}, {});
    report("produce_cancellable_xml_and_4_csv", clock.elapsed());
    QCOMPARE(session->rows.size(), 5);
    for (const auto &row : session->rows)
      QVERIFY2(row.outcome == FolderRowResult::Outcome::Imported, qPrintable(row.message));
    const QString staging = session->stagingRoot;
    QVERIFY(QDir(staging).exists());
    QVERIFY(session->intentPrepared);
    QVERIFY(!session->recoveryPending);
    session->cancelled = true;
    clock.restart();
    QCOMPARE(cancelling.commitImport(*session, &error), DataImportService::CommitStatus::Failed);
    session.reset();
    report("cancel_and_release_staging", clock.elapsed());
    QVERIFY(error.contains(QStringLiteral("取消")));
    QVERIFY(!QDir(staging).exists());
    QVERIFY(!QDir(cancelProject + QStringLiteral("/artifacts/staging")).exists());
    QVERIFY(cancelling.catalog()->entities().isEmpty());
    QVERIFY(cancelling.catalog()->assets().isEmpty());
    QVERIFY(cancelling.catalog()->versions().isEmpty());
    QVERIFY(cancelling.catalog()->links().isEmpty());
    QVERIFY(!QDir(cancelProject + QStringLiteral("/artifacts/raw")).exists());
    report("complete", total.elapsed());
  }
};

QTEST_GUILESS_MAIN(TestImportPerformance)
#include "tst_import_performance.moc"
