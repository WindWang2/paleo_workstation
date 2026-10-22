// 层：测试壳
#include <QtTest>
#include <QTemporaryDir>
#include <QtEndian>
#include <gdal.h>
#include "io/projectcoordinates.h"
#include "io/projectsetupscan.h"
#include "io/wellfactorfile.h"
#include "io/welltable.h"
#include "io/horizonbinner.h"
#include "metadata/nativereference.h"

class TestProjectCoordinates : public QObject {
  Q_OBJECT
  static void write(const QString &path, const QByteArray &bytes) {
    QFile f(path); QVERIFY(f.open(QIODevice::WriteOnly)); QCOMPARE(f.write(bytes), bytes.size());
  }
private slots:
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
