#include <QtTest>
#include <QCryptographicHash>
#include <QDir>
#include <QFile>
#include <QJsonArray>
#include <QJsonDocument>
#include <QJsonObject>
#include <QTemporaryDir>
#include "catalog/areasourceprojection.h"
#include "io/areamodel.h"
#include "io/ingestplan.h"
#include "io/originalpackage.h"
#include "metadata/paleoprojectfile.h"
#include "metadata/paleoprojectstore.h"
#include "services/dataexport.h"
#include <paleo_data/service.h>
#include <algorithm>

using paleo::exchange::receiveOriginalPackage;
namespace {
bool writeBytes(const QString &path, const QByteArray &data) {
    QFile file(path);
    return QDir().mkpath(QFileInfo(path).absolutePath()) && file.open(QIODevice::WriteOnly) && file.write(data) == data.size();
}
QByteArray bytesAt(const QString &path) {
    QFile file(path);
    return file.open(QIODevice::ReadOnly) ? file.readAll() : QByteArray();
}
struct Fixture {
    QTemporaryDir tmp;
    QString receiver = tmp.filePath("receiver");
    QString package = tmp.filePath("package");
    QString relative = "data/a1/v1/原始.las";
    QByteArray content = "~A\r\n1 42\r\n";
    QString filePath() const { return QDir(package).filePath(relative); }
    bool prepare(const QString &sender = "s1") const {
        const auto sha = QString::fromLatin1(QCryptographicHash::hash(content, QCryptographicHash::Sha256).toHex());
        const QJsonObject output{{"path", relative}, {"sha256", sha}, {"bytes", qint64(content.size())}};
        const QJsonObject asset{{"asset_id","a1"}, {"version_id","v1"}, {"stage","RAW"},
                                {"source_sha256",sha}, {"files",QJsonArray{output}}};
        const QJsonObject doc{{"schema","paleo_export_v1"}, {"project_id",sender}, {"format",0},
                              {"complete",true}, {"assets",QJsonArray{asset}}};
        return QDir().mkpath(receiver) && writeBytes(filePath(), content) &&
               writeBytes(QDir(package).filePath("metadata.json"), QJsonDocument(doc).toJson());
    }
};
}
class TestOriginalPackage : public QObject {
    Q_OBJECT
private slots:
    void fullNativeExchangeWithAreaImports() {
        QTemporaryDir temp;
        const auto source = temp.filePath("sources"), first = temp.filePath("projectA"), second = temp.filePath("projectB");
        const auto delivery = temp.filePath("bundle");
        QVERIFY(QDir().mkpath(source)); QVERIFY(QDir().mkpath(second));
        const QDir fixtures(QStringLiteral(PALEO_ORIGINAL_FIXTURES));
        QVERIFY(QFile::copy(fixtures.filePath("tests/fixtures/io_robustness/unit_metre.las"), QDir(source).filePath("W1.las")));
        QVERIFY(QFile::copy(fixtures.filePath("testdata/segy/synthetic_4x5.sgy"), QDir(source).filePath("cube.sgy")));
        QString error; paleo::data::ImportReport report;
        QVERIFY2(paleo::area_model::importDirectory(first, source, report, &error), qPrintable(error));
        auto project = projectFileForQgz(QDir(first).filePath("scene.qgz"));
        QVERIFY2(writeProjectFile(first, project, &error), qPrintable(error));
        DataCatalog catalog;
        QVERIFY2(catalog.open(first, &error), qPrintable(error));
        QVERIFY2(registerAreaSources(catalog, &error), qPrintable(error));
        paleo::exporting::DataRequest request;
        request.scope = paleo::exporting::Scope::Original;
        request.destination = delivery;
        for (const auto &version : catalog.versions()) request.versionIds.append(version.id);
        QCOMPARE(request.versionIds.size(), 2);
        const auto exported = paleo::exporting::execute(paleo::exporting::plan(catalog, first, request));
        QVERIFY2(exported.complete(), qPrintable(exported.error + exported.failures.join(" ")));
        const auto received = receiveOriginalPackage(second, delivery);
        QVERIFY2(received.error.isEmpty(), qPrintable(received.error));
        QCOMPARE(received.files.size(), 2);
        QVERIFY(received.reused.isEmpty());
        const auto retry = receiveOriginalPackage(second, delivery);
        QCOMPARE(retry.files, received.files);
        QCOMPARE(retry.reused, received.files);
        for (const auto &path : received.files) {
            const auto original = QDir(source).filePath(QFileInfo(path).fileName());
            QCOMPARE(bytesAt(path), bytesAt(original));
            QVERIFY(paleo::area_model::importFileWithCrs(second, path, report, &error, {}, {}, {}, {}));
        }
        paleo::data::DataService reopened;
        std::string detail;
        QVERIFY(reopened.openProjectReadOnly(paleo::area_model::directory(second).toStdString(), detail));
        std::vector<paleo::data::WellSummary> wells;
        QVERIFY(reopened.listWells(wells, detail));
        QVERIFY(std::any_of(wells.begin(), wells.end(), [](const auto &well) {return well.name == "W1" && well.curves > 0;}));
        std::vector<paleo::data::DataEntry> volumes;
        QVERIFY(reopened.listData("volumes", 0, volumes, detail));
        QCOMPARE(volumes.size(), size_t(1));
    }
    void changedReceivedLasCannotBeImportedOrReused() {
        Fixture f;
        const QDir fixtures(QStringLiteral(PALEO_ORIGINAL_FIXTURES));
        f.content = bytesAt(fixtures.filePath("tests/fixtures/io_robustness/unit_metre.las"));
        QVERIFY(f.content.contains("100 10"));
        QVERIFY(f.prepare());
        const auto received = receiveOriginalPackage(f.receiver, f.package);
        QCOMPARE(received.files.size(), 1);
        const auto path = received.files.first();
        auto changed = f.content;
        changed.replace("100 10", "100 11");
        QVERIFY(writeBytes(path, changed));

        PaleoProjectStore store;
        store.setProjectPaths(f.receiver + "/project.qgz", f.receiver + "/project.gpkg", f.receiver + "/project.sqlite");
        DataImportService imports(&store);
        imports.setProjectDir(f.receiver);
        QString error;
        const auto rejected = imports.importProjectFileEx(path, &error);
        QCOMPARE(rejected.outcome, DataImportService::ImportOutcome::Failed);
        QVERIFY2(error.contains(QStringLiteral("原件完整性")), qPrintable(error));
        QVERIFY(imports.catalog()->versions().isEmpty());

        paleo::data::ImportReport report;
        QVERIFY(!paleo::area_model::importFileWithCrs(f.receiver, path, report, &error, {}, {}, {}, {}));
        QVERIFY2(error.contains(QStringLiteral("原件完整性")), qPrintable(error));
        QVERIFY(!QFileInfo::exists(paleo::area_model::directory(f.receiver) + "/data.sqlite"));

        QVERIFY(writeBytes(path, f.content));
        QVERIFY2(!imports.importProjectFileEx(path, &error).assetId.isEmpty(), qPrintable(error));
        QCOMPARE(imports.catalog()->versions().size(), 1);
        const auto version = imports.catalog()->versions().first();
        PlannedItem duplicate;
        duplicate.path = path;
        duplicate.sha256 = version.sha256;
        duplicate.decision = QStringLiteral("skip");
        duplicate.duplicateOfVersionId = version.id;
        QVERIFY(writeBytes(path, changed));
        const auto row = imports.executePlannedItem(duplicate, &error);
        QCOMPARE(row.outcome, FolderRowResult::Outcome::Failed);
        QVERIFY2(row.message.contains(QStringLiteral("原件完整性")), qPrintable(row.message));
        QCOMPARE(imports.catalog()->versions().size(), 1);
    }
    void rejectConflictAndCorruptionPreservingReceipt() {
        Fixture f; QVERIFY(f.prepare());
        const auto first = receiveOriginalPackage(f.receiver, f.package);
        QVERIFY2(first.error.isEmpty(), qPrintable(first.error));
        const auto original = f.content;
        f.relative = "data/a1/v1/renamed.las";
        QVERIFY(f.prepare());
        QCOMPARE(receiveOriginalPackage(f.receiver, f.package).reused, first.files);
        f.content += "2 84\n";
        QVERIFY(f.prepare());
        const auto conflict = receiveOriginalPackage(f.receiver, f.package);
        QVERIFY(!conflict.error.isEmpty());
        QCOMPARE(bytesAt(first.files.first()), original);
        QVERIFY(f.prepare("different sender"));
        const auto independent = receiveOriginalPackage(f.receiver, f.package);
        QVERIFY2(independent.error.isEmpty(), qPrintable(independent.error));
        QVERIFY(independent.files != first.files);
        QVERIFY(writeBytes(f.filePath(), QByteArray(f.content.size(), 'x')));
        QVERIFY(!receiveOriginalPackage(f.receiver, f.package).error.isEmpty());
        QVERIFY(writeBytes(f.filePath(), "short"));
        QVERIFY(!receiveOriginalPackage(f.receiver, f.package).error.isEmpty());
    }
    void legacySenderAndMalformedMetadata() {
        Fixture f; QVERIFY(f.prepare({}));
        QVERIFY(!receiveOriginalPackage(f.receiver, f.package).error.isEmpty());
        const auto result = receiveOriginalPackage(f.receiver, f.package, "confirmed old project");
        QVERIFY2(result.error.isEmpty(), qPrintable(result.error));
        f.relative = "other/asset/version/other.las";
        QVERIFY(f.prepare());
        QVERIFY(!receiveOriginalPackage(f.receiver, f.package).error.isEmpty());
        f.relative = "data/a1/v1/unhandled.tif";
        QVERIFY(f.prepare());
        QVERIFY(!receiveOriginalPackage(f.receiver, f.package).error.isEmpty());
    }
    void cancelAndRetry() {
        Fixture f; f.content.fill('a', 3 * 1024 * 1024); QVERIFY(f.prepare());
        bool stop = false;
        const auto interrupted = receiveOriginalPackage(f.receiver, f.package, {}, [&] {return stop;},
            [&](qint64 copied, qint64, const QString &) {stop = copied > 0;});
        QVERIFY(interrupted.cancelled);
        QVERIFY(interrupted.files.isEmpty());
        QVERIFY(QDir(QDir(f.receiver).filePath("artifacts/received")).entryList(QDir::NoDotAndDotDot | QDir::AllEntries).isEmpty());
        const auto completed = receiveOriginalPackage(f.receiver, f.package);
        QVERIFY2(completed.error.isEmpty(), qPrintable(completed.error));
        QCOMPARE(bytesAt(completed.files.first()), f.content);
    }
    void recoverVerifiedPartialPrefix() {
        Fixture f; f.content.fill('g', 2 * 1024 * 1024); QVERIFY(f.prepare());
        const auto first = receiveOriginalPackage(f.receiver, f.package);
        QVERIFY2(first.error.isEmpty(), qPrintable(first.error));
        const QFileInfo primary(first.files.first());
        const auto directory = primary.absolutePath(), pending = directory + ".pending";
        QVERIFY(QDir().rename(directory, pending));
        QFile partial(QDir(pending).filePath(primary.fileName()));
        QVERIFY(partial.open(QIODevice::ReadWrite));
        QVERIFY(partial.resize(71111));
        partial.close();
        const auto resumed = receiveOriginalPackage(f.receiver, f.package);
        QVERIFY2(resumed.error.isEmpty(), qPrintable(resumed.error));
        QCOMPARE(resumed.files, first.files);
        QCOMPARE(bytesAt(resumed.files.first()), f.content);
    }
};
QTEST_GUILESS_MAIN(TestOriginalPackage)
#include "tst_originalpackage.moc"
