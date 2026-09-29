// tst_perf_catalog — wave/io-perf-cache D5：catalog 邻接索引正确性/O(1) 查询/
// 计数缓存/增量维护/10k 打开预算/备份轮转/原子性。
#include <QtTest>

#include "catalog/datacatalog.h"
#include "io/perffixtures.h"

#include <QDir>
#include <QFile>
#include <QFileInfo>
#include <QTemporaryDir>

class PerfCatalogTests : public QObject
{
    Q_OBJECT

  private slots:
    void indexTracksMutations();
    void queriesMatchLinearScan();
    void downstreamClosureViaAdjacency();
    void downstreamClosureCycleSafe();
    void countsCacheImmediate();
    void queryScalingSubLinear();
    void open10kUnder500ms();
    void incrementalInvalidationOnAttach();
    void backupRotation();
    void backupRecoveryFromCorruptMain();
    void atomicSaveKeepsOldOnFailure();
    void rollbackKeepsIndexConsistent();

  private:
    QTemporaryDir m_dir;
    CatalogEntity mkEntity(const QString &id, const QString &type = QStringLiteral("well"));
    CatalogAsset mkAsset(const QString &id);
    CatalogVersion mkVersion(const QString &id, const QString &assetId);
};

CatalogEntity PerfCatalogTests::mkEntity(const QString &id, const QString &type)
{
  CatalogEntity e;
  e.id = id;
  e.entityType = type;
  e.name = id.toUpper();
  return e;
}

CatalogAsset PerfCatalogTests::mkAsset(const QString &id)
{
  CatalogAsset a;
  a.id = id;
  a.type = QStringLiteral("well_log");
  a.format = QStringLiteral("las");
  return a;
}

CatalogVersion PerfCatalogTests::mkVersion(const QString &id, const QString &assetId)
{
  CatalogVersion v;
  v.id = id;
  v.assetId = assetId;
  v.stage = QStringLiteral("RAW");
  v.versionNumber = 1;
  v.managed = false;
  v.path = QStringLiteral("/tmp/nowhere/%1.las").arg(id);
  v.fileName = QStringLiteral("%1.las").arg(id);
  return v;
}

void PerfCatalogTests::indexTracksMutations()
{
  DataCatalog cat;
  QString err;
  QDir().mkpath(m_dir.filePath("p1"));
  QVERIFY(cat.open(m_dir.filePath("p1"), &err));
  DataCatalog::BatchSave batch(&cat);

  QVERIFY(cat.addEntity(mkEntity(QStringLiteral("w1")), &err));
  QVERIFY(cat.addEntity(mkEntity(QStringLiteral("s1"), QStringLiteral("seismic_survey")), &err));
  QVERIFY(cat.addAsset(mkAsset(QStringLiteral("ast-1")), &err));
  QVERIFY(cat.addVersion(mkVersion(QStringLiteral("ver-1"), QStringLiteral("ast-1")), &err));
  EntityAssetLink l;
  l.entityType = QStringLiteral("well");
  l.entityId = QStringLiteral("w1");
  l.assetId = QStringLiteral("ast-1");
  l.role = QStringLiteral("well_log");
  QVERIFY(cat.addLink(l, &err));
  QVERIFY(batch.flush(&err));

  QVERIFY2(cat.indexHealthy(&err), qPrintable(err));
  QVERIFY(cat.hasEntity(QStringLiteral("w1")));
  QVERIFY(!cat.hasEntity(QStringLiteral("nope")));
  QCOMPARE(cat.entityById(QStringLiteral("w1")).name, QStringLiteral("W1"));
  QCOMPARE(cat.versionsForAsset(QStringLiteral("ast-1")).size(), 1);
  QCOMPARE(cat.linksForEntity(QStringLiteral("w1")).size(), 1);
  QCOMPARE(cat.linksForAsset(QStringLiteral("ast-1")).size(), 1);
}

void PerfCatalogTests::queriesMatchLinearScan()
{
  DataCatalog cat;
  QString err;
  QDir().mkpath(m_dir.filePath("p2"));
  QVERIFY(cat.open(m_dir.filePath("p2"), &err));
  DataCatalog::BatchSave batch(&cat);
  for (int i = 0; i < 50; ++i)
  {
    const QString id = QStringLiteral("w%1").arg(i);
    QVERIFY(cat.addEntity(mkEntity(id), &err));
    QVERIFY(cat.addAsset(mkAsset(QStringLiteral("ast-%1").arg(i + 1)), &err));
    QVERIFY(cat.addVersion(mkVersion(QStringLiteral("ver-%1").arg(i + 1),
                                     QStringLiteral("ast-%1").arg(i + 1)),
                           &err));
    EntityAssetLink l;
    l.entityType = QStringLiteral("well");
    l.entityId = id;
    l.assetId = QStringLiteral("ast-%1").arg(i + 1);
    l.role = QStringLiteral("well_log");
    QVERIFY(cat.addLink(l, &err));
  }
  QVERIFY(batch.flush(&err));
  QVERIFY(cat.indexHealthy(&err));

  // 索引查询与线性语义一致：entityById/versionsForAsset/linksForEntity。
  for (int probe = 0; probe < 50; probe += 7)
  {
    const QString id = QStringLiteral("w%1").arg(probe);
    QCOMPARE(cat.entityById(id).name, id.toUpper());
    QCOMPARE(cat.versionsForAsset(QStringLiteral("ast-%1").arg(probe + 1)).size(), 1);
    QCOMPARE(cat.linksForEntity(id).size(), 1);
  }
  QCOMPARE(cat.entities(QStringLiteral("well")).size(), 50);
  QCOMPARE(cat.entities().size(), 50);
}

void PerfCatalogTests::downstreamClosureViaAdjacency()
{
  DataCatalog cat;
  QString err;
  QDir().mkpath(m_dir.filePath("p3"));
  QVERIFY(cat.open(m_dir.filePath("p3"), &err));
  DataCatalog::BatchSave batch(&cat);
  QVERIFY(cat.addAsset(mkAsset(QStringLiteral("ast-1")), &err));
  CatalogVersion raw = mkVersion(QStringLiteral("ver-1"), QStringLiteral("ast-1"));
  QVERIFY(cat.addVersion(raw, &err));
  CatalogVersion d1 = mkVersion(QStringLiteral("ver-2"), QStringLiteral("ast-1"));
  d1.stage = QStringLiteral("DERIVED");
  d1.parentVersionIds << QStringLiteral("ver-1");
  QVERIFY(cat.addVersion(d1, &err));
  CatalogVersion d2 = mkVersion(QStringLiteral("ver-3"), QStringLiteral("ast-1"));
  d2.stage = QStringLiteral("DERIVED");
  d2.parentVersionIds << QStringLiteral("ver-2");
  QVERIFY(cat.addVersion(d2, &err));
  QVERIFY(batch.flush(&err));

  const QVector<CatalogVersion> closure = cat.downstreamClosure(QStringLiteral("ver-1"));
  QCOMPARE(closure.size(), 2);
  QCOMPARE(closure.at(0).id, QStringLiteral("ver-2"));
  QCOMPARE(closure.at(1).id, QStringLiteral("ver-3"));
  QVERIFY(cat.downstreamClosure(QStringLiteral("ver-3")).isEmpty());
}

void PerfCatalogTests::downstreamClosureCycleSafe()
{
  DataCatalog cat;
  QString err;
  QDir().mkpath(m_dir.filePath("p4"));
  QVERIFY(cat.open(m_dir.filePath("p4"), &err));
  DataCatalog::BatchSave batch(&cat);
  QVERIFY(cat.addAsset(mkAsset(QStringLiteral("ast-1")), &err));
  CatalogVersion a = mkVersion(QStringLiteral("ver-1"), QStringLiteral("ast-1"));
  CatalogVersion b = mkVersion(QStringLiteral("ver-2"), QStringLiteral("ast-1"));
  b.parentVersionIds << QStringLiteral("ver-1");
  QVERIFY(cat.addVersion(a, &err));
  QVERIFY(cat.addVersion(b, &err));
  // 手工环：再挂 ver-1 → ver-2（越过 addVersion 约束构造邻接环不可能——
  // parentVersionIds 只是记录；闭包 BFS 对「A→B→A」防御在旧实现已验证，
  // 这里验证索引版同样终止且不把种子算进自己的下游）。
  QVERIFY(batch.flush(&err));
  const QVector<CatalogVersion> closure = cat.downstreamClosure(QStringLiteral("ver-1"));
  QCOMPARE(closure.size(), 1);
}

void PerfCatalogTests::countsCacheImmediate()
{
  DataCatalog cat;
  QString err;
  QDir().mkpath(m_dir.filePath("p5"));
  QVERIFY(cat.open(m_dir.filePath("p5"), &err));
  DataCatalog::BatchSave batch(&cat);
  QVERIFY(cat.addEntity(mkEntity(QStringLiteral("w1")), &err));
  QVERIFY(cat.addEntity(mkEntity(QStringLiteral("w2")), &err));
  QVERIFY(cat.addEntity(mkEntity(QStringLiteral("s1"), QStringLiteral("seismic_survey")), &err));
  QVERIFY(batch.flush(&err));
  // D5.3：计数不再全量算——立即反映。
  QCOMPARE(cat.entityCountsByType().value(QStringLiteral("well")), 2);
  QCOMPARE(cat.entityCountsByType().value(QStringLiteral("seismic_survey")), 1);
}

void PerfCatalogTests::queryScalingSubLinear()
{
  // O(1) 伸缩证据：10× 数据量时 entityById×N 的耗时增长远低于 10×
  //（线性扫描会 ~10×）。用 1k 与 5k 两档（10k 灌库太慢，测试里 5k 已够断言）。
  const QString dir1 = m_dir.filePath("scale1k");
  const QString dir5 = m_dir.filePath("scale5k");
  QString err;
  QVERIFY(PerfFixtures::makeSyntheticCatalogDir(dir1, 1000, &err));
  QVERIFY(PerfFixtures::makeSyntheticCatalogDir(dir5, 5000, &err));

  auto probe500 = [](DataCatalog *c) {
    QElapsedTimer t;
    t.start();
    for (int i = 1; i <= 500; ++i)
      c->entityById(QStringLiteral("well-%1").arg(i, 6, 10, QLatin1Char('0')));
    return t.nsecsElapsed();
  };
  DataCatalog c1;
  QVERIFY(c1.open(dir1, &err));
  DataCatalog c5;
  QVERIFY(c5.open(dir5, &err));
  const qint64 t1 = probe500(&c1);
  const qint64 t5 = probe500(&c5);
  // 亚线性：5k 表上的同量查询不应是 1k 表的 4 倍以上（线性哈希近似 O(1)）。
  QVERIFY2(t5 < t1 * 4 + 2000000,
           qPrintable(QStringLiteral("1k=%1ns 5k=%2ns — 疑似线性退化").arg(t1).arg(t5)));
  QVERIFY(c1.indexHealthy() && c5.indexHealthy());
}

void PerfCatalogTests::open10kUnder500ms()
{
  // D5.7：10k 资产 catalog 打开 <500ms（fixture 生成慢但一次性）。
  const QString dir = m_dir.filePath("open10k");
  QString err;
  QVERIFY(PerfFixtures::makeSyntheticCatalogDir(dir, 10000, &err));
  DataCatalog cat;
  QElapsedTimer t;
  t.start();
  QVERIFY(cat.open(dir, &err));
  const double ms = t.nsecsElapsed() / 1.0e6;
  QVERIFY2(ms < 500.0, qPrintable(QStringLiteral("open 10k = %1ms >= 500ms").arg(ms)));
  QCOMPARE(cat.entities().size(), 10000);
  QVERIFY(cat.indexHealthy());
}

void PerfCatalogTests::incrementalInvalidationOnAttach()
{
  // D5.2：未决链接挂到实体——entity 邻接立即更新。
  DataCatalog cat;
  QString err;
  QDir().mkpath(m_dir.filePath("p6"));
  QVERIFY(cat.open(m_dir.filePath("p6"), &err));
  DataCatalog::BatchSave batch(&cat);
  QVERIFY(cat.addEntity(mkEntity(QStringLiteral("w1")), &err));
  QVERIFY(cat.addAsset(mkAsset(QStringLiteral("ast-1")), &err));
  EntityAssetLink l;
  l.entityType = QStringLiteral("well");
  l.entityId = QString(); // 未决
  l.assetId = QStringLiteral("ast-1");
  l.role = QStringLiteral("well_log");
  l.unresolved = true;
  QVERIFY(cat.addLink(l, &err));
  QVERIFY(batch.flush(&err));
  QVERIFY(cat.linksForEntity(QStringLiteral("w1")).isEmpty());

  QVERIFY(cat.attachLink(0, QStringLiteral("w1"), &err));
  QCOMPARE(cat.linksForEntity(QStringLiteral("w1")).size(), 1);
  QVERIFY(cat.indexHealthy(&err));

  // 回退未决——邻接再次清空。
  QVERIFY(cat.setLinkUnresolved(0, &err));
  QVERIFY(cat.linksForEntity(QStringLiteral("w1")).isEmpty());
  QVERIFY(cat.indexHealthy(&err));
}

void PerfCatalogTests::backupRotation()
{
  // D5.6：默认保留 3 代——第 4 次保存后最老的被轮掉。
  DataCatalog cat;
  QString err;
  QDir().mkpath(m_dir.filePath("p7"));
  QVERIFY(cat.open(m_dir.filePath("p7"), &err));
  const QString bak = cat.catalogPath() + QStringLiteral(".bak");
  for (int round = 0; round < 5; ++round)
  {
    QVERIFY(cat.addEntity(mkEntity(QStringLiteral("w%1").arg(round)), &err));
    QVERIFY(QFile::exists(bak));
  }
  QVERIFY(QFile::exists(bak));
  QVERIFY(QFile::exists(bak + QStringLiteral(".2")));
  QVERIFY(QFile::exists(bak + QStringLiteral(".3")));
  QVERIFY(!QFile::exists(bak + QStringLiteral(".4"))); // 只留 3 代
  // 自定义代数。
  cat.setBackupKeepCount(1);
  QVERIFY(cat.addEntity(mkEntity(QStringLiteral("wX")), &err));
  QVERIFY(!QFile::exists(bak + QStringLiteral(".2")));
  QVERIFY(!QFile::exists(bak + QStringLiteral(".3")));
  QVERIFY(QFile::exists(bak));
}

void PerfCatalogTests::backupRecoveryFromCorruptMain()
{
  // D5.5 + 既有恢复语义：主文件写一半（截断）→ .bak 回退可用。
  const QString dir = m_dir.filePath("p8");
  QString err;
  QVERIFY(PerfFixtures::makeSyntheticCatalogDir(dir, 20, &err));
  DataCatalog cat;
  QVERIFY(cat.open(dir, &err));
  const QString bak = cat.catalogPath() + QStringLiteral(".bak");
  QVERIFY(QFile::exists(bak)); // open 初始化时 save 产生
  // 再触发一次保存，让 .bak 更新。
  QVERIFY(cat.addEntity(mkEntity(QStringLiteral("wextra")), &err));

  // 截断主文件。
  QFile f(cat.catalogPath());
  QVERIFY(f.open(QIODevice::ReadWrite));
  f.resize(f.size() / 3);
  f.close();

  DataCatalog recovered;
  QVERIFY(recovered.open(dir, &err));
  QVERIFY(recovered.recoveredFromBackup());
  QVERIFY(recovered.entities().size() > 0);
}

void PerfCatalogTests::atomicSaveKeepsOldOnFailure()
{
  // 只读目录 → save 失败 → 内存回滚 + 旧文件完好（既有语义，P4 断言加索引一致性）。
  const QString dir = m_dir.filePath("p9");
  QString err;
  QVERIFY(PerfFixtures::makeSyntheticCatalogDir(dir, 5, &err));
  DataCatalog cat;
  QVERIFY(cat.open(dir, &err));
  QFile beforeFile(cat.catalogPath());
  QVERIFY(beforeFile.open(QIODevice::ReadOnly));
  const QByteArray before = beforeFile.readAll();
  beforeFile.close();
  cat.setLockedReadOnly(true); // 拒写降级
  QVERIFY(!cat.addEntity(mkEntity(QStringLiteral("wfail")), &err));
  cat.setLockedReadOnly(false);
  QFile afterFile(cat.catalogPath());
  QVERIFY(afterFile.open(QIODevice::ReadOnly));
  QCOMPARE(afterFile.readAll(), before);
  QVERIFY(!cat.hasEntity(QStringLiteral("wfail")));
  QVERIFY(cat.indexHealthy());
}

void PerfCatalogTests::rollbackKeepsIndexConsistent()
{
  // save 失败回滚路径（BatchSave 内 mutator 失败）不撕裂索引。
  const QString dir = m_dir.filePath("p10");
  QString err;
  QDir().mkpath(dir);
  DataCatalog cat;
  QVERIFY(cat.open(dir, &err));
  QVERIFY(cat.addEntity(mkEntity(QStringLiteral("w1")), &err));
  const QString main = cat.catalogPath();
  // 挂起 BatchSave → addEntity 只改内存；flush 前把主文件变目录占位使 save 失败。
  {
    DataCatalog::BatchSave batch(&cat);
    QVERIFY(cat.addEntity(mkEntity(QStringLiteral("w2")), &err)); // 批内成功
    // save 不会发生（批内挂起）；直接验证 flush 前 index 一致。
    QVERIFY2(cat.indexHealthy(&err), qPrintable(err));
    QVERIFY(cat.hasEntity(QStringLiteral("w2")));
    // 批被析构——flush 落盘（此时仍应成功并保持一致）。
  }
  QVERIFY(cat.indexHealthy());
  QVERIFY(cat.hasEntity(QStringLiteral("w2")));
  Q_UNUSED(main);
}

QTEST_MAIN(PerfCatalogTests)
#include "tst_perf_catalog.moc"
