#include <QtTest>
#include <QElapsedTimer>
#include <QSignalSpy>
#include <QTemporaryDir>
#include <QJsonDocument>
#include <QJsonObject>
#include <QFile>
#include <QDir>

#include "../src/catalog/datacatalog.h"

#include <algorithm>
#include <functional>
#include <memory>
#include <thread>

#ifdef Q_OS_WIN
#include <windows.h>
#endif

// plan §3 数据模型：实体—关联—资产—版本，catalog.json 是唯一主存储。
// 覆盖：JSON round-trip、井名规范化身份解析、双候选不合并（unresolved 语义）、
// 版本不可变递增、revision 单调。
class TestCatalog : public QObject
{
  Q_OBJECT

private slots:
  void roundTripsThroughJson();
  void normalizesWellName();
  void resolvesWellByName();
  void ambiguousNameYieldsBothCandidates();
  void unresolvedLinkRoundTripsWithEmptyEntityId();
  void resolvedLinkStillNeedsEntityId();
  void currentVersionPicksHighestNumber();
  void managedPathLayout();
  void unsafePathSegmentsRejected();
  void versionBySha256FindsStored();
  void managedPathRejectsSymlinksAndTraversal();
  void managedCatalogLoadRejectsEscape();
  void addLinkDemotesPreviousPrimaryForSameRole();
  void attachLinkResolvesUnresolvedAndDemotes();
  // ---- pass 2：T17/T20/T33 ----
  void versionSeqRestoredAfterReload();           // T17
  void duplicateVersionIdRejected();              // T17
  void refusesUnsupportedSchemaVersion();         // T20b
  void rotatesBakAndVerifiesWrite();              // T20c
  void refusesWritesAfterFailedOpen();            // T20a
  void unresolvedLinksIsTheExplicitAccessor();    // T33/row35
  void batchSaveCoalescesWrites();                // T33/row37
  void unsafeManagedPathSkippedOnLoad();          // T33
  // ---- D12（pass-2）：uwi/aliases 遗留字段剥离 ----
  void legacyCatalogFieldsIgnoredAndNotRewritten();
  void newEntitySerializationOmitsLegacyKeys();
  // ---- wave/data-integrity：role 词表强制（诚实降级而非硬拦） ----
  void unknownRoleLinkStillWrittenButDiagnosed();
  void unknownRoleDiagnosticJoinsExistingNote();
  void roleEntityTypeMismatchDiagnosedNotBlocked();
  void attachLinkReappliesRoleDiagnostics();
  void knownRoleLinksStayOutOfInvalidSet();
  // ---- wave/data-foundation：T5 .bak 腐败恢复 / T6 原子写 / T4 锁降级 /
  //      T3 千实体量化预算 ----
  void backupRecoveryChainDrill();               // T5+T7：连续两轮损坏→恢复→续存
  void recoveryDoesNotRotateCorruptPrimaryIntoBak(); // #79：恢复后首次 save 不污染 .bak
  void recoveryFallsBackToOlderGeneration();         // #79：.bak 坏时回退 .bak.2…
  void backupRecoveryRefusedOnSchemaMismatch();  // T5：未来版本不走 .bak 回退
  void bothCorruptRefusesWrites();               // T5：主/bak 都坏 → 拒写态
  void wellsGeoJsonKeepsOldFileOnFailedWrite();  // T6：写失败不截断旧文件
  void lockedReadOnlyRefusesMutationsButReadsFine(); // T4
  void thousandEntityQueryBudget();              // T3：catalog.sqlite 触发条件量化
  // 审计 02 M-8：staging 副本 / journal / 事务重放 / 线程亲和。
  void stagingCopyJournalsWithoutTouchingLive();
  void applyJournalIsAtomicOnFailure();
  void liveWritesRefusedOffOwnerThread();
};

void TestCatalog::roundTripsThroughJson()
{
  QTemporaryDir dir;
  QVERIFY(dir.isValid());

  {
    DataCatalog cat;
    QVERIFY(cat.open(dir.path()));
    CatalogEntity well;
    well.id = QStringLiteral("well-A1");
    well.entityType = QStringLiteral("well");
    well.name = QStringLiteral("A1");
    well.surfaceX = 5288.67;
    well.surfaceY = 8219.94;
    well.hasSurface = true;
    well.kb = 0.0;
    well.td = 2160.0;
    well.coordinateStatus = QStringLiteral("untransformed");
    QVERIFY(cat.addEntity(well));
    QVERIFY(cat.hasEntity(QStringLiteral("well-A1")));

    CatalogAsset asset;
    asset.id = QStringLiteral("ast-1");
    asset.type = QStringLiteral("well_log");
    asset.format = QStringLiteral("las");
    asset.displayName = QStringLiteral("A1.Las");
    QVERIFY(cat.addAsset(asset));

    CatalogVersion v;
    v.id = QStringLiteral("ver-1");
    v.assetId = asset.id;
    v.stage = QStringLiteral("RAW");
    v.versionNumber = 1;
    v.managed = true;
    v.path = QStringLiteral("raw/ast-1/ver-1/A1.Las");
    v.sourceUri = QStringLiteral("/somewhere/A1.Las");
    v.sha256 = QStringLiteral("abc123");
    v.fileName = QStringLiteral("A1.Las");
    QVERIFY(cat.addVersion(v));

    EntityAssetLink link;
    link.entityType = QStringLiteral("well");
    link.entityId = well.id;
    link.assetId = asset.id;
    link.role = QStringLiteral("well_log");
    link.isPrimary = true;
    link.unresolved = false;
    QVERIFY(cat.addLink(link));
  }

  DataCatalog reloaded;
  QVERIFY(reloaded.open(dir.path()));
  QCOMPARE(reloaded.entities().size(), 1);
  const CatalogEntity well = reloaded.entityById(QStringLiteral("well-A1"));
  QCOMPARE(well.name, QStringLiteral("A1"));
  QCOMPARE(well.coordinateStatus, QStringLiteral("untransformed"));
  QVERIFY(well.hasSurface);
  QCOMPARE(well.surfaceX, 5288.67);
  QCOMPARE(well.surfaceY, 8219.94);
  QCOMPARE(reloaded.assets().size(), 1);
  QCOMPARE(reloaded.assetById(QStringLiteral("ast-1")).type, QStringLiteral("well_log"));
  const CatalogVersion v = reloaded.currentVersion(QStringLiteral("ast-1"));
  QCOMPARE(v.stage, QStringLiteral("RAW"));
  QCOMPARE(v.sha256, QStringLiteral("abc123"));
  QCOMPARE(v.path, QStringLiteral("raw/ast-1/ver-1/A1.Las"));
  const auto links = reloaded.linksForEntity(QStringLiteral("well-A1"));
  QCOMPARE(links.size(), 1);
  QCOMPARE(links.front().role, QStringLiteral("well_log"));
  QVERIFY(!links.front().unresolved);
  QVERIFY(links.front().note.isEmpty());
  QVERIFY(reloaded.catalogRevision() >= 4);
}

// §3 修订：未决链接实体 id 留空 + note 字段，catalog.json 能往返；
// 老 catalog 没有 note 键时读为为空。
void TestCatalog::unresolvedLinkRoundTripsWithEmptyEntityId()
{
  QTemporaryDir dir;
  QVERIFY(dir.isValid());
  {
    DataCatalog cat;
    QVERIFY(cat.open(dir.path()));
    CatalogAsset asset;
    asset.id = QStringLiteral("ast-1");
    asset.type = QStringLiteral("well_log");
    asset.format = QStringLiteral("las");
    asset.displayName = QStringLiteral("dup.Las");
    QVERIFY(cat.addAsset(asset));

    EntityAssetLink link;
    link.entityType = QStringLiteral("well");
    // entityId 留空：addLink 只在 unresolved=true 时放行
    link.assetId = asset.id;
    link.role = QStringLiteral("well_log");
    link.unresolved = true;
    link.note = QStringLiteral("候选: x1(well-X1), x2(well-X2)");
    QVERIFY(cat.addLink(link));
  }

  DataCatalog reloaded;
  QVERIFY(reloaded.open(dir.path()));
  const auto links = reloaded.linksForAsset(QStringLiteral("ast-1"));
  QCOMPARE(links.size(), 1);
  QVERIFY(links.front().unresolved);
  QVERIFY(links.front().entityId.isEmpty());
  QCOMPARE(links.front().role, QStringLiteral("well_log"));
  QCOMPARE(links.front().note, QStringLiteral("候选: x1(well-X1), x2(well-X2)"));

  // 旧版 catalog.json 不含 note 键 → 读为为空（前向兼容）。
  QTemporaryDir dir2;
  QVERIFY(dir2.isValid());
  QVERIFY(QDir().mkpath(dir2.filePath(QStringLiteral("artifacts/metadata"))));
  QFile f(dir2.filePath(QStringLiteral("artifacts/metadata/catalog.json")));
  QVERIFY(f.open(QIODevice::WriteOnly));
  f.write(QByteArrayLiteral(
      "{\"schema_version\":1,\"catalog_revision\":1,"
      "\"entity_asset_links\":[{\"entity_type\":\"well\",\"asset_id\":\"ast-1\","
      "\"role\":\"well_log\",\"is_primary\":true,\"unresolved\":true}]}"));
  f.close();
  DataCatalog legacy;
  QVERIFY(legacy.open(dir2.path()));
  const auto legacyLinks = legacy.linksForAsset(QStringLiteral("ast-1"));
  QCOMPARE(legacyLinks.size(), 1);
  QVERIFY(legacyLinks.front().unresolved);
  QVERIFY(legacyLinks.front().entityId.isEmpty());
  QVERIFY(legacyLinks.front().note.isEmpty());
}

// 已决链接仍拒绝空实体 id；未决链接也必须有资产 id。
void TestCatalog::resolvedLinkStillNeedsEntityId()
{
  QTemporaryDir dir;
  QVERIFY(dir.isValid());
  DataCatalog cat;
  QVERIFY(cat.open(dir.path()));
  CatalogAsset asset;
  asset.id = QStringLiteral("ast-1");
  asset.type = QStringLiteral("well_log");
  QVERIFY(cat.addAsset(asset));

  QString err;
  EntityAssetLink link;
  link.entityType = QStringLiteral("well");
  link.assetId = asset.id;
  link.role = QStringLiteral("well_log");
  QVERIFY(!cat.addLink(link, &err));
  QVERIFY(!err.isEmpty());
  QVERIFY(cat.links().isEmpty());

  link.unresolved = true;
  link.assetId.clear();
  QVERIFY(!cat.addLink(link, &err));
  QVERIFY(!err.isEmpty());
  QVERIFY(cat.links().isEmpty());
}

void TestCatalog::normalizesWellName()
{
  // 绑定规则：比较前去首尾空白、连字符、空格，忽略大小写。
  QCOMPARE(DataCatalog::normalizeWellName(QStringLiteral("  A1 ")),
           DataCatalog::normalizeWellName(QStringLiteral("a1")));
  QCOMPARE(DataCatalog::normalizeWellName(QStringLiteral("A-1")),
           DataCatalog::normalizeWellName(QStringLiteral("a1")));
  QCOMPARE(DataCatalog::normalizeWellName(QStringLiteral("A 1")),
           DataCatalog::normalizeWellName(QStringLiteral("A1")));
  QCOMPARE(DataCatalog::normalizeWellName(QStringLiteral("HZ28-6-1")),
           QStringLiteral("hz2861"));
}

void TestCatalog::resolvesWellByName()
{
  // D12 后身份只走 name（规范化比较）；uwi/aliases 不再参与解析。
  QTemporaryDir dir;
  QVERIFY(dir.isValid());
  DataCatalog cat;
  QVERIFY(cat.open(dir.path()));

  CatalogEntity a1;
  a1.id = QStringLiteral("well-A1");
  a1.entityType = QStringLiteral("well");
  a1.name = QStringLiteral("A1");
  QVERIFY(cat.addEntity(a1));

  CatalogEntity a2;
  a2.id = QStringLiteral("well-A2");
  a2.entityType = QStringLiteral("well");
  a2.name = QStringLiteral("A2");
  QVERIFY(cat.addEntity(a2));

  QCOMPARE(cat.wellsMatchingName(QStringLiteral("a1")), QStringList{QStringLiteral("well-A1")});
  QCOMPARE(cat.wellsMatchingName(QStringLiteral(" a-2 ")),
           QStringList{QStringLiteral("well-A2")});
  QVERIFY(cat.wellsMatchingName(QStringLiteral("A3")).isEmpty());
}

void TestCatalog::ambiguousNameYieldsBothCandidates()
{
  QTemporaryDir dir;
  QVERIFY(dir.isValid());
  DataCatalog cat;
  QVERIFY(cat.open(dir.path()));

  // 同名两井（D12 前靠共享别名构造的歧义，现在直接同名）。
  CatalogEntity w1;
  w1.id = QStringLiteral("well-W1");
  w1.entityType = QStringLiteral("well");
  w1.name = QStringLiteral("dup");
  QVERIFY(cat.addEntity(w1));
  CatalogEntity w2;
  w2.id = QStringLiteral("well-W2");
  w2.entityType = QStringLiteral("well");
  w2.name = QStringLiteral("dup");
  QVERIFY(cat.addEntity(w2));

  // 双候选必须原样返回两个 id——调用方据此写 unresolved 链接，不并井。
  const QStringList cands = cat.wellsMatchingName(QStringLiteral("DUP"));
  QCOMPARE(cands.size(), 2);
  QVERIFY(cands.contains(QStringLiteral("well-W1")));
  QVERIFY(cands.contains(QStringLiteral("well-W2")));
}

void TestCatalog::currentVersionPicksHighestNumber()
{
  QTemporaryDir dir;
  QVERIFY(dir.isValid());
  DataCatalog cat;
  QVERIFY(cat.open(dir.path()));
  CatalogAsset a;
  a.id = QStringLiteral("ast-9");
  a.type = QStringLiteral("horizon");
  QVERIFY(cat.addAsset(a));
  for (int n = 1; n <= 3; ++n)
  {
    CatalogVersion v;
    v.id = QStringLiteral("ver-%1").arg(n);
    v.assetId = a.id;
    v.stage = (n == 3 ? QStringLiteral("DERIVED") : QStringLiteral("RAW"));
    v.versionNumber = n;
    v.fileName = QStringLiteral("f.dat");
    QVERIFY(cat.addVersion(v));
  }
  const CatalogVersion cur = cat.currentVersion(a.id);
  QCOMPARE(cur.id, QStringLiteral("ver-3"));
  QCOMPARE(cur.stage, QStringLiteral("DERIVED"));
}

void TestCatalog::managedPathLayout()
{
  // 受管路径 {stage}/{asset_id}/{version_id}/{filename}（plan §3）。
  QTemporaryDir dir;
  QVERIFY(dir.isValid());
  DataCatalog cat;
  QVERIFY(cat.open(dir.path()));
  const QString p = cat.managedPath(QStringLiteral("raw"), QStringLiteral("ast-7"),
                                    QStringLiteral("ver-2"), QStringLiteral("D61.dat"));
  QCOMPARE(p, QStringLiteral("raw/ast-7/ver-2/D61.dat"));
  // catalog.json lives under artifacts/metadata/ next to the project dir.
  QVERIFY(cat.catalogPath().endsWith(QStringLiteral("artifacts/metadata/catalog.json")));
}

// §3 路径卫生：受管路径段拒绝空段、"."、含 ".."、斜杠/反斜杠、换行、NUL 及
// 其他控制字符；managedPath 拒绝产出坏路径，addVersion 拒绝落坏段。
void TestCatalog::unsafePathSegmentsRejected()
{
  QVERIFY(DataCatalog::isSafePathSegment(QStringLiteral("D61.dat")));
  QVERIFY(DataCatalog::isSafePathSegment(QStringLiteral("A1.Las")));
  QVERIFY(DataCatalog::isSafePathSegment(QString::fromUtf8("层位.dat")));
  QVERIFY(DataCatalog::isSafePathSegment(QStringLiteral("a b.dat"))); // 空格允许

  QVERIFY(!DataCatalog::isSafePathSegment(QString()));
  QVERIFY(!DataCatalog::isSafePathSegment(QStringLiteral(".")));
  QVERIFY(!DataCatalog::isSafePathSegment(QStringLiteral("..")));
  QVERIFY(!DataCatalog::isSafePathSegment(QStringLiteral("..x")));
  QVERIFY(!DataCatalog::isSafePathSegment(QStringLiteral("a..b")));
  QVERIFY(!DataCatalog::isSafePathSegment(QStringLiteral("a/b")));
  QVERIFY(!DataCatalog::isSafePathSegment(QStringLiteral("a\\b")));
  QVERIFY(!DataCatalog::isSafePathSegment(QStringLiteral("a\nb")));
  QVERIFY(!DataCatalog::isSafePathSegment(QStringLiteral("a\tb")));
  QVERIFY(!DataCatalog::isSafePathSegment(QStringLiteral("a") + QChar(0) + QStringLiteral("b")));
  QVERIFY(!DataCatalog::isSafePathSegment(QString(QChar(0x1F))));

  // managedPath 任一段非法 → 空串（不产出坏路径）
  QVERIFY(DataCatalog::managedPath(QStringLiteral("raw"), QStringLiteral("ast-1"),
                                   QStringLiteral("ver-1"), QStringLiteral("a\nb.dat")).isEmpty());
  QVERIFY(DataCatalog::managedPath(QStringLiteral("raw"), QStringLiteral(".."),
                                   QStringLiteral("ver-1"), QStringLiteral("f.dat")).isEmpty());
  QCOMPARE(DataCatalog::managedPath(QStringLiteral("raw"), QStringLiteral("ast-1"),
                                    QStringLiteral("ver-1"), QStringLiteral("f.dat")),
           QStringLiteral("raw/ast-1/ver-1/f.dat"));

  // addVersion 拒绝坏 fileName / 受管 path 坏段；外链绝对 path 不查段。
  QTemporaryDir dir;
  QVERIFY(dir.isValid());
  DataCatalog cat;
  QVERIFY(cat.open(dir.path()));
  QString err;
  CatalogAsset a;
  a.id = QStringLiteral("ast-1");
  a.type = QStringLiteral("document");
  QVERIFY(cat.addAsset(a));
  CatalogVersion v;
  v.id = QStringLiteral("ver-1");
  v.assetId = QStringLiteral("ast-1");
  v.stage = QStringLiteral("RAW");
  v.fileName = QStringLiteral("a\nb.dat");
  QVERIFY(!cat.addVersion(v, &err));
  QVERIFY(!err.isEmpty());
  v.fileName = QStringLiteral("ok.dat");
  v.managed = true;
  v.path = QStringLiteral("raw/ast-1/../evil.dat");
  QVERIFY(!cat.addVersion(v, &err));
  QVERIFY(!err.isEmpty());
  QVERIFY(cat.versionsForAsset(QStringLiteral("ast-1")).isEmpty());
  v.managed = false;
  v.path = QStringLiteral("/abs/path/f.dat");
  QVERIFY(cat.addVersion(v, &err));
}

// §3 dedup 查询：同一 SHA-256 命中的版本能被找回（大小写不敏感）。
void TestCatalog::versionBySha256FindsStored()
{
  QTemporaryDir dir;
  QVERIFY(dir.isValid());
  DataCatalog cat;
  QVERIFY(cat.open(dir.path()));
  CatalogAsset a;
  a.id = QStringLiteral("ast-1");
  a.type = QStringLiteral("document");
  QVERIFY(cat.addAsset(a));
  QFile stored(dir.filePath(QStringLiteral("stored.dat")));
  QVERIFY(stored.open(QIODevice::WriteOnly));
  QCOMPARE(stored.write("valid bytes"), qint64(11));
  stored.close();
  const QString digest = DataCatalog::sha256FileHex(stored.fileName());
  CatalogVersion v;
  v.id = QStringLiteral("ver-1");
  v.assetId = QStringLiteral("ast-1");
  v.stage = QStringLiteral("RAW");
  v.managed = true;
  v.path = QStringLiteral("stored.dat");
  v.sha256 = digest;
  QVERIFY(cat.addVersion(v));

  QCOMPARE(cat.versionBySha256(digest).id, QStringLiteral("ver-1"));
  QCOMPARE(cat.versionBySha256(digest.toUpper()).id, QStringLiteral("ver-1"));
  QVERIFY(cat.versionBySha256(QStringLiteral("zzz")).id.isEmpty());
  QVERIFY(cat.versionBySha256(QString()).id.isEmpty()); // 空 sha 永不命中
  QVERIFY(stored.remove());
  QVERIFY(cat.versionBySha256(digest).id.isEmpty());
}

void TestCatalog::managedPathRejectsSymlinksAndTraversal()
{
  QTemporaryDir project;
  QTemporaryDir outside;
  QVERIFY(project.isValid() && outside.isValid());
  CatalogVersion version;
  version.managed = true;
  version.path = QStringLiteral("artifacts/raw/file.dat");
  QVERIFY(!DataCatalog::resolvedVersionPath(project.path(), version).isEmpty());
  version.path = QStringLiteral("artifacts/../outside.dat");
  QVERIFY(DataCatalog::resolvedVersionPath(project.path(), version).isEmpty());
#ifdef Q_OS_WIN
  // POSIX 绝对路径（/tmp/...）在 Windows 无盘符、不算绝对——用 Windows 形态。
  version.path = QStringLiteral("C:/outside.dat");
#else
  version.path = QStringLiteral("/tmp/outside.dat");
#endif
  QVERIFY(DataCatalog::resolvedVersionPath(project.path(), version).isEmpty());
  QVERIFY(QDir(project.path()).mkpath(QStringLiteral("artifacts")));
#ifndef Q_OS_WIN
  // Windows 的 QFile::link 生成 .lnk 快捷方式（非符号链接）——「raw」处
  // 不会有链接，拒绝语义无从谈起；符号链接子例在 POSIX 轮覆盖。
  const QString link = project.filePath(QStringLiteral("artifacts/raw"));
  QVERIFY(QFile::link(outside.path(), link));
  version.path = QStringLiteral("artifacts/raw/file.dat");
  QVERIFY(DataCatalog::resolvedVersionPath(project.path(), version).isEmpty());
#else
  Q_UNUSED(outside);
#endif
}

void TestCatalog::managedCatalogLoadRejectsEscape()
{
  QTemporaryDir dir;
  QVERIFY(dir.isValid());
  DataCatalog catalog;
  QVERIFY(catalog.open(dir.path()));
  CatalogAsset asset;
  asset.id = QStringLiteral("ast-1");
  QVERIFY(catalog.addAsset(asset));
  CatalogVersion version;
  version.id = QStringLiteral("ver-1");
  version.assetId = asset.id;
  version.managed = true;
  version.path = QStringLiteral("artifacts/raw/ast-1/ver-1/file.dat");
  QVERIFY(catalog.addVersion(version));
  QFile file(catalog.catalogPath());
  QVERIFY(file.open(QIODevice::ReadOnly));
  QJsonObject root = QJsonDocument::fromJson(file.readAll()).object();
  file.close();
  QJsonArray versions = root.value(QStringLiteral("versions")).toArray();
  QJsonObject bad = versions.at(0).toObject();
  bad.insert(QStringLiteral("path"), QStringLiteral("../../outside.dat"));
  versions[0] = bad;
  root.insert(QStringLiteral("versions"), versions);
  QVERIFY(file.open(QIODevice::WriteOnly | QIODevice::Truncate));
  file.write(QJsonDocument(root).toJson());
  file.close();
  DataCatalog reloaded;
  QString error;
  // 坏段版本如实跳过且不写回：装载照常成功，越界版本不进内存（audit row 36/T33）。
  QVERIFY(reloaded.open(dir.path(), &error));
  QVERIFY(error.isEmpty());
  QVERIFY(reloaded.versionsForAsset(asset.id).isEmpty());
  QJsonObject reloadedRoot;
  {
    QFile check(catalog.catalogPath());
    QVERIFY(check.open(QIODevice::ReadOnly));
    reloadedRoot = QJsonDocument::fromJson(check.readAll()).object();
    check.close();
  }
  QCOMPARE(reloadedRoot.value(QStringLiteral("versions")).toArray().at(0).toObject()
               .value(QStringLiteral("path")).toString(),
           QStringLiteral("../../outside.dat"));
}

// §3：新的已决主关联入库后，同一 (entityType, entityId, role) 的旧主关联降级——
// 同一角色只留一条主关联；不同角色/实体不受影响，未决链接不参与。
void TestCatalog::addLinkDemotesPreviousPrimaryForSameRole()
{
  QTemporaryDir dir;
  QVERIFY(dir.isValid());
  DataCatalog cat;
  QVERIFY(cat.open(dir.path()));
  CatalogEntity w;
  w.id = QStringLiteral("well-A1");
  w.entityType = QStringLiteral("well");
  w.name = QStringLiteral("A1");
  QVERIFY(cat.addEntity(w));
  for (const QString &id : {QStringLiteral("ast-1"), QStringLiteral("ast-2")})
  {
    CatalogAsset a;
    a.id = id;
    a.type = QStringLiteral("well_log");
    QVERIFY(cat.addAsset(a));
  }

  const auto addWellLink = [&cat](const QString &assetId, const QString &role) {
    EntityAssetLink l;
    l.entityType = QStringLiteral("well");
    l.entityId = QStringLiteral("well-A1");
    l.assetId = assetId;
    l.role = role;
    l.isPrimary = true;
    return cat.addLink(l);
  };
  QVERIFY(addWellLink(QStringLiteral("ast-1"), QStringLiteral("well_log")));
  QVERIFY(addWellLink(QStringLiteral("ast-2"), QStringLiteral("well_log"))); // 顶替
  QVERIFY(addWellLink(QStringLiteral("ast-1"), QStringLiteral("tops")));      // 不同角色不动

  const auto links = cat.linksForEntity(QStringLiteral("well-A1"));
  QCOMPARE(links.size(), 3);
  int primaryLogs = 0;
  for (const EntityAssetLink &l : links)
  {
    if (l.role == QLatin1String("well_log"))
    {
      // 旧资产的那条被降级，新资产的仍是主关联
      QCOMPARE(l.isPrimary, l.assetId == QLatin1String("ast-2"));
      if (l.isPrimary)
        ++primaryLogs;
    }
    else
      QVERIFY(l.isPrimary); // tops 关联不受影响
  }
  QCOMPARE(primaryLogs, 1);
}

// §3 dedup 补挂：attachLink 把未决链接挂到实体——置已决、清备注、升主，
// 并降级同实体同角色的其他主关联。已决链接/坏下标/空实体拒绝。
void TestCatalog::attachLinkResolvesUnresolvedAndDemotes()
{
  QTemporaryDir dir;
  QVERIFY(dir.isValid());
  DataCatalog cat;
  QVERIFY(cat.open(dir.path()));
  CatalogEntity w;
  w.id = QStringLiteral("well-G9");
  w.entityType = QStringLiteral("well");
  w.name = QStringLiteral("G9");
  QVERIFY(cat.addEntity(w));
  for (const QString &id : {QStringLiteral("ast-1"), QStringLiteral("ast-2")})
  {
    CatalogAsset a;
    a.id = id;
    a.type = QStringLiteral("well_log");
    QVERIFY(cat.addAsset(a));
  }

  EntityAssetLink pending;
  pending.entityType = QStringLiteral("well");
  pending.assetId = QStringLiteral("ast-1");
  pending.role = QStringLiteral("well_log");
  pending.unresolved = true;
  pending.note = QStringLiteral("未匹配井名: g9");
  QVERIFY(cat.addLink(pending));

  EntityAssetLink other;
  other.entityType = QStringLiteral("well");
  other.entityId = QStringLiteral("well-G9");
  other.assetId = QStringLiteral("ast-2");
  other.role = QStringLiteral("well_log");
  other.isPrimary = true;
  QVERIFY(cat.addLink(other));

  QString err;
  QVERIFY(!cat.attachLink(99, QStringLiteral("well-G9"), &err)); // 越界
  QVERIFY(!cat.attachLink(1, QString(), &err));                  // 空实体 id
  QVERIFY(cat.attachLink(0, QStringLiteral("well-G9"), &err));

  const auto links = cat.links();
  QCOMPARE(links.size(), 2);
  QVERIFY(!links.at(0).unresolved);
  QCOMPARE(links.at(0).entityId, QStringLiteral("well-G9"));
  QVERIFY(links.at(0).isPrimary);
  QVERIFY(links.at(0).note.isEmpty());
  QVERIFY(!links.at(1).isPrimary); // 同实体同角色的旧主关联降级

  QVERIFY(!cat.attachLink(0, QStringLiteral("well-G9"), &err)); // 已决不能再挂

  // 往返后仍是已决
  DataCatalog reloaded;
  QVERIFY(reloaded.open(dir.path()));
  QVERIFY(!reloaded.links().at(0).unresolved);
  QCOMPARE(reloaded.links().at(0).entityId, QStringLiteral("well-G9"));
}

// ---------------------------------------------------------------------------
// pass 2 回归：T17 ver-N 序号恢复 + 版本 id 唯一性；T20 耐久性（拒绝写入态、
// schema_version 闸门、.bak 轮换）；T33 unresolvedLinks()/批量写/装载段校验。
// ---------------------------------------------------------------------------

// T17：重开 catalog 后 m_versionSeq 必须恢复——再 addVersion 拿到的是下一个
// 未用 ver-N，绝不回发已用 id（否则两个版本同 id，versionById 语义全毁）。
void TestCatalog::versionSeqRestoredAfterReload()
{
  QTemporaryDir dir;
  QVERIFY(dir.isValid());
  {
    DataCatalog cat;
    QVERIFY(cat.open(dir.path()));
    CatalogAsset a;
    a.id = QStringLiteral("ast-1");
    a.type = QStringLiteral("well_log");
    QVERIFY(cat.addAsset(a));
    for (int n = 1; n <= 3; ++n)
    {
      CatalogVersion v;
      v.id = QStringLiteral("ver-%1").arg(n);
      v.assetId = a.id;
      v.stage = QStringLiteral("RAW");
      v.fileName = QStringLiteral("f.dat");
      QVERIFY(cat.addVersion(v));
    }
  }

  DataCatalog reloaded;
  QVERIFY(reloaded.open(dir.path()));
  const QString nextId = reloaded.nextVersionId();
  // 唯一性断言：新 id 不在已装载版本里；ver-4 说明序号恢复生效（bug 时是 ver-1）。
  for (const CatalogVersion &v : reloaded.versionsForAsset(QStringLiteral("ast-1")))
    QVERIFY(v.id != nextId);
  QCOMPARE(nextId, QStringLiteral("ver-4"));

  CatalogVersion v;
  v.id = nextId;
  v.assetId = QStringLiteral("ast-1");
  v.stage = QStringLiteral("RAW");
  v.fileName = QStringLiteral("f.dat");
  QVERIFY(reloaded.addVersion(v));
  QCOMPARE(reloaded.versionsForAsset(QStringLiteral("ast-1")).size(), 4);
}

// T17：显式重复 id 也要拒——不依赖 nextVersionId 的分配路径同样受约束。
void TestCatalog::duplicateVersionIdRejected()
{
  QTemporaryDir dir;
  QVERIFY(dir.isValid());
  DataCatalog cat;
  QVERIFY(cat.open(dir.path()));
  CatalogAsset a;
  a.id = QStringLiteral("ast-1");
  a.type = QStringLiteral("document");
  QVERIFY(cat.addAsset(a));
  CatalogVersion v;
  v.id = QStringLiteral("ver-1");
  v.assetId = QStringLiteral("ast-1");
  v.stage = QStringLiteral("RAW");
  v.fileName = QStringLiteral("f.dat");
  QVERIFY(cat.addVersion(v));

  QString err;
  CatalogVersion dup = v;
  QVERIFY(!cat.addVersion(dup, &err));
  QVERIFY(err.contains(QStringLiteral("duplicate")));
  QCOMPARE(cat.versionsForAsset(QStringLiteral("ast-1")).size(), 1);

  // 手工指定 "ver-9" 后序号随动——下一次分配不再回发已用 id。
  v.id = QStringLiteral("ver-9");
  QVERIFY(cat.addVersion(v));
  QCOMPARE(cat.nextVersionId(), QStringLiteral("ver-10"));
}

// T20b：schema_version 比 kSchemaVersion 新 → 如实拒绝（不读不写）；
// 缺键按 1 处理，旧 catalog 照常打开。
void TestCatalog::refusesUnsupportedSchemaVersion()
{
  const auto writeCatalog = [](const QString &dirPath, const QByteArray &json) {
    QVERIFY(QDir().mkpath(dirPath + QStringLiteral("/artifacts/metadata")));
    QFile f(dirPath + QStringLiteral("/artifacts/metadata/catalog.json"));
    QVERIFY(f.open(QIODevice::WriteOnly));
    f.write(json);
    f.close();
  };

  QTemporaryDir dir;
  QVERIFY(dir.isValid());
  writeCatalog(dir.path(), QByteArrayLiteral(
      "{\"schema_version\":999,\"catalog_revision\":7,\"entities\":[],"
      "\"assets\":[],\"versions\":[],\"entity_asset_links\":[]}"));
  DataCatalog cat;
  QString err;
  QVERIFY(!cat.open(dir.path(), &err));
  QVERIFY(err.contains(QStringLiteral("unsupported catalog schema")));
  QVERIFY(cat.refusesWrites());
  QVERIFY(!cat.openError().isEmpty());
  // 拒绝写入态：mutator 全拒，坏 catalog 不会被空内容覆盖。
  CatalogEntity e;
  e.id = QStringLiteral("well-X");
  e.entityType = QStringLiteral("well");
  QVERIFY(!cat.addEntity(e, &err));
  QVERIFY(!err.isEmpty());
  QVERIFY(cat.entities().isEmpty());

  // 缺 schema_version 键 = 版本 1（旧格式）——正常打开。
  QTemporaryDir legacy;
  QVERIFY(legacy.isValid());
  writeCatalog(legacy.path(), QByteArrayLiteral(
      "{\"catalog_revision\":3,\"entities\":[],\"assets\":[],"
      "\"versions\":[],\"entity_asset_links\":[]}"));
  DataCatalog legacyCat;
  QVERIFY2(legacyCat.open(legacy.path(), &err), qPrintable(err));
  QVERIFY(!legacyCat.refusesWrites());
  QVERIFY(legacyCat.addEntity(e, &err));
}

// T20c：每次 save 前把现有 catalog.json 轮转成 .bak——§9 回滚句有可恢复对象。
void TestCatalog::rotatesBakAndVerifiesWrite()
{
  QTemporaryDir dir;
  QVERIFY(dir.isValid());
  const QString catalogFile =
      dir.filePath(QStringLiteral("artifacts/metadata/catalog.json"));
  const QString bakFile = catalogFile + QStringLiteral(".bak");

  DataCatalog cat;
  QVERIFY(cat.open(dir.path())); // 初次建 catalog：没有旧文件可轮转
  QVERIFY(QFile::exists(catalogFile));
  QVERIFY(!QFile::exists(bakFile));

  CatalogEntity w;
  w.id = QStringLiteral("well-A1");
  w.entityType = QStringLiteral("well");
  QVERIFY(cat.addEntity(w)); // save → 轮转空 catalog 进 .bak

  QVERIFY(QFile::exists(bakFile));
  QFile bak(bakFile);
  QVERIFY(bak.open(QIODevice::ReadOnly));
  const QJsonObject bakRoot = QJsonDocument::fromJson(bak.readAll()).object();
  QCOMPARE(bakRoot.value(QStringLiteral("schema_version")).toInt(), 1);
  QVERIFY(bakRoot.value(QStringLiteral("entities")).toArray().isEmpty());

  QFile cur(catalogFile);
  QVERIFY(cur.open(QIODevice::ReadOnly));
  const QJsonObject curRoot = QJsonDocument::fromJson(cur.readAll()).object();
  QCOMPARE(curRoot.value(QStringLiteral("entities")).toArray().size(), 1);
  // .bak 是上一版：revision 严格小于当前。
  QVERIFY(bakRoot.value(QStringLiteral("catalog_revision")).toInt() <
          curRoot.value(QStringLiteral("catalog_revision")).toInt());
}

// T20a：catalog.json 损坏 → open 失败进拒绝写入态；所有 mutator 如实失败，
// 磁盘上的坏文件原样保留（不被空 catalog 覆盖）；重开正常目录可恢复。
void TestCatalog::refusesWritesAfterFailedOpen()
{
  QTemporaryDir dir;
  QVERIFY(dir.isValid());
  QVERIFY(QDir().mkpath(dir.filePath(QStringLiteral("artifacts/metadata"))));
  const QString catalogFile =
      dir.filePath(QStringLiteral("artifacts/metadata/catalog.json"));
  QFile f(catalogFile);
  QVERIFY(f.open(QIODevice::WriteOnly));
  f.write(QByteArrayLiteral("{ not json at all !!!"));
  f.close();
  const QByteArray corruptBytes = [&]() {
    QFile r(catalogFile);
    if (!r.open(QIODevice::ReadOnly))
      return QByteArray();
    return r.readAll();
  }();
  QVERIFY(!corruptBytes.isEmpty());

  DataCatalog cat;
  QVERIFY(cat.refusesWrites()); // 从未成功 open 过的 catalog 同样拒绝写入
  QString err;
  QVERIFY(!cat.open(dir.path(), &err));
  QVERIFY(err.contains(QStringLiteral("corrupt catalog")));
  QVERIFY(cat.refusesWrites());
  QCOMPARE(cat.openError(), err);

  CatalogEntity e;
  e.id = QStringLiteral("well-A1");
  e.entityType = QStringLiteral("well");
  CatalogAsset a;
  a.id = QStringLiteral("ast-1");
  CatalogVersion v;
  v.id = QStringLiteral("ver-1");
  v.assetId = a.id;
  v.fileName = QStringLiteral("f.dat");
  EntityAssetLink l;
  l.entityType = QStringLiteral("well");
  l.assetId = a.id;
  l.unresolved = true;
  QVERIFY(!cat.addEntity(e, &err));
  QVERIFY(!err.isEmpty());
  QVERIFY(!cat.addAsset(a, &err));
  QVERIFY(!cat.addVersion(v, &err));
  QVERIFY(!cat.addLink(l, &err));
  QVERIFY(!cat.attachLink(0, QStringLiteral("well-A1"), &err));
  QVERIFY(!cat.setLinkUnresolved(0, &err));
  QVERIFY(!cat.setLinkPrimary(0, &err));
  QVERIFY(cat.entities().isEmpty()); // 内存态也没被假变更污染

  // 关键不变量：坏文件字节原样在盘上——§9 回滚/人工修复还有救。
  QFile check(catalogFile);
  QVERIFY(check.open(QIODevice::ReadOnly));
  QCOMPARE(check.readAll(), corruptBytes);

  // 同一个对象重开一个好目录 → 拒绝态解除，正常读写。
  QTemporaryDir good;
  QVERIFY(good.isValid());
  QVERIFY2(cat.open(good.path(), &err), qPrintable(err));
  QVERIFY(!cat.refusesWrites());
  QVERIFY(cat.openError().isEmpty());
  QVERIFY(cat.addEntity(e, &err));
}

// T33 / audit row 35：未决集合用显式 unresolvedLinks()；linksForEntity("")
// 不再静默命中全部未决链接——如实回空。
void TestCatalog::unresolvedLinksIsTheExplicitAccessor()
{
  QTemporaryDir dir;
  QVERIFY(dir.isValid());
  DataCatalog cat;
  QVERIFY(cat.open(dir.path()));

  CatalogEntity w;
  w.id = QStringLiteral("well-A1");
  w.entityType = QStringLiteral("well");
  QVERIFY(cat.addEntity(w));
  for (const QString &id : {QStringLiteral("ast-1"), QStringLiteral("ast-2")})
  {
    CatalogAsset a;
    a.id = id;
    a.type = QStringLiteral("well_log");
    QVERIFY(cat.addAsset(a));
  }

  EntityAssetLink resolved;
  resolved.entityType = QStringLiteral("well");
  resolved.entityId = w.id;
  resolved.assetId = QStringLiteral("ast-1");
  resolved.role = QStringLiteral("well_log");
  QVERIFY(cat.addLink(resolved));

  EntityAssetLink pending = resolved;
  pending.entityId.clear();
  pending.assetId = QStringLiteral("ast-2");
  pending.unresolved = true;
  pending.isPrimary = false;
  pending.note = QStringLiteral("未匹配井名: zz");
  QVERIFY(cat.addLink(pending));

  const auto unresolved = cat.unresolvedLinks();
  QCOMPARE(unresolved.size(), 1);
  QCOMPARE(unresolved.front().assetId, QStringLiteral("ast-2"));
  QVERIFY(unresolved.front().unresolved);

  // 守卫空 id：空 entityId 返回空集，不等于未决集合。
  QVERIFY(cat.linksForEntity(QString()).isEmpty());
  QCOMPARE(cat.linksForEntity(w.id).size(), 1);
}

// T33 / audit row 37：BatchSave 作用域内 mutator 不落盘；作用域末一次
// save() + 一次 changed()；数据照常入库可重载。
void TestCatalog::batchSaveCoalescesWrites()
{
  QTemporaryDir dir;
  QVERIFY(dir.isValid());
  DataCatalog cat;
  QVERIFY(cat.open(dir.path()));
  QSignalSpy spy(&cat, &DataCatalog::changed);

  const QString catalogFile =
      dir.filePath(QStringLiteral("artifacts/metadata/catalog.json"));
  const qint64 sizeBefore = QFileInfo(catalogFile).size();

  {
    DataCatalog::BatchSave batch(&cat);
    CatalogEntity w;
    w.id = QStringLiteral("well-A1");
    w.entityType = QStringLiteral("well");
    QVERIFY(cat.addEntity(w));
    CatalogAsset a;
    a.id = QStringLiteral("ast-1");
    a.type = QStringLiteral("well_log");
    QVERIFY(cat.addAsset(a));
    EntityAssetLink l;
    l.entityType = QStringLiteral("well");
    l.entityId = w.id;
    l.assetId = a.id;
    l.role = QStringLiteral("well_log");
    QVERIFY(cat.addLink(l));
    // 挂起期间：changed() 未发、磁盘未动；内存读侧可见变更。
    QCOMPARE(spy.count(), 0);
    QCOMPARE(QFileInfo(catalogFile).size(), sizeBefore);
    QCOMPARE(cat.entities().size(), 1);
    QString ferr;
    QVERIFY(batch.flush(&ferr)); // 显式结算一次
    QCOMPARE(spy.count(), 1);
  } // 析构幂等——不再多发 changed()

  QCOMPARE(spy.count(), 1);
  DataCatalog reloaded;
  QVERIFY(reloaded.open(dir.path()));
  QCOMPARE(reloaded.entities().size(), 1);
  QCOMPARE(reloaded.links().size(), 1);
}

// T33：手改的 catalog 携带 '..' 受管路径——装载时如实跳过该版本（log+skip），
// 不装进内存、不写回磁盘；干净版本不受影响。
void TestCatalog::unsafeManagedPathSkippedOnLoad()
{
  QTemporaryDir dir;
  QVERIFY(dir.isValid());
  QVERIFY(QDir().mkpath(dir.filePath(QStringLiteral("artifacts/metadata"))));
  QFile f(dir.filePath(QStringLiteral("artifacts/metadata/catalog.json")));
  QVERIFY(f.open(QIODevice::WriteOnly));
  f.write(QByteArrayLiteral(
      "{\"schema_version\":1,\"catalog_revision\":1,\"entities\":[],"
      "\"assets\":[{\"id\":\"ast-1\",\"type\":\"well_log\",\"format\":\"las\","
      "\"display_name\":\"A1.Las\"}],"
      "\"versions\":["
      "{\"id\":\"ver-1\",\"asset_id\":\"ast-1\",\"stage\":\"RAW\","
      "\"version_number\":1,\"managed\":true,"
      "\"path\":\"raw/ast-1/ver-1/A1.Las\",\"file_name\":\"A1.Las\"},"
      "{\"id\":\"ver-2\",\"asset_id\":\"ast-1\",\"stage\":\"RAW\","
      "\"version_number\":2,\"managed\":true,"
      "\"path\":\"raw/ast-1/../../outside/evil.dat\",\"file_name\":\"evil.dat\"},"
      "{\"id\":\"ver-3\",\"asset_id\":\"ast-1\",\"stage\":\"RAW\","
      "\"version_number\":3,\"managed\":true,"
      "\"path\":\"raw/ast-1/ver-3/ok.dat\",\"file_name\":\"..\"}"
      "],\"entity_asset_links\":[]}"));
  f.close();

  DataCatalog cat;
  QString err;
  QVERIFY2(cat.open(dir.path(), &err), qPrintable(err));
  const auto versions = cat.versionsForAsset(QStringLiteral("ast-1"));
  QCOMPARE(versions.size(), 1); // ver-2 的 '..' 段、ver-3 的坏 fileName 都被跳过
  QCOMPARE(versions.front().id, QStringLiteral("ver-1"));
  // 序号恢复只看装进内存的版本——ver-2/ver-3 被拒不占序号。
  QCOMPARE(cat.nextVersionId(), QStringLiteral("ver-2"));
}

// D12：旧版 catalog 携带 uwi/aliases 键——打开不报错（键被忽略），井身份
// 只认 name；任何一次落盘后文件里不再出现这两个键（不回写）。
void TestCatalog::legacyCatalogFieldsIgnoredAndNotRewritten()
{
  QTemporaryDir dir;
  QVERIFY(dir.isValid());
  QVERIFY(QDir().mkpath(dir.filePath(QStringLiteral("artifacts/metadata"))));
  QFile f(dir.filePath(QStringLiteral("artifacts/metadata/catalog.json")));
  QVERIFY(f.open(QIODevice::WriteOnly));
  f.write(QByteArrayLiteral(
      "{\"schema_version\":1,\"catalog_revision\":1,"
      "\"entities\":[{\"id\":\"well-A1\",\"entity_type\":\"well\",\"name\":\"A1\","
      "\"uwi\":\"1005288123400\",\"aliases\":[\"Well-One\",\"A-1\"],"
      "\"coordinate_status\":\"missing\"}],"
      "\"assets\":[],\"versions\":[],\"entity_asset_links\":[]}"));
  f.close();

  DataCatalog cat;
  QString err;
  QVERIFY2(cat.open(dir.path(), &err), qPrintable(err));

  // uwi/aliases 不再是解析键：按 uwi、按旧别名都找不到井；按 name 找得到。
  QVERIFY(cat.wellsMatchingName(QStringLiteral("1005288123400")).isEmpty());
  QVERIFY(cat.wellsMatchingName(QStringLiteral("Well-One")).isEmpty());
  QCOMPARE(cat.wellsMatchingName(QStringLiteral("a-1")),
           QStringList{QStringLiteral("well-A1")});
  QCOMPARE(cat.entities(QStringLiteral("well")).size(), 1);

  // 触发一次落盘（addAsset），回读原始 JSON：两键消失。
  CatalogAsset a;
  a.id = QStringLiteral("ast-1");
  a.type = QStringLiteral("well_log");
  QVERIFY(cat.addAsset(a));
  QFile rf(cat.catalogPath());
  QVERIFY(rf.open(QIODevice::ReadOnly));
  const QByteArray raw = rf.readAll();
  rf.close();
  QVERIFY(!raw.contains(QByteArrayLiteral("\"uwi\"")));
  QVERIFY(!raw.contains(QByteArrayLiteral("\"aliases\"")));
  QVERIFY(raw.contains(QByteArrayLiteral("\"name\""))); // 实体本身仍在

  // 重开轮转后的文件仍然健康（.bak 是旧内容，catalog.json 是新 schema）。
  DataCatalog cat2;
  QVERIFY2(cat2.open(dir.path(), &err), qPrintable(err));
  QCOMPARE(cat2.entities(QStringLiteral("well")).size(), 1);
}

// D12：新建实体序列化不含 uwi/aliases 两键。
void TestCatalog::newEntitySerializationOmitsLegacyKeys()
{
  QTemporaryDir dir;
  QVERIFY(dir.isValid());
  DataCatalog cat;
  QVERIFY(cat.open(dir.path()));

  CatalogEntity well;
  well.id = QStringLiteral("well-A1");
  well.entityType = QStringLiteral("well");
  well.name = QStringLiteral("A1");
  well.coordinateStatus = QStringLiteral("missing");
  QVERIFY(cat.addEntity(well));

  QFile rf(cat.catalogPath());
  QVERIFY(rf.open(QIODevice::ReadOnly));
  const QByteArray raw = rf.readAll();
  rf.close();
  QVERIFY(!raw.contains(QByteArrayLiteral("\"uwi\"")));
  QVERIFY(!raw.contains(QByteArrayLiteral("\"aliases\"")));
}

namespace
{
  // qWarning 计数（词表诊断「每次违例告警一次」的断言用）：测试进程单线程，
  // 文件内静态计数器够用。
  int g_warningCount = 0;
  void countWarnings(QtMsgType type, const QMessageLogContext &, const QString &)
  {
    if (type == QtMsgType::QtWarningMsg)
      ++g_warningCount;
  }
}

// wave/data-integrity：未知角色不硬拦——链接照常写入（不丢数据）、role 原样
// 保留（不静默改词），note 追加「未知角色: <role>」诊断并 qWarning 一次；
// invalidRoleLinks() 可查；诊断随 catalog.json round-trip。
void TestCatalog::unknownRoleLinkStillWrittenButDiagnosed()
{
  QTemporaryDir dir;
  QVERIFY(dir.isValid());
  {
    DataCatalog cat;
    QVERIFY(cat.open(dir.path()));
    CatalogAsset a;
    a.id = QStringLiteral("ast-1");
    a.type = QStringLiteral("well_log");
    QVERIFY(cat.addAsset(a));

    const int warningsBefore = g_warningCount;
    const auto oldHandler = qInstallMessageHandler(countWarnings);
    EntityAssetLink l;
    l.entityType = QStringLiteral("well");
    l.entityId = QStringLiteral("well-A1");
    l.assetId = a.id;
    l.role = QStringLiteral("mystery_role");
    QString err;
    QVERIFY2(cat.addLink(l, &err), qPrintable(err)); // 词表外自定义：不拒收
    qInstallMessageHandler(oldHandler);
    QCOMPARE(g_warningCount - warningsBefore, 1); // 告警恰好一次

    const auto links = cat.links();
    QCOMPARE(links.size(), 1);
    QCOMPARE(links.front().role, QStringLiteral("mystery_role")); // 不改词
    QCOMPARE(links.front().note, QStringLiteral("未知角色: mystery_role"));
    QCOMPARE(cat.invalidRoleLinks().size(), 1);
    QCOMPARE(cat.invalidRoleLinks().front().assetId, QStringLiteral("ast-1"));
  }
  // 诊断活在 note 里 → 重开后仍然可查（诊断面跨会话存活）。
  DataCatalog reloaded;
  QVERIFY(reloaded.open(dir.path()));
  QCOMPARE(reloaded.invalidRoleLinks().size(), 1);
  QCOMPARE(reloaded.links().front().note, QStringLiteral("未知角色: mystery_role"));
}

// 已有 note 的未决链接拿到词表诊断：用「；」连接，不覆盖既有备注。
void TestCatalog::unknownRoleDiagnosticJoinsExistingNote()
{
  QTemporaryDir dir;
  QVERIFY(dir.isValid());
  DataCatalog cat;
  QVERIFY(cat.open(dir.path()));
  CatalogAsset a;
  a.id = QStringLiteral("ast-1");
  a.type = QStringLiteral("well_log");
  QVERIFY(cat.addAsset(a));

  EntityAssetLink l;
  l.entityType = QStringLiteral("well");
  l.assetId = a.id;
  l.role = QStringLiteral("mystery");
  l.unresolved = true;
  l.note = QStringLiteral("未匹配井名: g9");
  QVERIFY(cat.addLink(l));
  QCOMPARE(cat.links().front().note,
           QStringLiteral("未匹配井名: g9；未知角色: mystery"));
}

// 已知角色 + 实体类型不在词表 entityTypes 里：同样只诊断不拦（写入保留，
// note 记「角色与实体类型不符」）。
void TestCatalog::roleEntityTypeMismatchDiagnosedNotBlocked()
{
  QTemporaryDir dir;
  QVERIFY(dir.isValid());
  DataCatalog cat;
  QVERIFY(cat.open(dir.path()));
  CatalogAsset a;
  a.id = QStringLiteral("ast-1");
  a.type = QStringLiteral("horizon");
  QVERIFY(cat.addAsset(a));

  EntityAssetLink l;
  l.entityType = QStringLiteral("seismic_survey"); // well_log 词表只许 well
  l.entityId = QStringLiteral("svy-1");
  l.assetId = a.id;
  l.role = QStringLiteral("well_log");
  QString err;
  QVERIFY2(cat.addLink(l, &err), qPrintable(err));
  QCOMPARE(cat.links().size(), 1);
  QCOMPARE(cat.links().front().role, QStringLiteral("well_log"));
  QVERIFY(cat.links().front().note.contains(
      QStringLiteral("角色与实体类型不符: well_log 于 seismic_survey")));
  QCOMPARE(cat.invalidRoleLinks().size(), 1);
}

// attachLink 决议时 note 被清——词表诊断须重下：未知角色的链接决议后诊断
// 仍在（不因决议而消失），已知角色的链接决议后 note 干净、不进诊断集。
void TestCatalog::attachLinkReappliesRoleDiagnostics()
{
  QTemporaryDir dir;
  QVERIFY(dir.isValid());
  DataCatalog cat;
  QVERIFY(cat.open(dir.path()));
  CatalogAsset a1;
  a1.id = QStringLiteral("ast-1");
  a1.type = QStringLiteral("well_log");
  QVERIFY(cat.addAsset(a1));
  CatalogAsset a2;
  a2.id = QStringLiteral("ast-2");
  a2.type = QStringLiteral("well_log");
  QVERIFY(cat.addAsset(a2));

  EntityAssetLink mystery;
  mystery.entityType = QStringLiteral("well");
  mystery.assetId = a1.id;
  mystery.role = QStringLiteral("mystery");
  mystery.unresolved = true;
  mystery.note = QStringLiteral("未匹配井名: g9");
  QVERIFY(cat.addLink(mystery));

  EntityAssetLink normal = mystery;
  normal.assetId = a2.id;
  normal.role = QStringLiteral("well_log");
  QVERIFY(cat.addLink(normal));

  QString err;
  QVERIFY(cat.attachLink(0, QStringLiteral("well-A1"), &err));
  QCOMPARE(cat.links().at(0).note, QStringLiteral("未知角色: mystery")); // 候选备注清、诊断重下
  QVERIFY(cat.attachLink(1, QStringLiteral("well-A1"), &err));
  QVERIFY(cat.links().at(1).note.isEmpty());

  const auto invalid = cat.invalidRoleLinks();
  QCOMPARE(invalid.size(), 1);
  QCOMPARE(invalid.front().assetId, QStringLiteral("ast-1"));
}

// 词表内的干净链接不进诊断集：未决备注（候选/未匹配名）不是词表违例，
// invalidRoleLinks() 只收带词表诊断标记的链接。
void TestCatalog::knownRoleLinksStayOutOfInvalidSet()
{
  QTemporaryDir dir;
  QVERIFY(dir.isValid());
  DataCatalog cat;
  QVERIFY(cat.open(dir.path()));
  CatalogAsset a;
  a.id = QStringLiteral("ast-1");
  a.type = QStringLiteral("well_log");
  QVERIFY(cat.addAsset(a));

  EntityAssetLink resolved;
  resolved.entityType = QStringLiteral("well");
  resolved.entityId = QStringLiteral("well-A1");
  resolved.assetId = a.id;
  resolved.role = QStringLiteral("well_log");
  QVERIFY(cat.addLink(resolved));

  EntityAssetLink pending = resolved;
  pending.entityId.clear();
  pending.unresolved = true;
  pending.isPrimary = false;
  pending.note = QStringLiteral("候选: x1(well-X1), x2(well-X2)");
  QVERIFY(cat.addLink(pending));

  QCOMPARE(cat.links().size(), 2);
  QVERIFY(cat.invalidRoleLinks().isEmpty());
  QVERIFY(cat.links().at(0).note.isEmpty());
  QCOMPARE(cat.links().at(1).note, QStringLiteral("候选: x1(well-X1), x2(well-X2)"));
}


// ---------------------------------------------------------------------------
// wave/data-foundation T5+T7：.bak 链恢复演练——连续两轮「损坏主文件 → 恢复
// → 变更续存 → 再损坏 → 再恢复」，每轮恢复后数据完整且可继续演化。
// ---------------------------------------------------------------------------
void TestCatalog::backupRecoveryChainDrill()
{
  QTemporaryDir dir;
  QVERIFY(dir.isValid());
  const QString path = QDir(dir.path()).filePath(
      QStringLiteral("artifacts/metadata/catalog.json"));
  const QString bak = path + QStringLiteral(".bak");

  DataCatalog cat;
  QVERIFY(cat.open(dir.path()));
  CatalogEntity a1;
  a1.id = QStringLiteral("well-A1");
  a1.entityType = QStringLiteral("well");
  a1.name = QStringLiteral("A1");
  QVERIFY(cat.addEntity(a1));

  // 第一轮：save 已轮转出 .bak → 损坏主文件 → reopen 恢复。
  QVERIFY(QFile::exists(bak));
  QVERIFY(cat.addEntity([] {
    CatalogEntity b2;
    b2.id = QStringLiteral("well-B2");
    b2.entityType = QStringLiteral("well");
    b2.name = QStringLiteral("B2");
    return b2;
  }()));
  {
    QFile f(path);
    QVERIFY(f.open(QIODevice::WriteOnly));
    f.write("{ this is not json");
  }
  // .bak 语义（§9 回滚）：恢复点 = 最后一次成功 save 的「上一代」。B2 所在
  // 的 v2 被损坏时，.bak 还是 v1（A1 only）——最近一轮增量如实丢失，这是
  // 设计而非缺陷（QSaveFile 防写一半，.bak 防「写成功但内容错」）。
  DataCatalog reopened;
  QSignalSpy recovered(&reopened, &DataCatalog::backupRecovered);
  QVERIFY(reopened.open(dir.path()));
  QVERIFY(reopened.recoveredFromBackup());
  QVERIFY(!reopened.lastBackupRecoveryReason().isEmpty());
  QCOMPARE(recovered.count(), 1);
  QVERIFY(reopened.hasEntity(QStringLiteral("well-A1")));
  QVERIFY(!reopened.hasEntity(QStringLiteral("well-B2"))); // 最近一轮丢失（上一代恢复）

  // 恢复后续存两轮：C3 save 后 .bak=恢复内容(A1)；D4 save 后 .bak=A1+C3。
  CatalogEntity c3;
  c3.id = QStringLiteral("well-C3");
  c3.entityType = QStringLiteral("well");
  c3.name = QStringLiteral("C3");
  QVERIFY(reopened.addEntity(c3));
  CatalogEntity d4;
  d4.id = QStringLiteral("well-D4");
  d4.entityType = QStringLiteral("well");
  d4.name = QStringLiteral("D4");
  QVERIFY(reopened.addEntity(d4));

  // 第二轮：再损坏 → 再恢复（.bak 链一直可用；恢复点推进到 A1+C3 那代）。
  {
    QFile f(path);
    QVERIFY(f.open(QIODevice::WriteOnly));
    f.write("]]] corrupt again");
  }
  DataCatalog second;
  QVERIFY(second.open(dir.path()));
  QVERIFY(second.recoveredFromBackup());
  QVERIFY(second.hasEntity(QStringLiteral("well-A1")));
  QVERIFY(second.hasEntity(QStringLiteral("well-C3"))); // 倒数第二代全在
  QVERIFY(!second.hasEntity(QStringLiteral("well-D4"))); // 最近一轮丢失
  QCOMPARE(second.entities(QStringLiteral("well")).size(), 2);
}

namespace
{
  CatalogEntity wellEntity(const QString &name)
  {
    CatalogEntity e;
    e.id = QStringLiteral("well-") + name;
    e.entityType = QStringLiteral("well");
    e.name = name;
    return e;
  }

  bool parsesAsJsonObject(const QString &path)
  {
    QFile f(path);
    if (!f.open(QIODevice::ReadOnly))
      return false;
    QJsonParseError pe;
    const QJsonDocument doc = QJsonDocument::fromJson(f.readAll(), &pe);
    return pe.error == QJsonParseError::NoError && doc.isObject();
  }

  void corrupt(const QString &path, const QByteArray &junk)
  {
    QFile f(path);
    QVERIFY(f.open(QIODevice::WriteOnly));
    f.write(junk);
  }
} // namespace

// #79：恢复后的首次 save 不能把盘上仍损坏的主文件轮转成 .bak——否则
// 好的 .bak 被挤成 .bak.2，紧接着的第二次损坏就无法自动恢复。
void TestCatalog::recoveryDoesNotRotateCorruptPrimaryIntoBak()
{
  QTemporaryDir dir;
  QVERIFY(dir.isValid());
  const QString path = QDir(dir.path()).filePath(
      QStringLiteral("artifacts/metadata/catalog.json"));
  const QString bak = path + QStringLiteral(".bak");
  {
    DataCatalog cat;
    QVERIFY(cat.open(dir.path()));
    QVERIFY(cat.addEntity(wellEntity(QStringLiteral("A1"))));
    QVERIFY(cat.addEntity(wellEntity(QStringLiteral("B2"))));
  }
  corrupt(path, "{ broken");

  DataCatalog reopened;
  QVERIFY(reopened.open(dir.path()));
  QVERIFY(reopened.recoveredFromBackup());
  QVERIFY(reopened.hasEntity(QStringLiteral("well-A1")));
  // 恢复后只发生一次 save。
  QVERIFY(reopened.addEntity(wellEntity(QStringLiteral("C3"))));

  QVERIFY2(parsesAsJsonObject(path), "primary must be rewritten with recovered content");
  QVERIFY2(parsesAsJsonObject(bak), ".bak must not be the corrupt primary");
  const QStringList quarantined = QFileInfo(path).dir().entryList(
      QStringList{QStringLiteral("catalog.json.corrupt-*")}, QDir::Files);
  QCOMPARE(quarantined.size(), 1); // 损坏现场隔离留证，不进 .bak 链

  // 紧接着第二次损坏：.bak 仍可用 → 自动恢复。
  corrupt(path, "]]] again");
  DataCatalog second;
  QVERIFY(second.open(dir.path()));
  QVERIFY(second.recoveredFromBackup());
  QVERIFY(second.hasEntity(QStringLiteral("well-A1")));
}

// #79：.bak 也坏了时依次回退 .bak.2…，取第一份可解析的一代。
void TestCatalog::recoveryFallsBackToOlderGeneration()
{
  QTemporaryDir dir;
  QVERIFY(dir.isValid());
  const QString path = QDir(dir.path()).filePath(
      QStringLiteral("artifacts/metadata/catalog.json"));
  {
    DataCatalog cat;
    QVERIFY(cat.open(dir.path()));
    QVERIFY(cat.addEntity(wellEntity(QStringLiteral("A1"))));
    QVERIFY(cat.addEntity(wellEntity(QStringLiteral("B2"))));
    QVERIFY(cat.addEntity(wellEntity(QStringLiteral("C3"))));
  }
  // 现在：主 = A1+B2+C3，.bak = A1+B2，.bak.2 = A1。
  QVERIFY(parsesAsJsonObject(path + QStringLiteral(".bak.2")));
  corrupt(path, "nope");
  corrupt(path + QStringLiteral(".bak"), "also nope");

  DataCatalog cat;
  QString err;
  QVERIFY2(cat.open(dir.path(), &err), qPrintable(err));
  QVERIFY(cat.recoveredFromBackup());
  QVERIFY(cat.hasEntity(QStringLiteral("well-A1")));
  QVERIFY(!cat.hasEntity(QStringLiteral("well-B2")));
  QCOMPARE(cat.entities(QStringLiteral("well")).size(), 1);
}

// T5：schema 不匹配（未来版本）不走 .bak 回退——那是数据降级不是恢复。
void TestCatalog::backupRecoveryRefusedOnSchemaMismatch()
{
  QTemporaryDir dir;
  QVERIFY(dir.isValid());
  const QString path = QDir(dir.path()).filePath(
      QStringLiteral("artifacts/metadata/catalog.json"));

  {
    DataCatalog cat;
    QVERIFY(cat.open(dir.path()));
    CatalogEntity w;
    w.id = QStringLiteral("well-A1");
    w.entityType = QStringLiteral("well");
    w.name = QStringLiteral("A1");
    QVERIFY(cat.addEntity(w));
  }
  // 主文件写成 schema_version=99（好 JSON、坏版本）。
  {
    QFile f(path);
    QVERIFY(f.open(QIODevice::WriteOnly));
    f.write(R"({"schema_version": 99, "entities": []})");
  }
  DataCatalog cat;
  QString err;
  QVERIFY(!cat.open(dir.path(), &err));
  QVERIFY(err.contains(QStringLiteral("unsupported catalog schema")));
  QVERIFY(!cat.recoveredFromBackup());
  QVERIFY(cat.refusesWrites());
}

// T5：主/bak 都坏 → 拒写态（错误文案点名两份文件），不让空 catalog 覆盖。
void TestCatalog::bothCorruptRefusesWrites()
{
  QTemporaryDir dir;
  QVERIFY(dir.isValid());
  const QString path = QDir(dir.path()).filePath(
      QStringLiteral("artifacts/metadata/catalog.json"));
  {
    DataCatalog cat;
    QVERIFY(cat.open(dir.path()));
    CatalogEntity w;
    w.id = QStringLiteral("well-A1");
    w.entityType = QStringLiteral("well");
    w.name = QStringLiteral("A1");
    QVERIFY(cat.addEntity(w));
  }
  {
    QFile f(path);
    QVERIFY(f.open(QIODevice::WriteOnly));
    f.write("nope");
  }
  {
    QFile f(path + QStringLiteral(".bak"));
    QVERIFY(f.open(QIODevice::WriteOnly));
    f.write("also nope");
  }
  DataCatalog cat;
  QString err;
  QVERIFY(!cat.open(dir.path(), &err));
  QVERIFY(err.contains(QStringLiteral("corrupt catalog")));
  QVERIFY(err.contains(QStringLiteral("corrupt backup")));
  QVERIFY(cat.refusesWrites());
}

// T6：写盘失败（目标目录只读）不截断既有 wells.geojson——QSaveFile 语义。
void TestCatalog::wellsGeoJsonKeepsOldFileOnFailedWrite()
{
  QTemporaryDir dir;
  QVERIFY(dir.isValid());
  const QString sub = QDir(dir.path()).filePath(QStringLiteral("out"));
  QVERIFY(QDir().mkpath(sub));
  const QString geo = QDir(sub).filePath(QStringLiteral("wells.geojson"));

  DataCatalog cat;
  QVERIFY(cat.open(dir.path()));
  CatalogEntity w;
  w.id = QStringLiteral("well-A1");
  w.entityType = QStringLiteral("well");
  w.name = QStringLiteral("A1");
  w.surfaceX = 1.0;
  w.surfaceY = 2.0;
  w.hasSurface = true;
  QVERIFY(cat.addEntity(w));

  QVERIFY(cat.writeWellsGeoJson(geo));
  const QByteArray good = [&] {
    QFile f(geo);
    f.open(QIODevice::ReadOnly);
    return f.readAll();
  }();
  QVERIFY(!good.isEmpty());

  // 目录只读 → QSaveFile 开不出临时文件 → 失败且旧文件字节原样。
  // Windows：目录只读属性不挡在目录内创建文件，改为对目标文件持零共享
  // 句柄——QSaveFile 提交阶段的替换必败，同样保住旧文件字节。
#ifdef Q_OS_WIN
  // FILE_SHARE_READ：允许后续只读校验打开，但 QSaveFile 的替换提交
  // （需要 DELETE 共享）仍必败。
  HANDLE geoLock = CreateFileW(
      reinterpret_cast<const wchar_t *>(geo.utf16()), GENERIC_READ,
      FILE_SHARE_READ, nullptr, OPEN_EXISTING, FILE_ATTRIBUTE_NORMAL, nullptr);
  QVERIFY(geoLock != INVALID_HANDLE_VALUE);
#else
  QVERIFY(QFile::setPermissions(sub, QFileDevice::ReadOwner | QFileDevice::ExeOwner |
                                         QFileDevice::ReadGroup | QFileDevice::ExeGroup |
                                         QFileDevice::ReadOther | QFileDevice::ExeOther));
#endif
  QString werr;
  QVERIFY(!cat.writeWellsGeoJson(geo, &werr));
  QVERIFY(!werr.isEmpty());
  {
    QFile f(geo);
    QVERIFY(f.open(QIODevice::ReadOnly));
    QCOMPARE(f.readAll(), good); // 关键断言：没有半截文件
  }
#ifdef Q_OS_WIN
  CloseHandle(geoLock);
#else
  QVERIFY(QFile::setPermissions(sub, QFileDevice::ReadOwner | QFileDevice::WriteOwner |
                                         QFileDevice::ExeOwner |
                                         QFileDevice::ReadGroup | QFileDevice::WriteGroup |
                                         QFileDevice::ExeGroup |
                                         QFileDevice::ReadOther | QFileDevice::ExeOther));
#endif
}

// T4：锁降级只读——save/mutator 拒绝且内存回滚，读面照常。
void TestCatalog::lockedReadOnlyRefusesMutationsButReadsFine()
{
  QTemporaryDir dir;
  QVERIFY(dir.isValid());

  DataCatalog cat;
  QVERIFY(cat.open(dir.path()));
  CatalogEntity w;
  w.id = QStringLiteral("well-A1");
  w.entityType = QStringLiteral("well");
  w.name = QStringLiteral("A1");
  QVERIFY(cat.addEntity(w));
  const int revisionBefore = cat.catalogRevision();

  cat.setLockedReadOnly(true);
  QVERIFY(cat.refusesWrites());
  CatalogEntity b2;
  b2.id = QStringLiteral("well-B2");
  b2.entityType = QStringLiteral("well");
  b2.name = QStringLiteral("B2");
  QString err;
  QVERIFY(!cat.addEntity(b2, &err));
  QVERIFY(err.contains(QStringLiteral("另一个实例锁定")));
  QVERIFY(!cat.hasEntity(QStringLiteral("well-B2"))); // 内存回滚
  QCOMPARE(cat.catalogRevision(), revisionBefore);   // 落盘零次
  QVERIFY(cat.hasEntity(QStringLiteral("well-A1"))); // 读面照常
  QVERIFY(!cat.wellsMatchingName(QStringLiteral("A1")).isEmpty());

  // 解锁恢复可写。
  cat.setLockedReadOnly(false);
  QVERIFY(cat.addEntity(b2));
  QVERIFY(cat.hasEntity(QStringLiteral("well-B2")));
}

// T3：catalog.sqlite 递延项的触发条件量化——合成 1000 井/2000 资产/3000 版本
// /4000 链接的 catalog.json，量 open()/save()/各查询延迟。预算阈值 = 实测
// 均值量级 × 大余量（防 CI 抖动假红），实测数记 qInfo 供 docs/progress/
// data.md 论证引用。结论判据写在 docs：阈值远未触发 → sqlite 递延维持。
void TestCatalog::thousandEntityQueryBudget()
{
  QTemporaryDir dir;
  QVERIFY(dir.isValid());
  const QString projectDir = dir.path();
  const QString metaDir = QDir(projectDir).filePath(QStringLiteral("artifacts/metadata"));
  QVERIFY(QDir().mkpath(metaDir));
  const QString path = QDir(metaDir).filePath(QStringLiteral("catalog.json"));

  // 直接写合成 JSON（不经 mutator——逐条 add 是 O(n²) 全量重写，量的正是
  // 打开态查询，不是装载路径本身的写入放大）。
  {
    QJsonArray ents, asts, vers, lnks;
    for (int i = 0; i < 1000; ++i)
    {
      QJsonObject e;
      e.insert(QStringLiteral("id"), QStringLiteral("well-%1").arg(i));
      e.insert(QStringLiteral("entity_type"), QStringLiteral("well"));
      e.insert(QStringLiteral("name"), QStringLiteral("W%1").arg(i));
      e.insert(QStringLiteral("surface_x"), 1000.0 + i);
      e.insert(QStringLiteral("surface_y"), 2000.0 + i);
      e.insert(QStringLiteral("has_surface"), true);
      ents.append(e);
    }
    for (int i = 0; i < 2000; ++i)
    {
      QJsonObject a;
      a.insert(QStringLiteral("id"), QStringLiteral("ast-%1").arg(i + 1));
      a.insert(QStringLiteral("type"), QStringLiteral("well_log"));
      a.insert(QStringLiteral("format"), QStringLiteral("las"));
      a.insert(QStringLiteral("display_name"), QStringLiteral("log%1.las").arg(i));
      asts.append(a);
    }
    for (int i = 0; i < 3000; ++i)
    {
      QJsonObject v;
      v.insert(QStringLiteral("id"), QStringLiteral("ver-%1").arg(i + 1));
      v.insert(QStringLiteral("asset_id"), QStringLiteral("ast-%1").arg(i / 3 + 1));
      v.insert(QStringLiteral("stage"), QStringLiteral("RAW"));
      v.insert(QStringLiteral("version_number"), i % 3 + 1);
      v.insert(QStringLiteral("managed"), false);
      v.insert(QStringLiteral("path"),
               QStringLiteral("/nonexistent/external/log%1.las").arg(i));
      v.insert(QStringLiteral("sha256"),
               QStringLiteral("%1").arg(i, 64, 16, QChar(QLatin1Char('0'))));
      vers.append(v);
    }
    for (int i = 0; i < 4000; ++i)
    {
      QJsonObject l;
      l.insert(QStringLiteral("entity_type"), QStringLiteral("well"));
      l.insert(QStringLiteral("entity_id"), QStringLiteral("well-%1").arg(i % 1000));
      l.insert(QStringLiteral("asset_id"), QStringLiteral("ast-%1").arg(i % 2000 + 1));
      l.insert(QStringLiteral("role"), QStringLiteral("well_log"));
      l.insert(QStringLiteral("is_primary"), i % 4 == 0);
      l.insert(QStringLiteral("ordinal"), i % 4);
      lnks.append(l);
    }
    QJsonObject root;
    root.insert(QStringLiteral("schema_version"), 1);
    root.insert(QStringLiteral("catalog_revision"), 1);
    root.insert(QStringLiteral("entities"), ents);
    root.insert(QStringLiteral("assets"), asts);
    root.insert(QStringLiteral("versions"), vers);
    root.insert(QStringLiteral("entity_asset_links"), lnks);
    QFile f(path);
    QVERIFY(f.open(QIODevice::WriteOnly));
    f.write(QJsonDocument(root).toJson(QJsonDocument::Indented));
  }
  const qint64 jsonBytes = QFileInfo(path).size();
  QVERIFY(jsonBytes > 500 * 1024); // 合成库确实够大（>500KB）

  DataCatalog cat;
  QElapsedTimer clock;
  clock.start();
  QString oerr;
  QVERIFY2(cat.open(projectDir, &oerr), qPrintable(oerr));
  const qint64 openMs = clock.elapsed();
  QCOMPARE(cat.entities(QStringLiteral("well")).size(), 1000);

  // 各查询取 5 次中位数（单次微秒级，冷热差吸收在中位数里）。
  const auto medianOf = [](const std::function<void()> &fn) -> qint64 {
    QVector<qint64> xs;
    for (int i = 0; i < 5; ++i)
    {
      QElapsedTimer c;
      c.start();
      fn();
      xs.append(c.elapsed());
    }
    std::sort(xs.begin(), xs.end());
    return xs.at(xs.size() / 2);
  };
  const qint64 wellsMs = medianOf([&] {
    int n = 0;
    for (const CatalogEntity &e : cat.entities(QStringLiteral("well")))
      if (DataCatalog::normalizeWellName(e.name).startsWith(QLatin1Char('w')))
        ++n;
    QCOMPARE(n, 1000);
  });
  const qint64 matchMs = medianOf([&] { cat.wellsMatchingName(QStringLiteral("W999")); });
  const qint64 shaMissMs = medianOf([&] {
    cat.versionBySha256(QStringLiteral("deadbeef")); // miss：无文件 IO
  });
  const qint64 linksMs = medianOf([&] { cat.linksForEntity(QStringLiteral("well-500")); });
  const qint64 currentMs = medianOf([&] { cat.currentVersion(QStringLiteral("ast-1")); });

  clock.start();
  CatalogEntity extra;
  extra.id = QStringLiteral("well-NEW");
  extra.entityType = QStringLiteral("well");
  extra.name = QStringLiteral("NEW");
  QString serr;
  QVERIFY(cat.addEntity(extra, &serr)); // 10001 行全量重写（1k 井 + 2000 资产 + 3001 版本 + 4000 链接）
  const qint64 saveMs = clock.elapsed();

  qInfo("catalog thousand-entity budget: json=%lldKB open=%lldms wells=%lldms "
        "match=%lldms shaMiss=%lldms links=%lldms current=%lldms save=%lldms",
        static_cast<long long>(jsonBytes / 1024), static_cast<long long>(openMs),
        static_cast<long long>(wellsMs), static_cast<long long>(matchMs),
        static_cast<long long>(shaMissMs), static_cast<long long>(linksMs),
        static_cast<long long>(currentMs), static_cast<long long>(saveMs));

  // 预算 = 实测量级的大余量（阈值依据见 docs/progress/data.md；中位数×N
  // 之外的绝对上限防宿主级抖动假红）。
  QVERIFY2(openMs < 2000, qPrintable(QString::number(openMs)));
  QVERIFY2(wellsMs < 100, qPrintable(QString::number(wellsMs)));
  QVERIFY2(matchMs < 100, qPrintable(QString::number(matchMs)));
  QVERIFY2(shaMissMs < 100, qPrintable(QString::number(shaMissMs)));
  QVERIFY2(linksMs < 100, qPrintable(QString::number(linksMs)));
  QVERIFY2(currentMs < 100, qPrintable(QString::number(currentMs)));
  QVERIFY2(saveMs < 2000, qPrintable(QString::number(saveMs)));
}

// ---- 审计 02 M-8 ----

namespace
{
  CatalogEntity wellEntity(const QString &id, const QString &name)
  {
    CatalogEntity e;
    e.id = id;
    e.entityType = QStringLiteral("well");
    e.name = name;
    return e;
  }
} // namespace

// staging 副本上的写入只进副本 + journal（不落盘、不动活 catalog）；
// applyJournal 在活 catalog 上按原序重放、一次落盘（revision +1）。
void TestCatalog::stagingCopyJournalsWithoutTouchingLive()
{
  QTemporaryDir tmp;
  DataCatalog live;
  QString err;
  QVERIFY2(live.open(tmp.path(), &err), qPrintable(err));
  QVERIFY(live.addEntity(wellEntity(QStringLiteral("well-1"), QStringLiteral("A1")), &err));
  const int rev0 = live.catalogRevision();
  const quint64 seq0 = live.mutationSeq();
  const QByteArray disk0 = [&] {
    QFile f(live.catalogPath());
    return f.open(QIODevice::ReadOnly) ? f.readAll() : QByteArray();
  }();

  std::unique_ptr<DataCatalog> st = live.createStagingCopy(tmp.filePath(QStringLiteral("stage")));
  QVERIFY(st->isStaging());
  QVERIFY(st->hasEntity(QStringLiteral("well-1")));
  QVERIFY(st->addEntity(wellEntity(QStringLiteral("well-2"), QStringLiteral("B2")), &err));
  CatalogAsset a;
  a.id = st->nextAssetId();
  a.type = QStringLiteral("well_log");
  a.format = QStringLiteral("las");
  QVERIFY2(st->addAsset(a, &err), qPrintable(err));
  EntityAssetLink l;
  l.entityType = QStringLiteral("well");
  l.entityId = QStringLiteral("well-2");
  l.assetId = a.id;
  l.role = QStringLiteral("well_log");
  l.isPrimary = true;
  QVERIFY2(st->addLink(l, &err), qPrintable(err));
  QCOMPARE(st->journal().size(), 3);

  // 活 catalog 原样：内存、磁盘、revision、mutationSeq。
  QVERIFY(!live.hasEntity(QStringLiteral("well-2")));
  QVERIFY(live.assets().isEmpty());
  QCOMPARE(live.catalogRevision(), rev0);
  QCOMPARE(live.mutationSeq(), seq0);
  {
    QFile f(live.catalogPath());
    QVERIFY(f.open(QIODevice::ReadOnly));
    QCOMPARE(f.readAll(), disk0);
  }

  QSignalSpy changed(&live, &DataCatalog::changed);
  QVERIFY2(live.applyJournal(st->journal(), &err), qPrintable(err));
  QVERIFY(live.hasEntity(QStringLiteral("well-2")));
  QCOMPARE(live.assets().size(), 1);
  QCOMPARE(live.linksForAsset(a.id).size(), 1);
  QCOMPARE(live.catalogRevision(), rev0 + 1); // 整批一次落盘
  QCOMPARE(changed.count(), 1);
  QVERIFY(live.mutationSeq() != seq0);
  QString mismatch;
  QVERIFY2(live.indexHealthy(&mismatch), qPrintable(mismatch));
  // 空 journal：不落盘、revision 不动。
  QVERIFY(live.applyJournal({}, &err));
  QCOMPARE(live.catalogRevision(), rev0 + 1);
}

// 重放中途失败 / 落盘失败 → 活 catalog 内存逐字段还原、错误带序号/原因。
void TestCatalog::applyJournalIsAtomicOnFailure()
{
  QTemporaryDir tmp;
  DataCatalog live;
  QString err;
  QVERIFY2(live.open(tmp.path(), &err), qPrintable(err));
  QVERIFY(live.addEntity(wellEntity(QStringLiteral("well-1"), QStringLiteral("A1")), &err));
  const int rev0 = live.catalogRevision();

  std::unique_ptr<DataCatalog> st = live.createStagingCopy(QString());
  QVERIFY(st->addEntity(wellEntity(QStringLiteral("well-2"), QStringLiteral("B2")), &err));
  QVector<CatalogOp> ops = st->journal();
  CatalogOp dup; // 第二条：重复 id → addEntity 失败
  dup.kind = CatalogOp::Kind::AddEntity;
  dup.entity = wellEntity(QStringLiteral("well-1"), QStringLiteral("dup"));
  ops.append(dup);
  QVERIFY(!live.applyJournal(ops, &err));
  QVERIFY2(err.contains(QStringLiteral("2/2")), qPrintable(err));
  QVERIFY(!live.hasEntity(QStringLiteral("well-2"))); // 第一条已回滚
  QCOMPARE(live.entities().size(), 1);
  QCOMPARE(live.catalogRevision(), rev0);
  QString mismatch;
  QVERIFY2(live.indexHealthy(&mismatch), qPrintable(mismatch));

  // 落盘失败：catalog 目录只读。
  const QString dir = QFileInfo(live.catalogPath()).absolutePath();
  const auto perm = QFile::permissions(dir);
  QVERIFY(QFile::setPermissions(dir, QFileDevice::ReadOwner | QFileDevice::ExeOwner));
  QFile probe(QDir(dir).filePath(QStringLiteral(".probe")));
  if (probe.open(QIODevice::WriteOnly))
  {
    probe.close();
    probe.remove();
    QFile::setPermissions(dir, perm);
    QSKIP("目录权限不生效（root 运行？）");
  }
  err.clear();
  const bool ok = live.applyJournal(st->journal(), &err);
  QFile::setPermissions(dir, perm);
  QVERIFY(!ok);
  QVERIFY(!err.isEmpty());
  QVERIFY(!live.hasEntity(QStringLiteral("well-2")));
  QCOMPARE(live.catalogRevision(), rev0);
  QVERIFY2(live.indexHealthy(&mismatch), qPrintable(mismatch));
  // 权限恢复后同一 journal 可重放成功。
  QVERIFY2(live.applyJournal(st->journal(), &err), qPrintable(err));
  QVERIFY(live.hasEntity(QStringLiteral("well-2")));
}

// 活 catalog 只认所属线程写：别的线程写入被拒 + 计数；staging 副本无亲和。
void TestCatalog::liveWritesRefusedOffOwnerThread()
{
  QTemporaryDir tmp;
  DataCatalog live;
  QString err;
  QVERIFY2(live.open(tmp.path(), &err), qPrintable(err));
  std::unique_ptr<DataCatalog> st = live.createStagingCopy(QString());
  DataCatalog::resetThreadViolationCount();
  bool liveOk = true, stagingOk = false, applyOk = true;
  QString liveErr, stagingErr, applyErr;
  std::thread worker([&] {
    liveOk = live.addEntity(wellEntity(QStringLiteral("well-9"), QStringLiteral("W9")), &liveErr);
    stagingOk =
        st->addEntity(wellEntity(QStringLiteral("well-8"), QStringLiteral("W8")), &stagingErr);
    applyOk = live.applyJournal(st->journal(), &applyErr);
  });
  worker.join();
  QVERIFY(!liveOk);
  QVERIFY(!liveErr.isEmpty());
  QVERIFY2(stagingOk, qPrintable(stagingErr));
  QVERIFY(!applyOk);
  QVERIFY(!applyErr.isEmpty());
  QCOMPARE(DataCatalog::threadViolationCount(), 2);
  QVERIFY(!live.hasEntity(QStringLiteral("well-9")));
  QVERIFY(!live.hasEntity(QStringLiteral("well-8")));
  DataCatalog::resetThreadViolationCount();
}

QTEST_MAIN(TestCatalog)
#include "tst_catalog.moc"
