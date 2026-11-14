// 层：测试壳
#include "workflow/pythonscriptoperation.h"
#include "workflow/capabilitynativeoperation.h"
#include "services/pythoncapabilityspec.h"
#include "services/operationretention.h"
#include "catalog/datacatalog.h"
#include "metadata/projectlock.h"
#include "services/paleotaskservice.h"
#include <QtTest>
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
struct Host {
  DataCatalog catalog;std::unique_ptr<ProjectDirLock> lock;PaleoTaskService tasks;CapabilityRecovery recovery;
  bool execute=true;quint64 epoch=1;QString root;CapabilityExecutionAdapter adapter;
  bool open(const QString &directory) {
    root=directory;lock=std::make_unique<ProjectDirLock>(root);
    if(!lock->tryLock() || !catalog.open(root))return false;
    recovery.setTaskService(&tasks);recovery.setHostProvider([this] {
      RecoveryHost h;h.projectDir=root;h.operationDir=root+"/.paleo/capability-ops";h.catalog=&catalog;h.writeClaim=lock->claim();
      h.context.documentId="doc";h.context.epoch=epoch;h.context.open=true;h.context.persistentIdentity=true;
      h.context.readOnly=false;h.context.ownsWrites=true;h.context.allowedEffects=host::Read|host::PrivateStage|host::CatalogPublish|host::Presentation;
      if(execute)h.context.allowedEffects|=host::LocalExecute;
      h.validateFrozen=adapter.validateFrozen;h.restoreWorker=adapter.restoreWorker;h.restoreInputValidator=adapter.restoreInputValidator;return h;
    });return true;
  }
};
bool run(Host &host,const QString &scope,const QString &id,ExecuteResult &result,QString &error,bool resume=false) {
  QObject subscriber;QEventLoop loop;QTimer timer;timer.setSingleShot(true);bool done=false;
  QObject::connect(&timer,&QTimer::timeout,&loop,&QEventLoop::quit);
  auto *task=host.recovery.start(scope,id,&subscriber,"Python test",[&](ExecuteResult value){result=value;done=true;loop.quit();},resume,&error);
  if(!task)return false;
  timer.start(30000);if(!done)loop.exec();if(!done)task->requestCancel();return done;
}
FrozenOperation controlPlan(const QString &id) {
  FrozenOperation plan;plan.capabilityId="test.python-control";plan.implementationVersion="1";
  plan.callerScope="agent:test";plan.clientRequestId=id;plan.intent={{"id",id}};plan.phase="publish";
  plan.requiredEffects=host::LocalExecute;plan.outputSpec.id="test:python-control";plan.outputSpec.complete=true;
  AlgoPort output;output.id="OUTPUT";output.kind="file";plan.outputSpec.outputs={output};plan.outputPorts={"OUTPUT"};
  return plan;
}
}
class PythonOperationTests:public QObject {
  Q_OBJECT
private slots:
  void internalTemplateDoesNotBecomeAnAgentCapability() {
    const auto spec=CapabilityRegistry::spec(QLatin1String(python::SummaryCapability),1);
    QCOMPARE(spec.exposure,CapabilityExposure::Internal);QVERIFY(spec.executionEffects&host::LocalExecute);
    QVERIFY(!CapabilityRegistry::callableIds().contains(spec.capabilityId));
    CapabilityHost host;host.setCallerScope(host::Caller::Batch,"batch");
    host.setBindingProvider([] {
      HostBinding binding;binding.context.documentId="doc";binding.context.open=true;binding.context.persistentIdentity=true;
      binding.context.readOnly=false;binding.context.ownsWrites=true;binding.context.allowedEffects=127;return binding;
    });
    ExecuteRequest request;request.capabilityId=spec.capabilityId;request.clientRequestId="internal";request.publishIntent="compute-only";
    QCOMPARE(host.prepare(request,host::Caller::Batch).code,QString("capability_not_callable"));
  }
  void executionPermissionSurvivesPersistenceAndIsRecheckedAtCommit() {
    QTemporaryDir project;Host host;QVERIFY(host.open(project.path()));auto plan=controlPlan("effects");std::atomic_int count=0;
    plan.worker=[&](const OperationWorkContext &context) {
      ++count;OperationWorkResult result;QFile file(context.privateDir+"/OUTPUT.bin");
      result.ok=file.open(QIODevice::WriteOnly)&&file.write("result")==6;file.close();result.outputs.ok=result.ok;
      result.outputs.artifacts.append({"OUTPUT",file.fileName(),{},{},{},{}});return result;
    };
    host.execute=false;QCOMPARE(host.recovery.accept(plan).code,QString("effect_denied"));
    host.execute=true;const auto accepted=host.recovery.accept(plan);QVERIFY2(accepted.ok,qPrintable(accepted.code));
    QCOMPARE(accepted.record.frozenRequest.value("executionEffects").toInt(),int(host::LocalExecute));
    ExecuteResult result;QString error;host.execute=false;
    QVERIFY(!run(host,plan.callerScope,accepted.record.operationId,result,error));QCOMPARE(error,QString("effect_denied"));QCOMPARE(count.load(),0);
    host.execute=true;host.recovery.setCheckpointHook([&](const QString &point){if(point=="prepared")
      QMetaObject::invokeMethod(&host.recovery,[&]{host.execute=false;},Qt::QueuedConnection);});
    QVERIFY(run(host,plan.callerScope,accepted.record.operationId,result,error));QVERIFY(!result.ok);QCOMPARE(result.code,QString("effect_denied"));
    QCOMPARE(host.catalog.versions().size(),0);QCOMPARE(count.load(),1);
    host.execute=true;host.recovery.setCheckpointHook({});
    QVERIFY(run(host,plan.callerScope,accepted.record.operationId,result,error,true));QVERIFY2(result.ok,qPrintable(result.code));
    QCOMPARE(host.catalog.versions().size(),1);QCOMPARE(count.load(),1);
  }
  void failedDiagnosticsAreReadableAndCannotBePublished() {
    QTemporaryDir project;Host host;QVERIFY(host.open(project.path()));auto plan=controlPlan("logs");plan.phase="compute-only";
    AlgoPort log;log.id="LOG";log.kind="file";log.required=false;log.constraints={{"diagnostic",true}};plan.outputSpec.outputs.append(log);
    plan.worker=[](const OperationWorkContext &context) {
      OperationWorkResult result;result.code="python_process_failed";result.diagnostics.ok=true;
      QFile file(context.privateDir+"/log.txt");if(!file.open(QIODevice::WriteOnly)||file.write("failed-log")!=10)return result;
      file.close();result.diagnostics.artifacts.append({"LOG",file.fileName(),{},{},{},{}});return result;
    };
    const auto accepted=host.recovery.accept(plan);QVERIFY(accepted.ok);ExecuteResult result;QString error;
    QVERIFY(run(host,plan.callerScope,accepted.record.operationId,result,error));QVERIFY(!result.ok);
    const auto logs=host.recovery.results(plan.callerScope,accepted.record.operationId);const auto ports=logs.value("ports").toArray();
    QCOMPARE(ports.size(),1);QVERIFY(ports.first().toObject().value("diagnostic").toBool());
    QCOMPARE(logs.value("execution").toString(),QString("failed"));QCOMPARE(host.catalog.versions().size(),0);
    bool done=false;QJsonObject page;
    QVERIFY(host.recovery.read(plan.callerScope,accepted.record.operationId,"LOG",0,128,this,[&](QJsonObject value){page=value;done=true;},&error));
    QTRY_VERIFY_WITH_TIMEOUT(done,5000);QVERIFY(page.value("ok").toBool());QCOMPARE(QByteArray::fromBase64(page.value("data").toString().toLatin1()),QByteArray("failed-log"));
    OperationLog journal(project.path()+"/.paleo/capability-ops",project.path(),host.lock->claim());
    ExecuteRequest publication;publication.capabilityId=plan.capabilityId;publication.documentId="doc";publication.callerScope=plan.callerScope;
    freezeSourcePublication(publication,{},journal.recordById(accepted.record.operationId),&error);
    QCOMPARE(error,QString("source_operation_mismatch"));
    QCOMPARE(host.recovery.results("other-caller",accepted.record.operationId).value("code").toString(),QString("operation_not_found"));
  }
  void runningContextInvalidation_data() {
    QTest::addColumn<QString>("change");
    for(const auto &name:{"epoch","execute","invalidate","session"})QTest::newRow(name)<<QString::fromLatin1(name);
  }
  void runningContextInvalidation() {
    QFETCH(QString,change);QTemporaryDir project;Host host;QVERIFY(host.open(project.path()));
    auto plan=controlPlan("context-"+change);plan.phase="compute-only";std::atomic_bool entered=false,stopped=false;
    plan.worker=[&](const OperationWorkContext &context) {
      entered=true;QElapsedTimer elapsed;elapsed.start();
      while(!context.cancelled() && elapsed.elapsed()<3000)QThread::msleep(1);
      stopped=context.cancelled();OperationWorkResult result;result.code=stopped?"cancelled":"worker_wait_timeout";return result;
    };
    const auto accepted=host.recovery.accept(plan);QVERIFY(accepted.ok);bool done=false;ExecuteResult result;QString error;
    QVERIFY(host.recovery.start(plan.callerScope,accepted.record.operationId,this,"context",[&](ExecuteResult value){result=value;done=true;},false,&error));
    QTRY_VERIFY_WITH_TIMEOUT(entered.load(),1000);
    if(change=="epoch")++host.epoch;else if(change=="execute")host.execute=false;
    else if(change=="session")host.tasks.beginNewSession();else host.recovery.invalidateRuntimeHooks();
    QTRY_VERIFY_WITH_TIMEOUT(done,5000);QVERIFY(stopped.load());QVERIFY(!result.ok);
    QVERIFY(result.code=="cancelled" || result.code=="stale_session" || result.code=="effect_denied");
    QVERIFY(!QJsonDocument::fromJson(result.responseJson).object().value("ok").toBool());QCOMPARE(host.catalog.versions().size(),0);
    QVERIFY(!host.recovery.get(plan.callerScope,accepted.record.operationId).value("cancelRequested").toBool());
  }
  void taskAddedCanDeleteSubscriberOnPrepareWithoutAUserCallback() {
    QTemporaryDir project;Host host;QVERIFY(host.open(project.path()));auto plan=controlPlan("prepare-lifetime");plan.phase="prepare";
    const auto accepted=host.recovery.accept(plan);QVERIFY(accepted.ok);QObject *subscriber=new QObject;bool delivered=false;
    connect(&host.tasks,&PaleoTaskService::taskAdded,&host.tasks,[&]{delete subscriber;subscriber=nullptr;});
    QString error;QVERIFY(host.recovery.start(plan.callerScope,accepted.record.operationId,subscriber,"prepare",[&](auto){delivered=true;},false,&error));
    QTRY_COMPARE_WITH_TIMEOUT(host.tasks.runningCount(),0,5000);QVERIFY(!delivered);
  }
  void sessionInterruptionAfterPreparedPreventsCommitAndAllowsResume() {
    QTemporaryDir project;Host host;QVERIFY(host.open(project.path()));auto plan=controlPlan("interrupt-prepared");std::atomic_int computes=0;
    plan.worker=[&](const OperationWorkContext &context) {
      ++computes;OperationWorkResult result;QFile file(context.privateDir+"/OUTPUT.bin");
      result.ok=file.open(QIODevice::WriteOnly)&&file.write("result")==6;file.close();result.outputs.ok=result.ok;
      result.outputs.artifacts.append({"OUTPUT",file.fileName(),{},{},{},{}});return result;
    };
    const auto accepted=host.recovery.accept(plan);QVERIFY(accepted.ok);
    host.recovery.setCheckpointHook([&](const QString &point){if(point=="prepared")
      QMetaObject::invokeMethod(&host.recovery,[&]{host.tasks.beginNewSession();},Qt::QueuedConnection);});
    ExecuteResult result;QString error;QVERIFY(run(host,plan.callerScope,accepted.record.operationId,result,error));
    QVERIFY(!result.ok);QCOMPARE(result.code,QString("stale_session"));QCOMPARE(host.catalog.versions().size(),0);
    QVERIFY(!host.recovery.get(plan.callerScope,accepted.record.operationId).value("cancelRequested").toBool());
    host.recovery.setCheckpointHook({});
    QVERIFY(run(host,plan.callerScope,accepted.record.operationId,result,error,true));QVERIFY2(result.ok,qPrintable(result.code));
    QCOMPARE(host.catalog.versions().size(),1);QCOMPARE(computes.load(),1);
  }
  void computeOnlyCancellationAfterDurableOutputDoesNotDeliverSuccess() {
    QTemporaryDir project;Host host;QVERIFY(host.open(project.path()));auto plan=controlPlan("cancel-computed");plan.phase="compute-only";
    plan.worker=[](const OperationWorkContext &context) {
      OperationWorkResult result;QFile file(context.privateDir+"/OUTPUT.bin");
      result.ok=file.open(QIODevice::WriteOnly)&&file.write("result")==6;file.close();result.outputs.ok=result.ok;
      result.outputs.artifacts.append({"OUTPUT",file.fileName(),{},{},{},{}});result.summary={{"oldSuccess",true}};return result;
    };
    const auto accepted=host.recovery.accept(plan);QVERIFY(accepted.ok);QPointer<PaleoTask> task;
    connect(&host.tasks,&PaleoTaskService::taskAdded,&host.tasks,[&](PaleoTask *created){task=created;});
    host.recovery.setCheckpointHook([&](const QString &point){if(point=="prepared")
      QMetaObject::invokeMethod(&host.recovery,[&]{if(task)task->requestCancel();},Qt::QueuedConnection);});
    ExecuteResult result;QString error;QVERIFY(run(host,plan.callerScope,accepted.record.operationId,result,error));
    QVERIFY(!result.ok);QCOMPARE(result.code,QString("cancelled"));QVERIFY(result.summary.isEmpty());
    QVERIFY(!QJsonDocument::fromJson(result.responseJson).object().value("ok").toBool());QCOMPARE(host.catalog.versions().size(),0);
  }
  void taskAddedCanDeleteRecoveryBeforeStartReturns() {
    QTemporaryDir project;Host host;QVERIFY(host.open(project.path()));auto plan=controlPlan("owner-lifetime");
    const auto accepted=host.recovery.accept(plan);QVERIFY(accepted.ok);
    auto *recovery=new CapabilityRecovery;QPointer<CapabilityRecovery> owner(recovery);recovery->setTaskService(&host.tasks);
    recovery->setHostProvider([&]{
      RecoveryHost binding;binding.projectDir=project.path();binding.operationDir=project.path()+"/.paleo/capability-ops";
      binding.catalog=&host.catalog;binding.writeClaim=host.lock->claim();binding.context.documentId="doc";binding.context.epoch=1;
      binding.context.open=true;binding.context.persistentIdentity=true;binding.context.readOnly=false;binding.context.ownsWrites=true;binding.context.allowedEffects=127;
      binding.restoreWorker=[](const auto &,QString*){return OperationWorker([](const OperationWorkContext &context){
        OperationWorkResult result;result.code=context.cancelled()?"cancelled":"unexpected_worker";return result;
      });};return binding;
    });
    bool delivered=false;connect(&host.tasks,&PaleoTaskService::taskAdded,&host.tasks,[&]{delete recovery;recovery=nullptr;});
    QString error;QVERIFY(recovery->start(plan.callerScope,accepted.record.operationId,this,"owner",[&](auto){delivered=true;},false,&error));
    QVERIFY(!owner);QTRY_COMPARE_WITH_TIMEOUT(host.tasks.runningCount(),0,5000);QVERIFY(!delivered);QCOMPARE(host.catalog.versions().size(),0);
  }
  void realPinnedTemplateUsesExistingComputePublishAndRestore() {
    const auto runtimeRoot=qEnvironmentVariable("PALEO_TEST_AGENT_PYTHON_ROOT");
    if(runtimeRoot.isEmpty()){QVERIFY(!python::RuntimeProvider({}).inspect().ready());return;}
    python::ExecutionPolicy policy;policy.allowTrusted=true;
    auto provider=std::make_shared<python::RuntimeProvider>(python::RuntimeConfig{runtimeRoot,qEnvironmentVariable("PALEO_TEST_AGENT_PYTHON_MANIFEST_SHA256"),true},policy);
    const auto inspected=provider->inspect();QVERIFY2(inspected.ready(),qPrintable(inspected.code));
    QTemporaryDir project;Host host;QVERIFY(host.open(project.path()));
    host.adapter=python::trustedScriptAdapter(provider,QString::fromUtf8(PALEO_AGENT_PYTHON_TEMPLATE_ROOT));
    ExecuteRequest request;request.capabilityId=QLatin1String(python::SummaryCapability);request.documentId="doc";
    request.callerScope="agent:test";request.projectDir=project.path();request.clientRequestId="prepare";request.publishIntent="prepare";
    request.inputs={{"values",QVariantList{1.0,2.0,3.0}}};QString error;
    auto frozen=python::freezeTrustedScript(request,nativeIntent(request),provider,QString::fromUtf8(PALEO_AGENT_PYTHON_TEMPLATE_ROOT),&error);
    QVERIFY2(error.isEmpty(),qPrintable(error));auto prepared=host.recovery.accept(frozen);QVERIFY2(prepared.ok,qPrintable(prepared.code));
    ExecuteResult result;QVERIFY(run(host,request.callerScope,prepared.record.operationId,result,error));QCOMPARE(host.catalog.versions().size(),0);
    QCOMPARE(host.recovery.get(request.callerScope,prepared.record.operationId).value("computeCount").toInt(),0);
    request.clientRequestId="compute";request.publishIntent="compute-only";
    frozen=python::freezeTrustedScript(request,nativeIntent(request),provider,QString::fromUtf8(PALEO_AGENT_PYTHON_TEMPLATE_ROOT),&error);QVERIFY2(error.isEmpty(),qPrintable(error));
    const auto accepted=host.recovery.accept(frozen);QVERIFY2(accepted.ok,qPrintable(accepted.code));host.recovery.invalidateRuntimeHooks();
    QVERIFY(run(host,request.callerScope,accepted.record.operationId,result,error));QVERIFY2(result.ok,qPrintable(result.code));
    QCOMPARE(result.summary.value("report").toMap().value("sum").toDouble(),6.0);QCOMPARE(host.catalog.versions().size(),0);
    const auto retained=storage::operationRetention(project.path());QVERIFY2(retained.complete,qPrintable(retained.code));
    const auto inputPath=QDir(project.path()).filePath(frozen.frozenPlan.value("storedInput").toObject().value("directory").toString());
    QVERIFY(retained.canonicalRoots.contains(QFileInfo(inputPath).canonicalFilePath()));
    OperationLog journal(project.path()+"/.paleo/capability-ops",project.path(),host.lock->claim());
    request.inputs={{"source_operation_id",accepted.record.operationId}};request.clientRequestId="publish";request.publishIntent="publish";
    auto publication=freezeSourcePublication(request,nativeIntent(request),journal.recordById(accepted.record.operationId),&error);QVERIFY2(error.isEmpty(),qPrintable(error));
    const auto publish=host.recovery.accept(publication);QVERIFY2(publish.ok,qPrintable(publish.code));
    QVERIFY(run(host,request.callerScope,publish.record.operationId,result,error));QVERIFY2(result.ok,qPrintable(result.code));
    QCOMPARE(result.commitState,QString("committed"));QCOMPARE(host.catalog.versions().size(),1);
    host.execute=false;QVERIFY(run(host,request.callerScope,publish.record.operationId,result,error,true));QVERIFY(result.ok);
    QCOMPARE(host.catalog.versions().size(),1);QCOMPARE(host.recovery.get(request.callerScope,publish.record.operationId).value("computeCount").toInt(),0);
  }
};
QTEST_GUILESS_MAIN(PythonOperationTests)
#include "tst_pythonoperations.moc"
