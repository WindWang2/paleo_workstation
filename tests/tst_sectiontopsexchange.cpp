#include <QtTest>
#include <QFileInfo>
#include <QTemporaryDir>
#include <cmath>
#include "catalog/datacatalog.h"
#include "domain/wellsection.h"
#include "io/dataimportservice.h"
#include "io/welltable.h"
#include "io/wellfileparsers.h"
#include "io/checkshotexchange.h"
#include "io/importclassification.h"
#include "metadata/paleoprojectfile.h"
#include "metadata/paleoprojectstore.h"
#include "services/dataexport.h"
#include "workflow/wellsectionexport.h"
#include <paleo_data/service.h>

class TestSectionTopsExchange : public QObject {
    Q_OBJECT
private slots:
    void sectionExportImportsTypedMarker() {
        QTemporaryDir temp;
        const QString csv = temp.filePath("section.csv"), project = temp.filePath("project");
        wellsection::Well well; well.name = "W1"; well.id = "well-1";
        const QString horizon = QStringLiteral("T, \"甲\"\n界面");
        const double measured = 101.23254321;
        well.tops.append({horizon, measured});
        const auto table = wellsection::topsTable({well}, {wellsection::DatumMode::Flatten, "screen-only"},
                                                  wellsection::DepthDomain::TVD);
        QString error;
        QVERIFY2(wellsection::exportTopsCsv(csv, table.csv(), &error), qPrintable(error));
        const auto preview = paleo::io::previewWellTable(csv);
        QCOMPARE(preview.type, QString("well_stratification"));
        QVERIFY2(preview.error.isEmpty(), qPrintable(preview.error));
        QCOMPARE(preview.tops.size(), 1);
        QCOMPARE(preview.tops.first().topName, horizon);
        QCOMPARE(preview.tops.first().md, measured);
        QVERIFY(!preview.tops.first().hasTvd);
        PaleoProjectStore store;
        store.setProjectPaths(project+"/project.qgz", project+"/project.gpkg", project+"/project.qgz.project.sqlite");
        DataImportService imports(&store);
        imports.setProjectDir(project);
        CatalogEntity entity; entity.id = well.id; entity.name = well.name; entity.entityType = "well";
        QVERIFY(imports.catalog()->addEntity(entity, &error));
        DataImportService::ImportOptions options;
        options.linkExternal = true;
        const auto imported = imports.importProjectFileEx(csv, options, &error);
        QVERIFY2(imported.outcome == DataImportService::ImportOutcome::Imported, qPrintable(error));
        const auto repeated = imports.importProjectFileEx(csv, options, &error);
        QCOMPARE(repeated.outcome, DataImportService::ImportOutcome::AlreadyStored);
        QCOMPARE(repeated.assetId, imported.assetId);
        const auto version = imports.catalog()->currentVersion(imported.assetId);
        QCOMPARE(version.extra.value("wellTopDepthUnit").toString(), QString("m"));
        const auto model = version.extra.value("nativeModel").toMap().value("path").toString();
        QVERIFY(!model.isEmpty());
        std::string detail;
        paleo::data::DataService reader;
        QVERIFY2(reader.openProjectReadOnly(QFileInfo(QDir(project).filePath(model)).absolutePath().toUtf8().toStdString(), detail), detail.c_str());
        std::vector<paleo::data::WellSummary> wells;
        QVERIFY(reader.listWells(wells, detail));
        QCOMPARE(wells.size(), size_t(1));
        paleo::data::WellPlotData data;
        QVERIFY2(reader.readWellPlotData(wells.front().id, {}, data, detail), detail.c_str());
        QCOMPARE(data.markers.size(), size_t(1));
        const auto &track = data.markers.front().value;
        QCOMPARE(track.depthInfo.depthUnit, std::string("m"));
        QCOMPARE(track.depthInfo.depthType, std::string("MD"));
        QVERIFY(track.depthInfo.mdReference.empty());
        QCOMPARE(track.markers.size(), size_t(1));
        QCOMPARE(QString::fromStdString(track.markers.front().markerName), horizon);
        QCOMPARE(track.markers.front().depthValue, measured);
        QVERIFY(!std::isfinite(track.markers.front().tvd));
    }
    void ambiguousAxisOrUnitNeedsReview() {
        QTemporaryDir temp;
        const auto path = temp.filePath("review.csv");
        QString error;
        QVERIFY(wellsection::exportTopsCsv(path, QStringLiteral("井名,顶名,深度,基准面,显示\nW1,T1,30\n"), &error));
        const auto result = paleo::io::readWellTable(path);
        QVERIFY(result.error.contains(QStringLiteral("单位")));
        QVERIFY(result.tops.isEmpty());
    }
    void historicalCheckshotExchangeAndReopen() {
        QTemporaryDir temp;
        const auto first = temp.filePath("sender"), second = temp.filePath("receiver");
        QString error;
        DataCatalog source;
        QVERIFY(source.open(first, &error));
        const auto project = projectFileForQgz(QDir(first).filePath("scene.qgz"));
        QVERIFY(writeProjectFile(first, project, &error));
        const QByteArray content = "# Petrel checkshots format\n# Unit in depth: ft\n# Unit in time: s\n"
            "# Time reference: source datum\n# MD reference: KB\n# Unit in X and Y direction: m\n"
            "# Coordinate reference system X, Y: EPSG:32650\nBEGIN HEADER\nX\nY\nMD\nOWT\nWell\nEND HEADER\n"
            "100 205 125.5 0.25 \"W-A\"\n101 206 155 0.4 \"W-A\"\n";
        const auto write = [](const QString &path, const QByteArray &bytes) {
            QFile f(path); return f.open(QIODevice::WriteOnly) && f.write(bytes) == bytes.size() && f.flush();
        };
        CatalogAsset asset; asset.id = "field-td"; asset.type = "time_depth"; asset.format = "txt"; asset.displayName = "checkshots";
        QVERIFY(source.addAsset(asset, &error));
        CatalogVersion historical; historical.assetId = asset.id; historical.id = "td-v1"; historical.stage = "RAW";
        historical.managed = false; historical.path = temp.filePath("old.txt"); historical.fileName = "old.txt";
        QVERIFY(write(historical.path, content)); historical.sha256 = DataCatalog::sha256FileHex(historical.path);
        QVERIFY(source.addVersion(historical, &error));
        auto newer = historical; newer.id = "td-v2"; newer.versionNumber = 2; newer.path = temp.filePath("new.txt"); newer.fileName = "new.txt";
        QVERIFY(write(newer.path, QByteArray(content).replace("125.5", "150.5"))); newer.sha256 = DataCatalog::sha256FileHex(newer.path);
        QVERIFY(source.addVersion(newer, &error));
        paleo::exporting::DataRequest request; request.format = paleo::exporting::DataFormat::Exchange;
        request.versionIds = {historical.id}; request.destination = temp.filePath("delivery");
        const auto exported = paleo::exporting::execute(paleo::exporting::plan(source, first, request));
        QVERIFY2(exported.complete(), qPrintable(exported.failures.join(" ") + exported.error));
        const auto path = exported.files.filter(".checkshots").value(0);
        QVERIFY(paleo::io::isCheckshotExchange(path));
        PaleoProjectStore store; store.setProjectPaths(second+"/p.qgz", second+"/p.gpkg", second+"/p.qgz.project.sqlite");
        QString id;
        {
            DataImportService imports(&store); imports.setProjectDir(second);
            const auto received = imports.importProjectFileEx(path, {}, &error);
            QVERIFY2(received.outcome == DataImportService::ImportOutcome::Imported, qPrintable(error));
            id = received.assetId;
        }
        {
            DataImportService reopened(&store); reopened.setProjectDir(second);
            const auto repeated = reopened.importProjectFileEx(path, {}, &error);
            QVERIFY2(repeated.outcome == DataImportService::ImportOutcome::AlreadyStored, qPrintable(error));
            QCOMPARE(repeated.assetId, id);
            const auto v = reopened.catalog()->currentVersion(id);
            const auto provenance = v.extra.value("checkshotExchange").toMap();
            QCOMPARE(provenance.value("project_id").toString(), project.projectId);
            QCOMPARE(provenance.value("version_id").toString(), historical.id);
            QCOMPARE(provenance.value("payload_sha256").toString(), historical.sha256);
            const auto binding = v.extra.value("nativeModel").toMap();
            QVERIFY(binding.value("typed").toBool());
            std::string detail;
            paleo::data::DataService sdk;
            QVERIFY2(sdk.openProjectReadOnly(QFileInfo(QDir(second).filePath(binding.value("path").toString())).absolutePath().toUtf8().toStdString(), detail), detail.c_str());
            std::vector<paleo::data::DataEntry> rows;
            QVERIFY(sdk.listData("time_depth", 0, rows, detail)); QCOMPARE(rows.size(), size_t(1));
            paleo::data::TimeDepthTable relation; QVERIFY(sdk.readTimeDepth(rows.front().id, relation, detail));
            QCOMPARE(relation.relationName, std::string("old.txt"));
            QCOMPARE(relation.timeReference, std::string("source datum")); QCOMPARE(relation.depthInfo.mdReference, std::string("KB"));
            QCOMPARE(relation.depthTimePairs.front().depthValue, 125.5); QCOMPARE(relation.depthTimePairs.front().x, 100.0);
            QCOMPARE(relation.timeType, std::string("OWT")); QCOMPARE(relation.depthInfo.depthUnit, std::string("ft"));
            TimeDepthTable normalized;
            QVERIFY2(readTimeDepth(reopened.catalog()->versionFilePath(v), normalized, &error), qPrintable(error));
            QCOMPARE(normalized.rows.size(), 2); QCOMPARE(normalized.rows.first().timeMs, 500.);
            QCOMPARE(normalized.rows.first().md, 125.5 * .3048); QVERIFY(!normalized.rows.first().hasTvd);
            relation.timeReference.clear();
            QVERIFY(!paleo::io::checkshotSamples(relation, normalized, &error));
            QVERIFY(error.contains(QStringLiteral("复核")));
            QFile delivered(path); QVERIFY(delivered.open(QIODevice::ReadOnly)); auto forged = delivered.readAll(); delivered.close();
            forged.replace("source datum", "other datum");
            QVERIFY(write(path, forged));
            const auto invalid = reopened.importProjectFileEx(path, {}, &error);
            QVERIFY(invalid.outcome == DataImportService::ImportOutcome::Failed); QVERIFY(error.contains(QStringLiteral("摘要")));
            QCOMPARE(reopened.catalog()->versionsForAsset(id).size(), 1);
        }
        const auto unrelated = temp.filePath("ordinary.csv");
        QVERIFY(write(unrelated, "depth,time,well\n1,2,W1\n"));
        QVERIFY(classifyImportFileDetailed(unrelated).cls.type != "time_depth");
    }
};
QTEST_GUILESS_MAIN(TestSectionTopsExchange)
#include "tst_sectiontopsexchange.moc"
