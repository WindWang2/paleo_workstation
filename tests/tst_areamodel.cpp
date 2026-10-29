#include <QtTest>
#include <QTemporaryDir>
#include <paleo_data/service.h>
#include "io/areamodel.h"
#include "io/welllogread.h"
#include "services/previewdoc.h"
#include "domain/wellcompositemodel.h"
#include "catalog/areawellprojection.h"
#include "catalog/areasourceprojection.h"
#include "services/projectdata.h"
#include <QJsonDocument>
#include <QJsonArray>
#include <QJsonObject>

class TestAreaModel : public QObject {
  Q_OBJECT
private slots:
  void coordinateCsvBuildsDomainWells() {
    QTemporaryDir temp; const QString input=temp.filePath("input"); QVERIFY(QDir().mkpath(input));
    QFile f(input + "/coordinates.csv"); QVERIFY(f.open(QIODevice::WriteOnly));
    f.write("WELL,X,Y\nSYNTH-1,100,200\nSYNTH-2,0,500\nTOTAL,,\n"); f.close();
    const QString project=temp.filePath("project"); QString error; paleo::data::ImportReport report;
    QVERIFY2(paleo::area_model::importDirectory(project, input, report, &error), qPrintable(error));
    paleo::data::DataService reader; std::string detail;
    QVERIFY(reader.openProjectReadOnly(paleo::area_model::directory(project).toUtf8().toStdString(), detail));
    std::vector<paleo::data::WellInfoRecord> rows; QVERIFY(reader.listWellInfos(rows, detail));
    QCOMPARE(rows.size(), size_t(2)); QCOMPARE(rows[0].value.wellheadX, 100.0); QCOMPARE(rows[0].value.wellheadY, 200.0);
    QCOMPARE(rows[1].value.wellheadX, 0.0); QVERIFY(rows[0].value.coordinateSystem.empty());
    DataCatalog catalog; QVERIFY(catalog.open(project, &error)); QVERIFY(registerAreaWells(catalog, &error));
    QCOMPARE(catalog.entities("well").size(), 2); QCOMPARE(catalog.entities("well")[1].surfaceY, 500.0);
  }
  void sourceIndexDoesNotReadOriginalFiles() {
    QTemporaryDir temp; const QString input = temp.filePath("input"); QVERIFY(QDir().mkpath(input));
    const QString path = input + "/well.las"; QFile file(path); QVERIFY(file.open(QIODevice::WriteOnly));
    file.write("~Version\nVERS. 2.0\nWRAP. NO\n~Well\nWELL. SYNTH-1\nNULL. -999.25\n~Curve\nDEPT.M : depth\nGR.API : gamma\n~Ascii\n100 10\n101 20\n"); file.close();
    const QString project = temp.filePath("project"); QString error; paleo::data::ImportReport report;
    QVERIFY2(paleo::area_model::importDirectory(project, input, report, &error), qPrintable(error));
    paleo::data::DataService reader; std::string detail;
    QVERIFY(reader.openProjectReadOnly(paleo::area_model::directory(project).toUtf8().toStdString(), detail));
    std::vector<paleo::data::SourceIndexEntry> sources; QVERIFY(reader.listSourceIndexEntries(sources, detail));
    QCOMPARE(sources.size(), size_t(1)); QVERIFY(!sources.front().sha256.empty()); reader.closeProject();

    DataCatalog catalog; QVERIFY(catalog.open(project, &error)); QVERIFY(registerAreaWells(catalog, &error));
    QVERIFY(QFile::remove(path));
    QVERIFY2(registerAreaSources(catalog, &error), qPrintable(error));
    QCOMPARE(catalog.assets().size(), 1); QCOMPARE(catalog.assets().first().type, QString("well_log"));
    QCOMPARE(catalog.versions().size(), 1);
    const auto version = catalog.versions().first();
    QCOMPARE(version.sha256, QString::fromUtf8(sources.front().sha256));
    QCOMPARE(version.extra.value("areaModelSourceId").toLongLong(), sources.front().id);
    QVERIFY(!version.managed);
    const auto links = catalog.linksForAsset(version.assetId);
    QCOMPARE(links.size(), 1); QCOMPARE(links.first().role, QString("well_log"));
    QVERIFY2(registerAreaSources(catalog, &error), qPrintable(error));
    QCOMPARE(catalog.assets().size(), 1); QCOMPARE(catalog.versions().size(), 1);
    QCOMPARE(catalog.linksForAsset(version.assetId).size(), 1);
  }
  void wellCoordinatesComeFromAreaModel() {
    QTemporaryDir temp; const QString project = temp.filePath("project"); QString error;
    DataCatalog catalog; QVERIFY2(catalog.open(project, &error), qPrintable(error));
    CatalogEntity old; old.id="well-existing"; old.entityType="well"; old.name="SYNTH-1";
    old.hasSurface=true; old.surfaceX=11; old.surfaceY=22; old.coordinateStatus="ok";
    QVERIFY(catalog.addEntity(old, &error));
    QVERIFY(paleo::area_model::ensureProject(project, &error));
    paleo::data::DataService model; std::string detail;
    QVERIFY(model.openProject(paleo::area_model::directory(project).toUtf8().toStdString(), detail));
    paleo::data::WellInfo info; info.wellName="SYNTH-1"; info.wellheadX=100; info.wellheadY=200;
    info.coordinateSystem="EPSG:32649"; info.coordinateUnit="m"; info.totalDrilledDepth=3000;
    long long id=0; QVERIFY(model.createWell(info, id, detail));
    paleo::data::WellInfo absent; absent.wellName="SYNTH-2"; long long absentId=0;
    QVERIFY(model.createWell(absent, absentId, detail));
    QVERIFY2(registerAreaWells(catalog, &error), qPrintable(error));
    QCOMPARE(catalog.entities("well").size(), 2);
    QCOMPARE(catalog.entityById(old.id).surfaceX, 100.0);
    QCOMPARE(catalog.entityById(old.id).extra.value("wellCoordinatesCrs").toString(), QString("EPSG:32649"));
    ProjectDataFacade facade; facade.setCatalog(&catalog, project);
    info.wellheadX=0; info.wellheadY=250; info.wellName="SYNTH-RENAMED";
    QVERIFY(model.saveWellInfo(id, info, detail));
    QCOMPARE(catalog.entityById(old.id).surfaceX, 0.0); QCOMPARE(catalog.entityById(old.id).surfaceY, 250.0);
    QCOMPARE(catalog.entityById(old.id).name, QString("SYNTH-RENAMED"));
    QCOMPARE(facade.wells().first().surfaceY, 250.0);
    QVERIFY(!catalog.entities("well").last().hasSurface);
    const QString geojson=temp.filePath("wells.geojson"); QVERIFY(catalog.writeWellsGeoJson(geojson, &error));
    QFile f(geojson); QVERIFY(f.open(QIODevice::ReadOnly));
    const auto features=QJsonDocument::fromJson(f.readAll()).object()["features"].toArray();
    QCOMPARE(features.size(), 1); QCOMPARE(features[0].toObject()["geometry"].toObject()["coordinates"].toArray()[1].toDouble(), 250.0);
    // Removal of model coordinates cannot fall back to stale catalog coordinates.
    info.wellheadX=paleo::data::Missing; info.wellheadY=paleo::data::Missing;
    QVERIFY(model.saveWellInfo(id, info, detail)); QVERIFY(!catalog.entityById(old.id).hasSurface);
    QVERIFY(catalog.entityById(old.id).coordinateStatus == "missing");
  }
  void directoryModelDrivesBothPreviews() {
    QTemporaryDir temp;
    const QString input = temp.filePath("input"); QDir().mkpath(input);
    const QString source = input + "/well.xml";
    QFile file(source); QVERIFY(file.open(QIODevice::WriteOnly));
    file.write("<?xml version=\"1.0\"?><Workbook xmlns=\"urn:schemas-microsoft-com:office:spreadsheet\" xmlns:ss=\"urn:schemas-microsoft-com:office:spreadsheet\"><Worksheet ss:Name=\"地层单位道\"><Table>"
        "<Row><Cell><Data ss:Type=\"String\">井号</Data></Cell><Cell><Data ss:Type=\"String\">顶深</Data></Cell><Cell><Data ss:Type=\"String\">底深</Data></Cell><Cell><Data ss:Type=\"String\">地层</Data></Cell></Row>"
        "<Row><Cell><Data ss:Type=\"String\">SYNTH-1</Data></Cell><Cell><Data ss:Type=\"Number\">100</Data></Cell><Cell><Data ss:Type=\"Number\">200</Data></Cell><Cell><Data ss:Type=\"String\">original</Data></Cell></Row>"
        "</Table></Worksheet></Workbook>"); file.close();
    const QString project = temp.filePath("project");
    paleo::data::ImportReport report; QString error;
    QVERIFY2(paleo::area_model::importDirectory(project, input, report, &error), qPrintable(error));
    QVERIFY(report.imported > 0 || report.review > 0);
    paleo::data::DataService model; std::string detail;
    QVERIFY(model.openProject(paleo::area_model::directory(project).toUtf8().toStdString(), detail));
    std::vector<paleo::data::DataEntry> entries;
    QVERIFY(model.listData("intervals", 0, entries, detail));
    QVERIFY2(!entries.empty(), detail.c_str());
    std::vector<paleo::data::IntervalRecord> rows;
    QVERIFY(model.readIntervals(entries.front().id, paleo::data::Missing, paleo::data::Missing, rows, detail));
    QCOMPARE(rows.size(), size_t(1));
    rows[0].value.intervalName = "edited-in-model";
    QVERIFY(model.saveInterval(entries[0].id, rows[0], detail));
    // Snapshot must include committed WAL, not just copy the main DB file.
    paleo::data::DataService snapshotReader;
    QVERIFY(snapshotReader.openProjectReadOnly(paleo::area_model::directory(project).toUtf8().toStdString(), detail));
    const QString snapshotRoot = temp.filePath("snapshot"); QDir().mkpath(snapshotRoot);
    QVERIFY2(snapshotReader.snapshotDatabase((snapshotRoot + "/data.sqlite").toUtf8().toStdString(), detail), detail.c_str());
    snapshotReader.closeProject(); model.closeProject();
    QVERIFY(snapshotReader.openProjectReadOnly(snapshotRoot.toUtf8().toStdString(), detail));
    QVERIFY(snapshotReader.readIntervals(entries[0].id, paleo::data::Missing, paleo::data::Missing, rows, detail));
    QCOMPARE(rows[0].value.intervalName, std::string("edited-in-model")); snapshotReader.closeProject();
    paleo::area_model::registerProject(project, this);
    PreviewDocService::WorkbookPreview table;
    WellComposite::ComprehensiveWellData chart; bool chartOk = false;
    QVERIFY(PreviewDocService::areaPreviewAt(source, &table, &chart, &chartOk, &error));
    QVERIFY2(table.ok, qPrintable(table.error)); QVERIFY2(chartOk, qPrintable(error));
    QCOMPARE(chart.formationIntervals.size(), 1);
    QCOMPARE(chart.formationIntervals[0].name, QString("edited-in-model"));
    bool listed = false;
    for (const auto &sheet : table.sheets) for (const auto &row : sheet.rows)
      if (row.contains("edited-in-model")) listed = true;
    QVERIFY(listed);
    // A source change cannot silently switch either view back to a file parser.
    QVERIFY(file.open(QIODevice::WriteOnly | QIODevice::Truncate)); file.write("broken XML"); file.close();
    QVERIFY(PreviewDocService::wellCompositeAt(source, &chart, &error));
    QCOMPARE(chart.formationIntervals[0].name, QString("edited-in-model"));
    QVERIFY(PreviewDocService::xmlDataListAt(source).ok);
    paleo::area_model::unregisterProject(this);
    QVERIFY(!PreviewDocService::wellCompositeAt(source, &chart, &error));
  }
  void curveReadsUseTheAreaModel() {
    QTemporaryDir temp; const QString input = temp.filePath("input"); QDir().mkpath(input);
    const QString path = input + "/well.las"; QFile file(path); QVERIFY(file.open(QIODevice::WriteOnly));
    file.write("~Version\nVERS. 2.0\nWRAP. NO\n~Well\nWELL. SYNTH-1\nNULL. -999.25\n~Curve\nDEPT.M : depth\nGR.API : gamma\n~Ascii\n100 10\n101 20\n"); file.close();
    const QString project = temp.filePath("project"); QString error; paleo::data::ImportReport report;
    QVERIFY2(paleo::area_model::importDirectory(project, input, report, &error), qPrintable(error));
    paleo::area_model::registerProject(project, this);
    paleo::data::DataService service; std::string detail;
    QVERIFY(service.openProject(paleo::area_model::directory(project).toUtf8().toStdString(), detail));
    std::vector<paleo::data::DataEntry> entries; QVERIFY(service.listData("curves", 0, entries, detail));
    QCOMPARE(entries.size(), size_t(1));
    QVERIFY(service.updateCurveValue(entries[0].id, 0, 88.0, detail)); service.closeProject();
    LasHeaderInfo header; QList<LasCurve> curves;
    QVERIFY2(WellLogRead::parse(path, header, curves, &error), qPrintable(error));
    QCOMPARE(curves[1].values[0], 88.0);
    // Incremental directory import must preserve the user's model edit.
    QVERIFY(paleo::area_model::importDirectory(project, input, report, &error));
    QVERIFY(WellLogRead::parse(path, header, curves, &error)); QCOMPARE(curves[1].values[0], 88.0);
    // A cached missing source must become visible after a DB/WAL update.
    const QString second = input + "/second.las";
    long long sourceId = 0; paleo::data::DataService reader;
    QCOMPARE(paleo::area_model::openSource(second, reader, sourceId, &error), paleo::area_model::Read::Unbound);
    QVERIFY(QFile::copy(path, second));
    QFile secondFile(second); QVERIFY(secondFile.open(QIODevice::Append));
    secondFile.write("# independent source\n"); secondFile.close();
    QVERIFY(paleo::area_model::importDirectory(project, input, report, &error));
    QCOMPARE(paleo::area_model::openSource(second, reader, sourceId, &error), paleo::area_model::Read::Ready);
    QVERIFY(sourceId > 0);
    QVERIFY(reader.listSourceData("curves", sourceId, entries, detail));
    QCOMPARE(entries.size(), size_t(1)); QCOMPARE(entries[0].sourceId, sourceId);
    QVERIFY(reader.listSourceData("tables", sourceId, entries, detail));
    for (const auto &entry : entries) QCOMPARE(entry.sourceId, sourceId);
    QVERIFY(reader.listSourceData("curves", sourceId + 999999, entries, detail)); QVERIFY(entries.empty());
    QVERIFY(!reader.listSourceData("curves", 0, entries, detail));
    paleo::area_model::unregisterProject(this);
  }
  void excludedAndCancelledDirectories() {
    QTemporaryDir temp; QString error; paleo::data::ImportReport report;
    const QString source = temp.filePath("source"); QDir().mkpath(source + "/output");
    QFile f(source + "/output/data.txt"); QVERIFY(f.open(QIODevice::WriteOnly)); f.write("ignore"); f.close();
    QVERIFY(paleo::area_model::importDirectory(temp.filePath("project"), source, report, &error, {}, {source + "/output"}));
    QCOMPARE(report.seen, 0);
    QVERIFY(!paleo::area_model::importDirectory(temp.filePath("cancelled"), source, report, &error, [] { return true; }));
    QVERIFY(report.cancelled);
  }
};
QTEST_GUILESS_MAIN(TestAreaModel)
#include "tst_areamodel.moc"
