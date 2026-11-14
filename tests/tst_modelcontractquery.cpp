// 层：测试壳
#include <QtTest>
#include "ai/onnxfixture.h"
#include "ai/onnxpredictionservice.h"
#include <QFile>
#include <QJsonDocument>
#include <QTemporaryDir>

namespace {
ModelRegistryEntry entry(const QString &name) {
  ModelRegistryEntry value;value.name=name;value.file=name+".onnx";
  value.absolutePath="/private/project/models/"+value.file;value.version="2.3.4";
  value.sha256Pinned=QString(64,QLatin1Char('a'));value.sha256Actual=value.sha256Pinned;
  value.inputName="x";value.inputDtype="float32";value.inputShape={1,1,-1,-1};
  value.outputSemantics="logits";value.classes=3;value.featureContractVersion=1;
  value.features={QJsonObject{{"kind","scalar"},{"unit","mV"},{"domain","time"},{"preprocessing","raw_float32_v1"}}};
  return value;
}
ModelRegistryScan registry(int count=3) {
  ModelRegistryScan scan;scan.manifestFound=true;
  for(int i=count-1;i>=0;--i)scan.entries.append(entry(QString("model%1").arg(i,5,10,QLatin1Char('0'))));
  return scan;
}
QString code(const QJsonObject &value) {return value.value("code").toString();}
QJsonObject first(const QJsonObject &value) {return value.value("models").toArray().first().toObject();}
}
class TestModelContractQuery : public QObject {
  Q_OBJECT
private slots:
  void orderedKeysetPagesDoNotNeedModelFiles() {
    PaleoOnnxService service;const auto scan=registry(10000);service.setModelRegistry(scan);
    service.m_modelContractClock=[]{return qint64(100000);};
    auto page=service.modelContractPage({}, {}, "doc:1/chat:a",3);
    QVERIFY(page.value("ok").toBool());QCOMPARE(page.value("count").toInt(),10000);
    QCOMPARE(page.value("returnedCount").toInt(),3);QCOMPARE(first(page).value("name").toString(),QString("model00000"));
    const auto cursor=page.value("nextCursor").toString();QVERIFY(!cursor.isEmpty());
    const auto next=service.modelContractPage({},cursor,"doc:1/chat:a",3);
    QCOMPARE(first(next).value("name").toString(),QString("model00003"));
    QCOMPARE(service.modelContractPage({},cursor,"doc:1/chat:a",3),next);
    QCOMPARE(service.sessionPoolSize(),0);QCOMPARE(service.lastLoadStatus(),OnnxLoadStatus::NotFound);
    auto exact=service.modelContractPage("model09999",{},"doc:1/chat:a",3);
    QCOMPARE(exact.value("matchedCount").toInt(),1);QVERIFY(!exact.value("hasMore").toBool());
    QCOMPARE(first(exact).value("name").toString(),QString("model09999"));
    QCOMPARE(service.modelContractPage("MODEL09999",{},"doc:1/chat:a").value("matchedCount").toInt(),0);
  }
  void scopeEpochQueryAndExpiryAreDistinct() {
    PaleoOnnxService service;const auto scan=registry();service.setModelRegistry(scan);
    qint64 now=100000;service.m_modelContractClock=[&]{return now;};
    const auto cursor=service.modelContractPage({}, {}, "doc:a/chat:a",1).value("nextCursor").toString();
    QVERIFY(!cursor.isEmpty());
    QCOMPARE(code(service.modelContractPage({},cursor,"doc:a/chat:b",1)),QString("cursor_scope_mismatch"));
    QCOMPARE(code(service.modelContractPage("model00002",cursor,"doc:a/chat:a",1)),QString("cursor_query_mismatch"));
    QCOMPARE(code(service.modelContractPage({},cursor,"doc:a/chat:a",2)),QString("cursor_query_mismatch"));
    auto corrupted=cursor;corrupted[corrupted.size()-1]=corrupted.back()==QLatin1Char('a')?QLatin1Char('b'):QLatin1Char('a');
    QCOMPARE(code(service.modelContractPage({},corrupted,"doc:a/chat:a",1)),QString("cursor_invalid"));
    QCOMPARE(code(service.modelContractPage({},QString(100000,QLatin1Char('x')),"doc:a/chat:a",1)),QString("cursor_invalid"));
    now+=30*60*1000+1;
    QCOMPARE(code(service.modelContractPage({},cursor,"doc:a/chat:a",1)),QString("cursor_expired"));
    service.setModelRegistry(scan);
    QCOMPARE(code(service.modelContractPage({},cursor,"doc:a/chat:a",1)),QString("cursor_stale"));
  }
  void wholePageByteBudgetContinuesWithoutClipping() {
    PaleoOnnxService service;auto scan=registry();
    for(auto &model:scan.entries)model.features={QJsonObject{{"kind",QString(300,QChar(0x6c49))},{"unit","mV"}}};
    service.setModelRegistry(scan);qint64 now=100000;service.m_modelContractClock=[&]{return now;};
    QString cursor;QSet<QString> names;
    do {
      const auto page=service.modelContractPage({},cursor,"scope",64,3000);
      QVERIFY2(page.value("ok").toBool(),qPrintable(code(page)));
      const auto bytes=QJsonDocument(page).toJson(QJsonDocument::Compact);QVERIFY(bytes.size()<=3000);
      QJsonParseError error;QVERIFY(QJsonDocument::fromJson(bytes,&error).isObject());QCOMPARE(error.error,QJsonParseError::NoError);
      for(const auto &value:page.value("models").toArray()) {const auto name=value.toObject().value("name").toString();QVERIFY(!names.contains(name));names.insert(name);}
      cursor=page.value("nextCursor").toString();
      QCOMPARE(page.value("hasMore").toBool(),!cursor.isEmpty());
    } while(!cursor.isEmpty());
    QCOMPARE(names.size(),3);
    QCOMPARE(code(service.modelContractPage({}, {},"scope",1,1024)),QString("model_entry_byte_budget_exceeded"));
    QCOMPARE(code(service.modelContractPage({}, {},"scope",65)),QString("invalid_query_budget"));
    QCOMPARE(code(service.modelContractPage({}, {},"scope",1,32769)),QString("invalid_query_budget"));
  }
  void declarationProjectionPreservesUnitsAndHasNoPathsOrReadinessClaim() {
    QTemporaryDir dir;QVERIFY(dir.isValid());
    QVERIFY(OnnxFixtureWriter::writeAddScalar(dir.filePath("model.onnx"),40));
    auto model=entry("model");model.file="model.onnx";model.sha256Pinned=PaleoOnnxService::sha256OfFile(dir.filePath(model.file));
    const QJsonObject manifest{{"models",QJsonArray{QJsonObject{{"name","model"},{"file",model.file},
      {"version","1.2"},{"sha256",model.sha256Pinned},{"input",QJsonObject{{"dtype","float32"},{"shape",QJsonArray{1}},
      {"featureContractVersion",1},{"features",QJsonArray{QJsonObject{{"kind","unknown-scientific-feature"},{"unit","mV"}}}}}}}}}};
    QFile file(dir.filePath("manifest.json"));QVERIFY(file.open(QIODevice::WriteOnly));file.write(QJsonDocument(manifest).toJson());file.close();
    const auto scan=ModelRegistry::scan(dir.path());QCOMPARE(scan.entries.size(),1);QCOMPARE(scan.entries.first().status,ModelRegistryEntry::Status::Ok);
    PaleoOnnxService service;service.setModelRoot(dir.path());service.setModelRegistry(scan);
    QVERIFY(QFile::remove(dir.filePath("model.onnx")));QVERIFY(QFile::remove(dir.filePath("manifest.json")));
    const auto page=service.modelContractPage({}, {},"scope");QVERIFY(page.value("ok").toBool());
    const auto value=first(page);QCOMPARE(value.value("weightsSHA").toString(),model.sha256Pinned);
    const auto feature=value.value("input").toObject().value("features").toArray().first().toObject();
    QCOMPARE(feature.value("unit").toString(),QString("mV"));QCOMPARE(feature.value("kind").toString(),QString("unknown-scientific-feature"));
    QVERIFY(value.value("metadataOnly").toBool());QVERIFY(value.value("requires_prepare").toBool());QVERIFY(!value.value("runtimeVerified").toBool());
    QCOMPARE(value.value("availabilityReason").toString(),QString("runtime_validation_not_performed"));
    const auto bytes=QJsonDocument(page).toJson(QJsonDocument::Compact);
    QVERIFY(!bytes.contains(dir.path().toUtf8()));QVERIFY(!bytes.contains("absolutePath"));QVERIFY(!bytes.contains("model.onnx"));
    QCOMPARE(service.sessionPoolSize(),0);
  }
  void missingContractAndRejectedCountsRemainHonest() {
    PaleoOnnxService service;auto scan=registry(1);
    scan.entries[0].features={};scan.entries[0].featureContractVersion=0;scan.entries[0].sha256Pinned.clear();
    auto bad=entry("rejected");bad.status=ModelRegistryEntry::Status::FileMissing;scan.entries.append(bad);
    bad.name.clear();scan.entries.append(bad);service.setModelRegistry(scan);
    const auto page=service.modelContractPage({}, {},"scope");QVERIFY(page.value("ok").toBool());
    QCOMPARE(page.value("entryFilter").toString(),QString("manifest_status_ok"));QCOMPARE(page.value("count").toInt(),1);
    QCOMPARE(page.value("rejectedCount").toInt(),2);const auto value=first(page);QVERIFY(value.value("weightsSHA").isNull());
    const auto issues=value.value("declarationIssues").toArray();QVERIFY(issues.contains("model_feature_contract_unavailable"));QVERIFY(issues.contains("model_fingerprint_unpinned"));
    service.setModelRegistry({});QCOMPARE(code(service.modelContractPage({}, {},"scope")),QString("model_registry_unavailable"));
    scan.manifestError="private/path/error";service.setModelRegistry(scan);
    const auto error=service.modelContractPage({}, {},"scope");QCOMPARE(code(error),QString("model_registry_unavailable"));
    QVERIFY(!QJsonDocument(error).toJson().contains("private/path"));
  }
  void oversizedOrUnsupportedFeatureMetadataIsRejectedBeforeSerialization() {
    PaleoOnnxService service;auto scan=registry(1);
    scan.entries[0].features={QJsonObject{{"kind",QString(8*1024*1024,QLatin1Char('x'))}}};
    service.setModelRegistry(scan);
    QCOMPARE(code(service.modelContractPage({}, {},"scope")),QString("model_metadata_budget_or_fields_exceeded"));
    scan.entries[0].features={QJsonObject{{"path","/private/model"}}};service.setModelRegistry(scan);
    const auto error=service.modelContractPage({}, {},"scope");QCOMPARE(code(error),QString("model_metadata_budget_or_fields_exceeded"));
    QVERIFY(!QJsonDocument(error).toJson().contains("/private/model"));
    QJsonArray features;for(int i=0;i<65;++i)features.append(QJsonObject{{"kind","scalar"}});
    scan.entries[0].features=features;service.setModelRegistry(scan);
    QCOMPARE(code(service.modelContractPage({}, {},"scope")),QString("model_metadata_budget_or_fields_exceeded"));
    features={};for(int i=0;i<10;++i)features.append(QJsonObject{{"kind",QString(500,QLatin1Char('x'))},{"unit",QString(500,QLatin1Char('y'))}});
    scan.entries[0].features=features;service.setModelRegistry(scan);
    QCOMPARE(code(service.modelContractPage({}, {},"scope")),QString("model_metadata_budget_or_fields_exceeded"));
  }
  void registryReadHasAnIndependentByteAndRecordBound() {
    QTemporaryDir dir;QFile file(dir.filePath("manifest.json"));QVERIFY(file.open(QIODevice::WriteOnly));
    QVERIFY(file.resize(8*1024*1024+1));file.close();
    auto scan=ModelRegistry::scan(dir.path());QVERIFY(scan.manifestFound);
    QCOMPARE(scan.manifestError,QString("model_manifest_byte_budget_exceeded"));QVERIFY(scan.entries.isEmpty());
    QJsonArray models;for(int i=0;i<4097;++i)models.append(QJsonObject{{"name",QString::number(i)},{"file","missing.onnx"}});
    QVERIFY(file.open(QIODevice::WriteOnly|QIODevice::Truncate));file.write(QJsonDocument(QJsonObject{{"models",models}}).toJson());file.close();
    scan=ModelRegistry::scan(dir.path());QCOMPARE(scan.manifestError,QString("model_manifest_entry_budget_exceeded"));QVERIFY(scan.entries.isEmpty());
  }
};
QTEST_GUILESS_MAIN(TestModelContractQuery)
#include "tst_modelcontractquery.moc"
