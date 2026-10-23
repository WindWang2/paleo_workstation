// 层：测试壳
#include "domain/welllogfacies.h"
#include "domain/faciestaxonomy.h"
#include "ai/wellfaciesservice.h"
#include <QSignalSpy>
#include <QtTest>

class WellFaciesContractTests : public QObject {
  Q_OBJECT
private slots:
  void completeVocabularyFrozenBeforePrediction() {
    WellFaciesModel model;
    model.name = QStringLiteral("测试模型");
    model.labels = {QStringLiteral("河口坝"), QStringLiteral("远砂坝")};
    WellFaciesInput input;
    QString error;
    const QVariantList project{FaciesTaxonomy::resolve(FaciesTaxonomy::library(), QStringLiteral("河口坝"))};
    QVERIFY2(bindWellFaciesClasses(model, project, &input, &error), qPrintable(error));
    QCOMPARE(input.facies.size(), 2);
    QVERIFY(!FaciesTaxonomy::resolve(input.facies, QStringLiteral("远砂坝")).isEmpty());
    QVariantList schema;
    QVERIFY2(resolveWellFaciesClasses(input, {"mouth_bar"}, &schema, &error), qPrintable(error));
    QCOMPARE(schema, input.facies); // 未在本次结果出现的类别也有稳定编码。
    QVERIFY(!resolveWellFaciesClasses(input, {QStringLiteral("席状砂")}, &schema, &error));
    QVERIFY(!error.isEmpty());
  }
  void invalidDeclaredVocabulary_data() {
    QTest::addColumn<QStringList>("labels");
    QTest::newRow("missing") << QStringList{};
    QTest::newRow("empty") << QStringList{" "};
    QTest::newRow("unknown") << QStringList{QStringLiteral("河口坝"), QStringLiteral("未知沉积单元")};
    QTest::newRow("parent") << QStringList{QStringLiteral("三角洲相")};
  }
  void invalidDeclaredVocabulary() {
    QFETCH(QStringList, labels);
    WellFaciesModel model;
    model.name = QStringLiteral("类别不全模型"); model.labels = labels;
    WellFaciesInput input;
    input.rows.append(QJsonObject{{"深度", 100.0}});
    QString error;
    QVERIFY(!bindWellFaciesClasses(model, {}, &input, &error));
    QVERIFY(error.contains(model.name));
    WellFaciesService service;
    QSignalSpy failed(&service, &WellFaciesService::failed);
    service.predict(model, "W1", input);
    QCOMPARE(failed.size(), 1); // 在任何网络请求之前拒绝。
  }
  void projectSpecificLeafAccepted() {
    const QVariantList custom{QVariantMap{{"code", 71}, {"name", "地方微相"},
      {"microfacies", "地方微相"}, {"subfacies", "地方亚相"}, {"facies", "地方相"}}};
    WellFaciesModel model; model.labels = {QStringLiteral("地方微相")};
    WellFaciesInput input; QString error;
    QVERIFY2(bindWellFaciesClasses(model, custom, &input, &error), qPrintable(error));
    QVariantList resolved;
    QVERIFY(resolveWellFaciesClasses(input, model.labels, &resolved, &error));
    QCOMPARE(resolved.first().toMap().value("code").toInt(), 71);
  }
  void ambiguousMicrofaciesRequiresFullPath() {
    const QVariantList project{
      QVariantMap{{"code", 71}, {"microfacies", "地方微相"}, {"subfacies", "亚相甲"}, {"facies", "相甲"}},
      QVariantMap{{"code", 72}, {"microfacies", "地方微相"}, {"subfacies", "亚相乙"}, {"facies", "相乙"}}};
    WellFaciesModel model; model.labels = {QStringLiteral("地方微相")};
    WellFaciesInput input; QString error;
    QVERIFY(!bindWellFaciesClasses(model, project, &input, &error));
    model.labels = {QStringLiteral("相甲 / 亚相甲 / 地方微相")};
    QVERIFY2(bindWellFaciesClasses(model, project, &input, &error), qPrintable(error));
  }
};
QTEST_GUILESS_MAIN(WellFaciesContractTests)
#include "tst_wellfaciescontract.moc"
