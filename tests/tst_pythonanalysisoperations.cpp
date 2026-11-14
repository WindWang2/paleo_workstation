// 层：测试壳
#include "workflow/pythonanalysisoperation.h"
#include "workflow/pythonoperationdiagnostics.h"
#include "workflow/capabilitynativeoperation.h"
#include "services/pythonanalysiscapabilityspec.h"
#include "services/pythonanalysisreport.h"
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
#include <QJsonArray>
#include <QJsonDocument>
#include <QTemporaryDir>
#include <QTimer>
#include <atomic>

using namespace paleo;
using namespace paleo::capability;
namespace {
QString templateRoot() {return QString::fromUtf8(PALEO_TEST_PYTHON_ANALYSIS_TEMPLATES);}
python::ExecutionPolicy allowedPolicy() {python::ExecutionPolicy out;out.allowTrusted=true;return out;}
std::shared_ptr<python::RuntimeProvider> runtime(const python::ExecutionPolicy &policy=allowedPolicy()) {
  return std::make_shared<python::RuntimeProvider>(python::RuntimeConfig{
    qEnvironmentVariable("PALEO_TEST_AGENT_PYTHON_ROOT"),qEnvironmentVariable("PALEO_TEST_AGENT_PYTHON_MANIFEST_SHA256"),true},policy);
}
struct Fixture {
  QTemporaryDir temporary;
  QString project,model,error;
  data::DataService sdk;
  DataCatalog catalog;
  std::unique_ptr<ProjectDirLock> lock;
  PaleoTaskService tasks;
  python::AnalysisReadPlan plan;
  std::unique_ptr<CapabilityHost> host;
  CapabilityExecutionAdapter adapter;
  quint64 epoch=1;
  bool execute=true,ownerValid=true;
  std::atomic_bool captureOnOwner=true,workerOffOwner=false;
  std::atomic_int captures=0,materializations=0,ownerValidations=0;
  QThread *owner=QThread::currentThread();
  ~Fixture(){host.reset();tasks.shutdown(5000,true);}
  bool open(python::AnalysisKind kind=python::AnalysisKind::AttributeCorrelation) {
    project=temporary.path()+"/project";model=project+"/model";std::string detail;
    if(!QDir().mkpath(project))return false;
    lock=std::make_unique<ProjectDirLock>(project);
    if(!lock->tryLock(&error) || !catalog.open(project,&error))return false;
    if(!sdk.createProject(model.toUtf8().toStdString(),"Python operation fixture",detail))return false;
    data::ParsedFile parsed;parsed.kind="logs";parsed.status="parsed";
    data::WellDataset well;well.wellInfo.wellName="ANALYSIS-OP";
    data::LogSet logs;logs.logSetName="Explicit shared axis";logs.depthInfo={"MD","m","KB","","",""};
    data::LogCurve left,right;left.curveName="A";left.curveUnit="API";right.curveName="B";right.curveUnit="m/s";
    for(int i=0;i<1024;++i){logs.samplingAxis.depthValues.push_back(100+0.25*i);left.sampleValues.push_back(i);right.sampleValues.push_back(3*i+7);}
    logs.logCurves={left,right};well.wellbore.logSets={logs};parsed.wells={well};
    QFile source(project+"/source.data");const QByteArray bytes="Typed operation fixture\n";
    if(!source.open(QIODevice::WriteOnly) || source.write(bytes)!=bytes.size())return false;source.close();
    const auto hash=QCryptographicHash::hash(bytes,QCryptographicHash::Sha256).toHex();qlonglong sourceId=0;
    if(!sdk.importPreparedFile(source.fileName().toUtf8().toStdString(),source.fileName().toUtf8().toStdString(),
      source.fileName().toUtf8().toStdString(),hash.toStdString(),parsed,sourceId,detail)){error=QString::fromStdString(detail);return false;}
    CatalogAsset asset;asset.id="ast-source";asset.type="well_log";asset.format="data";asset.displayName="source.data";
    CatalogVersion version;version.id="ver-source";version.assetId=asset.id;version.stage="RAW";version.managed=false;
    version.path=source.fileName();version.fileName="source.data";version.sha256=QString::fromLatin1(hash);
    if(!catalog.addAsset(asset,&error) || !catalog.addVersion(version,&error))return false;
    std::vector<data::DataEntry> curves;data::SourceIndexEntry registered;
    if(!sdk.listData("curves",0,curves,detail) || curves.size()!=2 || !sdk.readSourceIndexEntry(sourceId,registered,detail))return false;
    plan.kind=kind;plan.projectDir=project;plan.modelDirectory=model;plan.documentId="doc";plan.callerScope="agent:test";
    if(!sdk.readModelChangeHead(plan.expectedHead,detail))return false;
    for(const auto &curve:curves){python::AnalysisCurveSelection selection;
      selection.source.ref={"doc","area-model",sourceId,QString::fromStdString(registered.sha256)};
      selection.source.fingerprint=document::SourceFingerprint::fromStored(selection.source.ref.fileRevisionId);
      selection.source.assetId=asset.id;selection.source.versionId=version.id;selection.source.modelDirectory=model;
      selection.source.path=version.path;selection.sourceVersionId=version.id;selection.wellId=curve.wellId;
      selection.logSetId=curve.parentId;selection.curveId=curve.id;selection.firstSample=100;selection.sampleCount=100;
      selection.unit=curve.name=="A"?"API":"m/s";selection.depthType="MD";selection.depthUnit="m";selection.depthReference="KB";
      plan.curves.append(selection);
    }
    return true;
  }
  python::AnalysisPlanProvider selector() {
    return [this](const ExecuteRequest &request,python::AnalysisReadPlan &out,std::function<QString()> &validate) {
      ++captures;if(QThread::currentThread()!=owner)captureOnOwner=false;
      if(request.inputs!=QVariantMap{{"selection",QString("registered-curves")}} || !request.parameters.isEmpty())return QString("invalid_python_parameters");
      out=plan;out.documentId=request.documentId;out.callerScope=request.callerScope;
      validate=[this]{++ownerValidations;if(QThread::currentThread()!=owner)captureOnOwner=false;
        return ownerValid?QString():QStringLiteral("python_analysis_source_changed");};return QString();
    };
  }
  void configure(const std::shared_ptr<python::RuntimeProvider> &provider,const QString &root=templateRoot()) {
    adapter=python::analysisScriptAdapter(plan.kind,provider,root,selector());
    const auto capture=adapter.capture;
    adapter.capture=[this,capture](const auto &request,const auto &intent,QString *error){
      auto preparation=capture(request,intent,error);const auto work=preparation.worker;
      if(work)preparation.worker=[this,work](const auto &cancelled,QString *error){++materializations;
        workerOffOwner=QThread::currentThread()!=owner;return work(cancelled,error);};return preparation;
    };
    host=std::make_unique<CapabilityHost>();host->setTaskService(&tasks);host->setCallerScope(host::Caller::Agent,"agent:test");
    host->setBindingProvider([this]{HostBinding h;h.projectDir=project;h.operationDir=project+"/.paleo/capability-ops";
      h.catalog=&catalog;h.writeClaim=lock->claim();h.context.documentId="doc";h.context.epoch=epoch;
      h.context.open=true;h.context.persistentIdentity=true;h.context.readOnly=false;h.context.ownsWrites=true;
      h.context.allowedEffects=host::Read|host::PrivateStage|host::CatalogPublish|host::Presentation;
      if(execute)h.context.allowedEffects|=host::LocalExecute;return h;});
    host->registerExecutionAdapter(python::analysisCapabilityId(plan.kind),adapter);
  }
  ExecuteRequest request(const QString &id,const QString &phase="compute-only") const {
    ExecuteRequest request;request.capabilityId=python::analysisCapabilityId(plan.kind);request.documentId="doc";
    request.clientRequestId=id;request.publishIntent=phase;request.inputs={{"selection",QString("registered-curves")}};return request;
  }
  OperationLog::Record record(const QString &id) const {
    OperationLog log(project+"/.paleo/capability-ops",project,lock->claim());return log.lookup("doc","agent:test",id).record;
  }
};
bool start(Fixture &fixture,const ExecuteRequest &request,QObject *,ExecuteResult &out,QString &error) {
  const auto prepared=fixture.host->prepare(request,host::Caller::Agent);if(!prepared.ok){error=prepared.code;return false;}
  bool done=false;QObject listener;QEventLoop loop;QTimer timer;timer.setSingleShot(true);
  QObject::connect(&timer,&QTimer::timeout,&loop,&QEventLoop::quit);
  auto *task=fixture.host->start(prepared.handle,request,&listener,"Analysis operation",[&](ExecuteResult value){out=value;done=true;loop.quit();},&error);
  if(!task)return false;timer.start(30000);if(!done)loop.exec();if(!done)task->requestCancel();return done;
}
bool resume(Fixture &fixture,const QString &operationId,QObject *,ExecuteResult &out,QString &error) {
  bool done=false;QObject listener;QEventLoop loop;QTimer timer;timer.setSingleShot(true);QObject::connect(&timer,&QTimer::timeout,&loop,&QEventLoop::quit);
  auto *task=fixture.host->recovery()->start("agent:test",operationId,&listener,"Restore analysis",[&](ExecuteResult value){out=value;done=true;loop.quit();},true,&error);
  if(!task)return false;timer.start(30000);if(!done)loop.exec();if(!done)task->requestCancel();return done;
}
}
class PythonAnalysisOperationTests:public QObject {
  Q_OBJECT
  std::shared_ptr<python::RuntimeProvider> provider;
  bool available() const {return provider && provider->snapshot().ready();}
private slots:
  void initTestCase() {
    provider=runtime();const auto state=provider->inspect();
    if(qEnvironmentVariableIsSet("PALEO_REQUIRE_AGENT_PYTHON_EXECUTION"))QVERIFY2(state.ready(),qPrintable(state.code));
    if(!qEnvironmentVariable("PALEO_TEST_AGENT_PYTHON_ROOT").isEmpty())QVERIFY2(state.ready(),qPrintable(state.code));
  }
  void missingEnvironmentAndPolicyRefuseBeforeSourceCapture() {
    Fixture f;QVERIFY(f.open());f.configure(runtime(python::ExecutionPolicy{}));
    QCOMPARE(f.host->prepare(f.request("denied"),host::Caller::Agent).code,QString("python_execution_denied"));QCOMPARE(f.captures.load(),0);
    f.configure(runtime());const auto prepared=f.host->prepare(f.request("missing"),host::Caller::Agent);
    QVERIFY(!prepared.ok);QCOMPARE(f.captures.load(),0);
  }
  void processLogsRemainPrivateAndUseTheBoundedFailurePrefix() {
    QTemporaryDir directory;OperationWorkContext context;context.privateDir=directory.path();OperationWorkResult result;
    python::ProcessResult process;process.code="python_output_budget_exceeded";process.output="unvalidated numerical output";
    process.outputLog="prefix ignored suffix";process.error="error ignored suffix";auto policy=allowedPolicy();policy.logBytes=6;
    python::appendPythonProcessDiagnostics(result,context,process,policy);
    QVERIFY(result.outputs.artifacts.isEmpty());QVERIFY(!result.outputs.ok);QCOMPARE(result.diagnostics.artifacts.size(),3);
    for(const auto &artifact:result.diagnostics.artifacts) {
      QVERIFY(artifact.extra.value("diagnostic").toBool());QFile file(artifact.absolutePath);QVERIFY(file.open(QIODevice::ReadOnly));
      if(artifact.portId=="STDOUT_LOG")QCOMPARE(file.readAll(),QByteArray("prefix"));
      if(artifact.portId=="STDERR_LOG")QCOMPARE(file.readAll(),QByteArray("error "));
    }
  }
  void changedOptionalTemplateIsRejectedBeforeMaterialization() {
    if(!available())return;Fixture f;QVERIFY(f.open());QTemporaryDir scripts;
    const auto name=python::analysisKindId(f.plan.kind)+".py";QVERIFY(QFile::copy(templateRoot()+'/'+name,scripts.path()+'/'+name));
    QFile changed(scripts.path()+'/'+name);QVERIFY(changed.open(QIODevice::Append));QVERIFY(changed.write("\n# modified\n")>0);changed.close();
    f.configure(provider,scripts.path());QCOMPARE(f.host->prepare(f.request("template-changed"),host::Caller::Agent).code,QString("python_template_changed"));
    QCOMPARE(f.materializations.load(),0);QCOMPARE(f.catalog.versions().size(),1);
  }
  void actualAnalysesUsePrepareComputePublishAndIdempotentReplay_data() {
    QTest::addColumn<bool>("quality");QTest::newRow("quality")<<true;QTest::newRow("correlation")<<false;
  }
  void actualAnalysesUsePrepareComputePublishAndIdempotentReplay() {
    if(!available())return;QFETCH(bool,quality);Fixture f;QVERIFY2(f.open(quality?python::AnalysisKind::WellQuality:python::AnalysisKind::AttributeCorrelation),qPrintable(f.error));f.configure(provider);
    const auto prepared=f.host->prepare(f.request("prepare","prepare"),host::Caller::Agent);QVERIFY2(prepared.ok,qPrintable(prepared.code));
    QCOMPARE(f.materializations.load(),0);QCOMPARE(f.catalog.versions().size(),1);
    ExecuteResult result;QString error;
    QVERIFY(start(f,f.request("prepare","prepare"),this,result,error));QVERIFY2(result.ok,qPrintable(result.code));
    QCOMPARE(f.record("prepare").computeCount,0);QCOMPARE(f.catalog.versions().size(),1);
    QVERIFY(f.captureOnOwner.load());QVERIFY(f.workerOffOwner.load());QVERIFY(f.ownerValidations.load()>0);
    QVERIFY(start(f,f.request("compute"),this,result,error));QVERIFY2(result.ok,qPrintable(result.code));QCOMPARE(f.catalog.versions().size(),1);
    const auto computed=f.record("compute");QCOMPARE(computed.computeCount,1);
    const auto metadata=computed.frozenRequest.value("plan").toObject().value("inputMetadata").toObject();
    const auto io=metadata.value("provenance").toObject().value("readMetrics").toObject();
    QCOMPARE(io.value("legacyBytes").toString(),QString("0"));QVERIFY(io.value("payloadBytes").toString().toLongLong()>0);
    QCOMPARE(computed.stagePlan.value("parents").toArray(),QJsonArray({"ver-source"}));
    const auto materializations=f.materializations.load();QVERIFY(start(f,f.request("compute"),this,result,error));QVERIFY(result.ok);
    QCOMPARE(f.materializations.load(),materializations);QCOMPARE(f.record("compute").computeCount,1);
    auto publication=f.request("publish","publish");publication.inputs={{"source_operation_id",computed.operationId}};
    QVERIFY(start(f,publication,this,result,error));QVERIFY2(result.ok,qPrintable(result.code));QCOMPARE(result.commitState,QString("committed"));
    QCOMPARE(f.catalog.versions().size(),2);QCOMPARE(f.record("publish").computeCount,0);QCOMPARE(f.materializations.load(),materializations);
    for(const auto &version:f.catalog.versions())if(version.stage=="DERIVED")QCOMPARE(version.parentVersionIds,QStringList({"ver-source"}));
    QVERIFY(start(f,publication,this,result,error));QVERIFY(result.ok);QCOMPARE(f.catalog.versions().size(),2);
    QCOMPARE(f.host->recovery()->results("other-caller",computed.operationId).value("code").toString(),QString("operation_not_found"));
  }
  void frozenBundleRestoresWithoutTemplateDirectoryOrNewSdkHead() {
    if(!available())return;Fixture f;QVERIFY(f.open());QTemporaryDir scripts;
    const auto name=python::analysisKindId(f.plan.kind)+".py";QVERIFY(QFile::copy(templateRoot()+'/'+name,scripts.path()+'/'+name));f.configure(provider,scripts.path());
    ExecuteRequest request=f.request("restore");request.projectDir=f.project;request.callerScope="agent:test";
    QString error;auto preparation=f.adapter.capture(request,{{"id","restore"}},&error);QVERIFY2(preparation.worker,qPrintable(error));
    auto future=QtConcurrent::run([&]{return preparation.worker({},&error);});future.waitForFinished();const auto frozen=future.result();QVERIFY2(error.isEmpty(),qPrintable(error));
    QVERIFY(preparation.validate().isEmpty());const auto accepted=f.host->recovery()->accept(frozen);QVERIFY2(accepted.ok,qPrintable(accepted.code));
    QVERIFY(QFile::remove(scripts.path()+'/'+name));std::string detail;
    QVERIFY(f.sdk.updateCurveValue(f.plan.curves.first().curveId,100,9999,detail));
    f.host->recovery()->invalidateRuntimeHooks(false);ExecuteResult result;
    QVERIFY(resume(f,accepted.record.operationId,this,result,error));QVERIFY2(result.ok,qPrintable(result.code));
    QCOMPARE(result.summary.value("jointValidCount").toInt(),100);QCOMPARE(result.summary.value("redundantPairs").toInt(),1);
    QCOMPARE(f.catalog.versions().size(),1);
    auto record=f.record("restore");QCOMPARE(record.computeCount,1);record.callerScope="other";
    QCOMPARE(f.adapter.validateFrozen(record),QString("invalid_python_analysis_plan"));
    record=f.record("restore");auto plan=record.frozenRequest.value("plan").toObject();plan.insert("scriptSha256",QString(64,'0'));record.frozenRequest.insert("plan",plan);
    QCOMPARE(f.adapter.validateFrozen(record),QString("invalid_python_analysis_plan"));
    const auto denied=python::analysisScriptAdapter(f.plan.kind,runtime(python::ExecutionPolicy{}),{},{});
    QCOMPARE(denied.validateFrozen(f.record("restore")),QString("python_execution_denied"));
  }
  void sourceValidationAndHostContextCancelPreparation_data() {
    QTest::addColumn<QString>("change");for(const auto &name:{"source","epoch","permission","owner","subscriber"})QTest::newRow(name)<<QString::fromLatin1(name);
  }
  void sourceValidationAndHostContextCancelPreparation() {
    if(!available())return;QFETCH(QString,change);Fixture f;QVERIFY(f.open());f.configure(provider);
    auto permit=python::claimExecutionPermit();const auto request=f.request("cancel-preparation");
    const auto prepared=f.host->prepare(request,host::Caller::Agent);QVERIFY(prepared.ok);auto *subscriber=new QObject;bool delivered=false;ExecuteResult result;QString error;
    QVERIFY(f.host->start(prepared.handle,request,subscriber,"Preparation cancellation",[&](ExecuteResult value){result=value;delivered=true;},&error));
    QTRY_COMPARE_WITH_TIMEOUT(f.materializations.load(),1,2000);
    if(change=="source")f.ownerValid=false;else if(change=="epoch")++f.epoch;else if(change=="permission")f.execute=false;
    else if(change=="owner")f.host.reset();else {delete subscriber;subscriber=nullptr;}
    permit.reset();QTRY_COMPARE_WITH_TIMEOUT(f.tasks.runningCount(),0,10000);
    if(change=="owner" || change=="subscriber")QVERIFY(!delivered);else {QVERIFY(delivered);QVERIFY(!result.ok);}
    QCOMPARE(f.catalog.versions().size(),1);delete subscriber;
  }
  void tamperedFrozenBundleNeverProducesAReport() {
    if(!available())return;Fixture f;QVERIFY(f.open());f.configure(provider);
    ExecuteRequest request=f.request("tamper");request.projectDir=f.project;request.callerScope="agent:test";QString error;
    auto preparation=f.adapter.capture(request,{},&error);QVERIFY(preparation.worker);
    auto future=QtConcurrent::run([&]{return preparation.worker({},&error);});future.waitForFinished();const auto frozen=future.result();QVERIFY(error.isEmpty());
    const auto accepted=f.host->recovery()->accept(frozen);QVERIFY2(accepted.ok,qPrintable(accepted.code));
    const auto bundle=python::verifyInputBundle(f.project,frozen.frozenPlan.value("storedInput").toObject(),provider->policy());QVERIFY(bundle.ok());
    QFile changed(bundle.root+"/data.bin");QVERIFY(changed.open(QIODevice::ReadWrite));QCOMPARE(changed.write("!"),1);changed.close();
    f.host->recovery()->invalidateRuntimeHooks(false);ExecuteResult result;QVERIFY(resume(f,accepted.record.operationId,this,result,error));
    QVERIFY(!result.ok);QCOMPARE(f.catalog.versions().size(),1);
    QVERIFY(f.host->recovery()->results("agent:test",accepted.record.operationId).value("ports").toArray().isEmpty());
  }
  void reportPagingAndContractValidationRetainIntegrity() {
    if(!available())return;Fixture f;QVERIFY(f.open());f.configure(provider);ExecuteResult result;QString error;
    QVERIFY(start(f,f.request("pages"),this,result,error));QVERIFY2(result.ok,qPrintable(result.code));const auto record=f.record("pages");
    QJsonObject page;bool done=false;
    QVERIFY(f.host->recovery()->read("agent:test",record.operationId,"REPORT",0,128,this,[&](QJsonObject value){page=value;done=true;},&error));
    QTRY_VERIFY_WITH_TIMEOUT(done,5000);QVERIFY(page.value("ok").toBool());QVERIFY(QByteArray::fromBase64(page.value("data").toString().toLatin1()).size()<=128);
    const auto ports=record.artifactManifest.value("ports").toArray();QString path;
    for(const auto &port:ports)if(port.toObject().value("portId")=="REPORT")path=port.toObject().value("path").toString();
    QVERIFY(!path.isEmpty());QFile report(QDir(f.project).filePath(path));QVERIFY(report.open(QIODevice::ReadOnly));const auto bytes=report.readAll();report.close();
    const auto metadata=record.frozenRequest.value("plan").toObject().value("inputMetadata").toObject();
    auto value=QJsonDocument::fromJson(bytes).object();auto provenance=value.value("provenance").toObject();provenance.insert("callerScope","other");value.insert("provenance",provenance);
    const auto altered=QJsonDocument(QJsonObject{{"contract","paleo.python-output.v1"},{"ports",QJsonObject{{"REPORT",value}}}}).toJson(QJsonDocument::Compact);
    QCOMPARE(python::validateAnalysisReport(altered,f.plan.kind,metadata,provider->policy()).code,QString("python_output_provenance_mismatch"));
    QVERIFY(report.open(QIODevice::ReadWrite));QCOMPARE(report.write("!"),1);report.close();done=false;
    QVERIFY(f.host->recovery()->read("agent:test",record.operationId,"REPORT",128,128,this,[&](QJsonObject value){page=value;done=true;},&error));
    QTRY_VERIFY_WITH_TIMEOUT(done,5000);QVERIFY(!page.value("ok").toBool());QCOMPARE(f.catalog.versions().size(),1);
  }
};
QTEST_GUILESS_MAIN(PythonAnalysisOperationTests)
#include "tst_pythonanalysisoperations.moc"
