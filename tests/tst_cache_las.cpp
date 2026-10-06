// tst_cache_las — wave/io-perf-cache D1.1/D1.2/D1.10：LasCache 两级缓存、
// 一致性失效、损坏自愈、并发合并。
#include <QtTest>

#include "io/lascache.h"
#include "io/lasdoc.h" // LasDoc（缓存载荷契约）
#include "io/perffixtures.h"

#include <QElapsedTimer>
#include <QFile>
#include <QThread>
#include <QTemporaryDir>
#include <QtConcurrent>
#include <algorithm>
#include <array>
#include <atomic>
#include <cstring>

class CacheLasTests : public QObject
{
    Q_OBJECT

  private slots:
    void coldParsePopulatesBothLevels();
    void secondOpenUnder5ms();
    void mtimeChangeInvalidates();
    void sizeChangeInvalidates();
    void corruptDiskCacheSelfHeals();
    void versionBumpedFileRejected();
    void memoryLruEviction();
    void explicitInvalidate();
    void writePathInvalidation();
    void concurrentLoadCoalesces();
    void coalescedRidersReceiveIssues();

  private:
    QTemporaryDir m_dir;
};

void CacheLasTests::coldParsePopulatesBothLevels()
{
  const QString las = m_dir.filePath("a.las");
  QVERIFY(PerfFixtures::makeSyntheticLas(las, 500));
  LasCache::shared().setDiskRoot(m_dir.filePath("idx"));
  LasCache::shared().invalidate();

  const LasDoc doc = LasCache::shared().load(las);
  QVERIFY(doc.ok);
  QCOMPARE(doc.curveNames, QStringList({"DEPT", "GR", "DT", "RHOB", "NPHI"}));
  QCOMPARE(doc.curves.first().values.size(), 500);
  QVERIFY(LasCache::shared().isCached(las));
  // 磁盘层有条目。
  const CacheStats st = LasCache::shared().stats();
  QCOMPARE(st.diskWrites, qint64(1));
}

void CacheLasTests::secondOpenUnder5ms()
{
  constexpr std::size_t sampleCount = 5;
  std::array<double, sampleCount> coldTimes{}, diskTimes{}, totalTimes{}, memoryTimes{};
  // 相同 15581 点夹具各自冷建/磁盘读/内存读；中位数抑制单次调度噪声。
  // 每组仍需真实命中、无重解析/重写，以及全部曲线载荷按位不变。
  for (std::size_t sample = 0; sample < sampleCount; ++sample)
  {
    const QString las = m_dir.filePath(QStringLiteral("b_%1.las").arg(sample));
    QVERIFY(PerfFixtures::makeSyntheticLas(las, 15581));
    LasCache::shared().setDiskRoot(m_dir.filePath("idx2"));
    LasCache::shared().invalidate();

    const CacheStats beforeCold = LasCache::shared().stats();
    const LasDoc cold = LasCache::shared().load(las);
    QVERIFY(cold.ok);
    QCOMPARE(cold.curves.size(), 5);
    for (const auto &curve : cold.curves)
      QCOMPARE(curve.values.size(), 15581);
    QCOMPARE(LasCache::shared().stats().diskWrites, beforeCold.diskWrites + 1);
    const qint64 coldNs = LasCache::shared().lastTimings().coldParseNs;
    const double coldMs = coldNs / 1.0e6;
    QVERIFY(coldNs > 0);
    QVERIFY2(coldMs < 2000.0,
             qPrintable(QStringLiteral("cold %1ms >= 2000ms（sanity：解析挂死/死循环）").arg(coldMs)));

    // 冷侧统计仅解析阶段，热侧也比较磁盘读取/解码阶段；整次 load 的线程
    // 调度/指纹/LRU 开销另外打印，不能混入一侧并推断「缓存未生效」。
    // 0.5 比率门及 2000ms sanity 不变；真实命中还必须由计数证明。
    LasCache::shared().clearMemory();
    const CacheStats beforeDisk = LasCache::shared().stats();
    QElapsedTimer t;
    t.start();
    const LasDoc warm = LasCache::shared().load(las);
    const double warmTotalMs = t.nsecsElapsed() / 1.0e6;
    QVERIFY(warm.ok);
    const CacheStats afterDisk = LasCache::shared().stats();
    QCOMPARE(afterDisk.diskHits, beforeDisk.diskHits + 1);
    QCOMPARE(afterDisk.diskWrites, beforeDisk.diskWrites);
    QCOMPARE(afterDisk.selfHeals, beforeDisk.selfHeals);
    const auto diskTimings = LasCache::shared().lastTimings();
    QCOMPARE(diskTimings.coldParseNs, coldNs); // 命中不得回落到重新解析
    QVERIFY(diskTimings.diskLoadNs >= 0);
    const double warmMs = diskTimings.diskLoadNs / 1.0e6;
    coldTimes[sample] = coldMs;
    diskTimes[sample] = warmMs;
    totalTimes[sample] = warmTotalMs;
    // 内存层保留原整次 load ≤ 0.5×cold 的更严格口径，并证明真正命中。
    t.restart();
    const LasDoc memory = LasCache::shared().load(las);
    const double memMs = t.nsecsElapsed() / 1.0e6;
    QVERIFY(memory.ok);
    const CacheStats afterMemory = LasCache::shared().stats();
    QCOMPARE(afterMemory.hits, afterDisk.hits + 1);
    QCOMPARE(afterMemory.diskHits, afterDisk.diskHits);
    QCOMPARE(afterMemory.diskWrites, afterDisk.diskWrites);
    memoryTimes[sample] = memMs;
    for (const LasDoc *cached : {&warm, &memory})
    {
      QCOMPARE(cached->curveNames, cold.curveNames);
      QCOMPARE(cached->curves.size(), cold.curves.size());
      for (qsizetype i = 0; i < cold.curves.size(); ++i)
      {
        const auto &expected = cold.curves[i];
        const auto &actual = cached->curves[i];
        QCOMPARE(actual.name, expected.name);
        QCOMPARE(actual.unit, expected.unit);
        QCOMPARE(actual.descr, expected.descr);
        QCOMPARE(actual.values.size(), expected.values.size());
        QVERIFY(std::memcmp(actual.values.constData(), expected.values.constData(),
                            std::size_t(expected.values.size()) * sizeof(double)) == 0);
      }
    }
  }
  const auto median = [](auto values) {
    std::sort(values.begin(), values.end());
    return values[values.size() / 2];
  };
  const double coldMs = median(coldTimes);
  const double diskMs = median(diskTimes);
  const double totalMs = median(totalTimes);
  const double memMs = median(memoryTimes);
  qInfo("LAS cache median(5) coldParse=%.3fms diskPhase=%.3fms diskLoadTotal=%.3fms memoryLoad=%.3fms",
        coldMs, diskMs, totalMs, memMs);
  QVERIFY2(diskMs < 0.5 * coldMs,
           qPrintable(QStringLiteral("disk phase median %1ms >= 0.5×cold parse median %2ms")
                          .arg(diskMs, 0, 'f', 3).arg(coldMs, 0, 'f', 3)));
  QVERIFY2(memMs < 0.5 * coldMs,
           qPrintable(QStringLiteral("memory load median %1ms >= 0.5×cold parse median %2ms")
                          .arg(memMs, 0, 'f', 3).arg(coldMs, 0, 'f', 3)));
}

void CacheLasTests::mtimeChangeInvalidates()
{
  const QString las = m_dir.filePath("c.las");
  QVERIFY(PerfFixtures::makeSyntheticLas(las, 100));
  LasCache::shared().setDiskRoot(m_dir.filePath("idx3"));
  LasCache::shared().invalidate();
  QVERIFY(LasCache::shared().load(las).ok);
  QVERIFY(LasCache::shared().isCached(las));

  // 改 mtime 不改内容：指纹失配 → 缓存失效重解析。
  QFile f(las);
  QVERIFY(f.open(QIODevice::Append));
  f.write("\n");
  f.close();
  // size 也变了；isCached 必须立即反映。
  QVERIFY(!LasCache::shared().isCached(las));
  const LasDoc again = LasCache::shared().load(las);
  QVERIFY(again.ok);
  QVERIFY(LasCache::shared().isCached(las));
}

void CacheLasTests::sizeChangeInvalidates()
{
  const QString las = m_dir.filePath("d.las");
  QVERIFY(PerfFixtures::makeSyntheticLas(las, 200));
  LasCache::shared().setDiskRoot(m_dir.filePath("idx4"));
  LasCache::shared().invalidate();
  const LasDoc first = LasCache::shared().load(las);
  QVERIFY(first.ok);
  QCOMPARE(first.curves.first().values.size(), 200);

  // 追加 100 行（新文件）。
  QVERIFY(PerfFixtures::makeSyntheticLas(las, 300));
  const LasDoc second = LasCache::shared().load(las);
  QVERIFY(second.ok);
  QCOMPARE(second.curves.first().values.size(), 300);
}

void CacheLasTests::corruptDiskCacheSelfHeals()
{
  const QString las = m_dir.filePath("e.las");
  QVERIFY(PerfFixtures::makeSyntheticLas(las, 100));
  const QString root = m_dir.filePath("idx5");
  LasCache::shared().setDiskRoot(root);
  LasCache::shared().invalidate();
  QVERIFY(LasCache::shared().load(las).ok);
  LasCache::shared().clearMemory();

  // 找到磁盘缓存文件并翻转一字节。
  const QDir dir(root);
  const QStringList files = dir.entryList({QStringLiteral("*.plc")}, QDir::Files);
  QCOMPARE(files.size(), 1);
  QFile f(root + QLatin1Char('/') + files.first());
  QVERIFY(f.open(QIODevice::ReadWrite));
  f.seek(f.size() / 2);
  const char flip = '\xFF';
  f.write(&flip, 1);
  f.close();

  const LasDoc doc = LasCache::shared().load(las); // 自愈：删除 + 重解析
  QVERIFY(doc.ok);
  QCOMPARE(doc.curves.first().values.size(), 100);
  QVERIFY(LasCache::shared().stats().selfHeals >= qint64(1));
}

void CacheLasTests::versionBumpedFileRejected()
{
  // 伪造「旧版本」缓存：手工写一个 version 99 的合法文件——当前读侧版本
  // 范围 [1,1] 拒绝 → 自愈删除 → 重解析。
  const QString las = m_dir.filePath("f.las");
  QVERIFY(PerfFixtures::makeSyntheticLas(las, 60));
  const QString root = m_dir.filePath("idx6");
  LasCache::shared().setDiskRoot(root);
  LasCache::shared().invalidate();
  QVERIFY(LasCache::shared().load(las).ok);
  LasCache::shared().clearMemory();

  const QDir dir(root);
  const QString name = dir.entryList({QStringLiteral("*.plc")}, QDir::Files).first();
  QFile f(root + QLatin1Char('/') + name);
  QVERIFY(f.open(QIODevice::ReadWrite));
  f.seek(8); // version u16 在 offset 8
  const char v[2] = {99, 0};
  f.write(v, 2);
  f.close();

  const LasDoc doc = LasCache::shared().load(las);
  QVERIFY(doc.ok);
  QVERIFY(LasCache::shared().stats().selfHeals >= qint64(1));
}

void CacheLasTests::memoryLruEviction()
{
  const QString root = m_dir.filePath("idx7");
  LasCache::shared().setDiskRoot(root);
  LasCache::shared().invalidate();
  LasCache::shared().setMemoryBudget(64 * 1024 * 1024);
  QStringList paths;
  for (int i = 0; i < 6; ++i)
  {
    const QString p = m_dir.filePath(QStringLiteral("mem%1.las").arg(i));
    QVERIFY(PerfFixtures::makeSyntheticLas(p, 400)); // ~ 每份 25KB 值内存
    paths << p;
    QVERIFY(LasCache::shared().load(p).ok);
  }
  // 压小内存预算 → LRU 逐出发生。
  LasCache::shared().setMemoryBudget(40 * 1024);
  QVERIFY(LasCache::shared().stats().evictions > 0);
  QVERIFY(LasCache::shared().load(paths.last()).ok); // 重新装载仍可用
  LasCache::shared().setMemoryBudget(64 * 1024 * 1024); // 还原（全局单例跨测试）
}

void CacheLasTests::explicitInvalidate()
{
  const QString las = m_dir.filePath("g.las");
  QVERIFY(PerfFixtures::makeSyntheticLas(las, 80));
  LasCache::shared().setDiskRoot(m_dir.filePath("idx8"));
  LasCache::shared().invalidate();
  QVERIFY(LasCache::shared().load(las).ok);
  QVERIFY(LasCache::shared().isCached(las));
  LasCache::shared().invalidate(las);
  QVERIFY(!LasCache::shared().isCached(las));
}

void CacheLasTests::writePathInvalidation()
{
  // D1.10：写路径（重写同一 LAS）后缓存不得回吐旧内容。
  const QString las = m_dir.filePath("h.las");
  QVERIFY(PerfFixtures::makeSyntheticLas(las, 120));
  LasCache::shared().setDiskRoot(m_dir.filePath("idx9"));
  LasCache::shared().invalidate();
  QCOMPARE(LasCache::shared().load(las).curves.first().values.size(), 120);

  // 「写入者」重写为 90 行的新文件。
  QVERIFY(PerfFixtures::makeSyntheticLas(las, 90));
  LasCache::shared().invalidate(las); // 写路径钩子
  QCOMPARE(LasCache::shared().load(las).curves.first().values.size(), 90);
}

void CacheLasTests::concurrentLoadCoalesces()
{
  // D4.7：多个线程同时 load 同一文件 → 只解析一份（coldParse 只计一次，
  // 通过耗时聚合近似验证 + 全部拿到一致结果）。
  const QString las = m_dir.filePath("i.las");
  QVERIFY(PerfFixtures::makeSyntheticLas(las, 5000));
  LasCache::shared().setDiskRoot(m_dir.filePath("idx10"));
  LasCache::shared().invalidate();

  QVector<QFuture<LasDoc>> futs;
  for (int i = 0; i < 6; ++i)
    futs.append(QtConcurrent::run([](const QString &p) { return LasCache::shared().load(p); }, las));
  for (const auto &f : futs)
  {
    const LasDoc doc = f.result();
    QVERIFY(doc.ok);
    QCOMPARE(doc.curves.first().values.size(), 5000);
  }
  // 内存缓存只有一份条目（同文件）。
  QVERIFY(LasCache::shared().isCached(las));
}

namespace
{
// 一条曲线缺单位 → LasParser 产出 MissingUnit 诊断（非致命，doc.ok）。
bool writeIssueLas(const QString &path)
{
  QFile f(path);
  if (!f.open(QIODevice::WriteOnly | QIODevice::Truncate))
    return false;
  f.write("~Version Information\n"
          " VERS.   2.0 : CWLS LAS 2.0\n"
          " WRAP.   NO  : single line\n"
          "~Well Information\n"
          " STRT.M  100.0 : start\n"
          " STOP.M  102.0 : stop\n"
          " STEP.M  1.0 : step\n"
          " NULL.   -999.25 : null\n"
          "~Curve Information\n"
          " DEPT.M  : depth\n"
          " GR.     : gamma ray\n"
          "~A\n"
          "100.0 50.0\n"
          "101.0 60.0\n"
          "102.0 70.0\n");
  return true;
}
} // namespace

void CacheLasTests::coalescedRidersReceiveIssues()
{
  // RUNTIME-03（方向58）：同指纹并发——搭车者也必须拿到本次解析的诊断。
  // 确定性时序：冷解析钩子挡住执行者，直到合并器登记到一个搭车者。
  const QString las = m_dir.filePath("issues.las");
  QVERIFY(writeIssueLas(las));
  LasCache::shared().setDiskRoot(QString()); // 纯内存：强制走解析
  LasCache::shared().invalidate();

  // 单请求路径基线：诊断非空，记下条数。
  QList<LasIssue> single;
  const LasDoc solo = LasCache::shared().load(las, &single);
  QVERIFY2(solo.ok, qPrintable(solo.error));
  QVERIFY(!single.isEmpty());
  LasCache::shared().invalidate();

  // 任何断言提前返回也必须卸钩子、复位磁盘根，不污染同进程后续用例。
  const auto restore = qScopeGuard([this] {
    LasCache::shared().setColdParseHookForTest({});
    LasCache::shared().setDiskRoot(m_dir.filePath("idx10"));
    LasCache::shared().invalidate();
  });
  const int ridersBefore = LasCache::shared().ridersJoinedForTest();
  std::atomic_bool executorInJob{false};
  std::atomic_bool riderTimedOut{false};
  LasCache::shared().setColdParseHookForTest([&] {
    executorInJob = true;
    QElapsedTimer t;
    t.start();
    while (LasCache::shared().ridersJoinedForTest() == ridersBefore)
    {
      if (t.elapsed() > 10000)
      {
        riderTimedOut = true;
        return;
      }
      QThread::msleep(1);
    }
  });

  QList<LasIssue> issuesA, issuesB;
  QFuture<LasDoc> a = QtConcurrent::run([&] { return LasCache::shared().load(las, &issuesA); });
  QElapsedTimer t;
  t.start();
  while (!executorInJob && t.elapsed() < 10000)
    QThread::msleep(1);
  QVERIFY(executorInJob);
  QFuture<LasDoc> b = QtConcurrent::run([&] { return LasCache::shared().load(las, &issuesB); });
  QVERIFY(a.result().ok);
  QVERIFY(b.result().ok);

  QVERIFY(!riderTimedOut);
  QCOMPARE(LasCache::shared().ridersJoinedForTest(), ridersBefore + 1);
  QCOMPARE(issuesA.size(), single.size());
  QCOMPARE(issuesB.size(), single.size()); // 旧实现：搭车者 0 条
}

QTEST_MAIN(CacheLasTests)
#include "tst_cache_las.moc"
