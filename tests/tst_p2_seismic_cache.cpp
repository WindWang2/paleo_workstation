// 层：测试壳
// tst_p2_seismic_cache — P2/#1024/#1025：时间切片 GeoTIFF writer 的独立
// 成功发布契约（独占暂存、旧缓存保全、发布失败清理）与缓存摘要的严格
// 数值解析。用真实 GDAL 写出/重开，不mock writer。
#include <QtTest>

#include "io/attrgridout.h"

#include <QDir>
#include <QFile>
#include <QFileInfo>
#include <QTemporaryDir>

#include <gdal.h>

#include <cmath>
#include <vector>

namespace
{
paleo::sattr::AttrTimeSliceGrid makeGrid()
{
  paleo::sattr::AttrTimeSliceGrid grid;
  grid.attrId = QStringLiteral("envelope");
  grid.nIl = 5;
  grid.nXl = 4;
  grid.ilValues = {1000, 1001, 1002, 1003, 1004};
  grid.xlValues = {2000, 2001, 2002, 2003};
  grid.values.assign(std::size_t(grid.nIl) * grid.nXl, 1.0f);
  grid.values[std::size_t(2 * grid.nXl + 1)] =
      std::numeric_limits<float>::quiet_NaN(); // 无效像元 → nodata
  grid.valueMin = 1.0;
  grid.valueMax = 1.0;
  grid.validCells = qint64(grid.nIl) * grid.nXl - 1;
  grid.sampleIndex = 64;
  grid.timeMs = 128.0;
  grid.sampleIntervalMs = 2.0;
  grid.startTimeMs = 0.0;
  grid.sourceSgyPath = QStringLiteral("/synthetic/p2.sgy");
  grid.paramHash = QStringLiteral("0123456789abcdef0123456789abcdef");
  return grid;
}

// 单位仿射（x=xline·10, y=inline·20）——有限、非退化。
const double kAffine[6] = {0.0, 10.0, 0.0, 20.0, 0.0, 0.0};
} // namespace

class P2SeismicCacheTests : public QObject
{
  Q_OBJECT

  private slots:
    void writerRoundtripAndIdentityMetadata();
    void writerPreservesExistingTargetOnFailure();
    void writerPreservesExistingCacheOnPostCreateFailure();
    void writerPublishFailureCleansOwnStaging();
    void writerRejectsInvalidInputs();
    void summaryRejectsGarbageNumericMetadata();

  private:
    QTemporaryDir m_dir;
    // 目录内全部文件（含隐藏暂存）——「失败不留残件」断言用。
    QStringList allEntries(const QString &dirPath) const
    {
      return QDir(dirPath).entryList(QDir::Files | QDir::Hidden | QDir::NoDotAndDotDot);
    }
};

void P2SeismicCacheTests::writerRoundtripAndIdentityMetadata()
{
  // QTemporaryDir 成员跨用例复用：每用例独立子目录，断言互不污染。
  const QString dir = m_dir.filePath(QStringLiteral("roundtrip"));
  QVERIFY(QDir().mkpath(dir));
  const QString path = QDir(dir).filePath(QStringLiteral("cache.tif"));
  QString err;
  QVERIFY2(paleo::sattr::writeTimeSliceGeoTiff(path, makeGrid(), kAffine, &err),
           qPrintable(err));
  // 成功发布后目录里只有正式产物——无暂存残件。
  QCOMPARE(allEntries(dir), QStringList{QStringLiteral("cache.tif")});

  paleo::sattr::AttrTimeSliceGrid summary;
  QVERIFY2(paleo::sattr::readTimeSliceGeoTiffSummary(path, &summary, &err),
           qPrintable(err));
  QCOMPARE(summary.attrId, QStringLiteral("envelope"));
  QCOMPARE(summary.paramHash, QStringLiteral("0123456789abcdef0123456789abcdef"));
  QCOMPARE(summary.sampleIndex, 64);
  QCOMPARE(summary.nIl, 5);
  QCOMPARE(summary.nXl, 4);
  QCOMPARE(summary.validCells, qint64(5 * 4 - 1));
  QCOMPARE(summary.timeMs, 128.0);
  QCOMPARE(summary.sampleIntervalMs, 2.0);

  // 覆盖发布：同名目标替换后内容仍是新身份（无暂存嵌套）。
  auto grid2 = makeGrid();
  grid2.paramHash = QStringLiteral("ffffffffffffffffffffffffffffffff");
  QVERIFY2(paleo::sattr::writeTimeSliceGeoTiff(path, grid2, kAffine, &err),
           qPrintable(err));
  QVERIFY2(paleo::sattr::readTimeSliceGeoTiffSummary(path, &summary, &err),
           qPrintable(err));
  QCOMPARE(summary.paramHash, grid2.paramHash);
  QCOMPARE(allEntries(dir), QStringList{QStringLiteral("cache.tif")});
}

void P2SeismicCacheTests::writerPreservesExistingTargetOnFailure()
{
  // 目标文件名合法，追加暂存后缀后超过文件系统的单段长度上限。
  // 这种创建暂存失败不依赖权限位，普通用户与管理员得到相同结果。
  const QString dir = m_dir.filePath(QStringLiteral("preserve"));
  QVERIFY(QDir().mkpath(dir));
  const QString original = QDir(dir).filePath(QStringLiteral("old.tif"));
  const QString name = QString(250, QLatin1Char('a')) + QStringLiteral(".tif");
  const QString path = QDir(dir).filePath(name);
  QString err;
  QVERIFY2(paleo::sattr::writeTimeSliceGeoTiff(original, makeGrid(), kAffine, &err),
           qPrintable(err));
  QFile old(original);
  QVERIFY(old.open(QIODevice::ReadOnly));
  const QByteArray before = old.readAll();
  old.close();
  QVERIFY2(old.rename(path), qPrintable(old.errorString()));

  auto replacement = makeGrid();
  replacement.paramHash = QStringLiteral("ffffffffffffffffffffffffffffffff");
  QVERIFY(!paleo::sattr::writeTimeSliceGeoTiff(path, replacement, kAffine, &err));
  QVERIFY2(err.contains(QStringLiteral("无法创建暂存栅格")), qPrintable(err));
  QCOMPARE(allEntries(dir), QStringList{name});

  QVERIFY(old.open(QIODevice::ReadOnly));
  QCOMPARE(old.readAll(), before); // 旧缓存逐字节不变
  old.close();
  // 用原短路径让 GDAL 重开，避开 Windows 原生路径长度限制。
  QVERIFY2(old.rename(original), qPrintable(old.errorString()));
  GDALAllRegister();
  GDALDatasetH ds = GDALOpen(original.toUtf8().constData(), GA_ReadOnly);
  QVERIFY(ds);
  GDALClose(ds);
}

void P2SeismicCacheTests::writerPreservesExistingCacheOnPostCreateFailure()
{
  // #1024：预置有效缓存后，在 GDALCreate 之后的元数据写入阶段注入失败（空 paramHash /
  // 空 sourceSgyPath），旧缓存字节与可读性必须完全不变且不留悬空暂存；随后正常覆盖成功。
  const QString dir = m_dir.filePath(QStringLiteral("postcreate_fail"));
  QVERIFY(QDir().mkpath(dir));
  const QString path = QDir(dir).filePath(QStringLiteral("cache.tif"));
  QString err;
  const auto originalGrid = makeGrid();
  QVERIFY2(paleo::sattr::writeTimeSliceGeoTiff(path, originalGrid, kAffine, &err),
           qPrintable(err));
  QFile existing(path);
  QVERIFY(existing.open(QIODevice::ReadOnly));
  const QByteArray before = existing.readAll();
  existing.close();

  auto badHash = makeGrid();
  badHash.paramHash.clear();
  QVERIFY(!paleo::sattr::writeTimeSliceGeoTiff(path, badHash, kAffine, &err));
  QVERIFY2(err.contains(QStringLiteral("元数据写入失败")), qPrintable(err));
  QCOMPARE(allEntries(dir), QStringList{QStringLiteral("cache.tif")});

  auto badSource = makeGrid();
  badSource.sourceSgyPath.clear();
  QVERIFY(!paleo::sattr::writeTimeSliceGeoTiff(path, badSource, kAffine, &err));
  QVERIFY2(err.contains(QStringLiteral("元数据写入失败")), qPrintable(err));
  QCOMPARE(allEntries(dir), QStringList{QStringLiteral("cache.tif")});

  // 旧缓存字节逐位一致，GDAL 读回摘要仍是旧身份。
  QVERIFY(existing.open(QIODevice::ReadOnly));
  QCOMPARE(existing.readAll(), before);
  existing.close();
  paleo::sattr::AttrTimeSliceGrid summary;
  QVERIFY2(paleo::sattr::readTimeSliceGeoTiffSummary(path, &summary, &err), qPrintable(err));
  QCOMPARE(summary.paramHash, originalGrid.paramHash);

  // 随后合法覆盖完整替换且无悬空暂存。
  auto updated = makeGrid();
  updated.paramHash = QStringLiteral("aaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaa");
  QVERIFY2(paleo::sattr::writeTimeSliceGeoTiff(path, updated, kAffine, &err), qPrintable(err));
  QCOMPARE(allEntries(dir), QStringList{QStringLiteral("cache.tif")});
  QVERIFY2(paleo::sattr::readTimeSliceGeoTiffSummary(path, &summary, &err), qPrintable(err));
  QCOMPARE(summary.paramHash, updated.paramHash);
}

void P2SeismicCacheTests::writerPublishFailureCleansOwnStaging()
{
  // 目标路径被目录占位：暂存写出+校验全部成功，rename 发布失败——
  // 只清理自己的暂存，不动占位目录（并发/权限场景的确定性接缝）。
  const QString dir = m_dir.filePath(QStringLiteral("pubfail"));
  QVERIFY(QDir().mkpath(dir));
  const QString path = QDir(dir).filePath(QStringLiteral("occupied.tif"));
  QString err;
  QVERIFY(QDir().mkdir(path));
  QVERIFY(!paleo::sattr::writeTimeSliceGeoTiff(path, makeGrid(), kAffine, &err));
  QVERIFY2(err.contains(QStringLiteral("发布失败")) ||
               err.contains(QStringLiteral("rename")),
           qPrintable(err));
  QVERIFY(QFileInfo(path).isDir()); // 占位目录未被误删（占位本身不是文件）
  // 无任何暂存残件（含隐藏文件）——Files 口径下目录占位不计入。
  const auto leftovers = allEntries(dir);
  QVERIFY2(leftovers.isEmpty(),
           qPrintable(QStringLiteral("unexpected leftovers: %1")
                          .arg(leftovers.join(QLatin1Char(',')))));
}

void P2SeismicCacheTests::writerRejectsInvalidInputs()
{
  const QString dir = m_dir.filePath(QStringLiteral("reject"));
  QVERIFY(QDir().mkpath(dir));
  const QString path = QDir(dir).filePath(QStringLiteral("bad.tif"));
  QString err;
  { // 仿射含 NaN。
    double nanAffine[6] = {0.0, 10.0, 0.0, std::nan(""), 0.0, 0.0};
    QVERIFY(!paleo::sattr::writeTimeSliceGeoTiff(path, makeGrid(), nanAffine, &err));
  }
  { // 轴值表长度不符。
    auto grid = makeGrid();
    grid.ilValues.pop_back();
    QVERIFY(!paleo::sattr::writeTimeSliceGeoTiff(path, grid, kAffine, &err));
  }
  { // 值块尺寸不符（乘法核验）。
    auto grid = makeGrid();
    grid.values.pop_back();
    QVERIFY(!paleo::sattr::writeTimeSliceGeoTiff(path, grid, kAffine, &err));
  }
  { // 时间元数据非有限。
    auto grid = makeGrid();
    grid.timeMs = std::numeric_limits<double>::quiet_NaN();
    QVERIFY(!paleo::sattr::writeTimeSliceGeoTiff(path, grid, kAffine, &err));
  }
  { // validCells 越界。
    auto grid = makeGrid();
    grid.validCells = qint64(grid.nIl) * grid.nXl + 7;
    QVERIFY(!paleo::sattr::writeTimeSliceGeoTiff(path, grid, kAffine, &err));
  }
  { // 值域倒挂。
    auto grid = makeGrid();
    grid.valueMin = 2.0;
    grid.valueMax = 1.0;
    QVERIFY(!paleo::sattr::writeTimeSliceGeoTiff(path, grid, kAffine, &err));
  }
  // 前置拒绝：目录里什么都不能有（连暂存都不产生）。
  const auto leftovers = allEntries(dir);
  QVERIFY2(leftovers.isEmpty(),
           qPrintable(QStringLiteral("unexpected leftovers: %1")
                          .arg(leftovers.join(QLatin1Char(',')))));
}

void P2SeismicCacheTests::summaryRejectsGarbageNumericMetadata()
{
  // P2/W5：非数字/非有限/超范围的数值元数据不得被 toDouble/toInt 静默
  // 归零后交给下游——一律判坏（视同未命中重算）。
  // 每例独立成图：同一文件反复 corrupt 会让后例踩在前例残值上通过
  // （review m1）；表驱动逐例重写完整元数据集合，只改目标键。
  const QString dir = m_dir.filePath(QStringLiteral("meta"));
  QVERIFY(QDir().mkpath(dir));
  struct Case { const char *file; const char *key; const char *value; };
  const Case cases[] = {
      {"a.tif", "PALEO_VALUE_MIN", "not-a-number"},
      {"b.tif", "PALEO_TIME_MS", "nan"},
      {"c.tif", "PALEO_VALID_CELLS", "999999999999"},
      {"d.tif", "PALEO_TIME_SAMPLE_INDEX", "-4"},
      {"e.tif", "PALEO_TIME_SAMPLE_INDEX", "sixty-four"},
      {"f.tif", "PALEO_DT_MS", "1e999"}, // 溢出 → inf（非有限）
  };
  paleo::sattr::AttrTimeSliceGrid summary;
  for (const Case &c : cases)
  {
    const QString path = QDir(dir).filePath(QString::fromUtf8(c.file));
    QString writeErr;
    const bool written =
        paleo::sattr::writeTimeSliceGeoTiff(path, makeGrid(), kAffine, &writeErr);
    QVERIFY2(written, qPrintable(writeErr));
    // 只改一个键，其余保持 writer 写出的合法值——确保被拒的是目标键。
    GDALAllRegister();
    GDALDatasetH ds = GDALOpen(path.toUtf8().constData(), GA_Update);
    QVERIFY(ds);
    QCOMPARE(GDALSetMetadataItem(ds, c.key, c.value, nullptr), CE_None);
    QCOMPARE(GDALClose(ds), CE_None);
    QString err;
    QVERIFY2(!paleo::sattr::readTimeSliceGeoTiffSummary(path, &summary, &err),
             qPrintable(QStringLiteral("%1=%2 accepted").arg(
                 QString::fromUtf8(c.key), QString::fromUtf8(c.value))));
    QVERIFY2(err.contains(QStringLiteral("不可解析")) ||
                 err.contains(QStringLiteral("数值")),
             qPrintable(err));
  }
}

QTEST_MAIN(P2SeismicCacheTests)
#include "tst_p2_seismic_cache.moc"
