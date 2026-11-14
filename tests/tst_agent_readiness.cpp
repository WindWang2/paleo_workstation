// 层：测试壳
#include <QtTest>
#include "workflow/aichatcontroller.h"
#include "workflow/capabilityhost.h"
#include "workflow/tileoperationservice.h"
#include "ai/onnxfixture.h"
#include "ai/onnxpredictionservice.h"
#include "catalog/datacatalog.h"
#include <QTemporaryDir>

using namespace paleo;
class TestAgentReadiness:public QObject {
  Q_OBJECT
private slots:
  void localModelPagesUseTrustedChatScopeAndNeverLoadAFile() {
    QTemporaryDir temp;DataCatalog catalog;QString error;QVERIFY(catalog.open(temp.path(),&error));
    capability::HostBinding binding;binding.context.open=true;binding.context.documentId="doc";
    binding.context.readOnly=true;binding.context.allowedEffects=host::Read;
    capability::CapabilityHost host;host.setBindingProvider([&]{return binding;});host.setCallerScope(host::Caller::Agent,"principal");
    PaleoOnnxService onnx;onnx.setModelRoot(temp.path());ModelRegistryScan scan;scan.manifestFound=true;
    for(const auto &name:{"a","b"}) {ModelRegistryEntry entry;entry.name=QLatin1String(name);
      entry.absolutePath=temp.filePath("never-read.onnx");scan.entries.append(entry);}
    onnx.setModelRegistry(scan);AiChatToolRunner runner;runner.setResultSession("chat-A");
    AiChatToolContext context;context.catalog=&catalog;context.documentId="doc";context.onnx=&onnx;
    context.capabilityHost=&host;context.requireCapabilityHost=true;runner.setContext(context);
    QSignalSpy done(&runner,&AiChatToolRunner::toolFinished);
    runner.run({"models-1","paleo.query_project",R"({"topic":"local_models","page_size":1,"max_bytes":4096})"});
    QTRY_COMPARE(done.size(),1);QVERIFY(done[0][1].toBool());
    const auto first=QJsonDocument::fromJson(done[0][2].toString().toUtf8()).object();
    QVERIFY(first.value("metadataOnly").toBool());QVERIFY(first.value("hasMore").toBool());
    QCOMPARE(first.value("models").toArray().size(),1);QCOMPARE(onnx.sessionPoolSize(),0);
    QVERIFY(!done[0][2].toString().contains(temp.path()));
    const auto cursor=first.value("nextCursor").toString();QVERIFY(!cursor.isEmpty());runner.setResultSession("chat-B");
    const QJsonObject args{{"topic","local_models"},{"page_size",1},{"max_bytes",4096},{"cursor",cursor}};
    runner.run({"models-2","paleo.query_project",QString::fromUtf8(QJsonDocument(args).toJson(QJsonDocument::Compact))});
    QTRY_COMPARE(done.size(),2);QVERIFY(!done[1][1].toBool());
    QCOMPARE(QJsonDocument::fromJson(done[1][2].toString().toUtf8()).object().value("code").toString(),QString("cursor_scope_mismatch"));
  }
  void modelMetadataQueriesNeedTransferPermissionButNoWriteOwnership() {
    QTemporaryDir temp;DataCatalog catalog;QString error;QVERIFY(catalog.open(temp.path(),&error));
    capability::HostBinding binding;binding.context.open=true;binding.context.documentId="doc";
    binding.context.readOnly=true;binding.context.allowedEffects=host::Read|host::ExternalTransfer;
    capability::CapabilityHost host;host.setBindingProvider([&]{return binding;});
    AiChatToolRunner runner;PaleoTaskService tasks;runner.setTaskService(&tasks);
    AiChatToolContext context;context.catalog=&catalog;context.documentId="doc";
    context.capabilityHost=&host;context.requireCapabilityHost=true;runner.setContext(context);
    const QJsonObject args{{"topic","well_facies_models"}};
    QCOMPARE(runner.readiness("paleo.query_project",args).code,QString("well_endpoint_unavailable"));
    WellFaciesConfig config;config.baseUrl=QUrl("http://127.0.0.1:1");config.apiKey="local-test";config.allowInsecureHttp=true;
    runner.setFaciesConfig(config);QVERIFY(runner.readiness("paleo.query_project",args).ready);
    QSignalSpy done(&runner,&AiChatToolRunner::toolFinished);
    runner.run({"metadata-cursor","paleo.query_project",R"({"topic":"well_facies_models","cursor":"unsupported"})"});
    QTRY_COMPARE(done.size(),1);QVERIFY(!done[0][1].toBool());
    QCOMPARE(QJsonDocument::fromJson(done[0][2].toString().toUtf8()).object().value("code").toString(),QString("unsupported_topic_argument"));
    binding.context.allowedEffects=host::Read;
    QCOMPARE(runner.readiness("paleo.query_project",args).code,QString("effect_denied"));
    QVERIFY(runner.readiness("paleo.query_project",{{"topic","summary"}}).ready);QVERIFY(tasks.tasks().isEmpty());
  }
  void effectsDescribeActualIntentRatherThanRouting() {
    const auto tile=aiToolSpec("paleo.tile_classification");
    QVERIFY(tile.legacyAdapter);QVERIFY(tile.effects & host::CatalogPublish);QVERIFY(tile.effects & host::Presentation);
    const auto facies=aiToolSpec("paleo.well_facies_prediction");QVERIFY(facies.effects & host::ExternalTransfer);
    const auto native=aiToolSpec("paleo.depth_convert_batch");
    QCOMPARE(native.effectsForIntent("compute-only"),unsigned(host::Read|host::PrivateStage));
    QVERIFY(native.effectsForIntent("publish") & host::CatalogPublish);
    QCOMPARE(aiToolSpec("paleo.query_project").effects,unsigned(host::Read));
  }
  void readOnlyHostAdvertisesQueriesAndRejectsWritesBeforeTasks() {
    QTemporaryDir temp;DataCatalog catalog;QString error;QVERIFY(catalog.open(temp.path(),&error));
    capability::HostBinding binding;auto &state=binding.context;
    state.open=true;state.documentId="doc";state.epoch=7;state.readOnly=true;state.allowedEffects=host::Read;
    capability::CapabilityHost service;service.setBindingProvider([&]{return binding;});
    AiChatController chat;AiChatToolContext ctx;ctx.catalog=&catalog;ctx.documentId="doc";ctx.documentEpoch=7;
    ctx.requireCapabilityHost=true;ctx.capabilityHost=&service;chat.toolRunner()->setContext(ctx);
    const auto tools=chat.tools();QStringList names;for(const auto &tool:tools)names.append(tool.name);
    QVERIFY(names.contains("paleo.query_project"));QVERIFY(names.contains("paleo.asset_lineage"));
    QVERIFY(!names.contains("paleo.tile_classification"));QVERIFY(!names.contains("paleo.depth_convert_batch"));
    QCOMPARE(chat.toolRunner()->readiness("paleo.depth_convert_batch",{{"publish_intent","compute-only"}}).code,QString("effect_denied"));
    ++state.epoch;QCOMPARE(chat.toolRunner()->readiness("paleo.query_project").code,QString("stale_session"));
  }
  void modelProviderAndSemanticFailuresHaveDistinctReasons() {
    QTemporaryDir temp;DataCatalog catalog;QString error;QVERIFY(catalog.open(temp.path(),&error));
    capability::HostBinding binding;auto &state=binding.context;
    state.open=state.persistentIdentity=state.ownsWrites=true;state.readOnly=false;state.documentId="doc";
    state.allowedEffects=host::Read|host::PrivateStage|host::ExternalTransfer;
    capability::CapabilityHost service;service.setBindingProvider([&]{return binding;});
    AiChatToolRunner runner;AiChatToolContext ctx;ctx.catalog=&catalog;ctx.documentId="doc";
    ctx.requireCapabilityHost=true;ctx.capabilityHost=&service;runner.setContext(ctx);
    const auto name=QString("paleo.horizon_tracking_suggestion");
    QCOMPARE(runner.readiness(name).code,QString("ort_provider_unavailable"));
    PaleoOnnxService onnx;onnx.setModelRoot(temp.path());ctx.onnx=&onnx;runner.setContext(ctx);
    QCOMPARE(runner.readiness(name).code,QString("model_unavailable"));
    QVERIFY(OnnxFixtureWriter::writeTraceScorer(temp.filePath("model.onnx"),1,0));
    QCOMPARE(runner.readiness(name,{{"model","model"}}).code,QString("tracking_provider_unavailable"));
    AiAssistWorkflow workflow(nullptr);runner.setWorkflow(&workflow);
    ctx.trackingInput=[](const auto &,auto *,QString *why){*why="tracking_time_reference_unknown";return false;};
    runner.setContext(ctx);QCOMPARE(runner.readiness(name,{{"model","model"}}).code,QString("tracking_time_reference_unknown"));
    PaleoTaskService tasks;runner.setTaskService(&tasks);QSignalSpy done(&runner,&AiChatToolRunner::toolFinished);
    runner.run({"call","paleo.horizon_tracking_suggestion",R"({"model":"model","window_samples":4,"radius":1,"seeds":[]})"});
    QTRY_COMPARE(done.size(),1);QVERIFY(tasks.tasks().isEmpty());QVERIFY(!done[0][1].toBool());
    QCOMPARE(QJsonDocument::fromJson(done[0][2].toString().toUtf8()).object().value("code").toString(),QString("tracking_time_reference_unknown"));
  }
  void tilePublicationRemainsDiscoverableWithoutAnInstalledModel() {
    capability::HostBinding binding;auto &state=binding.context;
    state.open=state.persistentIdentity=state.ownsWrites=true;state.readOnly=false;state.documentId="doc";
    state.allowedEffects=host::Read|host::PrivateStage|host::CatalogPublish|host::Presentation;
    capability::CapabilityHost host;host.setBindingProvider([&]{return binding;});PaleoTaskService tasks;
    TileOperationService tile;tile.configure(&host,&tasks,nullptr,{},{});
    AiChatToolRunner runner;AiChatToolContext context;context.documentId="doc";
    context.capabilityHost=&host;context.requireCapabilityHost=true;context.tileOperations=&tile;runner.setContext(context);
    const auto tools=runner.availableTools();QStringList names;for(const auto &spec:tools)names.append(spec.name);
    QVERIFY(names.contains("paleo.tile_classification_v2"));
    QVERIFY(runner.readiness("paleo.tile_classification_v2",{{"publish_intent","publish"}}).ready);
    QCOMPARE(tile.readiness({{"publish_intent","prepare"}},false),QString("tile_source_provider_unavailable"));
  }
};
QTEST_GUILESS_MAIN(TestAgentReadiness)
#include "tst_agent_readiness.moc"
