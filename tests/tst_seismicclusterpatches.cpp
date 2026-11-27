#include "qgis/seismicclusterpatches.h"
#include <QCryptographicHash>
#include <QTemporaryDir>
#include <QtTest>
#include <gdal_priv.h>
#include <ogrsf_frmts.h>

namespace {
using namespace SeismicClusterPatches;
QByteArray digest(const QString &path) {
  QFile file(path);
  if (!file.open(QIODevice::ReadOnly)) return {};
  QCryptographicHash hash(QCryptographicHash::Sha256);
  hash.addData(&file);
  return hash.result();
}
bool raster(const QString &path) {
  constexpr int n = -9999;
  int cells[]{1,1,1,n,1,
              1,n,1,2,n,
              1,1,1,n,2};
  GDALAllRegister();
  auto *ds = GetGDALDriverManager()->GetDriverByName("GTiff")->Create(
      path.toUtf8().constData(), 5, 3, 1, GDT_Int32, nullptr);
  if (!ds) return false;
  double gt[]{100,10,0,150,0,-10};
  auto *band = ds->GetRasterBand(1);
  const bool success = ds->SetGeoTransform(gt) == CE_None && band->SetNoDataValue(n) == CE_None &&
      band->RasterIO(GF_Write,0,0,5,3,cells,5,3,GDT_Int32,0,0,nullptr) == CE_None;
  GDALClose(ds);
  return success;
}
QMap<qint64, QVariantMap> byPatch(const QString &path) {
  QMap<qint64, QVariantMap> rows;
  QString error;
  for (const auto &row : choices(path, &error)) rows.insert(row.toMap()["patch_id"].toLongLong(), row.toMap());
  return rows;
}
const QVariantList schema{QVariantMap{{"code",1},{"name","channel"}},QVariantMap{{"code",2},{"name","bar"}}};
}

class SeismicClusterPatchesTest : public QObject {
  Q_OBJECT
private slots:
  void preservesGridPatchesAndHole() {
    QTemporaryDir dir;
    const QString raw = dir.filePath("cluster.tif"), patches = dir.filePath("patches.gpkg");
    QVERIFY(raster(raw));
    QString error;
    QVERIFY2(polygonize(raw,patches,{},&error),qPrintable(error));
    const auto rows = byPatch(patches);
    QCOMPARE(rows.keys(), (QList<qint64>{0,4,8,14}));
    QCOMPARE(rows[0]["pixel_count"].toInt(), 8);
    QCOMPARE(rows[4]["cluster_id"].toInt(), 1);
    QCOMPARE(rows[4]["pixel_count"].toInt(), 1);
    QCOMPARE(rows[8]["cluster_id"].toInt(), 2);
    QCOMPARE(rows[14]["cluster_id"].toInt(), 2);
    QVERIFY(rows[0]["facies_code"].isNull());
    auto *ds = GDALDataset::Open(patches.toUtf8().constData(), GDAL_OF_VECTOR);
    QVERIFY(ds);
    auto *layer = ds->GetLayerByName("cluster_patches");
    QVERIFY(layer);
    QVERIFY(layer->FindFieldIndex("facies_code",TRUE) < 0);
    OGRFeatureUniquePtr ring(layer->GetFeature(rows[0]["feature_id"].toLongLong()));
    QVERIFY(ring && ring->GetGeometryRef());
    QCOMPARE(ring->GetGeometryRef()->toPolygon()->getNumInteriorRings(), 1);
    QCOMPARE(ring->GetGeometryRef()->toPolygon()->get_Area(), 800.0);
    ring.reset();
    GDALClose(ds);
  }
  void appliesWellSuggestionsAndScopedManualChanges() {
    QTemporaryDir dir;
    const auto raw = dir.filePath("cluster.tif"), patches = dir.filePath("patches.gpkg");
    const auto automatic = dir.filePath("well.gpkg"), first = dir.filePath("local.gpkg");
    const auto second = dir.filePath("whole.gpkg"), direct = dir.filePath("direct.gpkg");
    QVERIFY(raster(raw));
    QString error;
    QVERIFY2(polygonize(raw,patches,{},&error),qPrintable(error));
    const auto fingerprint = digest(patches);
    QVariantList wells{QVariantMap{{"id","A"},{"voted",true},{"cluster_id",1},{"cell_x",105},{"cell_y",145}},
                       QVariantMap{{"id","B"},{"voted",false},{"cluster_id",-9999}}};
    QVERIFY2(locateWells(patches,wells,&error),qPrintable(error));
    QCOMPARE(wells.first().toMap()["patch_id"].toLongLong(), 0LL);
    const QVariantList labels{QVariantMap{{"cluster_id",1},{"status","single_well"},{"facies_code",1},{"vote_count",1}},
                              QVariantMap{{"cluster_id",2},{"status","no_wells"},{"vote_count",0}}};
    QVERIFY2(writeWellAssignment(patches,automatic,schema,labels,wells,&error),qPrintable(error));
    const auto suggested = byPatch(automatic);
    QCOMPARE(suggested[4]["facies_code"].toInt(), 1);
    QCOMPARE(suggested[4]["cluster_wells"].toInt(), 1);
    QCOMPARE(suggested[4]["patch_wells"].toInt(), 0);
    QCOMPARE(suggested[0]["patch_wells"].toInt(), 1);
    QVERIFY(suggested[8]["facies_code"].isNull());
    int changed = 0;
    QVERIFY2(writeManualAssignment(automatic,first,schema,4,false,2,&changed,&error),qPrintable(error));
    QCOMPARE(changed, 1);
    const auto manual = byPatch(first);
    QCOMPARE(manual[4]["facies_code"].toInt(), 2);
    QCOMPARE(manual[4]["well_suggestion"].toInt(), 1);
    QCOMPARE(manual[4]["assignment_status"].toString(), QStringLiteral("manual"));
    QCOMPARE(manual[0]["facies_code"].toInt(), 1);
    QVERIFY2(writeManualAssignment(first,second,schema,8,true,2,&changed,&error),qPrintable(error));
    QCOMPARE(changed, 2);
    const auto both = byPatch(second);
    QCOMPARE(both[8]["facies_code"].toInt(), 2);
    QCOMPARE(both[14]["facies_code"].toInt(), 2);
    QVERIFY2(writeManualAssignment(patches,direct,schema,4,false,2,&changed,&error),qPrintable(error));
    QVERIFY(byPatch(direct)[0]["facies_code"].isNull());
    QCOMPARE(digest(patches), fingerprint);
    QCOMPARE(byPatch(automatic), suggested);
  }
  void batchEditsResolveOverlapsInApplicationOrder() {
    QTemporaryDir dir;
    const auto input = dir.filePath("clusters.tif"), patches = dir.filePath("patches.gpkg");
    const auto output = dir.filePath("interpreted.gpkg");
    QString error;
    QVERIFY(raster(input));
    QVERIFY2(polygonize(input, patches, {}, &error), qPrintable(error));
    const auto original = digest(patches);
    const auto rows = byPatch(patches);
    const auto edit = [&](int patch, bool group, int code) {
      return QVariantMap{{"feature_id", rows.value(patch).value("feature_id")}, {"patch_id", patch},
                         {"whole_cluster", group}, {"facies_code", code}};
    };
    int changed = 0;
    QVERIFY2(writeManualAssignments(patches, output, schema, {edit(0, true, 1), edit(4, false, 2)},
                                    &changed, &error), qPrintable(error));
    QCOMPARE(byPatch(output)[0]["facies_code"].toInt(), 1);
    QCOMPARE(byPatch(output)[4]["facies_code"].toInt(), 2);
    const auto failPath = dir.filePath("rejected.gpkg");
    QVERIFY(!writeManualAssignments(patches, failPath, schema, {edit(0, true, 1), edit(4, false, 999)},
                                    &changed, &error));
    QVERIFY(!QFile::exists(failPath));
    QCOMPARE(digest(patches), original);
  }
  void rejectsMismatchedInputs() {
    QTemporaryDir dir;
    const auto raw = dir.filePath("cluster.tif"), patches = dir.filePath("patches.gpkg"), output = dir.filePath("invalid.gpkg");
    QString error;
    QVERIFY(raster(raw));
    QVERIFY2(polygonize(raw,patches,{},&error),qPrintable(error));
    QVariantList wells{QVariantMap{{"voted",true},{"cluster_id",2},{"cell_x",105},{"cell_y",145}}};
    QVERIFY(!locateWells(patches,wells,&error));
    int changes = 0;
    QVERIFY(!writeManualAssignment(patches,output,schema,999,false,1,&changes,&error));
    QVERIFY(!writeManualAssignment(patches,output,schema,0,true,999,&changes,&error));
    QVERIFY(!writeWellAssignment(patches,output,schema,{},{},&error));
    QVERIFY(!QFileInfo::exists(output));
    QVERIFY(!polygonize(raw,output,{},&error,[]{ return true; }));
    QVERIFY(!QFileInfo::exists(output));
  }
  void pagesByFeatureIdAndCountsWholeClusterBeyondHighlightLimit() {
    QTemporaryDir dir;
    const auto path = dir.filePath("many.gpkg");
    GDALAllRegister();
    auto *driver = GetGDALDriverManager()->GetDriverByName("GPKG");
    QVERIFY(driver);
    auto *ds = driver->Create(path.toUtf8().constData(), 0, 0, 0, GDT_Unknown, nullptr);
    QVERIFY(ds);
    auto *layer = ds->CreateLayer("cluster_patches", nullptr, wkbPolygon);
    QVERIFY(layer);
    for (const auto *name : {"cluster_id", "patch_id", "pixel_count"}) {
      OGRFieldDefn def(name, OFTInteger64);
      QCOMPARE(layer->CreateField(&def), OGRERR_NONE);
    }
    QCOMPARE(layer->StartTransaction(), OGRERR_NONE);
    for (int i = 0; i < 320; ++i) {
      OGRFeature f(layer->GetLayerDefn());
      f.SetFID(1000 + 2 * i);
      f.SetField("cluster_id", 1);
      f.SetField("patch_id", i);
      f.SetField("pixel_count", 1);
      QCOMPARE(layer->CreateFeature(&f), OGRERR_NONE);
    }
    QCOMPARE(layer->CommitTransaction(), OGRERR_NONE);
    GDALClose(ds);
    QString error;
    const auto page = choices(path, &error, 1201, 3);
    QVERIFY2(error.isEmpty(), qPrintable(error));
    QCOMPARE(page.size(), 3);
    QCOMPARE(page.first().toMap().value("feature_id").toLongLong(), 1202LL);
    QCOMPARE(page.last().toMap().value("patch_id").toLongLong(), 103LL);
    QVERIFY(choices(path, &error, 1700).isEmpty());
    QCOMPARE(choices(path, &error).size(), 129);
    QList<qint64> highlighted;
    QCOMPARE(clusterSelection(path, 1, highlighted, &error), 320LL);
    QCOMPARE(highlighted.size(), 256);
  }
};
QTEST_GUILESS_MAIN(SeismicClusterPatchesTest)
#include "tst_seismicclusterpatches.moc"
