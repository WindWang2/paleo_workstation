#include <QtTest>
#include <cmath>
#include <QComboBox>
#include <QDir>
#include <QFile>
#include <QJsonDocument>
#include <QJsonArray>
#include <QJsonObject>
#include <QLineEdit>
#include <QPushButton>
#include <QTemporaryDir>
#include <QTabBar>
#include <QTableWidget>
#include <QCheckBox>
#include "../src/services/paleomapfiles.h"
#include "../src/services/paleotaskservice.h"
#include "../src/io/welltable.h"
#include "../src/io/cuttingsdoc.h"
#include "../src/workflow/wellsectionworkflow.h"
#include "../src/domain/lithologystyle.h"
#include "../src/io/wellfileparsers.h"
#include "../src/io/ziparchive.h"
#include "../src/io/projectsetupscan.h"
#include "../src/io/importclassification.h"
#include "../src/io/dataimportservice.h"
#include "../src/io/horizonbinner.h"
#include "../src/services/projectdata.h"
#include "../src/services/previewdoc.h"
#include "../src/workflow/folderimport.h"
#include "../src/workflow/projectsetup.h"
#include "../src/qgis/qgisruntime.h"
#include "../src/qgis/qgisprojectservice.h"
#include "../src/qgis/projectmapreference.h"
#include "../src/catalog/datacatalog.h"
#include "../src/metadata/paleoprojectstore.h"
#include "../src/metadata/layermanifest.h"
#include "../src/qgis/qgislayerservice.h"
#include "../src/workflow/projectlayerrefresh.h"
#include "../src/ui/dialogs/projectsetupdialog.h"
#include "../src/ui/dialogs/projectmapsettingsdialog.h"
#include "../src/ui/paleotheme.h"
#include "../src/ui/datapreview/datapreviewtabs.h"
#include "../src/ui/wellcomposite/wellcompositepanel.h"
#include "../src/ui/wellcomposite/wellcompositecanvas.h"
#include <qgscoordinatetransform.h>
#include <qgsproject.h>
#include <qgsvectorlayer.h>
#include <qgsmaprenderersequentialjob.h>
#include "../src/qgis/facieshierarchyrenderer.h"

class TestProjectSetup : public QObject
{
  Q_OBJECT
  static bool write(const QString &path, const QByteArray &bytes) {
    QDir().mkpath(QFileInfo(path).absolutePath()); QFile f(path);
    return f.open(QIODevice::WriteOnly) && f.write(bytes) == bytes.size();
  }
  static QString xlsx(const QString &path, const QString &sheet, const QVector<QStringList> &rows) {
    QByteArray xml("<worksheet xmlns=\"http://schemas.openxmlformats.org/spreadsheetml/2006/main\"><sheetData>");
    for (int i = 0; i < rows.size(); ++i) {
      xml += "<row r=\"" + QByteArray::number(i + 1) + "\">";
      for (int c = 0; c < rows[i].size(); ++c) xml += "<c r=\"" + QByteArray(1, char('A' + c)) + QByteArray::number(i + 1) + "\" t=\"inlineStr\"><is><t>" + rows[i][c].toHtmlEscaped().toUtf8() + "</t></is></c>";
      xml += "</row>";
    }
    xml += "</sheetData></worksheet>";
    const QByteArray workbook = QString("<workbook xmlns=\"http://schemas.openxmlformats.org/spreadsheetml/2006/main\" xmlns:r=\"http://schemas.openxmlformats.org/officeDocument/2006/relationships\"><sheets><sheet name=\"%1\" sheetId=\"1\" r:id=\"rId1\"/></sheets></workbook>").arg(sheet).toUtf8();
    QDir().mkpath(QFileInfo(path).absolutePath()); QString error;
    return paleo::io::zipWriteArchive(path, {{"xl/workbook.xml", workbook}, {"xl/_rels/workbook.xml.rels", "<Relationships xmlns=\"http://schemas.openxmlformats.org/package/2006/relationships\"><Relationship Id=\"rId1\" Target=\"worksheets/sheet1.xml\"/></Relationships>"}, {"xl/worksheets/sheet1.xml", xml}}, {}, &error) ? path : QString();
  }
private slots:
  void interpretedTablesAreReadyOnImport() {
    QTemporaryDir source, projectDir; QString error;
    const auto heads = xlsx(source.filePath("heads.xlsx"), "data", {{"WELL", "X", "Y"}, {"A4", "10", "20"}, {"B4", "30", "40"}});
    const auto tops = xlsx(source.filePath("tops.xlsx"), "data", {{"WELL", QStringLiteral("层位"), QStringLiteral("顶界垂深(m)"), QStringLiteral("底界垂深(m)")}, {"A4", "D61", "100", "150"}, {"A4", "D62", "150", "200"}, {"B4", "D61", "80", "110"}});
    const auto cuttings = xlsx(source.filePath(QStringLiteral("A4/A4岩屑录井数据.xlsx")), "Sheet1", {{QStringLiteral("顶深(m)"), QStringLiteral("底深(m)"), QStringLiteral("岩石定名"), QStringLiteral("颜色"), QStringLiteral("岩性描述")}, {"120", "180", QStringLiteral("泥岩"), QStringLiteral("深灰"), QStringLiteral("保留描述，含换行\n第二行")}});
    const auto td = source.filePath("A4_td.dat"); QVERIFY(write(td, "# Well : A4\n0 0 0 0\n100 100 100 120\n200 200 200 240\n"));
    PaleoProjectStore store; DataImportService service(&store); service.setProjectDir(projectDir.path());
    auto session = service.beginImport(); QVERIFY(session); session->linkExternal = true;
    DataImportService::produceFiles(*session, {heads, tops, td, cuttings}, {{heads, "well_head"}, {tops, "well_stratification"}, {td, "time_depth"}}, {});
    QVERIFY2(session->error.isEmpty(), qPrintable(session->error));
    for (const auto &row : session->rows) QVERIFY2(row.outcome != FolderRowResult::Outcome::Failed, qPrintable(row.message));
    QVERIFY2(service.commitImport(*session, &error) == DataImportService::CommitStatus::Committed, qPrintable(error));
    auto *cat = service.catalog(); QCOMPARE(cat->entities("well").size(), 2); CatalogEntity well;
    for (const auto &candidate : cat->entities("well")) if (candidate.name == "A4") well = candidate;
    ProjectDataFacade data; data.setCatalog(cat); const auto layers = data.topsFor(well.id);
    QCOMPARE(layers.size(), 2); QCOMPARE(layers[0].md, 120.0); QCOMPARE(layers[1].md, 180.0); QCOMPARE(layers[1].tvd, 150.0);
    CatalogVersion normalized, rock;
    for (const auto &link : cat->linksForEntity(well.id)) {
      if (link.role == "tops") normalized = cat->currentVersion(link.assetId);
      if (link.role == "cuttings") rock = cat->currentVersion(link.assetId);
    }
    QVERIFY(normalized.managed); QVERIFY(normalized.extra.contains("depthResolution"));
    const auto bases = paleo::io::readWellTableBases(cat->versionFilePath(normalized)); QCOMPARE(bases.size(), 3); QCOMPARE(bases[1].md, 240.0); QCOMPARE(bases[1].tvd, 200.0);
    QVERIFY(rock.managed); QCOMPARE(rock.fileName, QString("cuttings.csv"));
    const auto table = paleo::io::readCuttingsFile(cat->versionFilePath(rock)); QVERIFY(table.ok); QCOMPARE(table.intervals.size(), 1);
    QCOMPARE(table.intervals[0].description, QStringLiteral("保留描述，含换行\n第二行"));
    QCOMPARE(WellComposite::LithologyStyle::colorFromName(table.intervals[0].litho), QColor("#616161"));
    QVERIFY(QFile::remove(tops)); QVERIFY(QFile::remove(cuttings));
    WellSectionWorkflow lithology(cat); QCOMPARE(lithology.lithologiesFor({well.id}).value(well.id).size(), 1);
    PreviewDocService doc(&service); QVector<WellTopRecord> display; QHash<QString, double> displayBases;
    doc.wellTopsForLog(well.id, &display, nullptr, &displayBases);
    QCOMPARE(displayBases.size(), 2); QCOMPARE(displayBases.value("D61"), 180.0); QCOMPARE(displayBases.value("D62"), 240.0);
  }
  void pointsOptionalAndMapping() {
    QTemporaryDir dir;
    const auto path = xlsx(dir.filePath("points.xlsx"), "UTM49N", {{QStringLiteral("井号"), QStringLiteral("井口横坐标X_UTM49N(m)"), QStringLiteral("井口纵坐标Y_UTM49N(m)")}, {"A1", "233240.46", "3997621.12"}, {"A2", "bad", "1"}});
    const auto t = paleo::io::readWellTable(path);
    QCOMPARE(t.heads.size(), 1); QCOMPARE(t.report.rejectedRows, 1); QCOMPARE(t.crs, QString("EPSG:32649")); QVERIFY(std::isnan(t.heads.first().kb));
    QCOMPARE(classifyImportFile(path).type, QString("well_head"));
    const auto custom = xlsx(dir.filePath("custom.xlsx"), "data", {{"ID", "E", "N"}, {"A9", "10", "20"}});
    QVERIFY(paleo::io::readWellTable(custom).heads.isEmpty());
    const auto mapped = paleo::io::readWellTable(custom, {{"well", "ID"}, {"x", "E"}, {"y", "N"}});
    QCOMPARE(mapped.heads.first().name, QString("A9"));
  }
  void stratificationDepthAndSubsets() {
    QTemporaryDir dir;
    const QVector<QStringList> rows{{QStringLiteral("层位"), QStringLiteral("顶界垂深(m)"), QStringLiteral("底界垂深(m)")}, {"D61", "100", "120"}, {"D62", "120", "130"}};
    auto path = xlsx(dir.filePath(QStringLiteral("井/总表.xlsx")), "A1", rows);
    xlsx(dir.filePath(QStringLiteral("井/A1/分层.xlsx")), "A1", rows);
    const auto table = paleo::io::readWellTable(path);
    QCOMPARE(table.tops.size(), 2); QVERIFY(table.tops[0].hasTvd); QVERIFY(!table.tops[0].hasMd);
    const auto bases = paleo::io::readWellTableBases(path); QCOMPARE(bases.size(), 2); QCOMPARE(bases[1].tvd, 130.0); QVERIFY(!bases[1].hasMd);
    ProjectSetupSources s; s.wells = dir.filePath(QStringLiteral("井"));
    const auto p = scanProjectSources(s); QCOMPARE(p.tops, 2); QCOMPARE(p.paths.size(), 1); QVERIFY(!p.notes.isEmpty());
  }
  void multiplePointSheetsAndDepthBases() {
    QTemporaryDir dir; QString error;
    const auto path = xlsx(dir.filePath("points.xlsx"), "first", {{"WELL", "X", "Y", "KB"}, {"Well A", "10", "20", "-99999"}});
    const auto archive = paleo::io::zipReadArchive(path, &error);
    QByteArray workbook, relations, sheet;
    QVERIFY(paleo::io::zipExtractBytes(archive, "xl/workbook.xml", &workbook, &error));
    QVERIFY(paleo::io::zipExtractBytes(archive, "xl/_rels/workbook.xml.rels", &relations, &error));
    QVERIFY(paleo::io::zipExtractBytes(archive, "xl/worksheets/sheet1.xml", &sheet, &error));
    workbook.replace("</sheets>", "<sheet name=\"second\" sheetId=\"2\" r:id=\"rId2\"/></sheets>");
    relations.replace("</Relationships>", "<Relationship Id=\"rId2\" Target=\"worksheets/sheet2.xml\"/></Relationships>");
    QByteArray second = sheet; second.replace("Well A", "Well B");
    QVERIFY(paleo::io::zipWriteArchive(path, {{"xl/workbook.xml", workbook}, {"xl/_rels/workbook.xml.rels", relations}, {"xl/worksheets/sheet1.xml", sheet}, {"xl/worksheets/sheet2.xml", second}}, {}, &error));
    const auto first = paleo::io::readWellTable(path);
    QCOMPARE(first.sheets.size(), 2); QCOMPARE(first.heads.size(), 1); QCOMPARE(first.mapping.value("sheet").toString(), QString("first"));
    QCOMPARE(first.heads[0].name, QString("Well A")); QVERIFY(std::isnan(first.heads[0].kb));
    const auto roundtrip = parseWellHeadText(first.normalized); QCOMPARE(roundtrip.size(), 1); QCOMPARE(roundtrip[0].name, QString("Well A"));
    const auto selected = paleo::io::readWellTable(path, {{"sheet", "second"}});
    QCOMPARE(selected.heads.size(), 1); QCOMPARE(selected.heads[0].name, QString("Well B"));
    const auto tops = xlsx(dir.filePath("tops.xlsx"), "A1", {{"TOP", "MD", "TVD"}, {"D61", "105", "100"}, {"D62", "120", "-99999"}});
    const auto depths = paleo::io::readWellTable(tops);
    QCOMPARE(depths.tops.size(), 2); QVERIFY(depths.tops[0].hasMd); QVERIFY(depths.tops[0].hasTvd);
    QCOMPARE(depths.tops[0].md, 105.0); QCOMPARE(depths.tops[0].tvd, 100.0);
    QVERIFY(depths.tops[1].hasMd); QVERIFY(!depths.tops[1].hasTvd);
  }
  void vectorsAndText() {
    QTemporaryDir dir;
    const auto text = dir.filePath("points.txt"); QVERIFY(write(text, "A1 10 20\nA2 30 40\n"));
    const auto t = paleo::io::readWellTable(text); QCOMPARE(t.heads.size(), 2); QVERIFY(std::isnan(t.heads[0].td));
    const auto vector = dir.filePath("points.geojson");
    QVERIFY(write(vector, R"({"type":"FeatureCollection","features":[{"type":"Feature","properties":{"name":"A1"},"geometry":{"type":"Point","coordinates":[108,36]}}]})"));
    const auto v = paleo::io::readWellTable(vector); QCOMPARE(v.heads.size(), 1); QCOMPARE(v.heads[0].x, 108.0); QVERIFY(!v.crs.isEmpty());
  }
  void noCoordinatesStillCreatesWell() {
    QTemporaryDir dir;
    const auto tops = xlsx(dir.filePath("tops.xlsx"), "A1", {{QStringLiteral("层位"), QStringLiteral("顶界垂深(m)")}, {"D61", "100"}});
    PaleoProjectStore store; DataImportService service(&store); service.setProjectDir(dir.filePath("project")); QString error;
    QVERIFY2(!service.importProjectFile(tops, &error).isEmpty(), qPrintable(error));
    const auto wells = service.catalog()->entities("well"); QCOMPARE(wells.size(), 1); QVERIFY(!wells.first().hasSurface);
  }
  void nativeHorizonAndBoundary() {
    QTemporaryDir dir; QString error;
    const QByteArray bytes("# Grid_size:2x2\n# P1: 1,1,0,0\n# P2: 1,2,20,0\n# P3: 2,2,20,40\n# Projection: UTM Zone 49N (WGS84)\n# Z_units: ms\n230000 4000000 100 1 1\n230020 4000001 101 1 2\n230002 4000040 102 2 1\n230022 4000041 103 2 2\n");
    BinnedHorizon grid; QVERIFY2(binHorizon(bytes, &grid, &error), qPrintable(error));
    QVERIFY(std::abs(grid.originX - 230002) < 1e-6); QVERIFY(std::abs(grid.originY - 4000040) < 1e-6); QVERIFY(std::abs(grid.columnY - 1) < 1e-8); QVERIFY(grid.geometryResidualM < 1e-6);
    const auto path = dir.filePath("boundary.geojson");
    QVERIFY(write(path, R"({"type":"FeatureCollection","features":[{"type":"Feature","properties":{},"geometry":{"type":"Polygon","coordinates":[[[0,0],[10,0],[5,8],[0,0]]]}}]})"));
    PaleoProjectStore store; DataImportService service(&store); service.setProjectDir(dir.filePath("project")); DataImportService::ImportOptions options; options.forceType = "boundary";
    QVERIFY2(!service.importProjectFile(path, options, &error).isEmpty(), qPrintable(error));
    const auto output = dir.filePath("survey.geojson"); QVERIFY(service.catalog()->writeSurveyGeoJson(output, &error));
    QFile f(output); QVERIFY(f.open(QIODevice::ReadOnly)); const auto coordinates = QJsonDocument::fromJson(f.readAll()).object()["features"].toArray().first().toObject()["geometry"].toObject()["coordinates"].toArray();
    QCOMPARE(coordinates.first().toArray().size(), 4); // triangle, not its bounding rectangle
  }
  void importAndReopen() {
    QTemporaryDir dir;
    const auto points = xlsx(dir.filePath("src/points.xlsx"), "UTM49N", {{QStringLiteral("井号"), "X", "Y"}, {"A1", "233240.46", "3997621.12"}});
    const auto tops = xlsx(dir.filePath("src/tops.xlsx"), "A1", {{QStringLiteral("层位"), QStringLiteral("顶界垂深(m)")}, {"D61", "100"}});
    PaleoProjectStore store; DataImportService service(&store); service.setProjectDir(dir.filePath("project")); QString error;
    const auto rows = service.importFiles({tops, points}, &error);
    QVERIFY2(error.isEmpty(), qPrintable(error)); QCOMPARE(service.catalog()->entities("well").size(), 1);
    for (const auto &row : rows) QVERIFY2(row.outcome != FolderRowResult::Outcome::Failed, qPrintable(row.message));
    ProjectDataFacade data; data.setCatalog(service.catalog(), dir.filePath("project"));
    const auto well = service.catalog()->entities("well").first();
    const auto imported = data.topsFor(well.id); QCOMPARE(imported.size(), 1); QVERIFY(std::isnan(imported[0].md)); QCOMPARE(imported[0].tvd, 100.0);
    const auto count = service.catalog()->assets().size(); service.importFiles({points, tops}, &error); QCOMPARE(service.catalog()->assets().size(), count);
    QVERIFY(service.catalog()->writeWellsGeoJson(dir.filePath("project/artifacts/layers/wells.geojson"), &error));
    DataCatalog reopened; QVERIFY(reopened.open(dir.filePath("project"), &error)); QCOMPARE(reopened.entities("well").size(), 1); QVERIFY(std::isnan(reopened.entities("well").first().kb));
    data.setCatalog(&reopened, dir.filePath("project")); QCOMPARE(data.topsFor(well.id).size(), 1);
  }
  void nativeProjectionRoundTrip() {
    PaleoGeoreference g; g.kind = "crs"; g.sourceCrs = "EPSG:32649"; g.targetCrs = "EPSG:4326";
    PaleoProjectFile settings; settings.georeference = g; ProjectMapSettingsDialog settingsDialog(settings); QCOMPARE(settingsDialog.configuration().georeference->sourceCrs, g.sourceCrs);
    QVERIFY(g.isComplete()); double lon = 0, lat = 0; QVERIFY(applyGeoreference(g, 233240.46, 3997621.12, &lon, &lat)); QVERIFY(std::abs(lon - 108.037366) < 0.000001);
    PaleoGeoreference copy; QString error; QVERIFY(paleoGeoreferenceFromJson(paleoGeoreferenceToJson(g), &copy, &error)); QCOMPARE(copy.sourceCrs, g.sourceCrs);
    QgsProject project; QVERIFY2(paleo::mapreference::configure(&project, g, "EPSG:3857", &error), qPrintable(error));
    QgsCoordinateTransform ct(QgsCoordinateReferenceSystem::fromWkt(DataCatalog::localGridCrsWkt()), QgsCoordinateReferenceSystem("EPSG:4326"), project.transformContext());
    const auto actual = ct.transform(QgsPointXY(233240.46, 3997621.12)); QVERIFY(std::abs(actual.x() - lon) < 1e-8); QVERIFY(std::abs(actual.y() - lat) < 1e-8);
  }
  void emptyWizard() {
    QgisProjectService project; PaleoProjectStore store; DataImportService service(&store); FolderImportWorkflow imports(&service, nullptr); ProjectSetupWorkflow workflow(&project, &imports);
    ProjectSetupDialog dialog(&workflow); dialog.findChild<QPushButton *>("createEmptyProjectButton")->click(); QCOMPARE(dialog.result(), int(QDialog::Accepted)); QVERIFY(dialog.preview().paths.isEmpty());
  }
  void createWorkflowAndReopen() {
    QTemporaryDir dir;
    const auto path = xlsx(dir.filePath("input/points.xlsx"), "UTM49N", {{"ID", "E", "N"}, {"A1", "233240.46", "3997621.12"}});
    QgisProjectService project; PaleoProjectStore store; DataImportService service(&store);
    connect(&project, &QgisProjectService::projectOpened, &service, [&service](const QString &p) { service.setProjectDir(QFileInfo(p).absolutePath()); });
    FolderImportWorkflow imports(&service, nullptr); ProjectSetupWorkflow workflow(&project, &imports);
    ProjectSetupSources s; s.points = path; s.pointMapping = {{"well", "ID"}, {"x", "E"}, {"y", "N"}};
    const auto p = scanProjectSources(s); QVERIFY(p.errors.isEmpty()); QString error;
    QSignalSpy finished(&workflow, &ProjectSetupWorkflow::finished);
    const auto target = dir.filePath("output/project.qgz"); QVERIFY2(workflow.create(target, s, p, &error), qPrintable(error));
    QTRY_COMPARE_WITH_TIMEOUT(finished.size(), 1, 20000); QVERIFY2(finished.first()[1].toBool(), qPrintable(finished.first()[0].toString()));
    QCOMPARE(service.catalog()->entities("well").size(), 1);
    QCOMPARE(service.catalog()->entities("well").first().surfaceX, 233240.46);
    QCOMPARE(workflow.currentSources().pointMapping.value("well").toString(), QString("ID"));
    QCOMPARE(project.mapConfiguration().sourceStats.value("pointMapping").toMap().value("well").toString(), QString("ID"));
    project.closeProject(); QVERIFY(project.openProject(target));
    QVERIFY(project.georeference().has_value()); QCOMPARE(project.georeference()->sourceCrs, QString("EPSG:32649"));
    QCOMPARE(workflow.currentSources().pointMapping.value("well").toString(), QString("ID"));
    QVERIFY(!workflow.create(target, s, p, &error));
  }
  void realWizardPreview() {
    const QString root = qEnvironmentVariable("PALEO_SETUP_SAMPLE_ROOT"); if (root.isEmpty()) QSKIP("Set PALEO_SETUP_SAMPLE_ROOT");
    QgisProjectService project; PaleoProjectStore store; DataImportService service(&store); FolderImportWorkflow imports(&service, nullptr); ProjectSetupWorkflow workflow(&project, &imports);
    ProjectSetupDialog dialog(&workflow); dialog.setSourceRoot(root); dialog.show();
    QTRY_VERIFY_WITH_TIMEOUT(dialog.preview().wells == 20, 90000);
    auto *maps = dialog.findChild<QTableWidget *>("projectPaleomapChoices"); QVERIFY(maps);
    int selected = 0; for (int row = 0; row < maps->rowCount(); ++row) if (maps->item(row, 0)->checkState() == Qt::Checked) ++selected;
    QCOMPARE(selected, 3);
    maps->item(0, 0)->setCheckState(Qt::Unchecked); QVERIFY(!dialog.preview().maps[0].included);
    maps->item(0, 0)->setCheckState(Qt::Checked); QVERIFY(dialog.preview().maps[0].included);
    const QString image = qEnvironmentVariable("PALEO_SETUP_SCREENSHOT");
    if (!image.isEmpty()) {
      dialog.findChild<QTabBar *>()->setCurrentIndex(2); QTest::qWait(100); QVERIFY(dialog.grab().save(image + ".sources.png"));
      dialog.findChild<QTabBar *>()->setCurrentIndex(3); QTest::qWait(100); QVERIFY(dialog.grab().save(image));
    }
  }
  void realSample() {
    const QString root = qEnvironmentVariable("PALEO_SETUP_SAMPLE_ROOT"); if (root.isEmpty()) QSKIP("Set PALEO_SETUP_SAMPLE_ROOT for read-only real data verification");
    const auto sources = discoverProjectSources(root); const auto p = scanProjectSources(sources);
    QVERIFY2(p.errors.isEmpty(), qPrintable(p.errors.join('\n'))); QCOMPARE(p.wells, 20); QCOMPARE(p.tops, 516); QCOMPARE(p.logs, 21); QCOMPARE(p.seismic, 1); QCOMPARE(p.horizons, 8); QCOMPARE(p.sourceCrs, QString("EPSG:32649"));
    for (const auto &path : p.paths) QVERIFY(!path.contains(QStringLiteral("_旧项目工作区")));
    QCOMPARE(QFileInfo(sources.paleomaps).fileName(), QStringLiteral("古地理图"));
    int maps = 0; for (const auto &map : p.maps) if (map.included) { ++maps; QCOMPARE(QFileInfo(map.path).suffix(), QString("gpkg")); QVERIFY(!map.horizon.isEmpty()); QVERIFY(map.problem.isEmpty()); }
    QCOMPARE(maps, 3);
  }
  void mapBundlesValidateAndKeepStyles() {
    QTemporaryDir dir; QString error;
    const auto path = dir.filePath("D61_map.shp");
    QVERIFY(write(path, "shape")); QVERIFY(write(dir.filePath("D61_map.shx"), "index"));
    ProjectSetupSources s; s.paleomaps = dir.path(); auto p = scanProjectSources(s);
    QCOMPARE(p.maps.size(), 1); QVERIFY(!p.maps[0].included); QVERIFY(!p.maps[0].problem.isEmpty());
    QVERIFY(write(dir.filePath("D61_map.dbf"), "attributes")); QVERIFY(write(dir.filePath("D61_map.cpg"), "UTF-8"));
    QVERIFY(write(dir.filePath("D61_map.qml"), "style"));
    const auto members = PaleoMapFiles::members(path, &error); QCOMPARE(members.size(), 5);
    QVariantMap hashes; for (const auto &member : members) hashes.insert(member, DataCatalog::sha256FileHex(member));
    QVERIFY(write(dir.filePath("D61_map.dbf"), "changed"));
    QVERIFY(PaleoMapFiles::copyDataset(path, dir.filePath("copy"), [] { return false; }, &error, hashes).isEmpty());
    QVERIFY(error.contains(QStringLiteral("预览")));
    const auto gpkg = dir.filePath("D62_map.gpkg"); QVERIFY(write(gpkg, "package"));
    QVERIFY(write(gpkg + ".qml", "package-specific style"));
    QVERIFY(PaleoMapFiles::members(gpkg, &error).contains(gpkg + ".qml"));
  }
  void realMapWizardImportAndReopen() {
    const QString root = qEnvironmentVariable("PALEO_SETUP_SAMPLE_ROOT"); if (root.isEmpty()) QSKIP("Set PALEO_SETUP_SAMPLE_ROOT");
    QTemporaryDir dir; QString error;
    QgisProjectService project; PaleoProjectStore store; DataImportService service(&store);
    connect(&project, &QgisProjectService::projectOpened, &service, [&service](const QString &path) { service.setProjectDir(QFileInfo(path).absolutePath()); });
    LayerManifest manifest(dir.filePath("metadata/project.sqlite"));
    QgisLayerService layers(&project, &manifest); PaleoTaskService tasks;
    FolderImportWorkflow imports(&service, nullptr); ProjectSetupWorkflow workflow(&project, &imports); workflow.setMapImportServices(&layers, &tasks);
    ProjectSetupSources s; s.paleomaps = QDir(root).filePath(QStringLiteral("古地理图"));
    const auto p = scanProjectSources(s); QVERIFY(p.errors.isEmpty());
    ProjectSetupSources single; single.paleomaps = QDir(s.paleomaps).filePath("D61_historical_facies_map.shp");
    const auto singlePreview = scanProjectSources(single); QCOMPARE(singlePreview.maps.size(), 1); QVERIFY(singlePreview.maps[0].included);
    QCOMPARE(p.projectCrs, QString("EPSG:32649"));
    QSignalSpy finished(&workflow, &ProjectSetupWorkflow::finished);
    // Create first so the manifest parent directory exists before map publication.
    const auto path = dir.filePath("project.qgz"); QVERIFY2(workflow.create(path, s, p, &error), qPrintable(error));
    QVERIFY2(manifest.open(&error), qPrintable(error));
    QTRY_COMPARE_WITH_TIMEOUT(finished.size(), 1, 90000); QVERIFY2(finished[0][1].toBool(), qPrintable(finished[0][0].toString()));
    int maps = 0, features = 0;
    for (const auto &asset : service.catalog()->assets()) if (asset.type == "paleogeographic_map") {
      ++maps; const auto current = service.catalog()->currentVersion(asset.id); QCOMPARE(current.stage, QString("DERIVED"));
      auto *layer = qobject_cast<QgsVectorLayer *>(layers.instantiate(current.extra.value("layer_id").toString(), &error));
      QVERIFY2(layer, qPrintable(error)); features += int(layer->featureCount());
      for (const auto &level : {QString("facies"), QString("sub_facies"), QString("micro_facies")}) {
        FaciesHierarchyRenderer::apply(layer, current.extra.value("facies").toList(), level);
        QgsMapSettings settings; settings.setDestinationCrs(project.project()->crs());
        settings.setTransformContext(project.project()->transformContext()); settings.setLayers({layer});
        settings.setOutputSize(QSize(700, 800)); settings.setBackgroundColor(Qt::white);
        QgsCoordinateTransform transform(layer->crs(), settings.destinationCrs(), settings.transformContext());
        settings.setExtent(transform.transformBoundingBox(layer->extent()));
        QgsMapRendererSequentialJob job(settings); job.start(); job.waitForFinished();
        int painted = 0; const auto image = job.renderedImage();
        for (int y = 0; y < image.height(); ++y) for (int x = 0; x < image.width(); ++x)
          if (image.pixelColor(x, y) != QColor(Qt::white)) ++painted;
        qInfo() << current.id << level << "painted" << painted << "errors" << job.errors().size();
        QVERIFY2(painted > 1000, qPrintable(current.fileName + " " + level));
      }
    }
    QCOMPARE(maps, 3); QCOMPARE(features, 32); QCOMPARE(manifest.all().size(), 6);
    QVERIFY(project.writeProject()); project.closeProject(); QVERIFY(project.openProject(path));
    QCOMPARE(workflow.currentSources().paleomaps, s.paleomaps); QCOMPARE(workflow.currentSources().mapChoices.size(), p.maps.size());
    const auto before = service.catalog()->assets().size(); finished.clear();
    QVERIFY(workflow.applySources(workflow.currentSources(), p, &error));
    QTRY_COMPARE_WITH_TIMEOUT(finished.size(), 1, 90000); QVERIFY2(finished[0][1].toBool(), qPrintable(finished[0][0].toString()));
    QCOMPARE(service.catalog()->assets().size(), before);
    auto shapePreview = p;
    for (auto &map : shapePreview.maps) map.included = map.horizon == "D61" && QFileInfo(map.path).suffix() == "shp";
    finished.clear(); QVERIFY(workflow.applySources(s, shapePreview, &error));
    QTRY_COMPARE_WITH_TIMEOUT(finished.size(), 1, 90000); QVERIFY2(finished[0][1].toBool(), qPrintable(finished[0][0].toString()));
    QCOMPARE(service.catalog()->assets().size(), before + 1);
    bool checkedShape = false;
    for (const auto &asset : service.catalog()->assets()) if (asset.type == "paleogeographic_map" && asset.format == "shp") {
      for (const auto &raw : service.catalog()->versionsForAsset(asset.id)) if (raw.stage == "RAW") {
        const auto members = raw.extra.value("bundle_members").toList(); QCOMPARE(members.size(), 6);
        for (const auto &member : members) {
          const auto row = member.toMap(); const auto managed = QFileInfo(service.catalog()->versionFilePath(raw)).dir().filePath(row.value("file").toString());
          QVERIFY(QFileInfo::exists(managed)); QCOMPARE(DataCatalog::sha256FileHex(managed), row.value("sha256").toString());
        }
        checkedShape = true;
      }
    }
    QVERIFY(checkedShape);
  }
  void realSampleImport() {
    const QString root = qEnvironmentVariable("PALEO_SETUP_SAMPLE_ROOT");
    if (root.isEmpty() || !qEnvironmentVariableIsSet("PALEO_SETUP_IMPORT_REAL")) QSKIP("Opt in with PALEO_SETUP_IMPORT_REAL");
    QTemporaryDir dir; QString error;
    QgisProjectService project; QVERIFY(project.createProject(dir.filePath("sample.qgz")));
    LayerManifest manifest(dir.filePath("metadata/project.sqlite")); QVERIFY(manifest.open(&error));
    QgisLayerService layers(&project, &manifest);
    PaleoProjectStore store; store.setProjectPaths(project.projectPath(), dir.filePath("sample.gpkg"), dir.filePath("metadata/project.sqlite"));
    DataImportService service(&store); service.setProjectDir(dir.path());
    connect(&service, &DataImportService::layerDeclared, &layers, [&layers](const LayerDeclaration &d) { QString e; QVERIFY2(layers.declare(d, &e), qPrintable(e)); });
    const auto sources = discoverProjectSources(root); const auto p = scanProjectSources(sources);
    auto config = project.mapConfiguration(); PaleoGeoreference reference; reference.kind = "crs"; reference.sourceCrs = p.projectCrs; reference.targetCrs = "EPSG:4326"; config.georeference = reference;
    QVERIFY2(project.updateMapConfiguration(config, &error), qPrintable(error)); service.setGeoreference(reference);
    auto session = service.beginImport(); QVERIFY(session);
    session->linkExternal = true; // Current UI default: indexed RAW, managed derived layers.
    session->wellTargets = p.wellTargets;
    session->tableMappings = p.mappings;
    session->expectedSourceHashes = p.hashes;
    DataImportService::produceFiles(*session, p.paths, p.types, {});
    QVERIFY2(service.commitImport(*session, &error) == DataImportService::CommitStatus::Committed, qPrintable(error));
    const auto rows = session->rows;
    QVERIFY2(error.isEmpty(), qPrintable(error));
    int failures = 0; for (const auto &row : rows) if (row.outcome == FolderRowResult::Outcome::Failed) { qWarning() << row.path << row.message; ++failures; }
    QCOMPARE(failures, 0); QCOMPARE(service.catalog()->entities("well").size(), 20); QCOMPARE(service.catalog()->entities("seismic_survey").size(), 1);
    const auto prepared = ProjectLayerRefreshWorkflow::prepare(*service.catalog(), dir.path()); QVERIFY(prepared.wellsPrepared);
    const QString wells = dir.filePath("artifacts/layers/wells.geojson"), survey = dir.filePath("artifacts/layers/survey_area.geojson");
    QVERIFY(write(wells, prepared.wellsBytes)); QVERIFY(service.catalog()->writeSurveyGeoJson(survey, &error));
    for (const auto &pair : {qMakePair(QString("wells"), wells), qMakePair(QString("survey.area"), survey)}) {
      LayerDeclaration d; d.layerId = pair.first; d.type = "vector"; d.source = pair.second; d.group = "00_Data";
      QVERIFY2(layers.declare(d, &error), qPrintable(error));
      auto *layer = qobject_cast<QgsVectorLayer *>(layers.instantiate(d.layerId, &error)); QVERIFY2(layer, qPrintable(error));
      QCOMPARE(layer->featureCount(), pair.first == "wells" ? qint64(20) : qint64(1));
      QVERIFY(layer->extent().xMinimum() > 200000); QVERIFY(layer->extent().yMinimum() > 3900000);
    }
    QVERIFY(project.writeProject());
    DataCatalog reopened; QVERIFY(reopened.open(dir.path(), &error));
    ProjectDataFacade data; data.setCatalog(&reopened, dir.path());
    int topCount = 0, rockWells = 0; WellSectionWorkflow lithology(&reopened);
    for (const auto &well : reopened.entities("well")) {
      const auto tops = data.topsFor(well.id); topCount += tops.size();
      for (const auto &top : tops) QVERIFY2(std::isfinite(top.md), qPrintable(well.name + " " + top.horizon));
      const auto segments = lithology.lithologiesFor({well.id}).value(well.id); if (!segments.isEmpty()) ++rockWells;
      qInfo() << well.name << "tops" << tops.size() << "lithology" << segments.size();
    }
    QCOMPARE(topCount, 516); QCOMPARE(rockWells, 20);
    QString a4;
    for (const auto &well : service.catalog()->entities("well")) if (well.name == "A4") a4 = well.id;
    QVERIFY(!a4.isEmpty()); QString log;
    for (const auto &link : service.catalog()->linksForEntity(a4)) if (link.role == "well_log" && link.isPrimary) log = link.assetId;
    QVERIFY(!log.isEmpty());
    DataPreviewTabs preview; preview.setImportService(&service); preview.openAsset(log);
    auto *panel = preview.findChild<WellComposite::WellCompositePanel *>("wellCompositePanel"); QVERIFY(panel);
    QTRY_COMPARE_WITH_TIMEOUT(panel->currentData().lithologyIntervals.size(), 703, 30000);
    QCOMPARE(panel->currentData().formationIntervals.size(), 28);
    for (const auto &base : paleo::io::readWellTableBases(QDir(root).filePath(QStringLiteral("井/分层数据总表.xlsx")))) {
      if (base.wellName != "A4" || !base.hasTvd) continue;
      for (const auto &layer : panel->currentData().formationIntervals)
        if (layer.name == base.topName && base.tvd > layer.topDepth) QVERIFY(std::abs(double(layer.bottomDepth) - base.tvd) < .001);
    }
    auto *canvas = panel->findChild<WellComposite::WellCompositeCanvas *>(); QVERIFY(canvas);
    bool hasRockTrack = false;
    for (const auto &track : canvas->tracks()) if (track->type() == WellComposite::TrackType::Lithology) hasRockTrack = true;
    QVERIFY(hasRockTrack);
    const auto capture = qEnvironmentVariable("PALEO_IMPORT_ROCK_CAPTURE");
    if (!capture.isEmpty()) { preview.resize(1450, 1050); preview.show(); QTest::qWait(100); canvas->setScrollDepth(1690); QTest::qWait(100); QVERIFY(panel->grab().save(capture)); }
    QCOMPARE(manifest.all().size(), 10); // eight horizons + wells + area
  }
};
int main(int argc, char **argv) {
  if (!QgisRuntime::initialize()) return 1;
  PaleoTheme::ensureApplicationFonts(); PaleoTheme::applyLightTheme();
  TestProjectSetup test; const int result = QTest::qExec(&test, argc, argv); QgisRuntime::shutdown(); return result;
}
#include "tst_projectsetup.moc"
