// 层：测试壳
#include <QtTest>
#include <QTemporaryDir>
#include <QDirIterator>
#include <QtEndian>
#include <gdal.h>
#include "io/projectcoordinates.h"
#include "io/projectsetupscan.h"
#include "io/wellfactorfile.h"
#include "io/welltable.h"
#include "io/horizonbinner.h"
#include "metadata/nativereference.h"
#include "metadata/paleoprojectstore.h"
#include "io/dataimportservice.h"
#include "services/projectdata.h"

class TestProjectCoordinates : public QObject {
  Q_OBJECT
  static void write(const QString &path, const QByteArray &bytes) {
    QFile f(path); QVERIFY(f.open(QIODevice::WriteOnly)); QCOMPARE(f.write(bytes), bytes.size());
  }
private slots:
  void hxnCoordinateSourceRequiresReviewWhenProvided() {
    const auto root = qEnvironmentVariable("PALEO_HXN_DATA_DIR");
    if (root.isEmpty()) QSKIP("Set PALEO_HXN_DATA_DIR for private coordinate acceptance");
    const auto path = root + QStringLiteral("/坐标统计.xlsx");
    const auto table = paleo::io::readWellTable(path);
    QVERIFY2(table.error.isEmpty(), qPrintable(table.error));
    QCOMPARE(table.heads.size(), 55); QCOMPARE(table.report.rejectedRows, 1);
    QVERIFY(table.heads.first().x > 20'000'000);
    QVERIFY(table.heads.first().y > 2'000'000 && table.heads.first().y < 3'000'000);
    const auto checked = paleo::io::readWellTable(path, {{"crs", "EPSG:2436"}, {"projectCrs", "EPSG:2436"}});
    QVERIFY(checked.error.contains(QStringLiteral("EPSG:2436")));
    QVERIFY(checked.normalized.isEmpty());
  }
  void surveyAxisLabelsOverrideLetters() {
    QTemporaryDir root;
    const auto path = root.filePath("survey.csv");
    write(path, QStringLiteral("井号,X（纵坐标）,Y（横坐标）\nA1,2361000,303500\n").toUtf8());
    const auto table = paleo::io::readWellTable(path, {{"crs", "EPSG:2436"}, {"projectCrs", "EPSG:2436"}});
    QVERIFY2(table.error.isEmpty(), qPrintable(table.error));
    QCOMPARE(table.heads.size(), 1);
    QCOMPARE(table.heads.first().x, 303500.);
    QCOMPARE(table.heads.first().y, 2361000.);
    QCOMPARE(table.mapping.value("x").toString(), QStringLiteral("Y（横坐标）"));
    // A user's explicit mapping remains authoritative.
    const auto explicitTable = paleo::io::readWellTable(path, {{"x", "X（纵坐标）"}, {"y", "Y（横坐标）"}});
    QCOMPARE(explicitTable.heads.first().x, 2361000.);
  }
  void prefixedGaussKrugerCoordinatesRequireReview() {
    QVector<WellHeadRecord> heads{{"A1", 20'300'000, 2'360'000}};
    QString error;
    QVERIFY(!paleo::io::projectWellCoordinates(&heads, "EPSG:2436", "EPSG:2436", &error));
    QVERIFY(error.contains("A1")); QVERIFY(error.contains("EPSG:2436"));
    QCOMPARE(heads.first().x, 20'300'000.);
    heads.first().x = 300'000;
    QVERIFY2(paleo::io::projectWellCoordinates(&heads, "EPSG:2436", "EPSG:4326", &error), qPrintable(error));
    QVERIFY(heads.first().x > 114 && heads.first().x < 117);
    QVERIFY(heads.first().y > 20 && heads.first().y < 23);
  }
  // review-fix-dataio：CRS 字符串来自工程文件/导入表头，旧实现直接
  // SetFromUserInput 会把它当文件路径/URL 打开（任意读 + 网络请求）。
  // 带 SET_FROM_USER_INPUT_LIMITATIONS 后只接受 EPSG/WKT/PROJ 等内联定义。
  void crsUserInputRefusesFilePaths() {
    QTemporaryDir root; const auto wktFile = root.filePath("crs.wkt");
    const auto wkt = paleo::io::coordinateWkt("EPSG:32649");
    QVERIFY(!wkt.isEmpty());
    write(wktFile, wkt.toUtf8());
    // 内联定义照常可用（兼容既有工程）。
    QVERIFY(paleo::io::isMetricProjectCrs("EPSG:32649"));
    QVERIFY(paleo::io::isMetricProjectCrs(wkt));
    QVERIFY(validNativeCrs("EPSG:4326"));
    QVERIFY(sameNativeCrs("EPSG:32649", wkt));
    // 文件路径一律拒绝。
    QVERIFY(!paleo::io::isMetricProjectCrs(wktFile));
    QVERIFY(paleo::io::coordinateWkt(wktFile).isEmpty());
    QVERIFY(!validNativeCrs(wktFile));
    QVERIFY(!sameNativeCrs(wktFile, wkt));
  }
  void depthOnlyStratificationImportsIntoProjectedProject() {
    QTemporaryDir root; const auto heads = root.filePath("heads.csv");
    const auto tops = root.filePath("tops.csv"), td = root.filePath("td.dat");
    write(heads, "WELL,X,Y\nA1,233000,3990000\n");
    const QByteArray original = "WELL,HORIZON,TVD,BASE_TVD\nA1,D61,1000,1100\nA1,D62,1100,1200\n";
    write(tops, original);
    write(td, "#TimeDepth File From SMI\n# Well : A1\n0 0 0 0\n1000 1000 1000 1200\n2000 2000 2000 2500\n");
    const auto parsed = paleo::io::readWellTable(tops, {{"projectCrs", "EPSG:32649"}});
    QVERIFY2(parsed.error.isEmpty(), qPrintable(parsed.error));
    QVERIFY(parsed.heads.isEmpty()); QCOMPARE(parsed.tops.size(), 2); QVERIFY(parsed.normalized.contains("D61"));
    // Real XY records still require a declared source CRS.
    const auto unknownHeadCrs = paleo::io::readWellTable(heads, {{"projectCrs", "EPSG:32649"}});
    QVERIFY(!unknownHeadCrs.error.isEmpty());
    PaleoProjectStore store; DataImportService imports(&store); imports.setProjectDir(root.filePath("project"));
    PaleoGeoreference reference; reference.kind = "crs"; reference.sourceCrs = "EPSG:32649";
    reference.targetCrs = "EPSG:4326"; imports.setGeoreference(reference);
    DataImportService::ImportOptions options; options.tableMapping = {{"crs", "EPSG:32649"}}; QString error;
    QVERIFY2(!imports.importProjectFile(heads, options, &error).isEmpty(), qPrintable(error));
    const auto rows = imports.importFiles({tops, td}, &error); QVERIFY2(error.isEmpty(), qPrintable(error));
    QCOMPARE(rows.size(), 2);
    for (const auto &row : rows) QVERIFY2(row.outcome != FolderRowResult::Outcome::Failed, qPrintable(row.message));
    ProjectDataFacade data; data.setCatalog(imports.catalog(), imports.projectDir());
    const auto wells = data.wells(); QCOMPARE(wells.size(), 1);
    const auto layers = data.topsForMapping(wells.first().id); QCOMPARE(layers.size(), 2);
    QCOMPARE(layers.first().horizon, QString("D61")); QCOMPARE(layers.first().tvd, 1000.0);
    QVERIFY(std::abs(layers.first().md - 1200.0) < 1e-6);
    QFile unchanged(tops); QVERIFY(unchanged.open(QIODevice::ReadOnly)); QCOMPARE(unchanged.readAll(), original);
  }

  void realStratificationImportsOnlyIntoDiagnosticCopy() {
    const auto project = qEnvironmentVariable("PALEO_STRATIFICATION_PROJECT_COPY");
    const auto source = qEnvironmentVariable("PALEO_STRATIFICATION_SOURCE");
    if (project.isEmpty() || source.isEmpty()) QSKIP("Set explicit diagnostic project copy and source table");
    QVERIFY(QFileInfo::exists(project + "/.diagnostic-copy"));
    QFile original(source); QVERIFY(original.open(QIODevice::ReadOnly)); const auto before = original.readAll(); original.close();
    PaleoProjectStore store; DataImportService imports(&store); imports.setProjectDir(project);
    PaleoGeoreference reference; reference.kind = "crs"; reference.sourceCrs = "EPSG:32649";
    reference.targetCrs = "EPSG:4326"; imports.setGeoreference(reference); QString error;
    const auto rows = imports.importFiles({source}, &error); QVERIFY2(error.isEmpty(), qPrintable(error));
    QCOMPARE(rows.size(), 1); QVERIFY2(!rows.first().assetId.isEmpty(), qPrintable(rows.first().message));
    ProjectDataFacade data; data.setCatalog(imports.catalog(), project); const auto wells = data.wells(); QVERIFY(!wells.isEmpty());
    int ready = 0;
    for (const auto &well : wells) {
      const auto tops = data.topsForMapping(well.id);
      for (const auto &top : tops) if (top.horizon == "D61" && std::isfinite(top.md)) { ++ready; break; }
    }
    qInfo() << "DIAGNOSTIC wells=" << wells.size() << "readable_D61_MD=" << ready;
    QCOMPARE(ready, wells.size());
    QVERIFY(original.open(QIODevice::ReadOnly)); QCOMPARE(original.readAll(), before);
  }

  void explicitProjectBaseline() {
    QTemporaryDir root;
    const auto file = root.filePath("wells.csv");
    write(file, "well,x,y\nA1,108.037366,36.086627\n");
    ProjectSetupSources sources; sources.points = file; sources.projectCrs = "EPSG:32649";
    sources.pointMapping = {{"crs", "EPSG:4326"}};
    const auto preview = scanProjectSources(sources);
    QVERIFY2(preview.errors.isEmpty(), qPrintable(preview.errors.join('\n')));
    QCOMPARE(preview.sourceCrs, QString("EPSG:4326")); QCOMPARE(preview.projectCrs, sources.projectCrs);
    sources.projectCrs = "EPSG:4326";
    QVERIFY(!scanProjectSources(sources).errors.isEmpty());
    QVERIFY(!paleo::io::isMetricProjectCrs("EPSG:2263")); // US survey feet cannot silently become metres.
  }
  void factorNullZeroAndConflicts() {
    QTemporaryDir root; const auto file = root.filePath("A1_factors.csv"); QString error;
    const QByteArray header = "well_name,horizon,sand_thick,sand_ratio,log_layer_thickness_md,source\n";
    write(file, header + "A1,D61,0,0,20,\"quoted, source\"\nA1,D62,,,20,missing\n");
    const auto rows = paleo::io::readWellFactorFile(file, &error);
    QVERIFY2(error.isEmpty(), qPrintable(error)); QCOMPARE(rows.size(), 2);
    QCOMPARE(rows[0].toMap().value("sand_thick").toDouble(), 0.);
    QVERIFY(!rows[1].toMap().contains("sand_thick"));
    QCOMPARE(rows[0].toMap().value("source").toString(), QString("quoted, source"));
    write(file, header + "A1,D61,30,1.5,20,x\n");
    QVERIFY(paleo::io::readWellFactorFile(file, &error).isEmpty()); QVERIFY(!error.isEmpty());
    write(file, header + "A1,D61,10,0.5,20,x\nA1,D61,10,0.5,20,x\n");
    QVERIFY(paleo::io::readWellFactorFile(file, &error).isEmpty());
  }
  void seismicHeadersTransformWithoutChangingSamples() {
    QTemporaryDir root; QString error;
    QByteArray file(3600, ' '); file.replace(3200, 400, QByteArray(400, '\0'));
    file.replace(30 * 80, 80, QByteArray("C31 CRS EPSG:32649").leftJustified(80, ' '));
    qToBigEndian<quint16>(2, file.data() + 3220); qToBigEndian<quint16>(5, file.data() + 3224);
    QByteArray trace(240, '\0'); qToBigEndian<quint16>(2, trace.data() + 114);
    qToBigEndian<qint16>(-100, trace.data() + 70);
    qToBigEndian<qint32>(23324046, trace.data() + 180); qToBigEndian<qint32>(399762112, trace.data() + 184);
    const QByteArray samples = QByteArray::fromHex("3f800000c0000000");
    const auto from = root.filePath("input.sgy"), to = root.filePath("output.sgy");
    write(from, file + trace + samples);
    QVERIFY2(paleo::io::projectSegyFile(from, to, "EPSG:32649", "EPSG:3857", &error), qPrintable(error));
    QFile converted(to); QVERIFY(converted.open(QIODevice::ReadOnly)); const auto bytes = converted.readAll();
    QCOMPARE(bytes.right(8), samples); QCOMPARE(bytes.size(), file.size() + trace.size() + samples.size());
    QCOMPARE(paleo::io::wellSourceCrsHint(QString::fromLatin1(bytes.left(3200))), QString("EPSG:3857"));
    const auto x = qFromBigEndian<qint32>(bytes.constData() + 3600 + 180) / 100.;
    const auto y = qFromBigEndian<qint32>(bytes.constData() + 3600 + 184) / 100.;
    double lon, lat; QVERIFY(nativeToGeographic("EPSG:3857", x, y, &lon, &lat));
    QVERIFY(std::abs(lon - 108.037366) < 1e-6); QVERIFY(std::abs(lat - 36.086627) < 1e-6);
    QFile original(from); QVERIFY(original.open(QIODevice::ReadOnly)); QCOMPARE(original.readAll(), file + trace + samples);
  }
  void nativeHorizonAcceptsRealRotatedCorners() {
    const QByteArray text = "# Projection: EPSG:32649\n# Grid_size: 2x2\n"
        "# P1: 1,1,233000,3990000\n# P2: 1,2,233020,3990002\n# P3: 2,2,233017,3990022\n"
        "233000 3990000 100 1 1\n233020 3990002 101 1 2\n"
        "232997 3990020 102 2 1\n233017 3990022 103 2 2\n";
    BinnedHorizon grid; QString error;
    QVERIFY2(binHorizon(text, &grid, &error), qPrintable(error));
    QVERIFY(std::abs(grid.dx - 20) < 1e-6);
    QVERIFY(std::abs(grid.rowX - 3) < 1e-6);
    QVERIFY(std::abs(grid.columnY - 2) < 1e-6);
    QCOMPARE(grid.z.size(), 4);
  }
  // #668：层位网格头声明 EPSG:32649，工程基准 EPSG:3857。批量导入后工程里
  // 不得留下未转换（仍是 32649）或未登记的 GeoTIFF；发布的栅格必须是 3857。
  void folderImportedHorizonRasterIsProjectedOrAbsent() {
    QTemporaryDir root; GDALAllRegister();
    const auto project = root.filePath("project"); QVERIFY(QDir().mkpath(project));
    const auto sourceDir = root.filePath(QString::fromUtf8("source/层位")); QVERIFY(QDir().mkpath(sourceDir));
    PaleoProjectStore store;
    store.setProjectPaths(project + "/project.qgz", project + "/project.gpkg", project + "/metadata/project.sqlite");
    DataImportService imports(&store); imports.setProjectDir(project);
    PaleoGeoreference reference; reference.kind = "crs"; reference.sourceCrs = "EPSG:3857";
    imports.setGeoreference(reference);
    const QByteArray grid = "# Projection: EPSG:32649\n# Grid_size: 2x2\n"
        "# P1: 1,1,233000,3990000\n# P2: 1,2,233020,3990000\n# P3: 2,2,233020,3990020\n"
        "233000 3990000 100 1 1\n233020 3990000 101 1 2\n"
        "233000 3990020 102 2 1\n233020 3990020 103 2 2\n";
    QStringList files; QMap<QString, QString> types;
    for (const char *name : {"D61.dat", "D62.dat"}) {
      // 两份字节不同（避免字节去重），都需要从 32649 转到 3857。
      const auto path = QDir(sourceDir).filePath(name);
      write(path, grid + "# " + QByteArray(name) + "\n"); files << path; types.insert(path, "horizon");
    }
    QString error;
    const auto rows = imports.importFiles(files, &error, types);
    QCOMPARE(rows.size(), 2);
    int imported = 0;
    for (const auto &row : rows) if (!row.assetId.isEmpty()) ++imported;
    QVERIFY2(imported == 2, qPrintable(error + " | " + rows.first().message + " | " + rows.last().message));
    QDirIterator it(project, {"*.tif", "*.tiff"}, QDir::Files, QDirIterator::Subdirectories);
    int rasters = 0;
    while (it.hasNext()) {
      const auto tif = it.next();
      if (tif.contains("/.staging") || tif.contains("staging/")) continue;
      QVERIFY2(!tif.contains(".partial"), qPrintable(tif));
      auto *ds = GDALOpen(tif.toUtf8().constData(), GA_ReadOnly); QVERIFY2(ds, qPrintable(tif));
      const bool projected = sameNativeCrs(QString::fromUtf8(GDALGetProjectionRef(ds)), "EPSG:3857");
      GDALClose(ds);
      QVERIFY2(projected, qPrintable(tif));
      ++rasters;
    }
    QVERIFY(rasters >= 2);
  }
  void projectedRasterHasNewCoordinates() {
    QTemporaryDir root; const auto path = root.filePath("source.tif"); QString error;
    GDALAllRegister();
    auto ds = GDALCreate(GDALGetDriverByName("GTiff"), path.toUtf8().constData(), 2, 2, 1, GDT_Float32, nullptr);
    QVERIFY(ds); double gt[]{233200,20,0,3997660,0,-20}; GDALSetGeoTransform(ds,gt);
    GDALSetProjection(ds, paleo::io::coordinateWkt("EPSG:32649").toUtf8().constData()); GDALClose(ds);
    QVERIFY2(paleo::io::projectRasterFile(path,"EPSG:3857",&error),qPrintable(error));
    ds = GDALOpen(path.toUtf8().constData(),GA_ReadOnly); QVERIFY(ds);
    QVERIFY(sameNativeCrs(QString::fromUtf8(GDALGetProjectionRef(ds)),"EPSG:3857"));
    GDALGetGeoTransform(ds,gt); QVERIFY(gt[0]>10000000); GDALClose(ds);
  }
};
QTEST_GUILESS_MAIN(TestProjectCoordinates)
#include "tst_projectcoordinates.moc"
