#include <QtTest>

#include "../src/catalog/catalogindex.h"
#include "../src/catalog/datacatalog.h"

class TestCatalogCatalogIndex : public QObject
{
  Q_OBJECT

private slots:
  void emptyIndexQueries();
  void rebuildAndVerifyConsistency();
  void incrementalEntityAndAssetAddition();
  void incrementalVersionAndShaLookup();
  void incrementalLinkAndMutatedLinks();
  void prefixSequenceTracking();
  void mutationDemonstration_shaNormalization();
};

void TestCatalogCatalogIndex::emptyIndexQueries()
{
  CatalogIndex index;
  QCOMPARE(index.entityRow(QStringLiteral("ent-1")), -1);
  QCOMPARE(index.assetRow(QStringLiteral("ast-1")), -1);
  QCOMPARE(index.versionRow(QStringLiteral("ver-1")), -1);
  QVERIFY(index.versionRowsForAsset(QStringLiteral("ast-1")).isEmpty());
  QVERIFY(index.linkRowsForEntity(QStringLiteral("ent-1")).isEmpty());
  QVERIFY(index.linkRowsForAsset(QStringLiteral("ast-1")).isEmpty());
  QVERIFY(index.entityRowsByType(QStringLiteral("well")).isEmpty());
  QVERIFY(index.childVersionIds(QStringLiteral("ver-1")).isEmpty());
  QVERIFY(index.versionRowsForSha(QStringLiteral("abc")).isEmpty());
  QCOMPARE(index.maxEntitySeqForPrefix(QStringLiteral("well")), 0);
  QCOMPARE(index.entityCountByType(QStringLiteral("well")), 0);
  QCOMPARE(index.linkCount(), 0);
}

void TestCatalogCatalogIndex::rebuildAndVerifyConsistency()
{
  QVector<CatalogEntity> entities;
  CatalogEntity e1;
  e1.id = QStringLiteral("well-1");
  e1.entityType = QStringLiteral("well");
  entities.append(e1);

  CatalogEntity e2;
  e2.id = QStringLiteral("seismic-1");
  e2.entityType = QStringLiteral("seismic_line");
  entities.append(e2);

  QVector<CatalogAsset> assets;
  CatalogAsset a1;
  a1.id = QStringLiteral("ast-1");
  a1.type = QStringLiteral("well_log");
  assets.append(a1);

  QVector<CatalogVersion> versions;
  CatalogVersion v1;
  v1.id = QStringLiteral("v-1");
  v1.assetId = QStringLiteral("ast-1");
  v1.sha256 = QStringLiteral("abcdef1234567890");
  versions.append(v1);

  CatalogVersion v2;
  v2.id = QStringLiteral("v-2");
  v2.assetId = QStringLiteral("ast-1");
  v2.parentVersionIds = QStringList{QStringLiteral("v-1")};
  v2.sha256 = QStringLiteral("0987654321fedcba");
  versions.append(v2);

  QVector<EntityAssetLink> links;
  EntityAssetLink l1;
  l1.entityId = QStringLiteral("well-1");
  l1.assetId = QStringLiteral("ast-1");
  l1.isPrimary = true;
  links.append(l1);

  CatalogIndex index;
  index.rebuild(entities, assets, versions, links);

  QString mismatch;
  const bool verified = index.verifyAgainst(entities, assets, versions, links, &mismatch);
  QVERIFY2(verified, qPrintable(mismatch));

  QCOMPARE(index.entityRow(QStringLiteral("well-1")), 0);
  QCOMPARE(index.entityRow(QStringLiteral("seismic-1")), 1);
  QCOMPARE(index.assetRow(QStringLiteral("ast-1")), 0);
  QCOMPARE(index.versionRow(QStringLiteral("v-2")), 1);
  QCOMPARE(index.childVersionIds(QStringLiteral("v-1")), QStringList{QStringLiteral("v-2")});
  QCOMPARE(index.linkCount(), 1);
}

void TestCatalogCatalogIndex::incrementalEntityAndAssetAddition()
{
  CatalogIndex index;
  index.entityAdded(0, QStringLiteral("well-1"), QStringLiteral("well"));
  index.entityAdded(1, QStringLiteral("well-2"), QStringLiteral("well"));
  index.entityAdded(2, QStringLiteral("flt-1"), QStringLiteral("fault"));

  QCOMPARE(index.entityRow(QStringLiteral("well-1")), 0);
  QCOMPARE(index.entityRow(QStringLiteral("well-2")), 1);
  QCOMPARE(index.entityRow(QStringLiteral("flt-1")), 2);
  QCOMPARE(index.entityCountByType(QStringLiteral("well")), 2);
  QCOMPARE(index.entityCountByType(QStringLiteral("fault")), 1);
  QCOMPARE(index.entityRowsByType(QStringLiteral("well")), QVector<int>({0, 1}));

  index.assetAdded(0, QStringLiteral("ast-100"), QStringLiteral("well_log"));
  QCOMPARE(index.assetRow(QStringLiteral("ast-100")), 0);
  QCOMPARE(index.assetRow(QStringLiteral("ast-nonexistent")), -1);
}

void TestCatalogCatalogIndex::incrementalVersionAndShaLookup()
{
  CatalogIndex index;
  CatalogVersion v1;
  v1.id = QStringLiteral("v-10");
  v1.assetId = QStringLiteral("ast-A");
  v1.sha256 = QStringLiteral("A1B2C3D4");

  CatalogVersion v2;
  v2.id = QStringLiteral("v-11");
  v2.assetId = QStringLiteral("ast-A");
  v2.parentVersionIds = QStringList{QStringLiteral("v-10")};
  v2.sha256 = QStringLiteral("a1b2c3d4"); // 相同哈希（不同大小写）

  index.versionAdded(0, v1);
  index.versionAdded(1, v2);

  QCOMPARE(index.versionRowsForAsset(QStringLiteral("ast-A")), QVector<int>({0, 1}));
  // 大小写归一化检索
  QCOMPARE(index.versionRowsForSha(QStringLiteral("a1b2c3d4")), QVector<int>({0, 1}));
  QCOMPARE(index.childVersionIds(QStringLiteral("v-10")), QStringList{QStringLiteral("v-11")});
}

void TestCatalogCatalogIndex::incrementalLinkAndMutatedLinks()
{
  CatalogIndex index;
  EntityAssetLink l1;
  l1.entityId = QStringLiteral("e-1");
  l1.assetId = QStringLiteral("a-1");
  index.linkAdded(0, l1);

  QCOMPARE(index.linkCount(), 1);
  QCOMPARE(index.linkRowsForEntity(QStringLiteral("e-1")), QVector<int>({0}));
  QCOMPARE(index.linkRowsForAsset(QStringLiteral("a-1")), QVector<int>({0}));

  // 全表重挂
  EntityAssetLink l2;
  l2.entityId = QStringLiteral("e-1");
  l2.assetId = QStringLiteral("a-2");
  QVector<EntityAssetLink> newLinks{l1, l2};
  index.linksMutated(newLinks);

  QCOMPARE(index.linkCount(), 2);
  QCOMPARE(index.linkRowsForEntity(QStringLiteral("e-1")), QVector<int>({0, 1}));
  QCOMPARE(index.linkRowsForAsset(QStringLiteral("a-2")), QVector<int>({1}));
}

void TestCatalogCatalogIndex::prefixSequenceTracking()
{
  CatalogIndex index;
  index.entityAdded(0, QStringLiteral("well-1"), QStringLiteral("well"));
  index.entityAdded(1, QStringLiteral("well-15"), QStringLiteral("well"));
  index.entityAdded(2, QStringLiteral("well-03"), QStringLiteral("well"));
  index.entityAdded(3, QStringLiteral("well-invalid"), QStringLiteral("well"));
  index.entityAdded(4, QStringLiteral("seismic-9"), QStringLiteral("seismic"));

  QCOMPARE(index.maxEntitySeqForPrefix(QStringLiteral("well")), 15);
  QCOMPARE(index.maxEntitySeqForPrefix(QStringLiteral("seismic")), 9);
  QCOMPARE(index.maxEntitySeqForPrefix(QStringLiteral("horizon")), 0);
}

void TestCatalogCatalogIndex::mutationDemonstration_shaNormalization()
{
  CatalogIndex index;
  CatalogVersion v;
  v.id = QStringLiteral("ver-test");
  v.assetId = QStringLiteral("ast-1");
  v.sha256 = QStringLiteral("CAFEBABE");
  index.versionAdded(0, v);

  // 必须能通过小写 sha 查询命中
  const QVector<int> hits = index.versionRowsForSha(QStringLiteral("cafebabe"));
  QCOMPARE(hits.size(), 1);
  QCOMPARE(hits.at(0), 0);
}

QTEST_GUILESS_MAIN(TestCatalogCatalogIndex)
#include "tst_catalog_catalogindex.moc"
