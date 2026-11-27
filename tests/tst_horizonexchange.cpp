#include <QtTest>
#include <QFile>
#include <QTemporaryDir>
#include "catalog/datacatalog.h"
#include "io/dataimportservice.h"
#include "io/horizongridexchange.h"
#include "metadata/paleoprojectfile.h"
#include "metadata/paleoprojectstore.h"
#include "services/dataexport.h"
#include "services/seismictaskservice.h"

class TestHorizonExchange : public QObject {
    Q_OBJECT
private slots:
    void independentGridExchangeAndReopen() {
        QTemporaryDir temp;
        const auto first = temp.filePath("A"), second = temp.filePath("B");
        QString error;
        DataCatalog source;
        QVERIFY(source.open(first, &error));
        const auto project = projectFileForQgz(QDir(first).filePath("scene.qgz"));
        QVERIFY(writeProjectFile(first, project, &error));
        CatalogAsset volume; volume.id = "asset-seismic"; volume.displayName = "cube"; volume.type = "seismic"; volume.format = "sgy";
        QVERIFY(source.addAsset(volume, &error));
        CatalogVersion raw; raw.assetId = volume.id; raw.id = "raw-survey-volume"; raw.stage = "RAW";
        raw.managed = false; raw.fileName = "cube.sgy"; raw.path = QDir(first).filePath(raw.fileName);
        QVERIFY(source.addVersion(raw, &error));
        CatalogEntity survey; survey.id = "survey-A"; survey.entityType = "seismic_survey"; survey.name = "Survey A";
        QVERIFY(source.addEntity(survey, &error));
        EntityAssetLink link; link.assetId = volume.id; link.entityType = survey.entityType; link.entityId = survey.id; link.role = "seismic_volume";
        QVERIFY(source.addLink(link, &error));
        seismic::SeismicPick point; point.inlineNo = 100; point.xlineNo = 205; point.twtMs = 100.123456;
        point.confidence = 0.76251f; point.horizonName = "T1";
        auto next = point; next.xlineNo += 2; next.twtMs = 101.125;
        const auto path = seismic::SeismicTaskService::registerPropagatedHorizonAsset(
            &source, volume.id, raw.id, point.horizonName, {point, next}, QDir(first).filePath("interpretation"), &error);
        QVERIFY2(!path.isEmpty(), qPrintable(error));
        const auto resultVersion = source.currentVersion("seis_horizon_asset-seismic_T1");
        paleo::exporting::DataRequest request;
        request.versionIds = {resultVersion.id}; request.format = paleo::exporting::DataFormat::Exchange;
        request.destination = temp.filePath("delivery");
        const auto written = paleo::exporting::execute(paleo::exporting::plan(source, first, request));
        QVERIFY2(written.complete(), qPrintable(written.error + written.failures.join(" ")));
        const auto csv = written.files.filter(".csv").value(0);
        QVERIFY(paleo::io::isHorizonGridExchange(csv));
        PaleoProjectStore store;
        store.setProjectPaths(second+"/p.qgz", second+"/p.gpkg", second+"/p.qgz.project.sqlite");
        QString id;
        {
            DataImportService imports(&store);
            imports.setProjectDir(second);
            const auto imported = imports.importProjectFileEx(csv, {}, &error);
            QVERIFY2(imported.outcome == DataImportService::ImportOutcome::Imported, qPrintable(error));
            id = imported.assetId;
        }
        {
            DataImportService imports(&store);
            imports.setProjectDir(second);
            const auto renamed = temp.filePath("D61.csv");
            QVERIFY(QFile::copy(csv, renamed));
            const auto secondImport = imports.importProjectFileEx(renamed, {}, &error);
            QVERIFY2(secondImport.outcome == DataImportService::ImportOutcome::AlreadyStored, qPrintable(error));
            QCOMPARE(secondImport.assetId, id);
            QCOMPARE(imports.catalog()->versionsForAsset(id).size(), 1);
            const auto local = imports.catalog()->currentVersion(id);
            const auto ourProject = projectFileForQgz(QDir(second).filePath("scene.qgz"));
            QVERIFY(writeProjectFile(second, ourProject, &error));
            request.versionIds = {local.id}; request.destination = temp.filePath("forward");
            const auto forwarded = paleo::exporting::execute(paleo::exporting::plan(*imports.catalog(), second, request));
            QVERIFY2(forwarded.complete(), qPrintable(forwarded.failures.join(" ") + forwarded.error));
            QCOMPARE(DataCatalog::sha256FileHex(forwarded.files.filter(".csv").value(0)), local.sha256);
            QFile changed(renamed); QVERIFY(changed.open(QIODevice::ReadOnly));
            auto header = changed.readLine(); const auto payload = changed.readAll(); changed.close();
            header.replace("T1", "T9"); // same valid points, incompatible identity metadata
            QVERIFY(changed.open(QIODevice::WriteOnly)); QVERIFY(changed.write(header + payload) > 0); changed.close();
            const auto conflict = imports.importProjectFileEx(renamed, {}, &error);
            QVERIFY(conflict.outcome == DataImportService::ImportOutcome::Failed);
            QVERIFY(error.contains(QStringLiteral("不同内容")));
        }
        DataCatalog reopened;
        QVERIFY2(reopened.open(second, &error), qPrintable(error));
        const auto local = reopened.currentVersion(id);
        QCOMPARE(reopened.assetById(id).type, QString("horizon"));
        const auto binding = local.extra.value("horizonExchange").toMap();
        QCOMPARE(binding.value("project_id").toString(), project.projectId);
        QCOMPARE(binding.value("version_id").toString(), resultVersion.id);
        QCOMPARE(binding.value("parent_version_id").toString(), raw.id);
        QCOMPARE(binding.value("survey_ids").toStringList(), QStringList{survey.id});
        QCOMPARE(binding.value("time_unit").toString(), QString("ms"));
        QVERIFY(binding.value("time_reference").toString().isEmpty());
        QVERIFY(local.parentVersionIds.isEmpty());
        const auto links = reopened.linksForAsset(id);
        QCOMPARE(links.size(), 1);
        QVERIFY(links.first().unresolved);
        QVariantMap sourceInfo;
        QList<paleo::io::InterpretationPoint> points;
        QVERIFY2(paleo::io::readHorizonGridExchange(reopened.versionFilePath(local), local.sha256, sourceInfo, &error, {},
            [&](const auto &p){ points.append(p); }), qPrintable(error));
        QCOMPARE(points.size(), 2);
        QCOMPARE(points.first().inlineNumber, point.inlineNo);
        QCOMPARE(points.first().crosslineNumber, point.xlineNo);
        QCOMPARE(points.first().twtMs, point.twtMs);
        QCOMPARE(float(points.first().confidence), point.confidence);
        QCOMPARE(points.last().crosslineNumber, next.xlineNo);
        QCOMPARE(sourceInfo, binding);
        QFile changed(csv);
        QVERIFY(changed.open(QIODevice::Append)); QVERIFY(changed.write("101,207,56,0.7\n") > 0); changed.close();
        QVERIFY(!paleo::io::readHorizonGridExchange(csv, {}, sourceInfo, &error));
        QVERIFY(error.contains(QStringLiteral("摘要")));
    }
};
QTEST_GUILESS_MAIN(TestHorizonExchange)
#include "tst_horizonexchange.moc"
