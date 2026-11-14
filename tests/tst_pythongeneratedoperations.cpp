// 层：测试壳
#include "workflow/pythongeneratedoperation.h"
#include "services/pythongenerated.h"
#include "services/pythonanalysisselection.h"
#include "services/paleotaskservice.h"
#include "catalog/datacatalog.h"
#include "metadata/projectlock.h"
#include <paleo_data/service.h>
#include <QtTest>
#include <QtConcurrent>
#include <QCryptographicHash>
#include <QDir>
#include <QEventLoop>
#include <QFile>
#include <QFutureWatcher>
#include <QJsonArray>
#include <QJsonDocument>
#include <QTemporaryDir>
#include <QThread>
#include <QTimer>
#include <atomic>

using namespace paleo;
using namespace paleo::capability;
namespace {
constexpr auto Scope="agent:generated-test";
template<class T> T completed(QFuture<T> future) {
  QFutureWatcher<T> watcher;QEventLoop loop;QObject::connect(&watcher,&QFutureWatcherBase::finished,&loop,&QEventLoop::quit);
  watcher.setFuture(future);if(!watcher.isFinished())loop.exec();return watcher.result();
}
QByteArray sourceCode() {
  return R"PY(import sys, struct, json, numpy as np
n=struct.unpack('<I',sys.stdin.buffer.read(4))[0]
m=json.loads(sys.stdin.buffer.read(n))
r,c=m['shape'];raw=sys.stdin.buffer.read()
v=np.frombuffer(raw,dtype='<f8',count=r*c,offset=r*8).reshape(r,c)
q=np.frombuffer(raw,dtype='u1',count=r*c,offset=r*8+r*c*8).reshape(r,c)
metrics=[]
for i in range(c):
 x=v[:,i][np.isfinite(v[:,i]) & ((q[:,i]&3)==0)]
 metrics.append({'id':'mean_'+str(i),'unit':m['columns'][i]['unit'],'value':float(np.mean(x)) if x.size else None})
print(json.dumps({'contract':'paleo.python-output.v1','ports':{'REPORT':{'contract':'paleo.python-generated-report.v1','metrics':metrics,'tables':[],'notes':['Selected window means.']}}},allow_nan=False))
)PY";
}
struct Fixture {
  QTemporaryDir temp;QString project,model,error,tamper;
  data::DataService sdk;DataCatalog catalog;std::unique_ptr<ProjectDirLock> lock;
  PaleoTaskService tasks;python::AnalysisReadPlan plan;QJsonObject selection;
  std::unique_ptr<CapabilityHost> host;
  QThread *owner=QThread::currentThread();std::atomic_bool ownerOnly=true,workerOffOwner=false;
  std::atomic_int captures=0,materializations=0,computations=0;
  ~Fixture(){host.reset();tasks.shutdown(5000,true);}
  bool open() {
    project=temp.path()+"/project";model=project+"/artifacts/metadata/area-model";std::string why;
    if(!QDir().mkpath(project))return false;lock=std::make_unique<ProjectDirLock>(project);
    if(!lock->tryLock(&error) || !catalog.open(project,&error) || !sdk.createProject(model.toUtf8().toStdString(),"Generated fixture",why))return false;
    data::ParsedFile parsed;parsed.kind="logs";parsed.status="parsed";data::WellDataset well;well.wellInfo.wellName="GENERATED-1";
    data::LogSet logs;logs.logSetName="Declared shared MD";logs.depthInfo={"MD","m","KB","","",""};
    data::LogCurve a,b;a.curveName="A";a.curveUnit="API";b.curveName="B";b.curveUnit="m/s";
    for(int i=0;i<100;++i){logs.samplingAxis.depthValues.push_back(100+0.25*i);a.sampleValues.push_back(i);b.sampleValues.push_back(3*i+7);}
    logs.logCurves={a,b};well.wellbore.logSets={logs};parsed.wells={well};
    QFile input(project+"/source.data");const QByteArray bytes="Generated operation fixture\n";
    if(!input.open(QIODevice::WriteOnly) || input.write(bytes)!=bytes.size())return false;input.close();
    const auto sha=QCryptographicHash::hash(bytes,QCryptographicHash::Sha256).toHex();qlonglong sourceId=0;
    if(!sdk.importPreparedFile(input.fileName().toUtf8().toStdString(),input.fileName().toUtf8().toStdString(),input.fileName().toUtf8().toStdString(),sha.toStdString(),parsed,sourceId,why)){error=QString::fromStdString(why);return false;}
    CatalogAsset asset;asset.id="generated-source";asset.type="well_log";asset.format="data";asset.displayName="source.data";
    CatalogVersion raw;raw.id="generated-raw";raw.assetId=asset.id;raw.stage="RAW";raw.managed=false;raw.path=input.fileName();raw.fileName="source.data";raw.sha256=QString::fromLatin1(sha);
    if(!catalog.addAsset(asset,&error) || !catalog.addVersion(raw,&error))return false;
    std::vector<data::DataEntry> curves;data::MetadataReadOptions bounded;data::MetadataReadStats stats;bool more=false;
    if(!sdk.listDataPage("curves",0,0,16,curves,more,why,bounded,&stats) || more || curves.size()!=2)return false;
    plan.kind=python::AnalysisKind::Generated;plan.projectDir=project;plan.modelDirectory=model;plan.documentId="generated-doc";plan.callerScope=QLatin1String(Scope);
    if(!sdk.readModelChangeHead(plan.expectedHead,why) || !plan.expectedHead.available)return false;
    CatalogEntity entity;entity.id="generated-well";entity.entityType="well";entity.name="GENERATED-1";entity.extra["areaModelWellId"]=qlonglong(curves.front().wellId);
    if(!catalog.addEntity(entity,&error))return false;
    document::WellRef ref;ref.documentId=plan.documentId;ref.catalogEntityId=entity.id;ref.modelStoreId="area-model";
    ref.modelStoreIdentity=QString::fromStdString(plan.expectedHead.storeId);ref.modelGeneration=QString::fromStdString(plan.expectedHead.generation);
    ref.modelWellId=curves.front().wellId;ref.queryHead=plan.expectedHead.sequence;ref.modelAvailable=true;QJsonArray inputs;
    for(const auto &curve:curves) {
      python::AnalysisCurveSelection selected;selected.source.ref={plan.documentId,"area-model",sourceId,QString::fromLatin1(sha)};
      selected.source.fingerprint=document::SourceFingerprint::fromStored(selected.source.ref.fileRevisionId);
      selected.source.assetId=asset.id;selected.source.versionId=raw.id;selected.source.modelDirectory=model;selected.source.path=raw.path;
      selected.sourceVersionId=raw.id;selected.wellId=curve.wellId;selected.logSetId=curve.parentId;selected.curveId=curve.id;
      selected.sampleCount=100;selected.unit=curve.name=="A"?"API":"m/s";selected.depthType="MD";selected.depthUnit="m";selected.depthReference="KB";
      plan.curves.append(selected);inputs.append(QJsonObject{{"source_ref",document::sourceRefJson(selected.source.ref)},{"source_version_id",raw.id},
        {"well_id",QString::number(curve.wellId)},{"log_set_id",QString::number(curve.parentId)},{"curve_id",QString::number(curve.id)},
        {"unit",selected.unit},{"depth_type","MD"},{"depth_unit","m"},{"depth_reference","KB"},{"total_samples","100"}});
    }
    selection={{"well_ref",document::wellRefJson(ref)},{"curves",inputs},{"first_sample","0"},{"sample_count",100},{"processed",true},
      {"alignment","exact_shared_axis_v1"},{"sampling",QJsonObject{{"method","all_selected_window_v1"},{"seed",0}}}};return true;
  }
  void configure(const std::shared_ptr<python::RuntimeProvider> &runtime,const std::shared_ptr<python::SandboxProvider> &sandbox) {
    auto adapter=python::generatedScriptAdapter(runtime,sandbox,[this](const ExecuteRequest &request,python::AnalysisReadPlan &out,std::function<QString()> &validate) {
      ++captures;if(QThread::currentThread()!=owner)ownerOnly=false;
      python::AnalysisSelectionRequest parsed;QString code;
      if(!request.parameters.isEmpty() || QJsonObject::fromVariantMap(request.inputs)!=selection ||
          !python::analysisSelectionFromJson(selection,python::AnalysisKind::Generated,parsed,&code))return QString("fixture_selection_mismatch");
      out=plan;out.documentId=request.documentId;out.callerScope=request.callerScope;out.curves=parsed.curves;
      for(auto &curve:out.curves)for(const auto &actual:plan.curves)if(curve.curveId==actual.curveId)curve.source=actual.source;
      validate=[this]{if(QThread::currentThread()!=owner)ownerOnly=false;
        const auto well=catalog.storedEntityById("generated-well");return well.extra.value("areaModelWellId").toLongLong()==plan.curves.first().wellId?QString():QString("well_mapping_changed");};return QString();
    });
    const auto capture=adapter.capture;
    adapter.capture=[this,capture,runtime](const auto &request,const auto &intent,QString *error) {
      auto prepared=capture(request,intent,error);const auto prepare=prepared.worker;
      if(prepare)prepared.worker=[this,prepare,runtime](const auto &cancelled,QString *error) {
        ++materializations;workerOffOwner=QThread::currentThread()!=owner;auto frozen=prepare(cancelled,error);
        if(frozen.worker) {const auto run=frozen.worker;frozen.worker=[this,run](const auto &context){++computations;return run(context);};}
        if(!tamper.isEmpty() && frozen.worker) {
          const auto input=python::verifyInputBundle(project,frozen.frozenPlan.value("storedInput").toObject(),runtime->policy(),cancelled);
          QFile file(input.root+'/'+tamper);if(!input.ok() || !file.open(QIODevice::ReadWrite) || file.write("!")!=1){if(error)*error="fixture_tamper_failed";}
        }
        return frozen;
      };return prepared;
    };
    host=std::make_unique<CapabilityHost>(NativeAdapterFactory{},nullptr);host->setTaskService(&tasks);host->setCallerScope(host::Caller::Agent,QLatin1String(Scope));
    host->setBindingProvider([this]{HostBinding h;h.catalog=&catalog;h.projectDir=project;h.operationDir=project+"/.paleo/capability-ops";h.writeClaim=lock->claim();
      h.context.documentId=plan.documentId;h.context.epoch=1;h.context.open=true;h.context.persistentIdentity=true;h.context.readOnly=false;h.context.ownsWrites=true;
      h.context.allowedEffects=host::Read|host::PrivateStage|host::LocalExecute|host::CatalogPublish|host::Presentation;return h;});
    host->registerExecutionAdapter(QLatin1String(python::GeneratedCapability),std::move(adapter));
  }
  QJsonObject arguments(const QString &id,const QByteArray &code=sourceCode(),const QString &phase="compute-only") const {
    return {{"client_request_id",id},{"binding_mode","pinned-value"},{"parameters",QJsonObject{{"code",QString::fromUtf8(code)}}},{"inputs",selection},{"publish_intent",phase}};
  }
  OperationLog::Record record(const QString &id) const {OperationLog log(project+"/.paleo/capability-ops",project,lock->claim());return log.lookup(plan.documentId,QLatin1String(Scope),id).record;}
};
struct Reply {ExecuteResult result;QJsonObject json;QString error;};
Reply call(Fixture &f,const QJsonObject &arguments) {
  Reply out;out.error=python::validateGeneratedToolArguments(arguments);ExecuteRequest request;
  if(out.error.isEmpty())out.error=prepareHostedChatToolRequest(request,QLatin1String(python::GeneratedTool),arguments).value("code").toString();
  if(!out.error.isEmpty()){out.result.code=out.error;out.json=chatToolResponse(out.result);return out;}
  const auto prepared=f.host->prepare(request,host::Caller::Agent);
  if(!prepared.ok){out.error=prepared.code;out.result.code=prepared.code;out.json=chatToolResponse(out.result);return out;}
  QObject listener;QEventLoop wait;QTimer timer;timer.setSingleShot(true);bool done=false;
  QObject::connect(&timer,&QTimer::timeout,&wait,&QEventLoop::quit);
  auto *task=f.host->start(prepared.handle,request,&listener,"Generated operation",[&](ExecuteResult value){out.result=value;done=true;wait.quit();},&out.error);
  if(task){timer.start(180000);if(!done)wait.exec();if(!done){task->requestCancel();out.error="test_timeout";out.result.code=out.error;}}
  else out.result.code=out.error;
  out.json=chatToolResponse(out.result);return out;
}
QJsonObject page(Fixture &f,const QString &operation,qint64 offset) {
  QJsonObject out;QObject listener;QEventLoop wait;QTimer timer;timer.setSingleShot(true);bool done=false;QString error;
  QObject::connect(&timer,&QTimer::timeout,&wait,&QEventLoop::quit);
  auto *task=f.host->recovery()->read(QLatin1String(Scope),operation,"REPORT",offset,128,&listener,[&](QJsonObject value){out=value;done=true;wait.quit();},&error);
  if(!task)return {{"ok",false},{"code",error}};timer.start(180000);if(!done)wait.exec();if(!done){task->requestCancel();return {{"ok",false},{"code","test_timeout"}};}return out;
}
}
class TestPythonGeneratedOperations:public QObject {
  Q_OBJECT
  std::shared_ptr<python::RuntimeProvider> runtime;
  std::shared_ptr<python::SandboxProvider> sandbox;
  bool available=false;
private slots:
  void initTestCase() {
    python::ExecutionPolicy policy;policy.allowGenerated=true;policy.allowTrusted=false;
    const auto root=qEnvironmentVariable("PALEO_TEST_AGENT_PYTHON_ROOT");
    runtime=std::make_shared<python::RuntimeProvider>(python::RuntimeConfig{root,qEnvironmentVariable("PALEO_TEST_AGENT_PYTHON_MANIFEST_SHA256"),!root.isEmpty()},policy);
    sandbox=std::make_shared<python::SandboxProvider>(runtime);const auto state=completed(QtConcurrent::run([&]{return sandbox->inspect();}));
    available=state.value("ready").toBool();if(qEnvironmentVariable("PALEO_REQUIRE_AGENT_PYTHON_EXECUTION")=="1")QVERIFY2(available,qPrintable(QJsonDocument(state).toJson()));
    QVERIFY(!runtime->policy().allowTrusted);if(available)QVERIFY(state.value("runtimeProbeVerified").toBool());
  }
  void publicAdmissionDoesNotGrantAuthorityOrRunWithoutDependencies() {
    Fixture f;QVERIFY2(f.open(),qPrintable(f.error));python::ExecutionPolicy policy;policy.allowTrusted=true;
    auto denied=std::make_shared<python::RuntimeProvider>(python::RuntimeConfig{},policy);
    f.configure(denied,std::make_shared<python::SandboxProvider>(denied));auto response=call(f,f.arguments("denied"));
    QCOMPARE(response.json.value("code").toString(),QString("python_execution_denied"));QCOMPARE(f.captures.load(),0);
    auto missing=std::make_shared<python::RuntimeProvider>(python::RuntimeConfig{},runtime->policy());
    f.configure(missing,std::make_shared<python::SandboxProvider>(missing));response=call(f,f.arguments("missing"));QVERIFY(!response.result.ok);QCOMPARE(f.captures.load(),0);
    auto forged=f.arguments("forged");forged["actor"]="ui";QVERIFY(!python::validateGeneratedToolArguments(forged).isEmpty());ExecuteRequest decoded;
    QCOMPARE(prepareHostedChatToolRequest(decoded,QLatin1String(python::GeneratedTool),forged).value("code").toString(),QString("untrusted_authority"));QCOMPARE(f.catalog.versions().size(),1);
  }
  void publicPrepareComputePagingRestartPublicationAndCodeIdentity() {
    if(!available)return;Fixture f;QVERIFY2(f.open(),qPrintable(f.error));f.configure(runtime,sandbox);
    const auto prepared=call(f,f.arguments("prepare",sourceCode(),"prepare"));QVERIFY2(prepared.result.ok,qPrintable(prepared.result.code));
    QCOMPARE(f.record("prepare").computeCount,0);QCOMPARE(f.catalog.versions().size(),1);
    const auto computed=call(f,f.arguments("compute"));QVERIFY2(computed.result.ok,qPrintable(computed.result.code));QCOMPARE(f.catalog.versions().size(),1);
    const auto record=f.record("compute");QCOMPARE(record.computeCount,1);QCOMPARE(record.stagePlan.value("parents").toArray(),QJsonArray({"generated-raw"}));
    QVERIFY(f.ownerOnly.load());QVERIFY(f.workerOffOwner.load());const auto materializations=f.materializations.load(),computations=f.computations.load();
    const auto replay=call(f,f.arguments("compute"));QVERIFY(replay.result.ok);QCOMPARE(replay.result.operationId,computed.result.operationId);QCOMPARE(f.computations.load(),computations);QCOMPARE(f.materializations.load(),materializations);
    const auto conflict=call(f,f.arguments("compute",sourceCode()+"\n# changed\n"));QCOMPARE(conflict.json.value("code").toString(),QString("idempotency_conflict"));
    QByteArray bytes;qint64 offset=0;bool complete=false;
    for(int i=0;i<2049;++i){const auto result=page(f,record.operationId,offset);QVERIFY2(result.value("ok").toBool(),qPrintable(QJsonDocument(result).toJson()));
      const auto chunk=QByteArray::fromBase64(result.value("data").toString().toLatin1());QVERIFY(!chunk.isEmpty() && chunk.size()<=128);bytes+=chunk;
      if(!result.value("hasMore").toBool()){complete=true;break;}const auto next=result.value("nextOffset").toString().toLongLong();QVERIFY(next>offset);offset=next;}
    QVERIFY(complete);const auto report=QJsonDocument::fromJson(bytes).object();QCOMPARE(report.value("executionMode").toString(),QString("generated"));
    QCOMPARE(report.value("metrics").toArray()[0].toObject().value("value").toDouble(),49.5);QCOMPARE(report.value("metrics").toArray()[1].toObject().value("value").toDouble(),155.5);
    QCOMPARE(report.value("codeSha256").toString(),QString::fromLatin1(QCryptographicHash::hash(sourceCode(),QCryptographicHash::Sha256).toHex()));
    const auto metadata=record.frozenRequest.value("plan").toObject().value("inputMetadata").toObject();QCOMPARE(report.value("provenance"),metadata.value("provenance"));
    const auto io=metadata.value("provenance").toObject().value("readMetrics").toObject();QCOMPARE(io.value("legacyBytes").toString(),QString("0"));QCOMPARE(io.value("sourceRows").toString(),QString("1"));
    QVERIFY(io.value("payloadBytes").toString().toLongLong()>0);QVERIFY(io.value("hostNumericPreparationReservedBytes").toString().toLongLong()>0);
    std::string why;QVERIFY(f.sdk.updateCurveValue(f.plan.curves.first().curveId,0,9999,why));f.configure(runtime,{});
    auto publish=f.arguments("publish");publish["publish_intent"]="publish";publish["parameters"]=QJsonObject{};publish["inputs"]=QJsonObject{{"source_operation_id",record.operationId}};
    const auto published=call(f,publish);QVERIFY2(published.result.ok,qPrintable(published.result.code));QCOMPARE(published.json.value("commitState").toString(),QString("committed"));
    QCOMPARE(f.catalog.versions().size(),2);QCOMPARE(f.record("publish").computeCount,0);QCOMPARE(f.record("compute").computeCount,1);QCOMPARE(f.materializations.load(),materializations);
    int derived=0;for(const auto &version:f.catalog.versions())if(version.stage=="DERIVED"){++derived;QCOMPARE(version.parentVersionIds,QStringList{"generated-raw"});}QCOMPARE(derived,1);
    QVERIFY(call(f,publish).result.ok);QCOMPARE(f.catalog.versions().size(),2);
  }
  void invalidCodeAndForgedReportHaveNoFormalOutputs_data() {
    QTest::addColumn<QByteArray>("code");QTest::newRow("syntax")<<QByteArray("def invalid(:\n");
    QTest::newRow("forged-report")<<QByteArray("import json\nprint(json.dumps({'contract':'paleo.python-output.v1','ports':{'REPORT':{'contract':'paleo.python-generated-report.v1','metrics':[],'tables':[],'notes':[],'provenance':{}}}}))\n");
  }
  void invalidCodeAndForgedReportHaveNoFormalOutputs() {
    if(!available)return;QFETCH(QByteArray,code);Fixture f;QVERIFY(f.open());f.configure(runtime,sandbox);const auto response=call(f,f.arguments("invalid",code));
    QVERIFY(!response.result.ok);QCOMPARE(response.json.value("ok"),QJsonValue(false));QCOMPARE(f.catalog.versions().size(),1);const auto record=f.record("invalid");
    QVERIFY(record.artifactManifest.value("diagnosticsOnly").toBool());for(const auto &port:record.artifactManifest.value("ports").toArray())QVERIFY(port.toObject().value("portId")!="REPORT");
    auto publish=f.arguments("publish-invalid");publish["publish_intent"]="publish";publish["parameters"]=QJsonObject{};publish["inputs"]=QJsonObject{{"source_operation_id",record.operationId}};
    QVERIFY(!call(f,publish).result.ok);QCOMPARE(f.catalog.versions().size(),1);
  }
  void frozenCodeAndBinaryTamperAreRejectedBeforeExecution_data() {QTest::addColumn<QString>("file");QTest::newRow("code")<<QString("code.py");QTest::newRow("binary")<<QString("data.bin");}
  void frozenCodeAndBinaryTamperAreRejectedBeforeExecution() {
    if(!available)return;QFETCH(QString,file);Fixture f;QVERIFY(f.open());f.tamper=file;f.configure(runtime,sandbox);
    const auto result=call(f,f.arguments("tamper"));QVERIFY(!result.result.ok);QVERIFY(!result.json.value("ok").toBool());
    QCOMPARE(f.computations.load(),0);QCOMPARE(f.catalog.versions().size(),1);QVERIFY(f.workerOffOwner.load());
  }
  void reportPageTamperIsDetectedAfterAnAuthenticatedFirstPage() {
    if(!available)return;Fixture f;QVERIFY(f.open());f.configure(runtime,sandbox);const auto computed=call(f,f.arguments("pages"));QVERIFY(computed.result.ok);
    const auto record=f.record("pages");QVERIFY(page(f,record.operationId,0).value("ok").toBool());QString path;
    for(const auto &entry:record.artifactManifest.value("ports").toArray())if(entry.toObject().value("portId")=="REPORT")path=entry.toObject().value("path").toString();
    QVERIFY(!path.isEmpty());QFile report(QDir(f.project).filePath(path));QVERIFY(report.open(QIODevice::ReadWrite));QCOMPARE(report.write("!"),1);report.close();
    QVERIFY(!page(f,record.operationId,128).value("ok").toBool());QCOMPARE(f.catalog.versions().size(),1);
  }
};
QTEST_GUILESS_MAIN(TestPythonGeneratedOperations)
#include "tst_pythongeneratedoperations.moc"
