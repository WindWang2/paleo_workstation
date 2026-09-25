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

QTEST_MAIN(TestCatalog)
#include "tst_catalog.moc"
