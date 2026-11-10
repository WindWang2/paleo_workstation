#include <QtTest>
#include <QTemporaryDir>
#include <QElapsedTimer>
#include <QCryptographicHash>
#include <QJsonDocument>
#include <QJsonObject>
#include <QDir>
#include <cmath>

#include "../src/io/horizonbinner.h"

#include <gdal.h>
#include <gdal_priv.h>

// plan §3/§6：层位点 → 装箱时间栅格。精确参数：
// 411×641；P1(1315,4165)=(0,0) P2(1315,4805)=(12793,0) P3(1725,4805)=(12793,16406)；
// X 随 crossline、Y 随 inline；dx=12793/640 dy=16406/410；北向上 geotransform
// 原点 (0,16406)、行方向 -dy；空道 nodata=-9999；同像元多点留最后一点并记碰撞数。
class TestHorizonBinner : public QObject
{
  Q_OBJECT

private slots:
  void parsesHeader();
  void binsSamplePoints();
  void countsCollisions();
  void rejectsMalformedHeaders();
  void rejectsExtremeLineNumbersWithoutOverflow(); // #595 L4.1
  void normalizesHorizonTimeUnits();
  void projectedGeoTiffKeepsSampleCenters();
  void writesGeoTiffWithGeotransform();
  void writesPaleoSurveyMetadata();
  void benchmarkLocalHorizons();

private:
  QString fixturePath() const
  {
    return QStringLiteral(PROJECT_FIXTURE_DIR) + QStringLiteral("/D61_sample.dat");
  }
  QByteArray readFile(const QString &p) const
  {
    QFile f(p);
    if (!f.open(QIODevice::ReadOnly))
      return QByteArray();
    return f.readAll();
  }
};

void TestHorizonBinner::benchmarkLocalHorizons()
{
  const auto root = qEnvironmentVariable("PALEO_HORIZON_BENCHMARK_INPUT");
  if (root.isEmpty()) QSKIP("Set PALEO_HORIZON_BENCHMARK_INPUT for local benchmarks");
  QJsonObject results;
  qint64 total = 0;
  for (const auto &name : QDir(root).entryList({"*.dat"}, QDir::Files, QDir::Name)) {
    const auto bytes = readFile(QDir(root).filePath(name));
    QVERIFY(!bytes.isEmpty());
    BinnedHorizon grid; QString error; QElapsedTimer clock; clock.start();
    QVERIFY2(binHorizon(bytes, &grid, &error), qPrintable(error));
    const auto elapsed = clock.elapsed(); total += elapsed;
    const auto digest = QCryptographicHash::hash(QByteArrayView(reinterpret_cast<const char *>(grid.z.constData()),
        grid.z.size() * sizeof(float)), QCryptographicHash::Sha256).toHex();
    QJsonObject record{{"sha256", QString::fromLatin1(digest)}, {"rows", grid.rows}, {"cols", grid.cols},
      {"filled", grid.filledCells}, {"collisions", grid.collisions}, {"rejected", grid.rejected},
      {"origin_x", grid.originX}, {"origin_y", grid.originY}, {"dx", grid.dx}, {"dy", grid.dy},
      {"row_x", grid.rowX}, {"column_y", grid.columnY}, {"residual", grid.geometryResidualM},
      {"z_min", grid.zMin}, {"z_max", grid.zMax}, {"elapsed_ms", elapsed}};
    results.insert(name, record);
    qInfo() << "horizon-benchmark" << name << elapsed << "ms" << grid.filledCells << digest;
  }
  QVERIFY(!results.isEmpty()); qInfo() << "horizon-benchmark total_ms" << total;
  const auto output = qEnvironmentVariable("PALEO_HORIZON_BENCHMARK_OUTPUT");
  if (!output.isEmpty()) {
    QFile file(output); QVERIFY(file.open(QIODevice::WriteOnly));
    QVERIFY(file.write(QJsonDocument(results).toJson()) > 0);
  }
}

void TestHorizonBinner::parsesHeader()
{
  const QByteArray text = readFile(fixturePath());
  HorizonHeader h;
  QString err;
  QVERIFY(parseHorizonHeader(text, &h, &err));
  QCOMPARE(h.gridRows, 411);
  QCOMPARE(h.gridCols, 641);
  QCOMPARE(h.p1Inline, 1315);
  QCOMPARE(h.p1Xline, 4165);
  QCOMPARE(h.p1x, 0.0);
  QCOMPARE(h.p1y, 0.0);
  QCOMPARE(h.p2Inline, 1315);
  QCOMPARE(h.p2Xline, 4805);
  QCOMPARE(h.p2x, 12793.0);
  QCOMPARE(h.p3Inline, 1725);
  QCOMPARE(h.p3Xline, 4805);
  QCOMPARE(h.p3x, 12793.0);
  QCOMPARE(h.p3y, 16406.0);
  QCOMPARE(h.zUnits, QStringLiteral("ms"));
}

void TestHorizonBinner::binsSamplePoints()
{
  BinnedHorizon b;
  QString err;
  QVERIFY(binHorizon(readFile(fixturePath()), &b, &err));
  QCOMPARE(b.rows, 411);
  QCOMPARE(b.cols, 641);
  QCOMPARE(b.rows * b.cols, b.z.size());
  // dx/dy
  QCOMPARE(b.dx, 12793.0 / 640.0);
  QCOMPARE(b.dy, 16406.0 / 410.0);
  // 北向上：行 0 是最大 y（inline 1725 侧），行方向 -dy。
  QCOMPARE(b.originX, 0.0);
  QCOMPARE(b.originY, 16406.0);
  // 第一条数据 (0,0,1254.548,1315,4165) 落在 inline=1315 → 行 410、crossline=4165 → 列 0
  QCOMPARE(b.z.at(410 * 641 + 0), 1254.548f);
  // 样本只覆盖前几千点 → 大量空道为 nodata
  QCOMPARE(b.z.at(0), -9999.0f);
  QVERIFY(b.collisions >= 0);
  auto normalized = readFile(fixturePath());
  normalized.replace("\r\n", "\n"); normalized.replace('\r', '\n');
  while (normalized.endsWith('\n')) normalized.chop(1);
  const auto scatter = parseHorizonScatter(normalized);
  for (const QByteArray ending : {QByteArray("\n"), QByteArray("\r\n"), QByteArray("\r")}) {
    auto variant = normalized; variant.replace("\n", ending);
    BinnedHorizon other;
    QVERIFY2(binHorizon(variant, &other, &err), qPrintable(err));
    QCOMPARE(other.z, b.z); QCOMPARE(other.filledCells, b.filledCells);
    QCOMPARE(other.collisions, b.collisions); QCOMPARE(other.rejected, b.rejected);
    const auto points = parseHorizonScatter(variant);
    QCOMPARE(points.points.size(), scatter.points.size()); QCOMPARE(points.skipped, scatter.skipped);
  }
}

void TestHorizonBinner::countsCollisions()
{
  // 同像元两点：保留最后一点，碰撞数记 1。
  const QByteArray synth = QByteArray(
      "#XYZInlineCrossline Format Horizon File From SMI\n"
      "# Grid_size:2x2# Survey(Inline,Crossline,x,y)\n"
      "# P1:      100,      200,     0.00000,     0.00000\n"
      "# P2:      100,      201,    10.00000,     0.00000\n"
      "# P3:      101,      201,    10.00000,    10.00000\n"
      "# Z_units: ms\n"
      "0.0 0.0 111.0 100 200\n"
      "0.0 0.0 222.0 100 200\n"
      "0.0 0.0 333.0 101 201\n");
  BinnedHorizon b;
  QString err;
  QVERIFY(binHorizon(synth, &b, &err));
  QCOMPARE(b.rows, 2);
  QCOMPARE(b.cols, 2);
  QCOMPARE(b.collisions, 1);
  // 北向上：inline=100（p1）在末行；同像元留最后一点。
  QCOMPARE(b.z.at(1 * 2 + 0), 222.0f);
  QCOMPARE(b.z.at(0 * 2 + 1), 333.0f);
  QCOMPARE(b.z.at(0 * 2 + 0), -9999.0f);
  QCOMPARE(b.z.at(1 * 2 + 1), -9999.0f);
}

void TestHorizonBinner::normalizesHorizonTimeUnits()
{
  const QByteArray header(
      "# Grid_size:2x2\n"
      "# P1: 1,1,0,0\n"
      "# P2: 1,2,20,0\n"
      "# P3: 2,2,20,40\n");
  const QByteArray rows("0 0 1.25 1 1\n20 0 -9999 1 2\n0 40 1.75 2 1\n");
  BinnedHorizon grid;
  QString error;
  QVERIFY2(binHorizon(header + "# Z_units: s\n" + rows, &grid, &error), qPrintable(error));
  QCOMPARE(grid.z, QVector<float>({1750.f, -9999.f, 1250.f, -9999.f}));
  QCOMPARE(grid.zMin, 1250.0);
  QCOMPARE(grid.zMax, 1750.0);
  QCOMPARE(grid.filledCells, 2);

  QVERIFY2(binHorizon(header + rows, &grid, &error), qPrintable(error));
  QCOMPARE(grid.z.at(2), 1.25f);
  QVERIFY2(binHorizon(header + "# Z_units: MS\n" + rows, &grid, &error), qPrintable(error));
  QCOMPARE(grid.z.at(2), 1.25f);
  QVERIFY(!binHorizon(header + "# Z_units: m\n" + rows, &grid, &error));
  QVERIFY2(error.contains("unsupported horizon time unit"), qPrintable(error));
  QVERIFY(!binHorizon(header + "# Z_units: s\n0 0 -9.999 1 1\n", &grid, &error));
  QVERIFY2(error.contains("cannot be stored"), qPrintable(error));
}

void TestHorizonBinner::projectedGeoTiffKeepsSampleCenters()
{
  const QByteArray text(
      "# Grid_size:2x2\n"
      "# P1: 1,1,0,0\n"
      "# P2: 1,2,20,0\n"
      "# P3: 2,2,20,40\n"
      "# Projection: UTM Zone 49N (WGS84)\n"
      "# Z_units: s\n"
      "230000 4000000 1.00 1 1\n"
      "230020 4000001 1.01 1 2\n"
      "230002 4000040 1.02 2 1\n"
      "230022 4000041 1.03 2 2\n");
  BinnedHorizon grid;
  QString error;
  QVERIFY2(binHorizon(text, &grid, &error), qPrintable(error));
  QTemporaryDir temp;
  QVERIFY(temp.isValid());
  const QString path = temp.filePath(QStringLiteral("projected.tif"));
  QVERIFY2(writeHorizonGeoTiff(grid, path, &error), qPrintable(error));
  GDALDatasetH dataset = GDALOpen(path.toUtf8().constData(), GA_ReadOnly);
  QVERIFY(dataset);
  const QByteArray driver = GDALGetDriverShortName(GDALGetDatasetDriver(dataset));
  const QByteArray compression = GDALGetMetadataItem(dataset, "COMPRESSION", "IMAGE_STRUCTURE");
  GDALRasterBandH band = GDALGetRasterBand(dataset, 1);
  int blockWidth = 0, blockHeight = 0;
  GDALGetBlockSize(band, &blockWidth, &blockHeight);
  double gt[6]{};
  const auto transformStatus = GDALGetGeoTransform(dataset, gt);
  float timeMs = 0;
  const auto readStatus = GDALRasterIO(band, GF_Read,
                                      0, 0, 1, 1, &timeMs, 1, 1, GDT_Float32, 0, 0);
  GDALClose(dataset);
  QCOMPARE(driver, QByteArray("GTiff"));
  QCOMPARE(compression, QByteArray("DEFLATE"));
  QVERIFY(blockWidth > grid.cols && blockHeight > grid.rows);
  QCOMPARE(transformStatus, CE_None);
  QCOMPARE(readStatus, CE_None);
  QCOMPARE(timeMs, 1020.0f);
  QVERIFY(std::abs(gt[0] + 0.5 * (gt[1] + gt[2]) - 230002.0) < 1e-6);
  QVERIFY(std::abs(gt[3] + 0.5 * (gt[4] + gt[5]) - 4000040.0) < 1e-6);
  QVERIFY(grid.geometryResidualM < 1e-6);
}

// T21：P1/P2/P3 是网格几何唯一来源——缺角/非数值/与 Grid_size 不自洽/
// 退化（零像元尺寸）都必须拒绝装箱，而不是拿 0 坐标照样产栅格。
void TestHorizonBinner::rejectsMalformedHeaders()
{
  const QByteArray head = QByteArray(
      "# Grid_size:2x2# Survey(Inline,Crossline,x,y)\n"
      "# P1:      100,      200,     0.00000,     0.00000\n"
      "# P2:      100,      201,    10.00000,     0.00000\n"
      "# P3:      101,      201,    10.00000,    10.00000\n"
      "# Z_units: ms\n");
  QString err;
  BinnedHorizon b;
  QVERIFY(binHorizon(head + "0.0 0.0 111.0 100 200\n", &b, &err));

  // 缺 P3 → 拒绝。
  QVERIFY(!binHorizon(
      "# Grid_size:2x2\n"
      "# P1:      100,      200,     0.00000,     0.00000\n"
      "# P2:      100,      201,    10.00000,     0.00000\n"
      "0.0 0.0 111.0 100 200\n",
      &b, &err));
  QVERIFY2(err.contains(QStringLiteral("lacks P1/P2/P3")), qPrintable(err));

  // P 行字段非数值 → 拒绝。
  err.clear();
  QVERIFY(!binHorizon(
      "# Grid_size:2x2\n"
      "# P1:      100,      200,     0.00000,     0.00000\n"
      "# P2:      100,      201,    abcdefgh,     0.00000\n"
      "# P3:      101,      201,    10.00000,    10.00000\n"
      "0.0 0.0 111.0 100 200\n",
      &b, &err));
  QVERIFY2(err.contains(QStringLiteral("invalid P2 geometry")), qPrintable(err));

  // 号域与 Grid_size 不自洽（3x3 但角点只跨 1）→ 拒绝。
  err.clear();
  QVERIFY(!binHorizon(
      "# Grid_size:3x3\n"
      "# P1:      100,      200,     0.00000,     0.00000\n"
      "# P2:      100,      201,    10.00000,     0.00000\n"
      "# P3:      101,      201,    10.00000,    10.00000\n"
      "0.0 0.0 111.0 100 200\n",
      &b, &err));
  QVERIFY2(err.contains(QStringLiteral("Grid_size")), qPrintable(err));

  // 退化几何：P2.x 不大于 P1.x → dx=0 → 拒绝。
  err.clear();
  QVERIFY(!binHorizon(
      "# Grid_size:2x2\n"
      "# P1:      100,      200,     0.00000,     0.00000\n"
      "# P2:      100,      201,     0.00000,     0.00000\n"
      "# P3:      101,      201,    10.00000,    10.00000\n"
      "0.0 0.0 111.0 100 200\n",
      &b, &err));
  QVERIFY2(err.contains(QStringLiteral("do not match Grid_size")), qPrintable(err));

  // 超限 Grid_size (> 100M 像元) → 拒绝。
  err.clear();
  QVERIFY(!binHorizon(
      "# Grid_size:20000x20000\n"
      "# P1:      100,      200,     0.00000,     0.00000\n"
      "# P2:      100,    20199,    10.00000,     0.00000\n"
      "# P3:    20099,    20199,    10.00000,    10.00000\n"
      "0.0 0.0 111.0 100 200\n",
      &b, &err));
  QVERIFY2(err.contains(QStringLiteral("exceeds supported limits")), qPrintable(err));

  // 旋转/非正交网格：P2.y != P1.y → 拒绝。
  err.clear();
  QVERIFY(!binHorizon(
      "# Grid_size:2x2\n"
      "# P1:      100,      200,     0.00000,     0.00000\n"
      "# P2:      100,      201,    10.00000,     5.00000\n"
      "# P3:      101,      201,    10.00000,    10.00000\n"
      "0.0 0.0 111.0 100 200\n",
      &b, &err));
  QVERIFY2(err.contains(QStringLiteral("rotated or non-orthogonal")), qPrintable(err));
}

// #595（L4.1）：分箱索引用 int 减法算 row/col，而 inline/crossline 取自数据行
// 的任意 32 位取值——inl - p1Inline 溢出即有符号溢出 UB（UBSAN abort），且回绕
// 后的值可能骗过界检、越界写 b.z。改 qint64 后这些点如实计入 rejected。
void TestHorizonBinner::rejectsExtremeLineNumbersWithoutOverflow()
{
  const QByteArray head = QByteArray(
      "# Grid_size:2x2\n"
      "# P1:        1,        1,     0.00000,     0.00000\n"
      "# P2:        1,        2,    10.00000,     0.00000\n"
      "# P3:        2,        2,    10.00000,    10.00000\n"
      "# Z_units: ms\n");
  // inline = INT_MIN：INT_MIN - p1Inline(1) 在 int 上溢出（UB）。
  const QByteArray body = QByteArrayLiteral("0.0 0.0 111.0 -2147483648 -2147483648\n");

  BinnedHorizon b;
  QString err;
  QVERIFY2(binHorizon(head + body, &b, &err), qPrintable(err));
  QCOMPARE(b.z.size(), 4);
  QCOMPARE(b.rejected, 1); // 越界点只计数，不写入
  QCOMPARE(b.filledCells, 0);
  for (const float v : b.z)
    QCOMPARE(v, -9999.0f); // 无像元被写坏

  // 另一侧：inline = INT_MAX、crossline = INT_MAX 同理（row 回绕成大正数）。
  const QByteArray body2 = QByteArrayLiteral("0.0 0.0 111.0 2147483647 2147483647\n");
  BinnedHorizon b2;
  QVERIFY2(binHorizon(head + body2, &b2, &err), qPrintable(err));
  QCOMPARE(b2.rejected, 1);
  QCOMPARE(b2.filledCells, 0);

  // 合法点不受影响（p1Inline=1 → row = 1 - (2 - 1) = 0，col = 2 - 1 = 1）。
  const QByteArray body3 = QByteArrayLiteral("0.0 0.0 111.0 2 2\n");
  BinnedHorizon b3;
  QVERIFY2(binHorizon(head + body3, &b3, &err), qPrintable(err));
  QCOMPARE(b3.rejected, 0);
  QCOMPARE(b3.filledCells, 1);
  QCOMPARE(b3.z.at(0 * 2 + 1), 111.0f);
}

void TestHorizonBinner::writesGeoTiffWithGeotransform()
{
  BinnedHorizon b;
  QString err;
  QVERIFY(binHorizon(readFile(fixturePath()), &b, &err));

  QTemporaryDir dir;
  const QString tif = dir.path() + QStringLiteral("/d61.tif");
  QVERIFY(writeHorizonGeoTiff(b, tif, &err));

  GDALAllRegister();
  GDALDatasetH ds = GDALOpen(tif.toUtf8().constData(), GA_ReadOnly);
  QVERIFY2(ds, "cannot reopen written tif");
  double gt[6] = {0, 0, 0, 0, 0, 0};
  GDALGetGeoTransform(ds, gt);
  QCOMPARE(gt[0], 0.0);
  QCOMPARE(gt[1], 12793.0 / 640.0);
  QCOMPARE(gt[2], 0.0);
  QCOMPARE(gt[3], 16406.0);
  QCOMPARE(gt[4], 0.0);
  QCOMPARE(gt[5], -16406.0 / 410.0);
  QCOMPARE(GDALGetRasterCount(ds), 1);
  QCOMPARE(GDALGetRasterXSize(ds), 641);
  QCOMPARE(GDALGetRasterYSize(ds), 411);
  int hasNoData = 0;
  const double nd = GDALGetRasterNoDataValue(GDALGetRasterBand(ds, 1), &hasNoData);
  QVERIFY(hasNoData);
  QCOMPARE(nd, -9999.0);
  // CRS：局部测网自定义工程坐标——authid 不落 EPSG:4326
  const char *proj = GDALGetProjectionRef(ds);
  QVERIFY2(proj && *proj, "tif must carry the local-grid CRS");
  QVERIFY2(!QByteArray(proj).contains("4326"), "CRS must not be EPSG:4326");
  GDALClose(ds);
}

// T21/audit #38：产线写出的 GeoTIFF 必须自带 PALEO_INLINE_*/XLINE_*
// （验证残差行→地震剖面导航的测线号域就靠它；此前只在测试里手工注入）。
// DT/T0 来自 SEG-Y、散点文本没有——给了才写，不给绝不编值。
void TestHorizonBinner::writesPaleoSurveyMetadata()
{
  BinnedHorizon b;
  QString err;
  QVERIFY(binHorizon(readFile(fixturePath()), &b, &err));
  QCOMPARE(b.inlineMin, 1315);
  QCOMPARE(b.inlineMax, 1725);
  QCOMPARE(b.xlineMin, 4165);
  QCOMPARE(b.xlineMax, 4805);

  QTemporaryDir dir;
  const QString tif = dir.path() + QStringLiteral("/d61.tif");
  QVERIFY(writeHorizonGeoTiff(b, tif, &err));

  GDALAllRegister();
  GDALDatasetH ds = GDALOpen(tif.toUtf8().constData(), GA_ReadOnly);
  QVERIFY2(ds, "cannot reopen written tif");
  QCOMPARE(QByteArray(GDALGetMetadataItem(ds, "PALEO_INLINE_MIN", nullptr)),
           QByteArray("1315"));
  QCOMPARE(QByteArray(GDALGetMetadataItem(ds, "PALEO_INLINE_MAX", nullptr)),
           QByteArray("1725"));
  QCOMPARE(QByteArray(GDALGetMetadataItem(ds, "PALEO_XLINE_MIN", nullptr)),
           QByteArray("4165"));
  QCOMPARE(QByteArray(GDALGetMetadataItem(ds, "PALEO_XLINE_MAX", nullptr)),
           QByteArray("4805"));
  // 散点文本不含采样参数 → 默认不写（nullptr），不编 0。
  QVERIFY(GDALGetMetadataItem(ds, "PALEO_DT_MS", nullptr) == nullptr);
  QVERIFY(GDALGetMetadataItem(ds, "PALEO_T0_MS", nullptr) == nullptr);
  // 装箱计数随栅格落盘（T21「bin counts」）。
  QCOMPARE(QByteArray(GDALGetMetadataItem(ds, "PALEO_FILLED_CELLS", nullptr))
               .toInt(),
           b.filledCells);
  QCOMPARE(QByteArray(GDALGetMetadataItem(ds, "PALEO_COLLISIONS", nullptr))
               .toInt(),
           b.collisions);
  QCOMPARE(QByteArray(GDALGetMetadataItem(ds, "PALEO_REJECTED", nullptr))
               .toInt(),
           b.rejected);
  GDALClose(ds);

  // 调用方真的拿到 SEG-Y 采样参数时才落这两个键。
  BinnedHorizon withSeis = b;
  withSeis.dtMs = 2.0;
  withSeis.t0Ms = 0.0;
  const QString tif2 = dir.path() + QStringLiteral("/d61_dt.tif");
  QVERIFY(writeHorizonGeoTiff(withSeis, tif2, &err));
  ds = GDALOpen(tif2.toUtf8().constData(), GA_ReadOnly);
  QVERIFY(ds);
  QCOMPARE(QByteArray(GDALGetMetadataItem(ds, "PALEO_DT_MS", nullptr)),
           QByteArray("2"));
  QCOMPARE(QByteArray(GDALGetMetadataItem(ds, "PALEO_T0_MS", nullptr)),
           QByteArray("0"));
  GDALClose(ds);
}

QTEST_MAIN(TestHorizonBinner)
#include "tst_horizonbinner.moc"
