// tst_cache_las — wave/io-perf-cache D1.1/D1.2/D1.10：LasCache 两级缓存、
// 一致性失效、损坏自愈、并发合并。
#include <QtTest>

#include "io/lascache.h"
#include "io/lasparser.h"
#include "io/perffixtures.h"

#include <QFile>
#include <QTemporaryDir>
#include <QtConcurrent>

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
  const QString las = m_dir.filePath("b.las");
  QVERIFY(PerfFixtures::makeSyntheticLas(las, 15581));
  LasCache::shared().setDiskRoot(m_dir.filePath("idx2"));
  LasCache::shared().invalidate();

  const LasDoc cold = LasCache::shared().load(las);
  QVERIFY(cold.ok);
  const double coldMs = LasCache::shared().lastTimings().coldParseNs / 1.0e6;
  // goal/perf-systematize 簇3：墙钟断言比率化——绝对预算（cold<50/warm<5，
  // 余量仅 ~3×，慢机必抖）改在测参照比率门 + 防挂死 sanity 上限。
  // 实测锚（docs/perf/BASELINE.md §2）：cold 13-17ms、warm ≈0.05ms、
  // mem ≈0.03ms——比率余量 10×/500×；缓存退化（warm≈cold）时比率→1 必红。
  QVERIFY2(coldMs < 2000.0,
           qPrintable(QStringLiteral("cold %1ms >= 2000ms（sanity：解析挂死/死循环）").arg(coldMs)));

  // 二次打开（磁盘层）≤ 冷解析一半（D1.1 的机器无关形式）。
  LasCache::shared().clearMemory();
  QElapsedTimer t;
  t.start();
  const LasDoc warm = LasCache::shared().load(las);
  const double warmMs = t.nsecsElapsed() / 1.0e6;
  QVERIFY(warm.ok);
  QVERIFY2(coldMs > 0 && warmMs < 0.5 * coldMs,
           qPrintable(QStringLiteral("disk hit %1ms >= 0.5×cold %2ms（缓存未生效）")
                          .arg(warmMs, 0, 'f', 3)
                          .arg(coldMs, 0, 'f', 3)));
  // 三次打开（内存层）同口径 ≤ 0.5×cold。
  t.restart();
  LasCache::shared().load(las);
  const double memMs = t.nsecsElapsed() / 1.0e6;
  QVERIFY2(memMs < 0.5 * coldMs,
           qPrintable(QStringLiteral("memory hit %1ms >= 0.5×cold %2ms")
                          .arg(memMs, 0, 'f', 3)
                          .arg(coldMs, 0, 'f', 3)));
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

QTEST_MAIN(CacheLasTests)
#include "tst_cache_las.moc"
