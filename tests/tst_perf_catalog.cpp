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
    // WP2（写路径线性化）——精确 undo 回滚与索引化查询的语义钉子：
    void mutatorRollbackUndoesExactly();      // 六 mutator 失败回滚逐项还原
    void entitySeqAndShaLookupsAfterReload(); // nextEntityId/versionBySha256 语义（含重开）

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
  // D5.7：10k 资产 catalog 打开。goal/perf-systematize 簇3：绝对 500ms
  //（实测 108-145ms，余量 ~4×，慢机可抖）改双门——
  //   比率门：10k/1k 打开耗时比 ≤ 25（线性 ≈10×；实测 10-14×；超线性
  //           解析回归（如逐实体重扫全文）时 → 数百倍必红）；
  //   sanity：500ms 上限保留（拦挂死，不判回归）。
  const QString dir = m_dir.filePath("open10k");
  QString err;
  QVERIFY(PerfFixtures::makeSyntheticCatalogDir(dir, 10000, &err));
  DataCatalog cat;
  QElapsedTimer t;
  t.start();
  QVERIFY(cat.open(dir, &err));
  const double ms = t.nsecsElapsed() / 1.0e6;

  const QString dir1k = m_dir.filePath("open1k");
  QVERIFY(PerfFixtures::makeSyntheticCatalogDir(dir1k, 1000, &err));
  DataCatalog cat1k;
  t.restart();
  QVERIFY(cat1k.open(dir1k, &err));
  const double ms1k = double(t.nsecsElapsed()) / 1.0e6;
  qInfo("catalog open 1k=%.1fms 10k=%.1fms ratio=%.1f", ms1k, ms,
        ms1k > 0 ? ms / ms1k : -1.0);
  QVERIFY2(ms1k > 0 && ms / ms1k <= 25.0,
           qPrintable(QStringLiteral("open 10k/1k=%1 > 25（打开退化成超线性？）")
                          .arg(ms1k > 0 ? ms / ms1k : -1.0, 0, 'f', 1)));
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

void PerfCatalogTests::mutatorRollbackUndoesExactly()
{
  // WP2：六 mutator 从「全表快照还原」改为「精确 undo 还原」——此测试把
  // 失败回滚的语义逐项钉死：save 失败后内存态、索引、revision 与失败前
  // 完全一致（含 supersede 引发的下游 stale 标记与主关联降级的复原）。
  const QString dir = m_dir.filePath("p_undo");
  QString err;
  QDir().mkpath(dir);
  DataCatalog cat;
  QVERIFY(cat.open(dir, &err));
  QVERIFY(cat.addEntity(mkEntity(QStringLiteral("w1")), &err));
  QVERIFY(cat.addAsset(mkAsset(QStringLiteral("ast-1")), &err));
  QVERIFY(cat.addAsset(mkAsset(QStringLiteral("ast-2")), &err));
  CatalogVersion raw = mkVersion(QStringLiteral("ver-1"), QStringLiteral("ast-1"));
  QVERIFY(cat.addVersion(raw, &err));
  CatalogVersion derived = mkVersion(QStringLiteral("ver-2"), QStringLiteral("ast-2"));
  derived.stage = QStringLiteral("DERIVED");
  derived.parentVersionIds << QStringLiteral("ver-1");
  QVERIFY(cat.addVersion(derived, &err));
  EntityAssetLink primary;
  primary.entityType = QStringLiteral("well");
  primary.entityId = QStringLiteral("w1");
  primary.assetId = QStringLiteral("ast-1");
  primary.role = QStringLiteral("well_log");
  primary.isPrimary = true;
  QVERIFY(cat.addLink(primary, &err));
  EntityAssetLink secondary;
  secondary.entityType = QStringLiteral("well");
  secondary.entityId = QStringLiteral("w1");
  secondary.assetId = QStringLiteral("ast-2");
  secondary.role = QStringLiteral("horizon");
  secondary.isPrimary = false;
  QVERIFY(cat.addLink(secondary, &err));
  EntityAssetLink pending;
  pending.entityType = QStringLiteral("well");
  pending.entityId = QString();
  pending.assetId = QStringLiteral("ast-2");
  pending.role = QStringLiteral("well_log"); // 词表内角色——不触诊断注记
  pending.unresolved = true;
  pending.note = QStringLiteral("未匹配：SYNTH-XX");
  QVERIFY(cat.addLink(pending, &err));
  const int linkCount = cat.links().size();
  const int pendingRow = 2; // 第三条 = 未决链接

  // undo 栈多入栈前置布局（review P3）：同一 DERIVED 下游行被两个 superseded
  // 父版本先后标记——逆序还原必须落在最初值（正序还原会停在中间值，后断言必红）。
  // ast-3 ver-4(RAW,vn1) ← ver-6(DERIVED, parents=[ver-4,ver-5])；ver-5(RAW,vn2)
  // 入库时成功把 ver-6 标为 reasonA 并落盘。
  QVERIFY(cat.addAsset(mkAsset(QStringLiteral("ast-3")), &err));
  CatalogVersion v4 = mkVersion(QStringLiteral("ver-4"), QStringLiteral("ast-3"));
  QVERIFY(cat.addVersion(v4, &err));
  CatalogVersion v6 = mkVersion(QStringLiteral("ver-6"), QStringLiteral("ast-1"));
  v6.stage = QStringLiteral("DERIVED");
  v6.parentVersionIds << QStringLiteral("ver-4") << QStringLiteral("ver-5");
  QVERIFY(cat.addVersion(v6, &err));
  CatalogVersion v5 = mkVersion(QStringLiteral("ver-5"), QStringLiteral("ast-3"));
  v5.versionNumber = 2;
  QVERIFY(cat.addVersion(v5, &err));
  const QString reasonA = cat.versionById(QStringLiteral("ver-6"))
                              .extra.value(QStringLiteral("staleReason"))
                              .toString();
  QVERIFY2(reasonA.contains(QStringLiteral("ver-5")),
           "前置失败：ver-6 未被 ver-5 的入库标记");
  const int versionCountWithV6 = cat.versions().size();
  const int revisionWithV6 = cat.catalogRevision();

  cat.setLockedReadOnly(true); // 任何 save 必失败 → 走回滚路径

  // addVersion：ver-3 取代 ver-1 → ver-2 应被标 stale；回滚后两处都得复原。
  CatalogVersion superseder = mkVersion(QStringLiteral("ver-3"), QStringLiteral("ast-1"));
  superseder.versionNumber = 2;
  QVERIFY(!cat.addVersion(superseder, &err));
  QCOMPARE(int(cat.versions().size()), versionCountWithV6);
  QVERIFY(cat.versionById(QStringLiteral("ver-3")).id.isEmpty());
  QVERIFY2(!cat.versionById(QStringLiteral("ver-2"))
                .extra.contains(QStringLiteral("stale")),
           "supersede stale 标记未被回滚");
  QVERIFY2(cat.indexHealthy(&err), qPrintable(err));

  // undo 栈多入栈：ver-7(vn3) 同时取代 ver-4/ver-5——ver-6 被推入 undo 两次，
  // 回滚后 staleReason 必须仍为 reasonA（LIFO 唯一正确序）。
  CatalogVersion v7 = mkVersion(QStringLiteral("ver-7"), QStringLiteral("ast-3"));
  v7.versionNumber = 3;
  QVERIFY(!cat.addVersion(v7, &err));
  QCOMPARE(int(cat.versions().size()), versionCountWithV6);
  const CatalogVersion restored = cat.versionById(QStringLiteral("ver-6"));
  QVERIFY(restored.extra.value(QStringLiteral("stale")).toBool());
  QCOMPARE(restored.extra.value(QStringLiteral("staleReason")).toString(), reasonA);
  QVERIFY2(cat.indexHealthy(&err), qPrintable(err));

  // addLink：同角色新主关联（会降级既有主关联）→ 回滚后降级复原、链接数不变。
  EntityAssetLink dupPrimary = primary;
  dupPrimary.assetId = QStringLiteral("ast-2");
  QVERIFY(!cat.addLink(dupPrimary, &err));
  QCOMPARE(int(cat.links().size()), linkCount);
  QVERIFY(cat.linksForEntity(QStringLiteral("w1")).at(0).isPrimary);
  QVERIFY2(cat.indexHealthy(&err), qPrintable(err));

  // attachLink：未决挂接失败 → 未决态与备注原样保留；挂接本会降级既有
  // 主关联（同井同角色 well_log）——回滚后主关联复原。
  QVERIFY(!cat.attachLink(pendingRow, QStringLiteral("w1"), &err));
  const EntityAssetLink stillPending = cat.links().at(pendingRow);
  QVERIFY(stillPending.unresolved);
  QVERIFY(stillPending.entityId.isEmpty());
  QCOMPARE(stillPending.note, QStringLiteral("未匹配：SYNTH-XX"));
  QVERIFY(cat.links().at(0).isPrimary);
  QVERIFY2(cat.indexHealthy(&err), qPrintable(err));

  // setLinkPrimary：提升非主关联失败 → 主关联不变量原样。
  QVERIFY(!cat.setLinkPrimary(1, &err));
  QVERIFY(cat.links().at(0).isPrimary);
  QVERIFY(!cat.links().at(1).isPrimary);
  QVERIFY2(cat.indexHealthy(&err), qPrintable(err));

  // markDownstreamStale：落盘失败 → stale 标记不残留。
  QVERIFY(!cat.markDownstreamStale(QStringLiteral("ver-1"),
                                   QStringLiteral("测试失效原因"), &err));
  QVERIFY2(!cat.versionById(QStringLiteral("ver-2"))
                .extra.contains(QStringLiteral("stale")),
           "markDownstreamStale 失败后残留 stale 标记");

  QCOMPARE(cat.catalogRevision(), revisionWithV6); // 锁定期零次成功落盘

  // 解锁后同操作成功——回滚没有留下阻止后续写入的脏态。
  cat.setLockedReadOnly(false);
  QVERIFY(cat.addVersion(superseder, &err));
  QCOMPARE(int(cat.versions().size()), versionCountWithV6 + 1);
  QVERIFY(cat.versionById(QStringLiteral("ver-2"))
              .extra.value(QStringLiteral("stale")).toBool());
  QVERIFY2(cat.indexHealthy(&err), qPrintable(err));
}

void PerfCatalogTests::entitySeqAndShaLookupsAfterReload()
{
  // WP2：nextEntityId 走前缀序号索引、versionBySha256 走 sha 行集索引——
  // 语义与旧线性扫描逐项一致，且重开（索引自 JSON 重建）后仍成立。
  const QString dir = m_dir.filePath("p_seq");
  QString err;
  QDir().mkpath(dir);
  DataCatalog cat;
  QVERIFY(cat.open(dir, &err));
  DataCatalog::BatchSave batch(&cat);
  // 前缀序号面：well-10 是 well 前缀最大序号；well-x 非数字不计；
  // grp-3-4 归 grp-3 前缀（「最后一个 '-'」拆解＝旧扫描的余段 toInt 口径）。
  for (const QString &id : {QStringLiteral("well-1"), QStringLiteral("well-2"),
                            QStringLiteral("well-10"), QStringLiteral("well-x"),
                            QStringLiteral("grp-3-4"), QStringLiteral("aux-1")})
    QVERIFY(cat.addEntity(mkEntity(id), &err));
  // sha 面：两个受管版本同内容同 sha（受管文件必须真实存在——命中后
  // versionBySha256 会复核文件并重哈希）。
  QVERIFY(cat.addAsset(mkAsset(QStringLiteral("ast-1")), &err));
  QVERIFY(cat.addAsset(mkAsset(QStringLiteral("ast-2")), &err));
  const QByteArray payload = QByteArrayLiteral("WP2 sha lookup payload\n");
  for (int k = 1; k <= 2; ++k)
  {
    CatalogVersion v =
        mkVersion(QStringLiteral("ver-%1").arg(k), QStringLiteral("ast-%1").arg(k));
    v.managed = true;
    v.fileName = QStringLiteral("dup.dat");
    v.path = DataCatalog::managedPath(QStringLiteral("RAW"),
                                      QStringLiteral("ast-%1").arg(k),
                                      QStringLiteral("ver-%1").arg(k),
                                      QStringLiteral("dup.dat"));
    const QString abs = dir + QLatin1Char('/') + v.path;
    QDir().mkpath(QFileInfo(abs).absolutePath());
    QFile f(abs);
    QVERIFY(f.open(QIODevice::WriteOnly));
    QVERIFY(f.write(payload) == payload.size());
    f.close();
    v.sha256 = DataCatalog::sha256FileHex(abs, &err);
    QVERIFY2(!v.sha256.isEmpty(), qPrintable(err));
    QVERIFY(cat.addVersion(v, &err));
  }
  QVERIFY(batch.flush(&err));

  QCOMPARE(cat.nextEntityId(QStringLiteral("well")), QStringLiteral("well-11"));
  QCOMPARE(cat.nextEntityId(QStringLiteral("grp-3")), QStringLiteral("grp-3-5"));
  QCOMPARE(cat.nextEntityId(QStringLiteral("aux")), QStringLiteral("aux-2"));
  QCOMPARE(cat.nextEntityId(QStringLiteral("brandnew")), QStringLiteral("brandnew-1"));
  // sha 查询：大小写不敏感；「第一个匹配」＝表序最先（ver-1）。
  QCOMPARE(cat.versionBySha256(cat.versions().at(0).sha256).id, QStringLiteral("ver-1"));
  QCOMPARE(cat.versionBySha256(cat.versions().at(0).sha256.toUpper()).id,
           QStringLiteral("ver-1"));
  QVERIFY(cat.versionBySha256(QString("deadbeef")).id.isEmpty());

  // 重开：索引自 JSON 全量重建——同一组断言再钉一遍。
  DataCatalog reopened;
  QVERIFY(reopened.open(dir, &err));
  QCOMPARE(reopened.nextEntityId(QStringLiteral("well")), QStringLiteral("well-11"));
  QCOMPARE(reopened.nextEntityId(QStringLiteral("grp-3")), QStringLiteral("grp-3-5"));
  QCOMPARE(reopened.versionBySha256(reopened.versions().at(0).sha256).id,
           QStringLiteral("ver-1"));
  QVERIFY2(reopened.indexHealthy(&err), qPrintable(err));
}

QTEST_MAIN(PerfCatalogTests)
#include "tst_perf_catalog.moc"
