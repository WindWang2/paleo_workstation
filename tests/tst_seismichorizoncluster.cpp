#include <QtTest>
#include <QTemporaryDir>
#include <QSaveFile>
#include <QtEndian>
#include <cmath>
#include <cstring>

#include "../src/services/seismichorizoncluster.h"
#include "../src/services/seismicclusterlabels.h"

#include <gdal.h>
#include <cpl_conv.h>
#include <ogr_spatialref.h>

// 取窗契约锚（f0e7b10d「直接按原始层位提取地震窗并聚类」的文档化契约，
// docs/progress/seismichorizoncluster.md）：Scatter 路径按层位 TWT ±12ms
// 开窗（seismichorizoncluster.cpp:221-225）提均值/RMS/过零率三特征。
// 方言体（crossline@192 恒 0、xline 取 CDP@21、角点取 181-188——同
// tst_segy dialectLayout 回归的布局）× SMI 散点层位。
#include "seismichorizonfixture.h"
using namespace SeismicHorizonFixture;

// 非平行四边形的真实测网：P1/P2/P3 不能推断第四角。地震 XY 保留整数
// 量化，SMI 保留小数；实际同号道坐标吻合，旧三角点仿射检查会误拒绝。
static QPair<QByteArray, QByteArray> warpedSurvey(int scalar = 0, double shift = 0, double scale = 1)
{
  auto volume = buildVolume();
  QByteArray horizon = QStringLiteral("# Projection: EPSG:32649\n# Grid_size:4x4\n"
      "# P1: 100,2000,0,0\n# P2: 100,2003,%1,0\n# P3: 103,2003,%1,%2\n# Z_units: ms\n")
      .arg(30 * scale).arg(63.6 * scale).toUtf8();
  const double resolution = scalar == 0 ? 1.0 : scalar > 0 ? double(scalar) : 1.0 / -double(scalar);
  for (int row = 0; row < 4; ++row)
    for (int col = 0; col < 4; ++col) {
      const double x = (10.0 * col + 1.5 * row * (3 - col)) * scale, y = (20.0 * row + 0.4 * col * row) * scale;
      auto *header = reinterpret_cast<uchar *>(volume.data() + 3600 + (row * 4 + col) * 440);
      qToBigEndian<qint16>(qint16(scalar), header + 70);
      qToBigEndian<qint32>(qint32(std::lround(x / resolution)), header + 180);
      qToBigEndian<qint32>(qint32(std::lround(y / resolution)), header + 184);
      horizon += QStringLiteral("%1 %2 100 %3 %4\n").arg(x + shift, 0, 'f', 5)
          .arg(y, 0, 'f', 5).arg(100 + row).arg(2000 + col).toUtf8();
    }
  return {volume, horizon};
}

class TestSeismicHorizonCluster : public QObject
{
  Q_OBJECT
private slots:
  void actualTraceCoordinatesAcceptWarpedSurvey_data() {
    QTest::addColumn<int>("scalar");
    QTest::newRow("integer") << 0;
    QTest::newRow("decimetre") << -10;
    QTest::newRow("ten-metres") << 10;
  }
  void actualTraceCoordinatesAcceptWarpedSurvey() {
    QFETCH(int, scalar);
    QTemporaryDir dir;
    const auto data = warpedSurvey(scalar);
    const auto sgy = dir.filePath("v.sgy"), smi = dir.filePath("h.dat");
    QVERIFY(writeFile(sgy, data.first)); QVERIFY(writeFile(smi, data.second));
    const auto result = clusterSeismicHorizon(sgy, smi, dir.path(), {1, 2}, {}, {}, SeismicHorizonSource::Scatter);
    QVERIFY2(result.error.isEmpty(), qPrintable(result.error));
    QCOMPARE(result.rows, 4); QCOMPARE(result.columns, 4);
    QVERIFY(result.validCells > 8);
    QCOMPARE(result.validCells, result.cells.size() - result.cells.count(-9999));
    QVERIFY(result.extent.contains(QPointF(13.5, 60))); // 第四角实际位置
    OGRSpatialReference crs;
    QCOMPARE(crs.SetFromUserInput(result.crsWkt.toUtf8().constData()), OGRERR_NONE);
    QCOMPARE(QString::fromLatin1(crs.GetAuthorityCode(nullptr)), QStringLiteral("32649"));
    // 经磁盘索引恢复后仍须使用相同方言、坐标字段及 scalar。
    const auto cached = clusterSeismicHorizon(sgy, smi, dir.path(), {1, 2}, {}, {}, SeismicHorizonSource::Scatter);
    QCOMPARE(cached.error, result.error); QCOMPARE(cached.cells, result.cells);
  }
  void actualTraceCoordinatesRejectWrongSurvey() {
    QTemporaryDir dir;
    auto data = warpedSurvey(0, 1000);
    const auto sgy = dir.filePath("v.sgy"), smi = dir.filePath("h.dat");
    QVERIFY(writeFile(sgy, data.first)); QVERIFY(writeFile(smi, data.second));
    const auto result = clusterSeismicHorizon(sgy, smi, dir.path(), {1, 2}, {}, {}, SeismicHorizonSource::Scatter);
    QVERIFY(result.error.contains(QStringLiteral("inline 100")));
    QVERIFY(result.error.contains(QStringLiteral("1000.000")));
    QVERIFY(result.cells.isEmpty());
  }
  void projectedQuantizationUsesSurveySpacingAndRecordsResidual() {
    QTemporaryDir dir;
    auto data = warpedSurvey(0, 0, 4); // 非均匀测网最小间距约 22m
    data.second.replace("0.00000 0.00000 100 100 2000", "1.40000 1.20000 100 100 2000");
    const auto sgy = dir.filePath("v.sgy"), smi = dir.filePath("h.dat");
    QVERIFY(writeFile(sgy, data.first)); QVERIFY(writeFile(smi, data.second));
    const auto result = clusterSeismicHorizon(sgy, smi, dir.path(), {1, 2}, {}, {}, SeismicHorizonSource::Scatter);
    QVERIFY2(result.error.isEmpty(), qPrintable(result.error));
    QVERIFY(result.warnings.join(' ').contains(QStringLiteral("1.844")));
    // 同一文件若偏移到半道，仍应失败。
    data.second.replace("1.40000 1.20000 100 100 2000", "10.00000 0.00000 100 100 2000");
    QVERIFY(writeFile(smi, data.second));
    const auto shifted = clusterSeismicHorizon(sgy, smi, dir.path(), {1, 2}, {}, {}, SeismicHorizonSource::Scatter);
    QVERIFY(shifted.error.contains(QStringLiteral("10.000")));
  }
  void interiorMismatchCannotHideBehindMatchingCorners() {
    QTemporaryDir dir;
    const auto sgy = dir.filePath("v.sgy"), smi = dir.filePath("h.dat");
    auto horizon = buildHorizon(100);
    horizon.replace("10.00000 20.00000 100.0 101 2001", "15.00000 20.00000 100.0 101 2001");
    QVERIFY(writeFile(sgy, buildVolume())); QVERIFY(writeFile(smi, horizon));
    const auto result = clusterSeismicHorizon(sgy, smi, dir.path(), {1, 2}, {}, {}, SeismicHorizonSource::Scatter);
    QVERIFY(result.error.contains(QStringLiteral("inline 101")));
    QVERIFY(result.error.contains(QStringLiteral("crossline 2001")));
    QVERIFY(result.error.contains(QStringLiteral("5.000")));
  }
  void malformedDuplicateAndMissingTrace() {
    QTemporaryDir dir;
    const auto sgy = dir.filePath("v.sgy"), smi = dir.filePath("h.dat");
    auto volume = buildVolume();
    // 删除内部道，不应把不存在的道映射到邻道。
    volume.remove(3600 + 5 * 440, 440);
    QVERIFY(writeFile(sgy, volume)); QVERIFY(writeFile(smi, buildHorizon(100)));
    const auto result = clusterSeismicHorizon(sgy, smi, dir.path(), {1, 2}, {}, {}, SeismicHorizonSource::Scatter);
    QVERIFY2(result.error.isEmpty(), qPrintable(result.error));
    QCOMPARE(result.validCells, 15); QCOMPARE(result.cells[(103 - 101) * 4 + 1], -9999);
    QVERIFY(writeFile(smi, buildHorizon(100) + "0 0 100 100 2000\n"));
    const auto duplicate = clusterSeismicHorizon(sgy, smi, dir.path(), {1, 2}, {}, {}, SeismicHorizonSource::Scatter);
    QVERIFY(duplicate.error.contains(QStringLiteral("重复")));
  }
  void nodataBoundaryPreservesGridExtent() {
    QTemporaryDir dir;
    const auto sgy = dir.filePath("v.sgy"), smi = dir.filePath("h.dat");
    auto horizon = buildHorizon(100);
    horizon.replace("0.00000 0.00000 100.0 100 2000", "0.00000 0.00000 -9999 100 2000");
    QVERIFY(writeFile(sgy, buildVolume())); QVERIFY(writeFile(smi, horizon));
    const auto result = clusterSeismicHorizon(sgy, smi, dir.path(), {1, 2}, {}, {}, SeismicHorizonSource::Scatter);
    QVERIFY2(result.error.isEmpty(), qPrintable(result.error));
    QCOMPARE(result.validCells, 15); QCOMPARE(result.extent, QRectF(-5, -10, 40, 80));
  }
  void localMockAcceptanceOnReadOnlyInputs() {
    const QString root = qEnvironmentVariable("PALEO_SEISMIC_MOCK_INPUT");
    if (root.isEmpty()) QSKIP("Set PALEO_SEISMIC_MOCK_INPUT for local data acceptance");
    QTemporaryDir cache;
    const auto result = clusterSeismicHorizon(root + QStringLiteral("/地震/200P_seismic.sgy"),
        root + QStringLiteral("/层位/D61.dat"), cache.path(), {1, 2, 3}, {}, {}, SeismicHorizonSource::Scatter);
    QVERIFY2(result.error.isEmpty(), qPrintable(result.error));
    QVERIFY(result.validCells > 250000);
    QVERIFY(!result.crsWkt.isEmpty());
    qInfo() << "D61 local Mock" << result.columns << result.rows << result.validCells << result.extent << result.warnings;
  }
  void wellVotesReplaceClusterOrderAndAllowManyClustersPerFacies() {
    SeismicClusterResult grid;
    grid.columns = 3; grid.rows = 2; grid.extent = QRectF(0, 0, 30, 20);
    grid.cells = {1, 2, 3, 1, 2, -9999}; grid.validCells = 5;
    const QVector<SeismicClusterWell> wells{
      {"a", "v1", 1, 19, 7}, {"b", "v1", 2, 18, 7}, {"c", "v1", 3, 17, 2},
      {"d", "v1", 11, 19, 7}, {"e", "v1", 12, 18, 2},
      {"a", "v0", 4, 16, 2}, // duplicate well never gets another vote
      {"outside", "v1", 100, 100, 2}, {"nodata", "v1", 25, 5, 7}};
    const auto result = labelSeismicClusters(grid, wells);
    QVERIFY2(result.error.isEmpty(), qPrintable(result.error));
    QCOMPARE(result.cells[0], 7); QCOMPARE(result.cells[3], 7);
    QCOMPARE(result.cells[1], 2); QCOMPARE(result.cells[4], 2); // tied: lowest code
    QVERIFY(result.cells[2] == 2 || result.cells[2] == 7);
    QCOMPARE(result.cells[5], -9999);
    QCOMPARE(result.clusterMapping.size(), 3);
    QCOMPARE(result.clusterMapping[0].toMap().value("vote_count").toInt(), 3);
    QCOMPARE(result.clusterMapping[1].toMap().value("vote_count").toInt(), 2);
    QCOMPARE(result.clusterMapping[2].toMap().value("vote_count").toInt(), 0);
    QCOMPARE(result.wellVersionIds, QStringList{"v1"});
    QCOMPARE(result.cells, labelSeismicClusters(grid, wells).cells);
    auto onlyOneClass = wells;
    for (auto &well : onlyOneClass) well.code = 7;
    const auto one = labelSeismicClusters(grid, onlyOneClass);
    for (int code : one.cells) QVERIFY(code == 7 || code == -9999);
  }
  void emptyAndInvalidWellPredictionsCannotCreateRandomFacies() {
    SeismicClusterResult grid;
    grid.columns = grid.rows = 1; grid.extent = QRectF(0, 0, 10, 10); grid.cells = {1};
    QVERIFY(!labelSeismicClusters(grid, {}).error.isEmpty());
    QVERIFY(!labelSeismicClusters(grid, {{"a", "v", 5, 5, 0}}).error.isEmpty());
    QVERIFY(!labelSeismicClusters(grid, {{"a", "v", NAN, 5, 7}}).error.isEmpty());
    const auto cancelled = labelSeismicClusters(grid, {{"a", "v", 5, 5, 7}}, 42, [] { return true; });
    QCOMPARE(cancelled.error, QStringLiteral("已取消"));
  }
  void wellVotingUsesNorthUpCellsAndDoesNotClampOutsidePoints() {
    SeismicClusterResult grid;
    grid.columns = grid.rows = 2; grid.extent = QRectF(0, 0, 20, 20);
    grid.cells = {1, 2, 3, 4};
    const auto result = labelSeismicClusters(grid, {
      {"nw", "v", 0, 20, 7}, {"ne", "v", 15, 15, 2},
      {"sw", "v", 5, 5, 2}, {"se", "v", 15, 5, 7},
      {"east", "v", 20, 15, 7}, {"south", "v", 5, 0, 7}, {"west", "v", -1, 15, 2}});
    QVERIFY(result.error.isEmpty());
    QCOMPARE(result.cells, QVector<int>({7, 2, 2, 7}));
    for (const auto &row : result.clusterMapping) QCOMPARE(row.toMap().value("vote_count").toInt(), 1);
  }
  // 窗内（±12ms）特征分离成两簇，相码按 RMS 升序钉序：低 RMS（xl 偶，
  // 恒 +3）→ codes[0]，高 RMS（xl 奇，±30 交替）→ codes[1]。
  void scatterWindowSeparatesInWindowSignatures()
  {
    QTemporaryDir dir;
    QVERIFY(dir.isValid());
    const QString sgy = dir.filePath(QStringLiteral("vol.sgy"));
    const QString smi = dir.filePath(QStringLiteral("h.dat"));
    QVERIFY(writeFile(sgy, buildVolume()));
    QVERIFY(writeFile(smi, buildHorizon(100.0)));
    QTemporaryDir cache;
    const auto result = clusterSeismicHorizon(
        sgy, smi, cache.path(), {1, 2}, {}, {}, SeismicHorizonSource::Scatter);
    QVERIFY2(result.error.isEmpty(), qPrintable(result.error));
    QCOMPARE(result.columns, 4);
    QCOMPARE(result.rows, 4);
    QCOMPARE(result.validCells, 16);
    for (int il = 100; il <= 103; ++il)
      for (int xl = 2000; xl <= 2003; ++xl)
      {
        const int cell = (103 - il) * 4 + (xl - 2000);
        QCOMPARE(result.cells[cell], (xl % 2 == 0) ? 1 : 2);
      }
  }

  // B-01 生产侧回归：SMI 点栅格的 extent 半格外扩（p1−s/2 起、N·s 宽）——
  // 像元边界落在最近样本 Voronoi 边界（半倍 spacing）上，而不是样本线上。
  void scatterExtentCoversSampleVoronoiCells()
  {
    QTemporaryDir dir;
    QVERIFY(dir.isValid());
    const QString sgy = dir.filePath(QStringLiteral("vol.sgy"));
    const QString smi = dir.filePath(QStringLiteral("h.dat"));
    QVERIFY(writeFile(sgy, buildVolume()));
    QVERIFY(writeFile(smi, buildHorizon(100.0)));
    QTemporaryDir cache;
    const auto result = clusterSeismicHorizon(
        sgy, smi, cache.path(), {1, 2}, {}, {}, SeismicHorizonSource::Scatter);
    QVERIFY2(result.error.isEmpty(), qPrintable(result.error));
    QCOMPARE(result.extent, QRectF(-5.0, -10.0, 40.0, 80.0)); // sX=10, sY=20
  }
  // B-01 投票侧回归：extent 半格外扩口径下，恰在样本线上的井精确命中本
  // 样本格（旧口径 y 向错一整格、外半带零票）。每样本格给唯一簇号，
  // 井码=所在样本格簇号 → 每票必须落回自己的格。
  void scatterVoteBinsAlignToNearestSampleVoronoi()
  {
    SeismicClusterResult grid;
    grid.columns = 4; grid.rows = 4;
    grid.extent = QRectF(-5.0, -10.0, 40.0, 80.0);
    grid.cells.resize(16);
    for (int il = 0; il < 4; ++il)
      for (int xl = 0; xl < 4; ++xl)
        grid.cells[(3 - il) * 4 + xl] = il * 4 + xl + 1;
    grid.validCells = 16;
    QVector<SeismicClusterWell> wells;
    for (int il = 0; il < 4; ++il)
      for (int xl = 0; xl < 4; ++xl)
        wells.append({QStringLiteral("w%1_%2").arg(il).arg(xl), QStringLiteral("v1"),
                      10.0 * xl, 20.0 * il, grid.cells[(3 - il) * 4 + xl]});
    const auto result = labelSeismicClusters(grid, wells);
    QVERIFY2(result.error.isEmpty(), qPrintable(result.error));
    QCOMPARE(result.supportingWells.size(), wells.size());
    for (int i = 0; i < wells.size(); ++i) {
      const auto row = result.supportingWells.at(i).toMap();
      QVERIFY2(row.value("voted").toBool(),
               qPrintable(QStringLiteral("井 %1 零票").arg(wells.at(i).id)));
      QCOMPARE(row.value("cluster_id").toInt(), wells.at(i).code);
    }
  }

  // 同一体、层位挪到 TWT 160ms：窗（148-172ms → 样本 37-43）落在恒 1.0
  // 背景上——窗外签名（88-112ms 异常）不得影响特征，全体同簇 codes[0]。
  void scatterWindowIgnoresSignaturesOutsideWindow()
  {
    QTemporaryDir dir;
    QVERIFY(dir.isValid());
    const QString sgy = dir.filePath(QStringLiteral("vol.sgy"));
    const QString smi = dir.filePath(QStringLiteral("h.dat"));
    QVERIFY(writeFile(sgy, buildVolume()));
    QVERIFY(writeFile(smi, buildHorizon(160.0)));
    QTemporaryDir cache;
    const auto result = clusterSeismicHorizon(
        sgy, smi, cache.path(), {1, 2}, {}, {}, SeismicHorizonSource::Scatter);
    QVERIFY2(result.error.isEmpty(), qPrintable(result.error));
    QCOMPARE(result.validCells, 16);
    for (int cell = 0; cell < result.cells.size(); ++cell)
      QCOMPARE(result.cells[cell], 1);
  }

  // full-audit F13（R2-08 补测）：非 float 可表示的 nodata（1e30）——按
  // float 口径归一后哨兵格不再计入有效样点（旧比较 float==double 恒不等，
  // 哨兵格混进聚类）。栅格 4×4 与 buildVolume 的 CDP 网格对齐
  //（x∈[0,40)、y∈(-20,60]），6 格填 1e30、10 格填 100ms。
  void timeRasterNodataBeyondFloatPrecisionExcluded()
  {
    QTemporaryDir dir;
    QVERIFY(dir.isValid());
    GDALAllRegister();
    const QString tif = dir.filePath(QStringLiteral("h.tif"));
    {
      GDALDriverH drv = GDALGetDriverByName("GTiff");
      QVERIFY(drv);
      GDALDatasetH ds = GDALCreate(drv, tif.toUtf8().constData(), 4, 4, 1,
                                   GDT_Float32, nullptr);
      QVERIFY(ds);
      // 域贴体覆盖（x∈[0,30]、y∈[0,60]）：格心必须落在号域内——聚类不外推，
      // 越界格被跳过（首版 40×80 域被跳 2 格的教训）。
      double gt[6] = {0.0, 7.5, 0.0, 60.0, 0.0, -15.0};
      QVERIFY(GDALSetGeoTransform(ds, gt) == CE_None);
      GDALRasterBandH band = GDALGetRasterBand(ds, 1);
      GDALSetRasterNoDataValue(band, 1e30);
      float values[16];
      for (int i = 0; i < 16; ++i)
        values[i] = (i % 16 < 10) ? 100.0f : 1e30f;
      QVERIFY(GDALRasterIO(band, GF_Write, 0, 0, 4, 4, values, 4, 4,
                           GDT_Float32, 0, 0) == CE_None);
      GDALClose(ds);
    }
    const QString sgy = dir.filePath(QStringLiteral("vol.sgy"));
    QVERIFY(writeFile(sgy, buildVolume()));
    QTemporaryDir cache;
    const auto result = clusterSeismicHorizon(
        sgy, tif, cache.path(), {1, 2}, {}, {},
        SeismicHorizonSource::TimeRaster);
    QVERIFY2(result.error.isEmpty(), qPrintable(result.error));
    QCOMPARE(result.validCells, 10);
  }

};

QTEST_GUILESS_MAIN(TestSeismicHorizonCluster)
#include "tst_seismichorizoncluster.moc"
