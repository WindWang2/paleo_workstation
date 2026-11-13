// 层：测试壳
#include <QtTest>
#include "domain/wellref.h"

using namespace paleo::document;
class TestWellRef : public QObject {
  Q_OBJECT
private slots:
  void preservesLogicalStoreAndSdkIdentitySeparately() {
    WellRef well;
    well.documentId="document";well.catalogEntityId="catalog-well";well.modelStoreId="area-model";
    well.modelStoreIdentity="store-uuid";well.modelGeneration="generation";
    well.modelWellId=9223372036854775806LL;well.queryHead=9223372036854775805LL;well.modelAvailable=true;
    WellRef decoded;QString code;
    QVERIFY(wellRefFromJson(wellRefJson(well),decoded,&code));
    QVERIFY(code.isEmpty());
    QVERIFY(decoded==well);
    QCOMPARE(wellRefJson(well).value("model_well_id"),QJsonValue("9223372036854775806"));
    QCOMPARE(wellRefJson(well).value("model_store_id"),QJsonValue("area-model"));
    QCOMPARE(wellRefJson(well).value("model_store_identity"),QJsonValue("store-uuid"));
  }
  void unavailableCatalogWellIsNotAValueReference() {
    WellRef well;
    well.documentId="document";well.catalogEntityId="catalog-only";well.modelStoreId="area-model";
    WellRef decoded;
    QVERIFY(wellRefFromJson(wellRefJson(well),decoded));
    QVERIFY(decoded.valid());
    QVERIFY(!decoded.modelAvailable);
    QCOMPARE(decoded.modelWellId,0);
    QCOMPARE(decoded.queryHead,-1);
  }
  void rejectsAliasedOrRoundedIdentity_data() {
    QTest::addColumn<QJsonValue>("value");
    QTest::newRow("json-number")<<QJsonValue(9007199254740992.0);
    QTest::newRow("leading-zero")<<QJsonValue("01");
    QTest::newRow("plus")<<QJsonValue("+1");
    QTest::newRow("overflow")<<QJsonValue("9223372036854775808");
    QTest::newRow("negative")<<QJsonValue("-1");
    QTest::newRow("whitespace")<<QJsonValue("1 ");
  }
  void rejectsAliasedOrRoundedIdentity() {
    QFETCH(QJsonValue,value);
    WellRef well;
    well.documentId="doc";well.catalogEntityId="well";well.modelStoreId="area-model";
    well.modelStoreIdentity="sdk";well.modelGeneration="gen";well.modelWellId=1;well.queryHead=0;well.modelAvailable=true;
    auto json=wellRefJson(well);json.insert("model_well_id",value);
    WellRef decoded;QString code;
    QVERIFY(!wellRefFromJson(json,decoded,&code));
    QCOMPARE(code,QStringLiteral("invalid_well_ref"));
  }
};
QTEST_GUILESS_MAIN(TestWellRef)
#include "tst_wellref.moc"
