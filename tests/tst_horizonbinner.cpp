#include <QtTest>
#include <QTemporaryDir>

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
  void writesGeoTiffWithGeotransform();

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

QTEST_MAIN(TestHorizonBinner)
#include "tst_horizonbinner.moc"
