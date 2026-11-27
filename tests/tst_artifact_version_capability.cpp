// 层：测试壳
#include <QtTest>
#include <QJsonArray>
#include <QJsonDocument>
#include <QSignalSpy>
#include <QTemporaryDir>
#include "../src/catalog/datacatalog.h"
#include "../src/services/artifactversioncapability.h"
#include "../src/ai/chat/domaintools.h"
#include "../src/workflow/aichattoolrunner.h"
#include "../src/workflow/analysisresultsync.h"

using paleo::capability::ArtifactVersionCapability;
using paleo::capability::ArtifactVersionRequest;
namespace {
bool seed(DataCatalog &catalog, bool clusterResults = false) {
  for(int i=1;i<=2;++i) {
    CatalogAsset asset;asset.id=QString("asset-%1").arg(i);asset.type="well_log";asset.displayName="same-name";
    if (clusterResults) asset.type = i == 1 ? "seismic_cluster_patches" : "facies_polygons";
    if(!catalog.addAsset(asset))return false;
    CatalogVersion version;version.id=QString("version-%1").arg(i);version.assetId=asset.id;version.stage="RAW";
    version.managed=false;version.path=QString("Z:/offline/source-%1.dat").arg(i);version.fileName=QString("source-%1.dat").arg(i);
    if (clusterResults) {
      version.stage = "DERIVED";
      version.extra.insert("kind", asset.type);
      if (i == 2) {
        const QVariantMap unit{{"schemeId","scheme"},{"revision","r1"},{"intervalId","T70-T72"},
                               {"subIntervalId","S1"},{"displayName","T70–T72 / S1"},{"private","hidden"}};
        version.extra.insert("result_kind", "well_assigned_cluster_patches");
        version.extra.insert("horizon", "unit1");
        version.extra.insert("algorithm_parameters", QVariantMap{{"unitSelection",unit},{"private","hidden"}});
      }
    }
    if(i==2) for(int p=0;p<140;++p)version.parentVersionIds.append(QString("parent-%1").arg(p));
    version.extra.insert("opaqueLegacyPayload",QString(100000,'x'));
    if(!catalog.addVersion(version))return false;
  }
  return true;
}
}
class ArtifactVersionCapabilityTests : public QObject {
  Q_OBJECT
private slots:
  void exactVersionIsReadWithoutOriginalOrNameFallback();
  void wrongDocumentAndStaleRevisionAreRejected();
  void returnedMetadataIsBoundedAndMatchesSchema();
  void nativeAndAgentShareDescriptorAndResult();
  void derivedVersionsResolveTheSameFrozenUnitInWorkbenchAndPi();
  void typedToolRequiresHostDocumentIdentityAndVersion();
  void tileSchemaDescribesActualSummary();
};
void ArtifactVersionCapabilityTests::exactVersionIsReadWithoutOriginalOrNameFallback() {
  QTemporaryDir dir;DataCatalog catalog;QVERIFY(catalog.open(dir.path()));QVERIFY(seed(catalog));
  const auto revision=catalog.catalogRevision();
  ArtifactVersionRequest request{"doc-1","version-2",revision};
  const auto result=ArtifactVersionCapability::describe(&catalog,"doc-1",request);
  QVERIFY2(result.ok,qPrintable(result.error));QCOMPARE(result.version.value("assetId").toString(),QString("asset-2"));
  QCOMPARE(result.version.value("fileName").toString(),QString("source-2.dat"));
  request.versionId="same-name";const auto absent=ArtifactVersionCapability::describe(&catalog,"doc-1",request);
  QVERIFY(!absent.ok);QCOMPARE(absent.code,QString("version_not_found"));QVERIFY(absent.version.isEmpty());
  QCOMPARE(catalog.catalogRevision(),revision);
}
void ArtifactVersionCapabilityTests::wrongDocumentAndStaleRevisionAreRejected() {
  QTemporaryDir dir;DataCatalog catalog;QVERIFY(catalog.open(dir.path()));QVERIFY(seed(catalog));
  ArtifactVersionRequest request{"other-document","version-1",catalog.catalogRevision()};
  auto result=ArtifactVersionCapability::describe(&catalog,"doc-1",request);QVERIFY(!result.ok);QCOMPARE(result.code,QString("document_mismatch"));
  request.documentId="doc-1";--request.expectedCatalogRevision;
  result=ArtifactVersionCapability::describe(&catalog,"doc-1",request);QVERIFY(!result.ok);QCOMPARE(result.code,QString("stale_revision"));QVERIFY(result.version.isEmpty());
}
void ArtifactVersionCapabilityTests::returnedMetadataIsBoundedAndMatchesSchema() {
  QTemporaryDir dir;DataCatalog catalog;QVERIFY(catalog.open(dir.path()));QVERIFY(seed(catalog));
  const auto result=ArtifactVersionCapability::describe(&catalog,"doc-1",{"doc-1","version-2",catalog.catalogRevision()});
  QVERIFY(result.ok);QCOMPARE(result.version.value("parentVersionIds").toArray().size(),128);
  QVERIFY(result.version.value("parentsTruncated").toBool());QCOMPARE(result.version.value("parentCount").toString(),QString("140"));
  QVERIFY(!result.version.contains("opaqueLegacyPayload"));QVERIFY(!result.version.contains("path"));
  QVERIFY(QJsonDocument(result.toJson()).toJson().size()<64*1024);
  const auto schema=ArtifactVersionCapability::resultSchema();
  QCOMPARE(result.toJson().keys(),schema.value("properties").toObject().keys());
  const auto cardSchema=schema.value("properties").toObject().value("version").toObject().value("oneOf").toArray()[0].toObject();
  QCOMPARE(result.version.keys(),cardSchema.value("properties").toObject().keys());
  QVariantMap oversized{{"schemeId","scheme"},{"revision","r1"},{"intervalId","i"},
                        {"subIntervalId",QString(257,'a')}};
  QVERIFY(catalog.updateVersionExtra("version-2", {{"unitSelection",oversized},{"result_kind","private/freeform"}}, nullptr));
  const auto bounded=ArtifactVersionCapability::describe(&catalog,"doc-1",{"doc-1","version-2",catalog.catalogRevision()});
  QVERIFY(bounded.ok); QVERIFY(bounded.version.value("labelsTruncated").toBool());
  QVERIFY(bounded.version.value("unitSelection").toObject().isEmpty());
  QVERIFY(bounded.version.value("resultKind").toString().isEmpty());
}
void ArtifactVersionCapabilityTests::derivedVersionsResolveTheSameFrozenUnitInWorkbenchAndPi() {
  QTemporaryDir project;
  DataCatalog catalog;
  QVERIFY(catalog.open(project.path()));
  QVERIFY(seed(catalog, true));
  const auto source = catalog.versionById("version-2");
  const auto frozen = source.extra.value("algorithm_parameters").toMap().value("unitSelection").toMap();
  auto renamed = frozen;
  renamed["displayName"] = "S1 renamed";
  auto other = frozen;
  other["subIntervalId"] = "S2";

  const auto add = [&](const QString &id, const QStringList &parents, const QVariantMap &unit = {}) {
    CatalogVersion version;
    version.id = id;
    version.assetId = source.assetId;
    version.stage = "DERIVED";
    version.parentVersionIds = parents;
    if (!unit.isEmpty()) version.extra.insert("unitSelection", unit);
    return catalog.addVersion(version);
  };
  QVERIFY(add("edited", {source.id}));
  QVERIFY(add("renamed", {}, renamed));
  QVERIFY(add("same-unit", {"edited", "renamed"}));
  QVERIFY(add("other-unit", {}, other));
  QVERIFY(add("mixture", {"edited", "other-unit"}));
  QVERIFY(add("nested-mixture", {"mixture", "edited"}));
  QVERIFY(add("explicit-unit", {"mixture"}, other));
  QVERIFY(add("malformed-unit", {"edited"}));
  QVERIFY(catalog.updateVersionExtra("malformed-unit", "unitSelection", QStringLiteral("S2")));

  AiChatToolRunner runner;
  AiChatToolRunner::Context context;
  context.catalog = &catalog;
  context.documentId = "doc";
  runner.setContext(context);
  QSignalSpy finished(&runner, &AiChatToolRunner::toolFinished);
  const auto check = [&](const QString &id, const QString &expected) {
    const auto workbench = analysisResultUnitSelection(&catalog, catalog.versionById(id));
    QCOMPARE(workbench.subIntervalId, expected);
    ChatToolCall call;
    call.id = id;
    call.name = "paleo.query_project";
    call.argumentsJson = QString::fromUtf8(QJsonDocument(QJsonObject{
        {"topic", "version"}, {"version_id", id}}).toJson(QJsonDocument::Compact));
    runner.run(call);
    QTRY_COMPARE(finished.size(), 1);
    const auto reply = finished.takeFirst();
    QVERIFY(reply[1].toBool());
    const auto card = QJsonDocument::fromJson(reply[2].toString().toUtf8()).object()
                          .value("version").toObject().value("version").toObject();
    const auto unit = card.value("unitSelection").toObject();
    QCOMPARE(unit.value("subIntervalId").toString(), expected);
    QCOMPARE(unit.value("revision").toString(), workbench.revision);
    QVERIFY(!unit.contains("private"));
  };
  check("edited", "S1");
  check("same-unit", "S1");
  check("nested-mixture", {});
  check("explicit-unit", "S2");
  check("malformed-unit", {});
  bool inconsistent = false;
  analysisResultUnitSelection(&catalog, catalog.versionById("malformed-unit"), &inconsistent);
  QVERIFY(inconsistent);
}

void ArtifactVersionCapabilityTests::nativeAndAgentShareDescriptorAndResult() {
  QTemporaryDir dir;DataCatalog catalog;QVERIFY(catalog.open(dir.path()));QVERIFY(seed(catalog, true));
  const auto native=ArtifactVersionCapability::describe(&catalog,"doc-1",{"doc-1","version-2",catalog.catalogRevision()});QVERIFY(native.ok);
  AiChatToolRunner runner;AiChatToolRunner::Context context;context.catalog=&catalog;context.documentId="doc-1";runner.setContext(context);
  QSignalSpy finished(&runner,&AiChatToolRunner::toolFinished);
  ChatToolCall call;call.id="call-1";call.name="paleo.query_project";
  call.argumentsJson=QString::fromUtf8(QJsonDocument(QJsonObject{{"topic","version"},{"version_id","version-2"},{"catalog_revision",catalog.catalogRevision()}}).toJson());
  runner.run(call);QTRY_COMPARE(finished.size(),1);QVERIFY(finished.first()[1].toBool());
  const auto payload=QJsonDocument::fromJson(finished.first()[2].toString().toUtf8()).object();
  QCOMPARE(payload.value("version").toObject(),native.toJson());
  const auto card=native.version;
  QCOMPARE(card.value("assetType").toString(), QString("facies_polygons"));
  QCOMPARE(card.value("resultKind").toString(), QString("well_assigned_cluster_patches"));
  QCOMPARE(card.value("horizon").toString(), QString("unit1"));
  QCOMPARE(card.value("unitSelection").toObject().value("subIntervalId").toString(), QString("S1"));
  QCOMPARE(card.value("unitSelection").toObject().value("revision").toString(), QString("r1"));
  QVERIFY(!card.value("unitSelection").toObject().contains("private"));
  finished.clear(); call.id="assets";
  call.argumentsJson=QStringLiteral(R"({"topic":"assets"})");
  runner.run(call); QTRY_COMPARE(finished.size(),1); QVERIFY(finished.first()[1].toBool());
  const auto assets=QJsonDocument::fromJson(finished.first()[2].toString().toUtf8()).object().value("assets").toArray();
  QCOMPARE(assets.size(),2);
  QCOMPARE(assets[0].toObject().value("result_kind").toString(), QString("seismic_cluster_patches"));
  QCOMPARE(assets[1].toObject().value("result_kind").toString(), card.value("resultKind").toString());
  const auto query=aiToolSpec("paleo.query_project");
  const auto schema=QJsonDocument::fromJson(query.resultSchemaJson.toUtf8()).object();
  QCOMPARE(schema.value("properties").toObject().value("version").toObject(),ArtifactVersionCapability::resultSchema());
}
void ArtifactVersionCapabilityTests::typedToolRequiresHostDocumentIdentityAndVersion() {
  const auto spec=aiToolSpec("paleo.query_project");
  QVERIFY(!spec.validateParameters({{"topic","version"}}).isEmpty());
  QVERIFY(!spec.validateParameters({{"topic","version"},{"version_id","version-1"},{"catalog_revision","1"}}).isEmpty());
  QTemporaryDir dir;DataCatalog catalog;QVERIFY(catalog.open(dir.path()));QVERIFY(seed(catalog));
  AiChatToolRunner runner;AiChatToolRunner::Context context;context.catalog=&catalog;runner.setContext(context);
  QSignalSpy finished(&runner,&AiChatToolRunner::toolFinished);ChatToolCall call;call.id="call-1";call.name="paleo.query_project";
  call.argumentsJson=QStringLiteral("{\"topic\":\"version\",\"version_id\":\"version-1\"}");runner.run(call);
  QTRY_COMPARE(finished.size(),1);QVERIFY(!finished.first()[1].toBool());
  const auto result=QJsonDocument::fromJson(finished.first()[2].toString().toUtf8()).object().value("version").toObject();
  QCOMPARE(result.value("code").toString(),QString("document_mismatch"));QVERIFY(result.value("version").toObject().isEmpty());
}
void ArtifactVersionCapabilityTests::tileSchemaDescribesActualSummary() {
  const auto schema=QJsonDocument::fromJson(aiToolSpec("paleo.tile_classification").resultSchemaJson.toUtf8()).object();QVERIFY(!schema.isEmpty());
  const auto fields=schema.value("properties").toObject();
  QVERIFY(fields.contains("layer_ids"));QVERIFY(fields.contains("class_histogram"));QVERIFY(fields.contains("classified_pixels"));
  QVERIFY(!fields.contains("argmax"));QVERIFY(!fields.contains("confidence"));
}
QTEST_GUILESS_MAIN(ArtifactVersionCapabilityTests)
#include "tst_artifact_version_capability.moc"
