// 层：测试壳
#include "app/pythonanalysiswiring.h"
#include "services/pythonanalysiscapabilityspec.h"
#include "services/projectquerycapability.h"
#include "fixtures/documentsourcefixture.h"
#include "workflow/capabilityhost.h"
#include "workflow/aichattoolrunner.h"
#include "domain/wellref.h"
#include "workflow/capabilityrecovery.h"
#include "metadata/projectlock.h"
#include <QtTest>
#include <QJsonArray>
#include <QUuid>

using namespace paleo;
namespace {
struct Fixture:document_fixture::Fixture {
  capability::CapabilityHost host;
  host::CallContext context;
  document::WellRef well;
  QString model;
  QJsonObject selection;
  bool open() {
    if(!init())return false;
    model=QDir(project).filePath("artifacts/metadata/area-model");
    data::DataService sdk;std::string why;
    if(!sdk.createProject(model.toStdString(),"Production Python provider",why))return false;
    data::ParsedFile parsed;parsed.kind="logs";parsed.status="parsed";
    data::WellDataset dataset;dataset.wellInfo.wellName="PRODUCTION-1";
    data::LogSet logs;logs.logSetName="Explicit MD";logs.depthInfo={"MD","m","KB","","",""};
    logs.samplingAxis.depthValues={100,101,102,103};
    data::LogCurve curve;curve.curveName="A";curve.curveUnit="API";curve.sampleValues={1,2,3,4};
    logs.logCurves.push_back(curve);dataset.wellbore.logSets.push_back(logs);parsed.wells.push_back(dataset);
    const auto path=temp.filePath("source.data");const QByteArray bytes="Production typed input fixture\n";
    if(!document_fixture::write(path,bytes))return false;
    qlonglong sourceId=0;const auto sha=QCryptographicHash::hash(bytes,QCryptographicHash::Sha256).toHex();
    if(!sdk.importPreparedFile(path.toStdString(),path.toStdString(),path.toStdString(),sha.toStdString(),parsed,sourceId,why))return false;
    if(!registerAreaWells(catalog,&error) || !registerAreaSources(catalog,&error) || !bind())return false;
    const auto entities=catalog.storedEntities("well");if(entities.size()!=1)return false;
    data::ModelChangeHead head;if(!sdk.readModelChangeHead(head,why) || !head.available)return false;
    well.documentId=session.documentId();well.catalogEntityId=entities[0].id;well.modelStoreId="area-model";
    well.modelStoreIdentity=QString::fromStdString(head.storeId);well.modelGeneration=QString::fromStdString(head.generation);
    well.modelWellId=entities[0].extra.value("areaModelWellId").toLongLong();well.queryHead=head.sequence;well.modelAvailable=true;
    context.documentId=session.documentId();context.epoch=1;context.open=true;context.persistentIdentity=true;
    context.readOnly=false;context.ownsWrites=true;context.allowedEffects=host::Read|host::PrivateStage|host::LocalExecute;
    host.setBindingProvider([this]{capability::HostBinding out;out.catalog=&catalog;out.projectDir=project;out.context=context;return out;});
    query::ProjectQueryCapability query;query.bind(&catalog,{session.documentId(),1,catalog.mutationSeq(),"chat"});
    query.setSourceResolver([this](const document::SourceRef &ref){return session.source(ref);});
    const auto page=query.query({{"topic","well_curves"},{"well_ref",document::wellRefJson(well)}});
    const auto curves=page.value("curves").toArray();if(curves.size()!=1){error=QString::fromUtf8(QJsonDocument(page).toJson());return false;}
    const auto input=curves[0].toObject().value("input_ref").toObject();if(input.isEmpty())return false;
    selection={{"well_ref",document::wellRefJson(well)},{"curves",QJsonArray{input}},{"first_sample","1"},
      {"sample_count",2},{"processed",true},{"alignment","exact_shared_axis_v1"},
      {"sampling",QJsonObject{{"method","all_selected_window_v1"},{"seed",0}}}};
    return true;
  }
  capability::ExecuteRequest request() const {
    capability::ExecuteRequest r;r.capabilityId=QLatin1String(python::WellQualityCapability);
    r.documentId=session.documentId();r.callerScope="agent:production";r.bindingMode="pinned-value";
    r.inputs=selection.toVariantMap();return r;
  }
};
QJsonObject call(AiChatToolRunner &runner,const QString &name,const QJsonObject &args,bool &ok) {
  QSignalSpy done(&runner,&AiChatToolRunner::toolFinished);ok=false;
  runner.run({QUuid::createUuid().toString(QUuid::WithoutBraces),name,QString::fromUtf8(QJsonDocument(args).toJson(QJsonDocument::Compact))});
  if(done.isEmpty() && !done.wait(30000))return {};
  ok=done[0][1].toBool();return QJsonDocument::fromJson(done[0][2].toString().toUtf8()).object();
}
}
class TestPythonAnalysisWiring:public QObject {
  Q_OBJECT
private slots:
  void unprobedBackendCannotAdvertiseOrCaptureNewComputation() {
    bool captured=false;capability::CapabilityExecutionAdapter adapter;
    adapter.capture=[&](const auto &,const auto &,QString*){captured=true;return capability::CapabilityPreparation{};};
    const auto gated=gateTrustedAnalysis(adapter,{});QString code;
    QVERIFY(!gated.capture({}, {},&code).worker);QVERIFY(!captured);QCOMPARE(code,QString("python_trusted_execution_unprobed"));
    const auto status=pythonAnalysisReadiness({},{});QVERIFY(!status.value("ready").toBool());
    QVERIFY(!status.value("execution").toObject().value("executionReady").toBool());
  }
  void discoveryReferencesResolveToHostOwnedPathsAndFrozenHead() {
    Fixture f;QVERIFY2(f.open(),qPrintable(f.error));auto provider=assemblePythonAnalysisProvider(&f.catalog,&f.session,&f.host);
    python::AnalysisReadPlan plan;std::function<QString()> validate;const auto before=f.catalog.mutationSeq();
    QCOMPARE(provider(f.request(),plan,validate),QString());QVERIFY(validate);QCOMPARE(validate(),QString());
    QCOMPARE(plan.curves.size(),1);QCOMPARE(plan.documentId,f.session.documentId());
    QCOMPARE(plan.callerScope,QString("agent:production"));QCOMPARE(plan.expectedHead.sequence,f.well.queryHead);
    QCOMPARE(plan.modelDirectory,QFileInfo(f.model).canonicalFilePath());QVERIFY(!plan.curves[0].source.path.isEmpty());
    QCOMPARE(plan.curves[0].source.versionId,plan.curves[0].sourceVersionId);
    QCOMPARE(plan.curves[0].firstSample,qlonglong(1));QCOMPARE(plan.curves[0].sampleCount,2);
    QCOMPARE(f.catalog.mutationSeq(),before);
    f.context.allowedEffects=host::Read|host::PrivateStage;QCOMPARE(validate(),QString("effect_denied"));
  }
  void changedWellMappingAndClosedDocumentInvalidateCapturedPlan() {
    Fixture f;QVERIFY2(f.open(),qPrintable(f.error));const auto provider=assemblePythonAnalysisProvider(&f.catalog,&f.session,&f.host);
    python::AnalysisReadPlan plan;std::function<QString()> validate;
    QCOMPARE(provider(f.request(),plan,validate),QString());
    auto entity=f.catalog.storedEntityById(f.well.catalogEntityId);entity.extra["areaModelWellId"]=f.well.modelWellId+1;
    QVERIFY(f.catalog.updateEntity(entity,&f.error));QCOMPARE(validate(),QString("well_mapping_changed"));
    f.session.close();QCOMPARE(validate(),QString("document_unavailable"));
  }
  void rejectsForgedVersionsAndModelSuppliedPaths() {
    Fixture f;QVERIFY2(f.open(),qPrintable(f.error));const auto provider=assemblePythonAnalysisProvider(&f.catalog,&f.session,&f.host);
    auto request=f.request();auto inputs=QJsonObject::fromVariantMap(request.inputs);auto curves=inputs["curves"].toArray();
    auto ref=curves[0].toObject();ref["source_version_id"]="other-version";curves[0]=ref;inputs["curves"]=curves;request.inputs=inputs.toVariantMap();
    python::AnalysisReadPlan plan;std::function<QString()> validate;
    QCOMPARE(provider(request,plan,validate),QString("python_analysis_source_mismatch"));QVERIFY(!validate);
    request=f.request();request.inputs["path"]="/host/private";
    QCOMPARE(provider(request,plan,validate),QString("invalid_python_analysis_selection"));
  }
  void providerLifetimeAndRuntimeReadinessFailClosed() {
    auto *host=new capability::CapabilityHost;
    auto provider=assemblePythonAnalysisProvider(nullptr,nullptr,host);delete host;
    python::AnalysisReadPlan plan;std::function<QString()> validate;
    QCOMPARE(provider({},plan,validate),QString("python_analysis_provider_unavailable"));
    const auto readiness=pythonAnalysisReadiness({},"/not-a-template-store");
    QVERIFY(!readiness.value("ready").toBool());QCOMPARE(readiness.value("capabilities").toArray().size(),2);
    for(const auto &entry:readiness.value("capabilities").toArray()) {
      QCOMPARE(entry.toObject().value("code").toString(),QString("python_environment_disabled"));
      QVERIFY(!entry.toObject().contains("root"));
    }
  }
  void agentDiscoveryComputePagedReadRestartAndPublishUseOneChain() {
    const auto root=qEnvironmentVariable("PALEO_TEST_AGENT_PYTHON_ROOT");
    if(root.isEmpty()) {
      QVERIFY2(qEnvironmentVariable("PALEO_REQUIRE_AGENT_PYTHON_EXECUTION")!="1","Required real Python fixture missing");
      QVERIFY(!pythonAnalysisReadiness({},{}).value("ready").toBool());return;
    }
    Fixture f;QVERIFY2(f.open(),qPrintable(f.error));
    ProjectDirLock lock(f.project);QVERIFY2(lock.tryLock(&f.error),qPrintable(f.error));
    python::ExecutionPolicy policy;policy.allowTrusted=true;
    auto runtime=std::make_shared<python::RuntimeProvider>(python::RuntimeConfig{
      root,qEnvironmentVariable("PALEO_TEST_AGENT_PYTHON_MANIFEST_SHA256"),true},policy);
    const auto inspected=runtime->inspect();QVERIFY2(inspected.ready(),qPrintable(inspected.code));
    f.context.allowedEffects|=host::CatalogPublish|host::Presentation;
    const auto binding=[&]{capability::HostBinding h;h.catalog=&f.catalog;h.projectDir=f.project;
      h.operationDir=f.project+"/.paleo/capability-ops";h.writeClaim=lock.claim();h.context=f.context;return h;};
    const auto configure=[&](capability::CapabilityHost &owner,const QString &scripts) {
      owner.setBindingProvider(binding);owner.setTaskService(&f.tasks);owner.setCallerScope(host::Caller::Agent,"agent:production");
      owner.registerExecutionAdapter(QLatin1String(python::WellQualityCapability),python::analysisScriptAdapter(
        python::AnalysisKind::WellQuality,runtime,scripts,assemblePythonAnalysisProvider(&f.catalog,&f.session,&owner)));
    };
    configure(f.host,QString::fromUtf8(PALEO_TEST_PYTHON_ANALYSIS_TEMPLATES));
    AiChatToolRunner runner;runner.setTaskService(&f.tasks);AiChatToolRunner::Context context;
    context.catalog=&f.catalog;context.projectDir=f.project;context.documentId=f.session.documentId();context.documentEpoch=1;
    context.capabilityHost=&f.host;context.requireCapabilityHost=true;context.pythonRuntime=runtime;
    context.sourceResolver=[&](const auto &ref){return f.session.source(ref);};
    context.runtimeReadiness=[](const QString &,const QJsonObject &,bool){return QString();};
    runner.setContext(context);runner.setResultSession("python-analysis-chat");bool ok=false;
    const auto query=call(runner,"paleo.query_project",{{"topic","well_curves"},{"well_ref",document::wellRefJson(f.well)}},ok);
    QVERIFY2(ok,qPrintable(QJsonDocument(query).toJson()));QVERIFY(query.value("metadataOnly").toBool());
    QCOMPARE(query.value("numericPayloadBytes").toString(),QString("0"));
    auto inputs=f.selection;inputs["curves"]=QJsonArray{query.value("curves").toArray()[0].toObject().value("input_ref")};
    QJsonObject args{{"client_request_id","chat-compute"},{"binding_mode","pinned-value"},{"parameters",QJsonObject{}},
      {"inputs",inputs},{"publish_intent","compute-only"}};
    const auto count=f.catalog.versions().size();const auto computed=call(runner,QLatin1String(python::WellQualityTool),args,ok);
    QVERIFY2(ok,qPrintable(QJsonDocument(computed).toJson()));QCOMPARE(f.catalog.versions().size(),count);
    const auto operation=computed.value("operationId").toString();QVERIFY(!operation.isEmpty());
    const auto replay=call(runner,QLatin1String(python::WellQualityTool),args,ok);
    QVERIFY(ok);QCOMPARE(replay.value("operationId").toString(),operation);
    QByteArray report;QString offset="0";
    for(int i=0;i<64;++i) {
      const auto page=call(runner,"paleo.operation_read",{{"operation_id",operation},{"port_id","REPORT"},{"offset",offset},{"max_bytes",128}},ok);
      QVERIFY2(ok,qPrintable(QJsonDocument(page).toJson()));const auto bytes=QByteArray::fromBase64(page.value("data").toString().toLatin1());
      QVERIFY(!bytes.isEmpty() && bytes.size()<=128);report+=bytes;offset=page.value("nextOffset").toString();
      if(!page.value("hasMore").toBool())break;
    }
    QCOMPARE(QJsonDocument::fromJson(report).object().value("contract").toString(),QString("paleo.well-quality-report.v1"));
    // Reconstruct runtime hooks from durable records; publishing cannot rerun the old source script.
    capability::CapabilityHost restarted;configure(restarted,"/missing-original-template-generation");
    context.capabilityHost=&restarted;runner.setContext(context);
    args["client_request_id"]="chat-publish";args["publish_intent"]="publish";
    args["inputs"]=QJsonObject{{"source_operation_id",operation}};
    const auto published=call(runner,QLatin1String(python::WellQualityTool),args,ok);
    QVERIFY2(ok,qPrintable(QJsonDocument(published).toJson()));QCOMPARE(published.value("commitState").toString(),QString("committed"));
    QCOMPARE(f.catalog.versions().size(),count+1);
    const auto source=inputs.value("curves").toArray()[0].toObject().value("source_version_id").toString();
    for(const auto &version:f.catalog.versions())if(version.stage=="DERIVED")QCOMPARE(version.parentVersionIds,QStringList{source});
    call(runner,QLatin1String(python::WellQualityTool),args,ok);QVERIFY(ok);QCOMPARE(f.catalog.versions().size(),count+1);
  }
};
QTEST_GUILESS_MAIN(TestPythonAnalysisWiring)
#include "tst_pythonanalysiswiring.moc"
