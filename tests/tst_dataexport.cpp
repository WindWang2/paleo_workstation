#include <QtTest>
#include <QTemporaryDir>
#include <QFile>
#include <QJsonDocument>
#include <QJsonObject>
#include <QDir>
#include <gdal.h>
#include <ogr_api.h>
#include "../src/services/dataexport.h"
#include "../src/qgis/dataexportwriter.h"
#include "../src/workflow/dataexportworkflow.h"
#include "../src/services/paleotaskservice.h"
#include "../src/qgis/qgisruntime.h"

using namespace paleo::exporting;
class TestDataExport : public QObject {
  Q_OBJECT
  static QString add(DataCatalog &cat, const QString &name, const QByteArray &bytes,
      const QStringList &parents = {}, const QString &assetId = {}) {
    CatalogAsset a; a.id = assetId.isEmpty() ? cat.nextAssetId() : assetId;
    a.displayName = name; a.type = "auxiliary"; a.format = QFileInfo(name).suffix();
    if (assetId.isEmpty() && !cat.addAsset(a)) return {};
    CatalogVersion v; v.id = cat.nextVersionId(); v.assetId = a.id;
    v.stage = parents.isEmpty() ? "RAW" : "DERIVED"; v.parentVersionIds = parents;
    v.versionNumber = cat.versionsForAsset(a.id).size() + 1; v.fileName = name;
    v.path = DataCatalog::managedPath(v.stage, a.id, v.id, name);
    const auto path = QDir(cat.projectDir()).filePath(v.path);
    QDir().mkpath(QFileInfo(path).absolutePath());
    QFile file(path);
    if (!file.open(QIODevice::WriteOnly) || file.write(bytes) != bytes.size()) return {};
    file.close(); v.sha256 = DataCatalog::sha256FileHex(path);
    return cat.addVersion(v) ? v.id : QString();
  }
private slots:
  void interpretationSourcesComeFromTheGeoPackage() {
    QTemporaryDir tmp; DataCatalog cat; QVERIFY(cat.open(tmp.path()));
    const auto raw = add(cat, "calculation.dat", "calculation input");
    const auto evidence = add(cat, "evidence.dat", "manual interpretation source");
    const auto path = tmp.filePath("facies.gpkg");
    auto dataset = GDALCreate(GDALGetDriverByName("GPKG"), path.toUtf8().constData(), 0, 0, 0, GDT_Unknown, nullptr); QVERIFY(dataset);
    auto layer = GDALDatasetCreateLayer(dataset, "facies", nullptr, wkbPoint, nullptr); QVERIFY(layer);
    auto field = OGR_Fld_Create("facies_evidence", OFTString); QCOMPARE(OGR_L_CreateField(layer, field, true), OGRERR_NONE); OGR_Fld_Destroy(field);
    auto feature = OGR_F_Create(OGR_L_GetLayerDefn(layer));
    const auto json = QByteArray("[{\"id\":\"test-evidence\",\"role\":\"SUPPORTED_BY\",\"source_version\":\"") + evidence.toUtf8() + "\"}]";
    OGR_F_SetFieldString(feature, 0, json.constData()); QCOMPARE(OGR_L_CreateFeature(layer, feature), OGRERR_NONE);
    OGR_F_Destroy(feature); GDALClose(dataset);
    CatalogAsset asset; asset.id = cat.nextAssetId(); asset.type = "mapping_product"; asset.format = "gpkg"; asset.displayName = "facies.gpkg"; QVERIFY(cat.addAsset(asset));
    CatalogVersion version; version.id = cat.nextVersionId(); version.assetId = asset.id; version.stage = "DERIVED"; version.managed = false;
    version.path = path; version.fileName = "facies.gpkg"; version.parentVersionIds = {raw}; version.sha256 = DataCatalog::sha256FileHex(path); QVERIFY(cat.addVersion(version));
    DataRequest request; request.versionIds = {version.id}; request.scope = Scope::Sources;
    const auto p = plan(cat, tmp.path(), request);
    QVERIFY2(p.failures.isEmpty(), qPrintable(p.failures.join("\n")));
    QCOMPARE(p.items.size(), 3);
    bool found = false;
    for (const auto &relation : p.relations) found = found || relation.toMap().value("provenance").toString().startsWith("gpkg:facies_evidence#");
    QVERIFY(found);
  }
  void vectorExchangePreservesNumericValuesAndNulls() {
    QTemporaryDir tmp;
    const auto source = tmp.filePath("input.gpkg");
    auto dataset = GDALCreate(GDALGetDriverByName("GPKG"), source.toUtf8().constData(), 0, 0, 0, GDT_Unknown, nullptr); QVERIFY(dataset);
    auto layer = GDALDatasetCreateLayer(dataset, "factor", nullptr, wkbPoint, nullptr); QVERIFY(layer);
    auto field = OGR_Fld_Create("factor_value", OFTReal); QCOMPARE(OGR_L_CreateField(layer, field, true), OGRERR_NONE); OGR_Fld_Destroy(field);
    for (int i = 0; i < 2; ++i) {
      auto f = OGR_F_Create(OGR_L_GetLayerDefn(layer));
      if (i == 0) OGR_F_SetFieldDouble(f, 0, 0.123456789);
      QCOMPARE(OGR_L_CreateFeature(layer, f), OGRERR_NONE); OGR_F_Destroy(f);
    }
    GDALClose(dataset);
    const auto destination = tmp.filePath("converted"); QVERIFY(QDir().mkpath(destination));
    QStringList files; QVariantMap description; QString error;
    QVERIFY2(PaleoDataExportWriter::write(source, destination, DataFormat::GeoPackage, &files, &description, &error), qPrintable(error));
    dataset = GDALOpenEx(files.first().toUtf8().constData(), GDAL_OF_VECTOR | GDAL_OF_READONLY, nullptr, nullptr, nullptr); QVERIFY(dataset);
    layer = GDALDatasetGetLayer(dataset, 0); QCOMPARE(OGR_L_GetFeatureCount(layer, true), 2);
    auto f = OGR_L_GetNextFeature(layer); QVERIFY(f);
    QCOMPARE(OGR_F_GetFieldAsDouble(f, OGR_F_GetFieldIndex(f, "factor_value")), 0.123456789); OGR_F_Destroy(f);
    f = OGR_L_GetNextFeature(layer); QVERIFY(f); QVERIFY(!OGR_F_IsFieldSetAndNotNull(f, OGR_F_GetFieldIndex(f, "factor_value")));
    OGR_F_Destroy(f); GDALClose(dataset);
    const auto csvDirectory = tmp.filePath("csv"); QVERIFY(QDir().mkpath(csvDirectory));
    files.clear(); description.clear();
    QVERIFY2(PaleoDataExportWriter::write(source, csvDirectory, DataFormat::Csv, &files, &description, &error), qPrintable(error));
    QVERIFY(QFileInfo::exists(QDir(csvDirectory).filePath("data_1.csvt")));
    dataset = GDALOpenEx(QDir(csvDirectory).filePath("data_1.csv").toUtf8().constData(), GDAL_OF_VECTOR | GDAL_OF_READONLY, nullptr, nullptr, nullptr); QVERIFY(dataset);
    layer = GDALDatasetGetLayer(dataset, 0);
    const int valueField = OGR_FD_GetFieldIndex(OGR_L_GetLayerDefn(layer), "factor_value"); QVERIFY(valueField >= 0);
    QCOMPARE(OGR_Fld_GetType(OGR_FD_GetFieldDefn(OGR_L_GetLayerDefn(layer), valueField)), OFTReal);
    f = OGR_L_GetNextFeature(layer); QVERIFY(f); QCOMPARE(OGR_F_GetFieldAsDouble(f, valueField), 0.123456789);
    OGR_F_Destroy(f); GDALClose(dataset);
  }
  void originalsAndHistoryAreByteExact() {
    QTemporaryDir tmp; DataCatalog cat; QVERIFY(cat.open(tmp.path()));
    const auto raw = add(cat, "同名.dat", "old raw\n"); QVERIFY(!raw.isEmpty());
    const auto asset = cat.versionById(raw).assetId;
    QVERIFY(!add(cat, "同名.dat", "new raw\n", {}, asset).isEmpty());
    const auto other = add(cat, "同名.dat", "other well\n");
    const auto originalPath = cat.versionFilePath(cat.versionById(raw));
    QCOMPARE(versionIdsForPaths(cat, {originalPath, originalPath}).value(originalPath), QStringList{raw});
    DataRequest request; request.versionIds = {raw, other}; request.scope = Scope::Original;
    request.destination = tmp.filePath("out");
    const auto seq = cat.mutationSeq();
    const auto result = execute(plan(cat, tmp.path(), request));
    QVERIFY2(result.complete(), qPrintable(result.error + result.failures.join("\n")));
    QCOMPARE(result.succeeded, 2); QCOMPARE(cat.mutationSeq(), seq);
    QFile exported(QDir(result.directory).filePath("data/" + asset + "/" + raw + "/同名.dat"));
    QVERIFY(exported.open(QIODevice::ReadOnly)); QCOMPARE(exported.readAll(), QByteArray("old raw\n"));
    QVERIFY(!execute(plan(cat, tmp.path(), request)).complete()); // no overwrite
  }
  void sourcesUseHistoricalInputsAndDeduplicate() {
    QTemporaryDir tmp; DataCatalog cat; QVERIFY(cat.open(tmp.path()));
    const auto raw = add(cat, "well.dat", "historical\n");
    QVERIFY(!add(cat, "well.dat", "newest\n", {}, cat.versionById(raw).assetId).isEmpty());
    const auto one = add(cat, "one.dat", "one\n", {raw});
    const auto two = add(cat, "two.dat", "two\n", {raw});
    const auto output = add(cat, "out.dat", "result\n", {one, two});
    DataRequest request; request.versionIds = {output}; request.scope = Scope::Sources; request.destination = tmp.filePath("sources");
    const auto exportPlan = plan(cat, tmp.path(), request);
    QCOMPARE(exportPlan.items.size(), 4); QVERIFY(exportPlan.failures.isEmpty());
    QVERIFY(!exportPlan.relations.isEmpty());
    const auto result = execute(exportPlan); QVERIFY2(result.complete(), qPrintable(result.failures.join("\n")));
  }
  void tamperingAndCancellationNeverSucceed() {
    QTemporaryDir tmp; DataCatalog cat; QVERIFY(cat.open(tmp.path()));
    const auto id = add(cat, "well.dat", "registered");
    DataRequest request; request.versionIds = {id}; request.destination = tmp.filePath("tampered");
    QFile file(cat.versionFilePath(cat.versionById(id))); QVERIFY(file.open(QIODevice::WriteOnly)); file.write("tampered"); file.close();
    const auto result = execute(plan(cat, tmp.path(), request));
    QVERIFY(!result.complete()); QCOMPARE(result.succeeded, 0); QVERIFY(!result.failures.isEmpty());
    request.destination = tmp.filePath("cancelled");
    auto p = plan(cat, tmp.path(), request);
    const auto cancelled = execute(p, [] { return true; });
    QVERIFY(cancelled.cancelled); QVERIFY(!QFileInfo::exists(request.destination));
  }
  void missingSourceIsExplicitAndSnapshotIsReadOnly() {
    QTemporaryDir tmp; DataCatalog cat; QVERIFY(cat.open(tmp.path()));
    const auto raw = add(cat, "well.dat", "raw");
    const auto output = add(cat, "out.dat", "derived", {raw});
    QVERIFY(QFile::remove(cat.versionFilePath(cat.versionById(raw))));
    DataRequest request; request.versionIds = {output}; request.scope = Scope::Sources; request.destination = tmp.filePath("missing");
    const auto p = plan(cat, tmp.path(), request); QVERIFY(!p.failures.isEmpty());
    const auto result = execute(p); QVERIFY(!result.complete()); QCOMPARE(result.succeeded, 1);
    QFile metadata(QDir(result.directory).filePath("metadata.json")); QVERIFY(metadata.open(QIODevice::ReadOnly));
    QVERIFY(!QJsonDocument::fromJson(metadata.readAll()).object().value("complete").toBool());
  }
  void numericRasterRoundTrips() {
    QTemporaryDir tmp; DataCatalog cat; QVERIFY(cat.open(tmp.path()));
    const auto source = tmp.filePath("numeric.tif");
    auto driver = GDALGetDriverByName("GTiff"); QVERIFY(driver);
    auto dataset = GDALCreate(driver, source.toUtf8().constData(), 3, 2, 1, GDT_Float32, nullptr); QVERIFY(dataset);
    float values[] = {1.25f, -9999.f, 3.75f, 4.5f, 5.25f, 6.f};
    double transform[] = {100, 10, 0, 200, 0, -10}; GDALSetGeoTransform(dataset, transform);
    GDALSetProjection(dataset, DataCatalog::localGridCrsWkt().toUtf8().constData());
    GDALSetRasterNoDataValue(GDALGetRasterBand(dataset, 1), -9999);
    QCOMPARE(GDALRasterIO(GDALGetRasterBand(dataset, 1), GF_Write, 0, 0, 3, 2, values, 3, 2, GDT_Float32, 0, 0), CE_None);
    GDALClose(dataset);
    CatalogAsset a; a.id = cat.nextAssetId(); a.type = "raster"; a.format = "tif"; a.displayName = "numeric.tif"; QVERIFY(cat.addAsset(a));
    CatalogVersion v; v.id = cat.nextVersionId(); v.assetId = a.id; v.stage = "RAW"; v.managed = false; v.path = source;
    v.fileName = a.displayName; v.sha256 = DataCatalog::sha256FileHex(source); QVERIFY(cat.addVersion(v));
    DataRequest request; request.versionIds = {v.id}; request.format = DataFormat::GeoTiff; request.destination = tmp.filePath("raster");
    const auto result = execute(plan(cat, tmp.path(), request), {}, {}, PaleoDataExportWriter::write); QVERIFY2(result.complete(), qPrintable(result.failures.join("\n")));
    const auto path = QDir(result.directory).filePath("data/" + a.id + "/" + v.id + "/data.tif");
    dataset = GDALOpen(path.toUtf8().constData(), GA_ReadOnly); QVERIFY(dataset);
    float actual[6] = {}; QCOMPARE(GDALRasterIO(GDALGetRasterBand(dataset, 1), GF_Read, 0, 0, 3, 2, actual, 3, 2, GDT_Float32, 0, 0), CE_None);
    for (int i = 0; i < 6; ++i) QCOMPARE(actual[i], values[i]);
    QCOMPARE(GDALGetRasterDataType(GDALGetRasterBand(dataset, 1)), GDT_Float32);
    QCOMPARE(GDALGetRasterNoDataValue(GDALGetRasterBand(dataset, 1), nullptr), -9999.);
    double actualTransform[6]; GDALGetGeoTransform(dataset, actualTransform);
    for (int i = 0; i < 6; ++i) QCOMPARE(actualTransform[i], transform[i]);
    GDALClose(dataset);
  }
  void backgroundExportCompletes() {
    QTemporaryDir tmp; DataCatalog cat; QVERIFY(cat.open(tmp.path()));
    const auto id = add(cat, "a.dat", "payload");
    PaleoTaskService tasks;
    DataRequest request; request.versionIds = {id}; request.destination = tmp.filePath("async");
    Result result; bool called = false; QString error;
    auto *task = startDataExport(&cat, &tasks, request, this, [&](const Result &r) { result = r; called = true; }, &error);
    QVERIFY2(task, qPrintable(error)); QTRY_VERIFY_WITH_TIMEOUT(called, 10000);
    QVERIFY(result.complete()); QCOMPARE(task->state(), PaleoTask::State::Succeeded);
  }
};
int main(int argc, char **argv) {
  qputenv("QT_QPA_PLATFORM", "offscreen");
  if (!QgisRuntime::initialize("/usr")) return 1;
  TestDataExport test; const int result = QTest::qExec(&test, argc, argv); QgisRuntime::shutdown(); return result;
}
#include "tst_dataexport.moc"
