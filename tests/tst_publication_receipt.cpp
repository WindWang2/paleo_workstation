// 层：测试壳
#include <QtTest>
#include <QJsonArray>
#include <QJsonDocument>
#include <QSqlDatabase>
#include <QSqlQuery>
#include <QTemporaryDir>
#include <QUuid>
#include "catalog/catalogstore.h"
#include "metadata/projectlock.h"

namespace {
PublicationReceipt receipt(const QString &operation = QStringLiteral("op-one"),
                           const QString &set = QStringLiteral("set-one"), const QString &suffix = QStringLiteral("one")) {
  const QJsonObject member{{"portId", "OUTPUT"}, {"assetId", "asset-" + suffix}, {"versionId", "version-" + suffix},
      {"path", "artifacts/derived/asset-" + suffix + "/version-" + suffix + "/output.bin"},
      {"sha256", QString(64, 'a')}, {"expectedHead", ""}, {"byteSize", "4"}};
  PublicationReceipt out{{{"schema", 2}, {"ready", true}, {"code", "committed"},
      {"documentId", "doc"}, {"operationId", operation}, {"outputSetId", set},
      {"algorithmId", "test:one"}, {"requestHash", QString(64, 'b')},
      {"planHash", QString(64, 'c')}, {"members", QJsonArray{member}}}};
  out.value.insert("manifestHash", PublicationReceipt::hash(out.value));
  return out;
}

bool stage(CatalogStore &store, const PublicationReceipt &proof, QString &error) {
  const auto member = proof.value.value("members").toArray().first().toObject();
  CatalogAsset asset;
  asset.id = member.value("assetId").toString(); asset.type = "test"; asset.displayName = asset.id;
  CatalogVersion version;
  version.id = member.value("versionId").toString(); version.assetId = asset.id;
  version.stage = "DERIVED"; version.managed = true; version.path = member.value("path").toString();
  version.fileName = "output.bin"; version.sha256 = member.value("sha256").toString();
  version.versionNumber = 1; version.extra.insert("outputSet", proof.value.toVariantMap());
  return store.begin(&error) && store.upsertAsset(asset, &error) && store.upsertVersion(version, &error) &&
         store.insertPublication(proof, &error);
}

bool sql(const QString &path, const QString &statement) {
  const auto name = QUuid::createUuid().toString();
  bool ok;
  {
    auto db = QSqlDatabase::addDatabase("QSQLITE", name); db.setDatabaseName(path);
    ok = db.open();
    if (ok) { QSqlQuery query(db); ok = query.exec(statement); }
    db.close();
  }
  QSqlDatabase::removeDatabase(name);
  return ok;
}
}

class TestPublicationReceipt : public QObject {
  Q_OBJECT
private slots:
  void atomicCommitAndReload();
  void rollbackIsAuthoritativeAbsence();
  void operationAndOutputSetAreImmutable();
  void corruptProofAndMembersDoNotBecomeAbsence();
  void rewritesKeepProofAndRecoveryChangesNamespace();
  void legacyAndClosedStoresAreNotAbsence();
  void claimsRetainPhysicalOwnership();
  void malformedReceiptsAreRejected();
};

void TestPublicationReceipt::atomicCommitAndReload() {
  QTemporaryDir dir;
  CatalogStore store; CatalogStore::Tables tables; QString error;
  QVERIFY2(store.openProject(dir.path(), false, &tables, &error), qPrintable(error));
  const auto proof = receipt();
  QVERIFY(proof.valid());
  const auto before = store.publication("doc", "op-one", proof.requestHash());
  QCOMPARE(before.code, QStringLiteral("not_committed"));
  QVERIFY(!before.storeGeneration.isEmpty());
  QVERIFY2(stage(store, proof, error), qPrintable(error));
  QVERIFY(store.commit(&error));
  QCOMPARE(store.publication("doc", "op-one", proof.requestHash()).code, QStringLiteral("committed"));
  store.close();
  QVERIFY2(store.openProject(dir.path(), false, &tables, &error), qPrintable(error));
  QCOMPARE(tables.publications.size(), 1);
  QCOMPARE(tables.publications.first(), proof);
  QCOMPARE(tables.versions.size(), 1);
  QCOMPARE(tables.publicationGeneration, before.storeGeneration);
  QCOMPARE(store.publication("doc", "op-one", QString(64, 'd')).code, QStringLiteral("conflict"));
}

void TestPublicationReceipt::rollbackIsAuthoritativeAbsence() {
  QTemporaryDir dir;
  CatalogStore store; CatalogStore::Tables tables; QString error;
  QVERIFY(store.openProject(dir.path(), false, &tables, &error));
  const auto proof = receipt();
  const auto generation = store.publication("doc", "op-one", proof.requestHash()).storeGeneration;
  QVERIFY2(stage(store, proof, error), qPrintable(error));
  store.rollback();
  const auto missing = store.publication("doc", "op-one", proof.requestHash());
  QCOMPARE(missing.code, QStringLiteral("not_committed"));
  QCOMPARE(missing.storeGeneration, generation);
  store.close();
  QVERIFY(store.openProject(dir.path(), false, &tables, &error));
  QVERIFY(tables.publications.isEmpty()); QVERIFY(tables.versions.isEmpty()); QVERIFY(tables.assets.isEmpty());
}

void TestPublicationReceipt::operationAndOutputSetAreImmutable() {
  QTemporaryDir dir;
  CatalogStore store; CatalogStore::Tables tables; QString error;
  QVERIFY(store.openProject(dir.path(), false, &tables, &error));
  const auto first = receipt();
  QVERIFY(stage(store, first, error)); QVERIFY(store.commit(&error));
  QVERIFY(store.begin(&error)); QVERIFY(store.insertPublication(first, &error)); QVERIFY(store.commit(&error));
  const auto changed = receipt("op-one", "set-two", "two");
  QVERIFY(!stage(store, changed, error)); store.rollback();
  const auto duplicateSet = receipt("op-two", "set-one", "two");
  QVERIFY(!stage(store, duplicateSet, error)); store.rollback();
  store.close();
  QVERIFY(store.openProject(dir.path(), false, &tables, &error));
  QCOMPARE(tables.publications.size(), 1); QCOMPARE(tables.versions.size(), 1);
}

void TestPublicationReceipt::corruptProofAndMembersDoNotBecomeAbsence() {
  QTemporaryDir dir;
  CatalogStore store; CatalogStore::Tables tables; QString error;
  QVERIFY(store.openProject(dir.path(), false, &tables, &error));
  const auto proof = receipt(); QVERIFY(stage(store, proof, error)); QVERIFY(store.commit(&error));
  QVERIFY(sql(store.sqlitePath(), "UPDATE versions SET sha256='changed' WHERE id='version-one'"));
  QCOMPARE(store.publication("doc", "op-one", proof.requestHash()).code, QStringLiteral("receipt_corrupt"));
  QVERIFY(sql(store.sqlitePath(), "UPDATE paleo_operation_publications SET receipt_json='{}'"));
  QCOMPARE(store.publication("doc", "op-one", proof.requestHash()).code, QStringLiteral("receipt_corrupt"));
}

void TestPublicationReceipt::rewritesKeepProofAndRecoveryChangesNamespace() {
  QTemporaryDir dir;
  CatalogStore store; CatalogStore::Tables tables; QString error;
  QVERIFY(store.openProject(dir.path(), false, &tables, &error));
  const auto proof = receipt(); QVERIFY(stage(store, proof, error)); QVERIFY(store.commit(&error));
  const auto original = store.publication("doc", "op-one", proof.requestHash()).storeGeneration;
  store.close(); QVERIFY(store.openProject(dir.path(), false, &tables, &error));
  QVERIFY2(store.rewritePrimary(tables, false, &error), qPrintable(error));
  auto found = store.publication("doc", "op-one", proof.requestHash());
  QCOMPARE(found.code, QStringLiteral("committed")); QCOMPARE(found.storeGeneration, original);
  QVERIFY2(store.rewritePrimary(tables, true, &error), qPrintable(error));
  found = store.publication("doc", "op-one", proof.requestHash());
  QCOMPARE(found.code, QStringLiteral("committed")); QVERIFY(found.storeGeneration != original);
  QCOMPARE(store.publication("doc", "unseen", proof.requestHash()).storeGeneration, found.storeGeneration);
}

void TestPublicationReceipt::legacyAndClosedStoresAreNotAbsence() {
  QTemporaryDir dir;
  CatalogStore store; CatalogStore::Tables tables; QString error;
  QVERIFY(store.openProject(dir.path(), false, &tables, &error));
  const auto path = store.sqlitePath(); store.close();
  QCOMPARE(store.publication("doc", "op", QString(64, 'a')).code, QStringLiteral("unavailable"));
  QVERIFY(sql(path, "DROP TABLE paleo_operation_publications"));
  QVERIFY(store.openProject(dir.path(), true, &tables, &error));
  QCOMPARE(store.publication("doc", "op", QString(64, 'a')).code, QStringLiteral("legacy_unverifiable"));
}

void TestPublicationReceipt::claimsRetainPhysicalOwnership() {
  QTemporaryDir dir, other;
  auto owner = std::make_unique<ProjectDirLock>(dir.path()); QString error;
  QVERIFY(owner->tryLock(&error));
  auto claim = owner->claim(); QVERIFY(claim); QVERIFY(claim->isHeldFor(dir.path()));
  QVERIFY(!claim->isHeldFor(other.path()));
  owner.reset();
  ProjectDirLock contender(dir.path());
  QVERIFY(!contender.tryLock(&error));
  QVERIFY(claim->isHeldFor(dir.path()));
  claim.reset();
  QVERIFY2(contender.tryLock(&error), qPrintable(error));
}

void TestPublicationReceipt::malformedReceiptsAreRejected() {
  auto proof = receipt(); QString error;
  proof.value.insert("manifestHash", QString(64, '0')); QVERIFY(!proof.valid(&error));
  proof = receipt(); proof.value.insert("schema", 1); QVERIFY(!proof.valid(&error));
  proof = receipt(); auto member = proof.value.value("members").toArray().first().toObject();
  member.insert("path", "../escape.bin"); proof.value.insert("members", QJsonArray{member});
  proof.value.insert("manifestHash", PublicationReceipt::hash(proof.value)); QVERIFY(!proof.valid(&error));
}

QTEST_GUILESS_MAIN(TestPublicationReceipt)
#include "tst_publication_receipt.moc"
