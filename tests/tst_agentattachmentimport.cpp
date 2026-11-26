// 层：测试壳
#include "../src/io/dataimportservice.h"
#include "../src/metadata/paleoprojectstore.h"
#include "../src/metadata/paleoprojectfile.h"
#include "../src/services/dataexport.h"
#include "../src/services/paleotaskservice.h"
#include "../src/workflow/folderimport.h"

#include <QFile>
#include <QJsonDocument>
#include <QJsonObject>
#include <QTemporaryDir>
#include <QtTest>

namespace {
struct ImportProject
{
  QTemporaryDir directory;
  PaleoProjectStore store;
  DataImportService service{&store};

  ImportProject()
  {
    store.setProjectPaths(directory.filePath("project.qgz"), directory.filePath("project.gpkg"),
                          directory.filePath("metadata/project.sqlite"));
    service.setProjectDir(directory.path());
  }
};

QByteArray las(const QByteArray &well)
{
  return "~Version Information\nVERS. 2.0:\nWRAP. NO:\n~Well\nWELL. " + well +
         " : WELL\n~Curve\nDEPT.M :\nGR.API :\n~A DEPT GR\n100.0 50.0\n101.0 51.0\n";
}

bool writeFile(const QString &path, const QByteArray &bytes)
{
  QFile file(path);
  return file.open(QIODevice::WriteOnly) && file.write(bytes) == bytes.size();
}

QString exportOriginalLog(ImportProject &sender, const QString &destination)
{
  QString error;
  const auto project = projectFileForQgz(sender.directory.filePath("project.qgz"));
  if (!writeProjectFile(sender.directory.path(), project, &error)) return error;
  const QString log = sender.directory.filePath("A1.las");
  if (!writeFile(log, las("A1"))) return QStringLiteral("Could not write LAS fixture");
  const auto asset = sender.service.importFile("well_log", log, &error);
  if (asset.isEmpty()) return error;
  paleo::exporting::DataRequest request;
  request.scope = paleo::exporting::Scope::Original;
  request.versionIds = {sender.service.catalog()->currentVersion(asset).id};
  request.destination = destination;
  const auto batch = paleo::exporting::execute(paleo::exporting::plan(*sender.service.catalog(), sender.directory.path(), request));
  return batch.complete() ? QString() : QStringLiteral("Export failed: ") + batch.error + batch.failures.join("\n");
}
}

class TestAgentAttachmentImport : public QObject
{
  Q_OBJECT
private slots:
  void packageIsReceivedBeforeParsingAndRetriesReuseOriginal()
  {
    ImportProject sender;
    ImportProject receiver;
    QTemporaryDir delivery;
    const auto package = delivery.filePath("package");
    const QString error = exportOriginalLog(sender, package);
    QVERIFY2(error.isEmpty(), qPrintable(error));

    FolderImportWorkflow imports(&receiver.service, nullptr);
    QString durablePath;
    for (int attempt = 0; attempt < 2; ++attempt) {
      QStringList events;
      imports.importAgentAttachments({package}, [] { return true; },
        [&](const auto &rows, const QString &why, bool cancelled) {
          QVERIFY2(why.isEmpty(), qPrintable(why));
          QVERIFY(!cancelled);
          QCOMPARE(rows.size(), 1);
          QVERIFY2(!rows.front().assetId.isEmpty(), qPrintable(rows.front().message));
          QCOMPARE(rows.front().path, durablePath);
          events << "parsed";
        }, {}, [&](const QStringList &files, qsizetype reused, const QString &why, bool cancelled) {
          QVERIFY2(why.isEmpty(), qPrintable(why));
          QVERIFY(!cancelled);
          QCOMPARE(files.size(), 1);
          QCOMPARE(reused, qsizetype(attempt));
          QCOMPARE(receiver.service.catalog()->assets().size(), qsizetype(attempt));
          durablePath = files.front();
          QVERIFY(QFileInfo::exists(durablePath));
          events << "received";
        });
      QCOMPARE(events, QStringList({"received", "parsed"}));
    }
    QCOMPARE(receiver.service.catalog()->assets().size(), 1);
    const auto localVersion = receiver.service.catalog()->currentVersion(receiver.service.catalog()->assets().front().id);
    const auto inbox = QFileInfo(durablePath).dir();
    const auto records = inbox.entryList({QStringLiteral("import-*.json")}, QDir::Files);
    QVERIFY(!records.isEmpty());
    for (const auto &name : records) {
      QFile file(inbox.filePath(name));
      QVERIFY(file.open(QIODevice::ReadOnly));
      const auto parsed = QJsonDocument::fromJson(file.readAll()).object();
      QCOMPARE(parsed.value("receipt_id").toString(), inbox.dirName());
      QCOMPARE(parsed.value("source_sha256").toString(), localVersion.sha256);
      QCOMPARE(parsed.value("local_raw_version_id").toString(), localVersion.id);
    }
  }

  void switchingProjectKeepsOriginalReceiptAndSkipsParsing()
  {
    ImportProject sender, receiver;
    QTemporaryDir delivery, nextProject;
    const QString package = delivery.filePath("package");
    const QString error = exportOriginalLog(sender, package);
    QVERIFY2(error.isEmpty(), qPrintable(error));

    PaleoTaskService tasks;
    FolderImportWorkflow imports(&receiver.service, &tasks);
    QSignalSpy previews(&imports, &FolderImportWorkflow::previewActiveChanged);
    connect(&tasks, &PaleoTaskService::taskAdded, this, [&](PaleoTask *task) {
      // Switch after the worker publishes the file and before the workflow
      // decides whether that durable receipt can be parsed in this session.
      connect(task, &PaleoTask::finished, this, [&] { receiver.service.setProjectDir(nextProject.path()); });
    });
    const auto current = [&] { return receiver.service.projectDir() == receiver.directory.path(); };
    QString durablePath;
    for (int attempt = 0; attempt < 2; ++attempt) {
      receiver.service.setProjectDir(receiver.directory.path());
      bool finished = false;
      QStringList received;
      qsizetype reused = -1;
      imports.importAgentAttachments({package}, current,
        [&](const auto &rows, const QString &, bool) { finished = true; QVERIFY(rows.isEmpty()); }, {},
        [&](const QStringList &files, qsizetype count, const QString &detail, bool cancelled) {
          received = files;
          reused = count;
          QVERIFY(detail.contains(QStringLiteral("工程已切换")));
          QVERIFY(!cancelled);
        });
      QTRY_VERIFY_WITH_TIMEOUT(finished, 15000);
      QCOMPARE(received.size(), 1);
      QCOMPARE(reused, qsizetype(attempt));
      if (!durablePath.isEmpty()) QCOMPARE(received.front(), durablePath);
      durablePath = received.front();
      QVERIFY(QFileInfo::exists(durablePath));
      QCOMPARE(previews.count(), 0);
      QCOMPARE(receiver.service.projectDir(), nextProject.path());
      QVERIFY(receiver.service.catalog()->assets().isEmpty());
    }
  }

  void importsByContentAndReviewsOnlyAmbiguousFiles()
  {
    ImportProject project;
    QTemporaryDir source;
    auto &service = project.service;
    FolderImportWorkflow imports(&service, nullptr);
    const QString log = source.filePath("incoming.bin");
    const QString unknown = source.filePath("unknown.blob");
    const QString broken = source.filePath("broken.las");
    const QString noWell = source.filePath("ambiguous.csv");
    QVERIFY(writeFile(log, las("A1")));
    QVERIFY(writeFile(unknown, "unrecognized sample"));
    QVERIFY(writeFile(broken, "damaged LAS"));
    QVERIFY(writeFile(noWell, "DEPTH[M],GR[API]\n1,25\n2,26\n"));
    QVector<FolderRowResult> reports;
    QStringList events;
    QVector<FolderPreviewRow> review;
    const auto record = [&](const QVector<FolderRowResult> &rows, const QString &error, bool cancelled) {
      QVERIFY2(error.isEmpty(), qPrintable(error));
      QVERIFY(!cancelled);
      reports = rows;
      events << "done";
    };
    imports.importAgentAttachments({log, unknown, broken, noWell}, [] { return true; }, record,
      [&](const auto &rows) { review = rows; events << "review"; });
    QCOMPARE(events, QStringList({"done", "review"}));
    QCOMPARE(reports.size(), 2);
    QCOMPARE(review.size(), 2);
    QStringList reviewPaths;
    for (const auto &row : review) reviewPaths << row.path;
    QVERIFY(reviewPaths.contains(unknown));
    QVERIFY(reviewPaths.contains(noWell));
    QString assetId;
    for (const auto &row : reports) {
      if (row.path == log) {
        QCOMPARE(row.classifiedType, QStringLiteral("well_log"));
        QCOMPARE(row.outcome, FolderRowResult::Outcome::Imported);
        assetId = row.assetId;
      } else if (row.path == broken) {
        QCOMPARE(row.outcome, FolderRowResult::Outcome::Failed);
        QVERIFY(!row.message.isEmpty());
      }
    }
    QVERIFY(!assetId.isEmpty());
    QCOMPARE(service.catalog()->assetById(assetId).type, QStringLiteral("well_log"));
    QCOMPARE(service.catalog()->assets().size(), 1);
    imports.importAgentAttachments({log}, [] { return true; }, record);
    QCOMPARE(reports.size(), 1);
    QCOMPARE(reports.first().assetId, assetId);
    QCOMPARE(reports.first().outcome, FolderRowResult::Outcome::Skipped);
    QCOMPARE(service.catalog()->assets().size(), 1);
  }

  void identifiesOnlyRootGeoJsonInJsonAttachments()
  {
    ImportProject project;
    QTemporaryDir source;
    auto &service = project.service;
    FolderImportWorkflow imports(&service, nullptr);
    const QString map = source.filePath("facies.json");
    const QString settings = source.filePath("project.json");
    const QString invalid = source.filePath("invalid.json");
    const QByteArray feature =
        R"({"type":"Feature","geometry":{"type":"Point","coordinates":[1,2]},"properties":{}})";
    // Put the type beyond the initial 64 KiB probe and after the feature array.
    const QByteArray json = "{\"features\":[" + feature + "],\"note\":\"" +
                            QByteArray(70000, 'x') + "\",\"type\":\"FeatureCollection\"}";
    QVERIFY(writeFile(map, json));
    QVERIFY(writeFile(settings, "{\"project\":\"demo\",\"map\":" + json + "}"));
    QVERIFY(writeFile(invalid, R"({"type":"FeatureCollection","features":[{"label":"Feature"}]})"));
    QVector<FolderRowResult> results;
    QVector<FolderPreviewRow> review;
    imports.importAgentAttachments({map, settings, invalid}, [] { return true; },
      [&](const auto &rows, const QString &error, bool cancelled) {
        QVERIFY2(error.isEmpty(), qPrintable(error));
        QVERIFY(!cancelled);
        results = rows;
      }, [&](const auto &rows) { review = rows; });
    QCOMPARE(results.size(), 1);
    QCOMPARE(results.first().path, map);
    QCOMPARE(results.first().outcome, FolderRowResult::Outcome::Imported);
    QCOMPARE(results.first().classifiedType, QStringLiteral("geojson"));
    QCOMPARE(service.catalog()->assetById(results.first().assetId).type, QStringLiteral("geojson"));
    QCOMPARE(service.catalog()->assets().size(), 1);
    QCOMPARE(review.size(), 2);
    for (const auto &row : review) {
      QVERIFY(row.path == settings || row.path == invalid);
      QCOMPARE(row.classifiedType, QStringLiteral("unknown"));
    }
  }

  void rejectsChangedBytesAndProject()
  {
    ImportProject project;
    QTemporaryDir source;
    auto &service = project.service;
    FolderImportWorkflow imports(&service, nullptr);
    const QString path = source.filePath("incoming.bin");
    QVERIFY(writeFile(path, las("A1")));
    auto change = connect(&imports, &FolderImportWorkflow::importActiveChanged, this,
      [&](bool active) { if (active) QVERIFY(writeFile(path, las("B1"))); });
    int completions = 0;
    imports.importAgentAttachments({path}, [] { return true; }, [&](const auto &rows, const QString &, bool) {
      ++completions;
      QCOMPARE(rows.size(), 1);
      QCOMPARE(rows.first().outcome, FolderRowResult::Outcome::Failed);
    });
    QCOMPARE(completions, 1);
    QVERIFY(service.catalog()->assets().isEmpty());
    disconnect(change);
    bool current = true;
    connect(&imports, &FolderImportWorkflow::importActiveChanged, this,
            [&](bool active) { if (active) current = false; });
    imports.importAgentAttachments({path}, [&] { return current; }, [&](const auto &rows, const QString &error, bool) {
      ++completions;
      QVERIFY(!error.isEmpty());
      QVERIFY(rows.isEmpty());
    });
    QCOMPARE(completions, 2);
    QVERIFY(service.catalog()->assets().isEmpty());
  }

  void cancellingCompletedPreviewStartsNoImport()
  {
    ImportProject project;
    QTemporaryDir source;
    auto &service = project.service;
    PaleoTaskService tasks;
    FolderImportWorkflow imports(&service, &tasks);
    const QString valid = source.filePath("well.bin");
    const QString ambiguous = source.filePath("notes.blob");
    QVERIFY(writeFile(valid, las("A1")));
    QVERIFY(writeFile(ambiguous, "unrecognized sample"));
    PaleoTask *preview = nullptr;
    connect(&tasks, &PaleoTaskService::taskAdded, this, [&](PaleoTask *task) {
      if (!preview) preview = task;
    });
    // The worker has returned both a valid row and one requiring review. Cancel
    // at the last owner-thread handoff before either path can consume those rows.
    connect(&imports, &FolderImportWorkflow::previewActiveChanged, this, [&](bool active) {
      if (active) return;
      QVERIFY(preview);
      QCOMPARE(preview->state(), PaleoTask::State::Succeeded);
      preview->requestCancel();
    });
    QSignalSpy importsStarted(&imports, &FolderImportWorkflow::importActiveChanged);
    int completions = 0, reviews = 0;
    bool cancelled = false;
    QVector<FolderRowResult> results;
    imports.importAgentAttachments({valid, ambiguous}, [] { return true; },
      [&](const auto &rows, const QString &, bool stopped) {
        ++completions;
        cancelled = stopped;
        results = rows;
      }, [&](const auto &) { ++reviews; });
    QTRY_COMPARE_WITH_TIMEOUT(completions, 1, 15000);
    QVERIFY(cancelled);
    QVERIFY(results.isEmpty());
    QCOMPARE(reviews, 0);
    QCOMPARE(importsStarted.count(), 0);
    QCOMPARE(preview->state(), PaleoTask::State::Cancelled);
    QVERIFY(service.catalog()->assets().isEmpty());
  }

  void refusesPublicationAfterProjectSwitch()
  {
    ImportProject project;
    QTemporaryDir source;
    auto &service = project.service;
    PaleoTaskService tasks;
    FolderImportWorkflow imports(&service, &tasks);
    const QString path = source.filePath("selected.bin");
    QVERIFY(writeFile(path, las("A1")));
    bool current = true, importing = false;
    PaleoTask *publication = nullptr;
    connect(&imports, &FolderImportWorkflow::importActiveChanged, this,
            [&](bool active) { importing = active; });
    connect(&tasks, &PaleoTaskService::taskAdded, this, [&](PaleoTask *task) {
      if (!importing) return;
      publication = task;
      // Switch after worker production and immediately before the owner commits.
      connect(task, &PaleoTask::finished, this, [&] { current = false; });
    });
    QString error;
    QVector<FolderRowResult> results;
    bool completed = false;
    imports.importAgentAttachments({path}, [&] { return current; },
      [&](const auto &rows, const QString &reason, bool) {
        completed = true;
        error = reason;
        results = rows;
      });
    QTRY_VERIFY_WITH_TIMEOUT(completed, 15000);
    QVERIFY(publication);
    QCOMPARE(publication->state(), PaleoTask::State::Failed);
    QVERIFY(!error.isEmpty());
    QCOMPARE(results.size(), 1);
    QCOMPARE(results.first().outcome, FolderRowResult::Outcome::Failed);
    QVERIFY(results.first().assetId.isEmpty());
    QVERIFY(service.catalog()->assets().isEmpty());
  }
};

QTEST_GUILESS_MAIN(TestAgentAttachmentImport)
#include "tst_agentattachmentimport.moc"
