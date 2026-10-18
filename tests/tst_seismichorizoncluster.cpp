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

// 取窗契约锚（f0e7b10d「直接按原始层位提取地震窗并聚类」的文档化契约，
// docs/progress/seismichorizoncluster.md）：Scatter 路径按层位 TWT ±12ms
// 开窗（seismichorizoncluster.cpp:221-225）提均值/RMS/过零率三特征。
// 方言体（crossline@192 恒 0、xline 取 CDP@21、角点取 181-188——同
// tst_segy dialectLayout 回归的布局）× SMI 散点层位。
#include "seismichorizonfixture.h"
using namespace SeismicHorizonFixture;

class TestSeismicHorizonCluster : public QObject
{
  Q_OBJECT
private slots:
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
