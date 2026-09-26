#include <QtTest>
#include <QTemporaryDir>
#include <QJsonDocument>
#include <QJsonObject>

#include "../src/catalog/datacatalog.h"

// plan §3 数据模型：实体—关联—资产—版本，catalog.json 是唯一主存储。
// 覆盖：JSON round-trip、井名规范化身份解析、双候选不合并（unresolved 语义）、
// 版本不可变递增、revision 单调。
class TestCatalog : public QObject
{
  Q_OBJECT

private slots:
  void roundTripsThroughJson();
  void normalizesWellName();
  void resolvesWellByNameAndAlias();
  void ambiguousNameYieldsBothCandidates();
  void unresolvedLinkRoundTripsWithEmptyEntityId();
  void resolvedLinkStillNeedsEntityId();
  void currentVersionPicksHighestNumber();
  void managedPathLayout();
  void unsafePathSegmentsRejected();
  void versionBySha256FindsStored();
  void addLinkDemotesPreviousPrimaryForSameRole();
  void attachLinkResolvesUnresolvedAndDemotes();
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

void TestCatalog::resolvesWellByNameAndAlias()
{
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
  a2.aliases = QStringList{QStringLiteral("Well-Two")};
  QVERIFY(cat.addEntity(a2));

  QCOMPARE(cat.wellsMatchingName(QStringLiteral("a1")), QStringList{QStringLiteral("well-A1")});
  QCOMPARE(cat.wellsMatchingName(QStringLiteral("well-two")),
           QStringList{QStringLiteral("well-A2")});
  QVERIFY(cat.wellsMatchingName(QStringLiteral("A3")).isEmpty());
}

void TestCatalog::ambiguousNameYieldsBothCandidates()
{
  QTemporaryDir dir;
  QVERIFY(dir.isValid());
  DataCatalog cat;
  QVERIFY(cat.open(dir.path()));

  CatalogEntity w1;
  w1.id = QStringLiteral("well-W1");
  w1.entityType = QStringLiteral("well");
  w1.name = QStringLiteral("W1");
  w1.aliases = QStringList{QStringLiteral("duplicate")};
  QVERIFY(cat.addEntity(w1));
  CatalogEntity w2;
  w2.id = QStringLiteral("well-W2");
  w2.entityType = QStringLiteral("well");
  w2.name = QStringLiteral("W2");
  w2.aliases = QStringList{QStringLiteral("duplicate")};
  QVERIFY(cat.addEntity(w2));

  // 双候选必须原样返回两个 id——调用方据此写 unresolved 链接，不并井。
  const QStringList cands = cat.wellsMatchingName(QStringLiteral("duplicate"));
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
  CatalogVersion v;
  v.id = QStringLiteral("ver-1");
  v.assetId = QStringLiteral("ast-1");
  v.stage = QStringLiteral("RAW");
  v.sha256 = QStringLiteral("abc123");
  QVERIFY(cat.addVersion(v));

  QCOMPARE(cat.versionBySha256(QStringLiteral("abc123")).id, QStringLiteral("ver-1"));
  QCOMPARE(cat.versionBySha256(QStringLiteral("ABC123")).id, QStringLiteral("ver-1"));
  QVERIFY(cat.versionBySha256(QStringLiteral("zzz")).id.isEmpty());
  QVERIFY(cat.versionBySha256(QString()).id.isEmpty()); // 空 sha 永不命中
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

QTEST_MAIN(TestCatalog)
#include "tst_catalog.moc"
