// 层：测试壳
// tst_coldstart — goal/data-perf 轮4：数据路径冷启动分段计时。
//
// 「冷启动」在这里指**数据面**的冷启动，不是 GUI 进程的冷启动：GUI 那条
// （QGIS 初始化 / 面板 / 首帧）由 tst_startup_trace 的真实进程面负责，本机
// 跑不了（QGIS prefix 只有 4 个 dll、缺 apps/+share/，所有链 QGIS 的二进制
// 都是 STATUS_DLL_NOT_FOUND；且沙箱拦 QProcess）——那属于环境限制，不在
// 本方向内重做。
//
// 方向 21 要回答的是：用户打开一个工程，头几秒花在数据面的哪一步。所以这里
// 搭一个真实工作区（catalog.sqlite + SEG-Y + LAS），逐段量「首触」耗时，
// 再量同进程内「二触」耗时做对照：
//
//   catalog_open   打开工程 → catalog 装载（sqlite 稳态口径，见 tst_catalog）
//   segy_index     首次触碰一体 → 缓存命中读索引（轮1 修复后的口径）
//   segy_trace     索引里取一道的波形（数据面真正被用户看到的第一次读）
//   las_header     LAS 头解析（井口/曲线清单，列表页要的东西）
//   las_first_read LAS 数据段整读（首屏曲线）
//
// 断言用结构关系而非绝对毫秒（禁区）：
//   · 首触之和 ≤ 各段二触之和的某个倍数——即「缓存/索引起作用」这件事本身；
//   · 每段首触不得比二触显著更慢（若更慢，说明二触没走到缓存路径，探针失真）。
#include <QtTest>

#include <QDir>
#include <QElapsedTimer>
#include <QFileInfo>
#include <QTemporaryDir>
#include <QThread>

#include <chrono>

#include <algorithm>
#include <filesystem>

#include "../src/catalog/datacatalog.h"
#include "../src/io/lasparser.h"
#include "../src/io/perffixtures.h"
#include "domain/seismic/sgyindexcache.h"
#include "Data/Sgy/SgyIndexService.h"

namespace
{
using Clock = QElapsedTimer;

// 墙钟而不是 QElapsedTimer：catalog open 里有要拿排他锁的 PRAGMA，锁等待
// 期间后者的读数在本机测到与真实流逝差一个数量级（30s/60s 这种整数值就是
// 它失真的指纹）。全探针统一用 steady_clock，避免同一份报告里两种口径。
double medianOf(const std::function<void()> &fn, int runs)
{
  QVector<double> xs;
  for (int i = 0; i < runs; ++i)
  {
    const auto t0 = std::chrono::steady_clock::now();
    fn();
    const auto t1 = std::chrono::steady_clock::now();
    xs.append(std::chrono::duration<double, std::milli>(t1 - t0).count());
  }
  std::sort(xs.begin(), xs.end());
  return double(xs.at(xs.size() / 2));
}

seismic::SgyIndexBuildRequest indexRequest(const std::filesystem::path &sgy)
{
  seismic::SgyIndexBuildRequest request;
  request.path = sgy;
  request.useCache = true;
  request.saveCache = true;
  request.verbose = false;
  return request;
}
} // namespace

class ColdStartTests : public QObject
{
  Q_OBJECT

private slots:
  void initTestCase();
  void dataPathColdStartSegmentsAreOrderedAndCacheBacked();

private:
  QTemporaryDir m_dir;
  QString m_projectDir;
  QString m_sgyPath;
  QString m_lasPath;
  std::filesystem::path m_sgyStd;
  qint64 m_sgyBytes = 0;
  qint64 m_lasBytes = 0;
};

void ColdStartTests::initTestCase()
{
  QVERIFY(m_dir.isValid());
  m_projectDir = m_dir.path();

  // 索引缓存落在工程自己的隔离目录里（与产品路径同构：SEISMIC_INDEX_CACHE_DIR
  // 由产品代码注入，测试里显式给一份）。
  const QString cacheDir = m_dir.filePath(QStringLiteral("index-cache"));
  QVERIFY(QDir().mkpath(cacheDir));
  qputenv("SEISMIC_INDEX_CACHE_DIR", QFile::encodeName(cacheDir));
  QCOMPARE(QString::fromStdU16String(seismic::SgyIndexCache::CacheDirectory().u16string()),
           cacheDir);

  // SEG-Y：100 inline × 100 crossline = 10000 道 × 256 采样 ≈ 10.2MB fp32。
  // 这个尺寸让「整卷扫一遍」在几十毫秒量级——够慢到能被稳定测到，又不至于
  // 让测试变慢。
  m_sgyPath = m_dir.filePath(QStringLiteral("coldstart.sgy"));
  QString err;
  QVERIFY2(PerfFixtures::makeSyntheticSegy(m_sgyPath, 100, 100, 256, 1000, 2000, 2000, &err) ==
               10000,
           qPrintable(err));
  m_sgyStd = std::filesystem::path(m_sgyPath.toStdString());
  m_sgyBytes = QFileInfo(m_sgyPath).size();

  // LAS：5 万行 × 5 曲线。
  m_lasPath = m_dir.filePath(QStringLiteral("coldstart.las"));
  QVERIFY2(PerfFixtures::makeSyntheticLas(m_lasPath, 50000,
                                          {QStringLiteral("DEPT"), QStringLiteral("GR"),
                                           QStringLiteral("DT"), QStringLiteral("RHOB"),
                                           QStringLiteral("NPHI")},
                                          1000.0, 0.125, &err),
           qPrintable(err));
  m_lasBytes = QFileInfo(m_lasPath).size();

  // catalog：1000 资产（与 tst_catalog 的归因口径同量级）。
  QVERIFY2(PerfFixtures::makeSyntheticCatalogDir(m_projectDir, 1000, &err), qPrintable(err));

  // 灌完数据立刻打开同一目录会撞锁：CatalogStore 每次可写 open 都要
  // PRAGMA journal_mode = WAL（排他锁）+ wal_checkpoint(TRUNCATE)，
  // busy_timeout 5s 起。刚写完的库若还有残留句柄，这两步各等一轮。
  // 冷启动测量要的是「打开一个已经静置的工程」——真实用户场景里工程是
  // 上次会话留下的，不是三毫秒前刚被写过。这里显式让锁散掉。
  QThread::msleep(250);
  qInfo("coldstart: fixture ready");
}

void ColdStartTests::dataPathColdStartSegmentsAreOrderedAndCacheBacked()
{
  // --- 段 1：catalog 打开（真工区每次启动都走这条） ---
  // 「冷」有两层含义，必须分开量，否则会把一次性成本当成每次都付的钱：
  //   · 进程冷：进程内第一次碰 sqlite（Qt SQL 驱动插件加载 + 首条连接建立）
  //   //  文件冷：进程已热，但打开的是另一个工程目录（页缓存/连接都要新建）
  // 报告里 catalog_open 那一段要给的是这两个数，不是一个混在一起的数。
  double catalogOpenMs = -1.0;
  {
    DataCatalog cat;
    QString oerr;
    // 墙钟而不是 QElapsedTimer：catalog open 里含 PRAGMA journal_mode=WAL
    // 这类要拿排他锁的操作，锁等待期间 QElapsedTimer 的读数在本机测过
    // 与真实流逝时间差一个数量级（30s/60s 这种整数值就是它的指纹）。
    const auto t0 = std::chrono::steady_clock::now();
    QVERIFY2(cat.open(m_projectDir, &oerr), qPrintable(oerr));
    const auto t1 = std::chrono::steady_clock::now();
    catalogOpenMs = std::chrono::duration<double, std::milli>(t1 - t0).count();
    QCOMPARE(cat.entities(QStringLiteral("well")).size(), 1000);
  } // 析构关连接：Windows 上 sqlite 连接持文件句柄，同目录再开第二个连接
      // 不是稳定支持的（tst_catalog 的同类测量也都是分作用域开的）。
  qInfo("coldstart: segment catalog_open = %.1fms", catalogOpenMs);

  // 「进程已热时，换一个工程目录要付多少」——这是「第二次打开工程」的真实
  // 形态，也是启动序列里唯一会重复付的钱（用户连开两个工程）。
  //
  // 刻意换目录而不是把同一个目录再开一遍：CatalogStore 每次可写 open 都要
  // wal_checkpoint(TRUNCATE)（要写锁）+ 整库备份拷贝 + 关连接重连。同目录
  // 紧接着再开一次会在这三件事上等自己的锁（busy_timeout 5s 起），
  // 量到的是锁等待而不是 open 本身——那不是启动路径上的形态。
  double catalogOtherDirMs = -1.0;
  int otherAssets = 0;
  {
    // 规模刻意比主工程小（200 vs 2000）：这个对照只回答「换目录的固定成本
    // 是多少」，用同规模会把 makeSyntheticCatalogDir 的 O(n²) 灌数据成本
    // 翻一倍（initTestCase 已经十几秒），把探针变成慢测试。
    const QString otherDir = m_dir.filePath(QStringLiteral("project-2"));
    QString perr;
    QVERIFY2(PerfFixtures::makeSyntheticCatalogDir(otherDir, 200, &perr), qPrintable(perr));
    DataCatalog other;
    QString e;
    const auto t0 = std::chrono::steady_clock::now();
    const bool ok = other.open(otherDir, &e);
    const auto t1 = std::chrono::steady_clock::now();
    catalogOtherDirMs = std::chrono::duration<double, std::milli>(t1 - t0).count();
    QVERIFY2(ok, qPrintable(e));
    otherAssets = other.entities(QStringLiteral("well")).size();
  }
  QCOMPARE(otherAssets, 200);

  // --- 段 2：SEG-Y 索引（冷 = 首次触碰该体，必须扫） ---
  std::string removeErr;
  QVERIFY2(seismic::SgyIndexCache::Remove(m_sgyStd, removeErr),
           qPrintable(QString::fromStdString(removeErr)));
  const seismic::SgyIndexBuildOutcome cold = seismic::BuildSgyIndexAuto(indexRequest(m_sgyStd));
  QVERIFY2(cold.index != nullptr, qPrintable(QString::fromStdString(cold.message)));
  QCOMPARE(cold.fromCache, false);
  QCOMPARE(cold.index->traceCount, 10000);
  // 二触：缓存命中。轮1 的修复保证这条真的存在。
  const double segyIndexWarmMs = medianOf(
      [&] {
        const seismic::SgyIndexBuildOutcome warm =
            seismic::BuildSgyIndexAuto(indexRequest(m_sgyStd));
        QVERIFY(warm.index != nullptr);
        QVERIFY(warm.fromCache);
      },
      3);

  // --- 段 3：按 inline/xline 定位一道（数据面首次真正按几何找道） ---
  const seismic::SgyIndexPtr index = cold.index;
  QVERIFY(index->complete);
  double locateMs = -1.0;
  int located = -1;
  {
    const auto t0 = std::chrono::steady_clock::now();
    located = index->FindTraceIndex(1050, 2050);
    const auto t1 = std::chrono::steady_clock::now();
    locateMs = std::chrono::duration<double, std::milli>(t1 - t0).count();
  }
  QVERIFY(located >= 0);
  const auto &trace = index->traces[static_cast<std::size_t>(located)];
  QCOMPARE(trace.inlineNo, 1050);
  QCOMPARE(trace.xlineNo, 2050);
  QCOMPARE(trace.traceIndex, located);

  // --- 段 4：LAS 头解析（列表页只要头，不要数据） ---
  double headerMs = -1.0;
  int headerCurveCount = 0;
  {
    LasHeaderInfo info;
    QString herr;
    const auto t0 = std::chrono::steady_clock::now();
    const bool ok = LasParser::parseHeader(m_lasPath, info, &herr);
    const auto t1 = std::chrono::steady_clock::now();
    headerMs = std::chrono::duration<double, std::milli>(t1 - t0).count();
    QVERIFY2(ok, qPrintable(herr));
    headerCurveCount = info.curveNames.size();
  }
  QCOMPARE(headerCurveCount, 5);

  // --- 段 5：LAS 数据段整读（首屏曲线） ---
  double lasReadMs = -1.0;
  qint64 lasRows = -1;
  {
    const auto t0 = std::chrono::steady_clock::now();
    const LasDoc doc = LasParser::parseDoc(m_lasPath);
    const auto t1 = std::chrono::steady_clock::now();
    lasReadMs = std::chrono::duration<double, std::milli>(t1 - t0).count();
    QVERIFY2(doc.ok, qPrintable(doc.error));
    lasRows = doc.curves.isEmpty() ? 0 : doc.curves.first().values.size();
  }
  QCOMPARE(lasRows, qint64(50000));
  // 二触：整读再来一次（OS 页缓存 + 分配器已热）。
  const double lasRereadMs = medianOf(
      [&] {
        const LasDoc doc = LasParser::parseDoc(m_lasPath);
        QVERIFY(doc.ok);
        QCOMPARE(doc.curves.first().values.size(), qint64(50000));
      },
      3);

  // 冷态总时长 = 首触各段之和（进程冷那次 catalog open 是启动序列里真付的
  // 一笔）。热态对照 = 进程已热后再开一个工程 + 索引走缓存 + LAS 页缓存热。
  const double coldTotalMs = catalogOpenMs + cold.wallMs + locateMs + headerMs + lasReadMs;
  const double warmTotalMs = catalogOtherDirMs + segyIndexWarmMs + lasRereadMs;
  qInfo("coldstart: inputs segy=%lldKB las=%lldKB | "
        "cold[ catalog_open_process_cold(1000 assets)=%.1f segy_index=%.1f segy_locate=%.3f "
        "las_header=%.2f las_first_read=%.1f ] total=%.1fms | "
        "warm-path[ catalog_open_dir_cold(200 assets)=%.1f segy_index_cache=%.1f "
        "las_reread=%.1f ] total=%.1fms",
        static_cast<long long>(m_sgyBytes / 1024), static_cast<long long>(m_lasBytes / 1024),
        catalogOpenMs, cold.wallMs, locateMs, headerMs, lasReadMs, coldTotalMs,
        catalogOtherDirMs, segyIndexWarmMs, lasRereadMs, warmTotalMs);

  // 探针自身可信度：热路径上的每一段都必须不比冷态贵。若某段反而更贵得多，
  // 说明那一段没走到缓存/热路径（探针失真，后面所有比较都不可信）。
  // 门留 1.5× + 2ms 容差：LAS 整读两次的差只有 2%（8.7 vs 8.9ms，纯属
  // 分配器/页缓存抖动），卡死相等会把这类噪声变成假红。
  QVERIFY2(segyIndexWarmMs < cold.wallMs,
           qPrintable(QStringLiteral("cached segy index load %1ms is not faster than the cold scan "
                                     "%2ms — the probe is measuring the wrong thing")
                          .arg(segyIndexWarmMs)
                          .arg(cold.wallMs)));
  QVERIFY2(catalogOtherDirMs <= catalogOpenMs * 1.5 + 20.0,
           qPrintable(QStringLiteral("a warm-process catalog open %1ms is far more expensive than "
                                     "the process-cold one %2ms")
                          .arg(catalogOtherDirMs)
                          .arg(catalogOpenMs)));
  QVERIFY2(lasRereadMs <= lasReadMs * 1.5 + 2.0,
           qPrintable(QStringLiteral("second LAS read %1ms is far more expensive than the first %2ms "
                                     "— page cache is not doing its job, the probe is suspect")
                          .arg(lasRereadMs)
                          .arg(lasReadMs)));

  // 结构关系：索引缓存是这里最大的一笔（整卷扫 vs 读缓存文件），命中后总时长
  // 应当显著低于冷态。门用比率而非绝对毫秒——宿主机差异不该让这条变红。
  QVERIFY2(warmTotalMs < coldTotalMs,
           qPrintable(QStringLiteral("warm data-path total %1ms should be below the cold one %2ms")
                          .arg(warmTotalMs)
                          .arg(coldTotalMs)));

  // 每一段都要有非零计时：任何一段 0ms 说明计时器/打点没生效，整份报告作废。
  QVERIFY(catalogOpenMs > 0.0);
  QVERIFY(catalogOtherDirMs > 0.0);
  QVERIFY(cold.wallMs > 0.0);
  QVERIFY(headerMs > 0.0);
  QVERIFY(lasReadMs > 0.0);
}

QTEST_MAIN(ColdStartTests)
#include "tst_coldstart.moc"
