// 层：测试壳
#include "app/pythonanalysiswiring.h"
#include "services/pythonanalysiscapabilityspec.h"
#include "workflow/pythongeneratedoperation.h"
#include "services/pythongenerated.h"
#include <QtConcurrent>
#include <QFutureWatcher>
#include <QEventLoop>
#include "services/projectquerycapability.h"
#include "fixtures/documentsourcefixture.h"
#include "fixtures/pythonenvironmentfixture.h"
#include "workflow/capabilityhost.h"
#include "workflow/commanddispatcher.h"
#include "workflow/capabilitygateway.h"
#include <utility>
#include <QPointer>
#include <QTimer>
#include "workflow/aichattoolrunner.h"
#include "domain/wellref.h"
#include "workflow/capabilityrecovery.h"
#include "metadata/projectlock.h"
#include <QtTest>
#include <QJsonArray>
#include <QUuid>
#include <QScopeGuard>

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
  // The enforced execution policy allows 120 s, including sandbox snapshots.
  if(done.isEmpty() && !done.wait(180000))return {};
  ok=done[0][1].toBool();return QJsonDocument::fromJson(done[0][2].toString().toUtf8()).object();
}
bool runPreparedDispatcher(command::CommandDispatcher &dispatcher,const command::PreparedPlan &plan,
    capability::ExecuteResult &result,QString &error) {
  QEventLoop wait;QTimer timeout;QObject receiver;bool finished=false;timeout.setSingleShot(true);
  QObject::connect(&timeout,&QTimer::timeout,&wait,&QEventLoop::quit);
  QPointer<PaleoTask> task=dispatcher.start(plan,&receiver,QStringLiteral("Dispatcher Python compute"),
      [&](capability::ExecuteResult value){result=std::move(value);finished=true;wait.quit();},&error);
  if(!task)return false;
  timeout.start(180000);if(!finished)wait.exec();
  if(!finished){if(task)task->requestInterruption();error="test_dispatch_timeout";}
  return finished;
}
}
class TestPythonAnalysisWiring:public QObject {
  Q_OBJECT
private slots:
  void contextCallbackCannotDestroyTheResolvedCatalogBeforeSelectionChecks() {
    Fixture f;QVERIFY2(f.open(),qPrintable(f.error));
    auto catalog=std::make_unique<DataCatalog>();QVERIFY(catalog->open(f.project));int calls=0;
    f.host.setBindingProvider([&] {
      capability::HostBinding binding;binding.context=f.context;
      if(++calls==2)catalog.reset();return binding;
    });
    const auto provider=assemblePythonAnalysisProvider(catalog.get(),&f.session,&f.host);
    python::AnalysisReadPlan plan;std::function<QString()> validate;
    QCOMPARE(provider(f.request(),plan,validate),QString("document_unavailable"));
    QCOMPARE(calls,2);QVERIFY(!validate);
  }
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
  void agentDiscoveryComputePagedReadRestartAndPublishUseOneChain_data() {
    QTest::addColumn<bool>("generated");QTest::addColumn<bool>("managed");
    QTest::newRow("trusted-quality")<<false<<false;QTest::newRow("generated-numeric")<<true<<false;
    QTest::newRow("managed-trusted-quality")<<false<<true;QTest::newRow("managed-generated-numeric")<<true<<true;
  }
  void agentDiscoveryComputePagedReadRestartAndPublishUseOneChain() {
    QFETCH(bool,generated);QFETCH(bool,managed);
    const auto legacy=python::captureLegacyPythonEnvironment();
    if(managed)for(auto it=legacy.begin();it!=legacy.end();++it)qunsetenv(it.key().toLatin1().constData());
    const auto restoreEnvironment=qScopeGuard([&] {if(managed)for(auto it=legacy.begin();it!=legacy.end();++it)
      qputenv(it.key().toLatin1().constData(),it.value().toUtf8());});
    const auto tool=generated?QLatin1String(python::GeneratedTool):QLatin1String(python::WellQualityTool);
    const auto capability=generated?QLatin1String(python::GeneratedCapability):QLatin1String(python::WellQualityCapability);
    const auto contract=generated?QString("paleo.python-generated-report.v1"):QString("paleo.well-quality-report.v1");
    const auto root=qEnvironmentVariable("PALEO_TEST_AGENT_PYTHON_ROOT");
    if(root.isEmpty()) {
      QVERIFY2(qEnvironmentVariable("PALEO_REQUIRE_AGENT_PYTHON_EXECUTION")!="1","Required real Python fixture missing");
      QVERIFY(!pythonAnalysisReadiness({},{}).value("ready").toBool());return;
    }
    Fixture f;QVERIFY2(f.open(),qPrintable(f.error));
    ProjectDirLock lock(f.project);QVERIFY2(lock.tryLock(&f.error),qPrintable(f.error));
    python::ExecutionPolicy policy;policy.allowTrusted=!generated;policy.allowGenerated=generated;
    auto runtime=std::make_shared<python::RuntimeProvider>(python::RuntimeConfig{
      root,qEnvironmentVariable("PALEO_TEST_AGENT_PYTHON_MANIFEST_SHA256"),true},policy);
    const auto inspected=generated?runtime->inspectIntegrity():runtime->inspect();QVERIFY2(inspected.ready(),qPrintable(inspected.code));
    auto sandbox=std::make_shared<python::SandboxProvider>(runtime);
    python_environment_fixture::Fixture delivered(QString::fromUtf8(PALEO_TEST_PYTHON_ANALYSIS_TEMPLATES));
    auto registry=std::make_shared<python::PythonEnvironmentRegistry>(delivered.configurationPath());
    if(managed) {
      QVERIFY2(delivered.error.isEmpty(),qPrintable(delivered.error));
      python::EnvironmentCandidate candidate{root,qEnvironmentVariable("PALEO_TEST_AGENT_PYTHON_MANIFEST_SHA256"),
        generated?QString():delivered.scriptsRoot,generated?QString():delivered.scriptsSha,policy};
      const auto activated=registry->activate(candidate,0);QVERIFY2(activated.ok(),qPrintable(activated.code));
      QFutureWatcher<QJsonObject> watcher;QEventLoop wait;
      connect(&watcher,&QFutureWatcherBase::finished,&wait,&QEventLoop::quit);
      const auto id=activated.configuration.activeId;
      watcher.setFuture(QtConcurrent::run([registry,id,generated] {return registry->probe(id,
        generated?python::EnvironmentMode::Generated:python::EnvironmentMode::Trusted);}));
      if(!watcher.isFinished())wait.exec();
      const auto proof=watcher.result();QVERIFY2(proof.value("ready").toBool(),qPrintable(QJsonDocument(proof).toJson()));
    } else if(generated) {
      QFutureWatcher<QJsonObject> watcher;QEventLoop wait;
      connect(&watcher,&QFutureWatcherBase::finished,&wait,&QEventLoop::quit);
      watcher.setFuture(QtConcurrent::run([sandbox]{return sandbox->inspect();}));
      if(!watcher.isFinished())wait.exec();
      const auto status=watcher.result();QVERIFY2(status.value("ready").toBool(),qPrintable(QJsonDocument(status).toJson()));
    }
    f.context.allowedEffects|=host::CatalogPublish|host::Presentation;
    const auto binding=[&]{capability::HostBinding h;h.catalog=&f.catalog;h.projectDir=f.project;
      h.operationDir=f.project+"/.paleo/capability-ops";h.writeClaim=lock.claim();h.context=f.context;return h;};
    const auto configure=[&](capability::CapabilityHost &owner,const QString &scripts) {
      owner.setBindingProvider(binding);owner.setTaskService(&f.tasks);owner.setCallerScope(host::Caller::Agent,"agent:production");
      if(managed)return;
      if(generated)owner.registerExecutionAdapter(capability,python::generatedScriptAdapter(runtime,
        scripts.isEmpty()?std::shared_ptr<python::SandboxProvider>{}:sandbox,assemblePythonAnalysisProvider(&f.catalog,&f.session,&owner)));
      else owner.registerExecutionAdapter(capability,python::analysisScriptAdapter(
        python::AnalysisKind::WellQuality,runtime,scripts,assemblePythonAnalysisProvider(&f.catalog,&f.session,&owner)));
    };
    configure(f.host,QString::fromUtf8(PALEO_TEST_PYTHON_ANALYSIS_TEMPLATES));
    AiChatToolRunner runner;runner.setTaskService(&f.tasks);AiChatToolRunner::Context context;
    context.catalog=&f.catalog;context.projectDir=f.project;context.documentId=f.session.documentId();context.documentEpoch=1;
    context.capabilityHost=&f.host;context.requireCapabilityHost=true;context.pythonRuntime=runtime;
    context.sourceResolver=[&](const auto &ref){return f.session.source(ref);};
    context.runtimeReadiness=[](const QString &,const QJsonObject &,bool){return QString();};
    if(managed) {
      runner.setPythonEnvironments(registry);
      QVERIFY(assembleAgentPython(&runner,context,&f.catalog,&f.session,&f.host,[]{return true;}));
    }
    runner.setContext(context);runner.setResultSession("python-analysis-chat");bool ok=false;
    const auto query=call(runner,"paleo.query_project",{{"topic","well_curves"},{"well_ref",document::wellRefJson(f.well)}},ok);
    QVERIFY2(ok,qPrintable(QJsonDocument(query).toJson()));QVERIFY(query.value("metadataOnly").toBool());
    QCOMPARE(query.value("numericPayloadBytes").toString(),QString("0"));
    auto inputs=f.selection;inputs["curves"]=QJsonArray{query.value("curves").toArray()[0].toObject().value("input_ref")};
    const QByteArray source=R"PY(import sys, struct, json, numpy as np
stream=sys.stdin.buffer
n=struct.unpack('<I',stream.read(4))[0]
meta=json.loads(stream.read(n))
rows,cols=meta['shape']
raw=stream.read()
values=np.frombuffer(raw,dtype='<f8',count=rows*cols,offset=rows*8).reshape(rows,cols)
quality=np.frombuffer(raw,dtype='u1',count=rows*cols,offset=rows*8+rows*cols*8).reshape(rows,cols)
valid=np.isfinite(values[:,0]) & ((quality[:,0]&3)==0)
mean=float(np.mean(values[valid,0])) if np.any(valid) else None
out={'contract':'paleo.python-output.v1','ports':{'REPORT':{'contract':'paleo.python-generated-report.v1','metrics':[{'id':'mean','unit':'API','value':mean}],'tables':[],'notes':[]}}}
sys.stdout.write(json.dumps(out,allow_nan=False,separators=(',',':')))
)PY";
    QJsonObject args{{"client_request_id","chat-compute"},{"binding_mode","pinned-value"},{"parameters",QJsonObject{}},
      {"inputs",inputs},{"publish_intent","compute-only"}};
    if(generated)args["parameters"]=QJsonObject{{"code",QString::fromUtf8(source)}};
    if(generated) {
      auto failedArgs=args;failedArgs["client_request_id"]="chat-invalid-code";
      failedArgs["parameters"]=QJsonObject{{"code","def invalid(:\n"}};
      const auto before=f.catalog.versions().size();call(runner,tool,failedArgs,ok);
      QVERIFY(!ok);QCOMPARE(f.catalog.versions().size(),before);
    }
    const auto count=f.catalog.versions().size();const auto computed=call(runner,tool,args,ok);
    QVERIFY2(ok,qPrintable(QJsonDocument(computed).toJson()));QCOMPARE(f.catalog.versions().size(),count);
    const auto operation=computed.value("operationId").toString();QVERIFY(!operation.isEmpty());
    const auto replay=call(runner,tool,args,ok);
    QVERIFY(ok);QCOMPARE(replay.value("operationId").toString(),operation);
    if(generated) {
      auto changed=args;changed["parameters"]=QJsonObject{{"code",QString::fromUtf8(source)+"\n# different program\n"}};
      const auto conflict=call(runner,tool,changed,ok);QVERIFY(!ok);
      QCOMPARE(conflict.value("code").toString(),QString("idempotency_conflict"));
      QCOMPARE(f.catalog.versions().size(),count);
    }
    QByteArray report;QString offset="0";
    for(int i=0;i<64;++i) {
      const auto page=call(runner,"paleo.operation_read",{{"operation_id",operation},{"port_id","REPORT"},{"offset",offset},{"max_bytes",128}},ok);
      QVERIFY2(ok,qPrintable(QJsonDocument(page).toJson()));const auto bytes=QByteArray::fromBase64(page.value("data").toString().toLatin1());
      QVERIFY(!bytes.isEmpty() && bytes.size()<=128);report+=bytes;offset=page.value("nextOffset").toString();
      if(!page.value("hasMore").toBool())break;
    }
    const auto accepted=QJsonDocument::fromJson(report).object();
    QCOMPARE(accepted.value("contract").toString(),contract);
    if(generated) {
      QCOMPARE(accepted.value("codeSha256").toString(),QString::fromLatin1(QCryptographicHash::hash(source,QCryptographicHash::Sha256).toHex()));
      QCOMPARE(accepted.value("metrics").toArray()[0].toObject().value("value").toDouble(),2.5);
      QCOMPARE(accepted.value("executionMode").toString(),QString("generated"));
      QCOMPARE(accepted.value("claims").toString(),QString("agent_generated_values_not_independently_recomputed"));
      QVERIFY(!policy.allowTrusted);
    }
    if(managed) {
      // The dispatcher owns the base Agent scope; chat-session operations stay isolated.
      QTRY_COMPARE_WITH_TIMEOUT(f.tasks.runningCount(),0,180000);
      auto dispatcherArgs=args;dispatcherArgs["client_request_id"]="dispatcher-compute";
      capability::ExecuteRequest decoded;
      const auto rejected=capability::prepareHostedChatToolRequest(decoded,tool,dispatcherArgs);
      QVERIFY2(rejected.isEmpty(),qPrintable(QJsonDocument(rejected).toJson()));
      QCOMPARE(decoded.capabilityId,QString(tool));
      QCOMPARE(CapabilityRegistry::resolveAlias(decoded.capabilityId,nullptr),QString(capability));
      if(generated)QCOMPARE(decoded.parameters.value("code").toString(),QString::fromUtf8(source));
      command::CommandContext commandContext;commandContext.documentId=f.context.documentId;
      commandContext.callerScope=f.host.callerScope(host::Caller::Agent);commandContext.clientRequestId=decoded.clientRequestId;
      command::CommandDispatcher dispatcher;dispatcher.setCapabilityHost(&f.host,host::Caller::Agent);
      dispatcher.setContextProvider([commandContext]{return commandContext;});
      const auto dispatcherVersions=f.catalog.versions().size();
      {
        const auto allowed=f.context.allowedEffects;const auto restore=qScopeGuard([&]{f.context.allowedEffects=allowed;});
        f.context.allowedEffects&=~unsigned(host::LocalExecute);
        const auto tasksBefore=f.tasks.tasks().size();QSignalSpy added(&f.tasks,&PaleoTaskService::taskAdded);
        const auto denied=dispatcher.prepare(capability,commandContext,decoded.parameters,decoded.inputs,decoded.publishIntent);
        QVERIFY(!denied.ok);QCOMPARE(denied.code,QString("effect_denied"));QVERIFY(denied.hostHandle.isEmpty());
        QCOMPARE(added.size(),0);QCOMPARE(f.tasks.tasks().size(),tasksBefore);QCOMPARE(f.catalog.versions().size(),dispatcherVersions);
      }
      if(generated) {
        auto invalidParameters=decoded.parameters;invalidParameters["unexpected"]=true;
        const auto tasksBefore=f.tasks.tasks().size();QSignalSpy added(&f.tasks,&PaleoTaskService::taskAdded);
        const auto invalid=dispatcher.prepare(capability,commandContext,invalidParameters,decoded.inputs,decoded.publishIntent);
        QVERIFY(!invalid.ok);QCOMPARE(invalid.code,QString("python_code_required"));QVERIFY(invalid.hostHandle.isEmpty());
        QCOMPARE(added.size(),0);QCOMPARE(f.tasks.tasks().size(),tasksBefore);QCOMPARE(f.catalog.versions().size(),dispatcherVersions);
      }
      const auto prepared=dispatcher.prepare(capability,commandContext,decoded.parameters,decoded.inputs,decoded.publishIntent);
      QVERIFY2(prepared.ok,qPrintable(prepared.code));QCOMPARE(prepared.request.bindingMode,QString("pinned-value"));
      QCOMPARE(prepared.effectiveParameters,decoded.parameters);QCOMPARE(prepared.request.parameters,decoded.parameters);
      capability::ExecuteResult firstResult;QString dispatchError;
      QVERIFY2(runPreparedDispatcher(dispatcher,prepared,firstResult,dispatchError),qPrintable(dispatchError));
      const auto firstPayload=capability::chatToolResponse(firstResult);
      QVERIFY2(firstResult.ok,qPrintable(QJsonDocument(firstPayload).toJson()));
      const auto dispatcherOperation=firstPayload.value("operationId").toString();
      QVERIFY(!dispatcherOperation.isEmpty());QVERIFY(dispatcherOperation!=operation);
      QCOMPARE(firstPayload.value("computeCount").toInt(),1);QCOMPARE(f.catalog.versions().size(),dispatcherVersions);
      const auto again=dispatcher.prepare(capability,commandContext,decoded.parameters,decoded.inputs,decoded.publishIntent);
      QVERIFY2(again.ok,qPrintable(again.code));capability::ExecuteResult secondResult;
      QVERIFY2(runPreparedDispatcher(dispatcher,again,secondResult,dispatchError),qPrintable(dispatchError));
      const auto secondPayload=capability::chatToolResponse(secondResult);
      QVERIFY2(secondResult.ok,qPrintable(QJsonDocument(secondPayload).toJson()));
      QCOMPARE(secondResult.operationId,dispatcherOperation);QCOMPARE(secondPayload.value("computeCount").toInt(),1);
      const auto hidden=call(runner,"paleo.operation_get",{{"operation_id",dispatcherOperation}},ok);
      QVERIFY(!ok);QCOMPARE(hidden.value("code").toString(),QString("operation_not_found"));
      QVERIFY(!hidden.contains("operationId"));QCOMPARE(f.catalog.versions().size(),dispatcherVersions);
    }
    // Reconstruct runtime hooks from durable records; publishing cannot rerun the old source script.
    capability::CapabilityHost restarted;configure(restarted,generated?QString():QString("/missing-original-template-generation"));
    context.capabilityHost=&restarted;
    AiChatToolRunner restartedRunner;auto *publisher=&runner;
    if(managed) {
      if(!generated)QVERIFY(QFile::remove(delivered.scriptsRoot+"/scripts-manifest.json"));
      auto restored=std::make_shared<python::PythonEnvironmentRegistry>(delivered.configurationPath());
      const auto code=restored->refresh({},restored->beginRefresh());if(generated)QVERIFY2(code.isEmpty(),qPrintable(code));
      else QVERIFY(!code.isEmpty());
      restartedRunner.setTaskService(&f.tasks);restartedRunner.setPythonEnvironments(restored);
      QVERIFY(assembleAgentPython(&restartedRunner,context,&f.catalog,&f.session,&restarted,[]{return true;}));
      restartedRunner.setContext(context);restartedRunner.setResultSession("python-analysis-chat");publisher=&restartedRunner;
      // Production assembly starts its own asynchronous refresh/prewarm. The
      // synchronous refresh above cannot stand in for that new binding's
      // completion; pending provider snapshots deliberately fail closed.
      QTRY_COMPARE_WITH_TIMEOUT(f.tasks.runningCount(), 0, 180000);
      QCOMPARE(restored->status().value("runtimeIntegrityCode").toString(), QString("ok"));
    } else runner.setContext(context);
    args["client_request_id"]="chat-publish";args["publish_intent"]="publish";
    args["parameters"]=QJsonObject{};args["inputs"]=QJsonObject{{"source_operation_id",operation}};
    const auto published=call(*publisher,tool,args,ok);
    QVERIFY2(ok,qPrintable(QJsonDocument(published).toJson()));QCOMPARE(published.value("commitState").toString(),QString("committed"));
    QCOMPARE(f.catalog.versions().size(),count+1);
    const auto sourceVersion=inputs.value("curves").toArray()[0].toObject().value("source_version_id").toString();
    for(const auto &version:f.catalog.versions())if(version.stage=="DERIVED")QCOMPARE(version.parentVersionIds,QStringList{sourceVersion});
    call(*publisher,tool,args,ok);QVERIFY(ok);QCOMPARE(f.catalog.versions().size(),count+1);
  }
};
QTEST_GUILESS_MAIN(TestPythonAnalysisWiring)
#include "tst_pythonanalysiswiring.moc"
