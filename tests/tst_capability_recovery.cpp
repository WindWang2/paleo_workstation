// 层：测试壳
#include <QtTest>
#include "workflow/capabilityrecovery.h"
#include "workflow/capabilitypublication_internal.h"
#include "catalog/datacatalog.h"
#include "metadata/projectlock.h"
#include "services/paleotaskservice.h"
#include <QFile>
#include <QProcess>
#include <QTemporaryDir>
#include <cstdlib>
using namespace paleo;
using namespace paleo::capability;
namespace {
OperationWorker worker() {
  return [](const OperationWorkContext &c) {
    OperationWorkResult r;QFile file(c.privateDir+"/values.bin");
    r.ok=file.open(QIODevice::WriteOnly) && file.write("real-worker-bytes")==17;file.close();
    r.outputs.ok=r.ok;r.outputs.artifacts.append({"OUTPUT",file.fileName(),{},{},{},{}});return r;
  };
}
FrozenOperation plan(const QString &request="req") {
  FrozenOperation p;p.capabilityId="test.compute";p.implementationVersion="1";p.callerScope="agent:chat1";p.clientRequestId=request;
  p.intent={{"number",1}};p.frozenPlan={{"immutable",true}};p.phase="publish";p.worker=worker();
  p.outputSpec.id="test:values";p.outputSpec.complete=true;AlgoPort port;port.id="OUTPUT";port.kind="file";
  p.outputSpec.outputs={port};p.outputPorts={"OUTPUT"};return p;
}
struct Host {
  DataCatalog catalog;std::unique_ptr<ProjectDirLock> lock;PaleoTaskService tasks;CapabilityRecovery recovery;
  bool writable=true,allowExternal=false,presentationFails=false,retainClaimReadOnly=false;quint64 epoch=1;
  QString slotState="initial";int presentationCalls=0;
  QString ownerError;
  QString documentId="doc";
  bool open(const QString &root) {
    lock=std::make_unique<ProjectDirLock>(root);if(!lock->tryLock() || !catalog.open(root))return false;
    recovery.setTaskService(&tasks);recovery.setHostProvider([this,root] {
      RecoveryHost h;h.projectDir=root;h.operationDir=root+"/.paleo/capability-ops";h.catalog=&catalog;
      h.writeClaim=writable || retainClaimReadOnly?lock->claim():nullptr;h.context.documentId=documentId;h.context.epoch=epoch;h.context.open=true;
      h.context.persistentIdentity=true;h.context.readOnly=!writable;h.context.ownsWrites=bool(h.writeClaim);
      h.context.allowedEffects=host::Read|host::PrivateStage|host::CatalogPublish|host::Presentation;
      if(allowExternal)h.context.allowedEffects|=host::ExternalTransfer;
      h.restoreWorker=[](const OperationLog::Record&,QString*){return worker();};
      h.validateFrozen=[this](const OperationLog::Record&){return ownerError;};
      h.presentationState=[this](const QJsonObject&){return slotState;};
      h.present=[this](const QJsonObject &,const PublicationReceipt &){++presentationCalls;if(presentationFails)return QString("pending");slotState="presented";return slotState;};
      return h;
    });return true;
  }
};
int crashChild(const QString &root,const QString &point) {
  Host h;if(!h.open(root))return 60;
  h.recovery.setCheckpointHook([point](const QString &at){if(at==point)std::_Exit(73);});
  auto p=plan();
  if(point=="partial")p.worker=[](const OperationWorkContext &c)->OperationWorkResult {
    QFile file(c.privateDir+"/partial.bin");if(file.open(QIODevice::WriteOnly)){file.write("half");file.flush();}std::_Exit(73);
  };
  const auto accepted=h.recovery.accept(p);if(!accepted.ok)return 61;
  QString error;auto *task=h.recovery.start("agent:chat1",accepted.record.operationId,&h.recovery,"worker",[](ExecuteResult){},false,&error);
  if(!task)return 62;
  QEventLoop loop;QObject::connect(task,&PaleoTask::finished,&loop,&QEventLoop::quit);loop.exec();return 63;
}
}
class RecoveryTests:public QObject {
  Q_OBJECT
private slots:
  void atomicCommitReplayAndScope() {
    QTemporaryDir root;Host h;QVERIFY(h.open(root.path()));auto p=plan();const auto a=h.recovery.accept(p);QVERIFY2(a.ok,qPrintable(a.code));
    ExecuteResult result;bool done=false;QString error;
    QVERIFY(h.recovery.start(p.callerScope,a.record.operationId,this,"compute",[&](ExecuteResult r){result=r;done=true;},false,&error));
    QTRY_VERIFY(done);QVERIFY2(result.ok,qPrintable(result.code));QCOMPARE(result.commitState,QString("committed"));
    QCOMPARE(h.catalog.versions().size(),1);QCOMPARE(h.recovery.get("agent:chat2",a.record.operationId).value("code").toString(),QString("operation_not_found"));
    h.writable=false;++h.epoch;p.worker={};const auto replay=h.recovery.accept(p);QVERIFY(replay.ok);QCOMPARE(replay.record.operationId,a.record.operationId);
    done=false;QVERIFY(h.recovery.start(p.callerScope,a.record.operationId,this,"replay",[&](ExecuteResult r){result=r;done=true;},false,&error));
    QTRY_VERIFY(done);QCOMPARE(result.commitState,QString("committed"));QCOMPARE(replay.record.computeCount,1);QCOMPARE(h.catalog.versions().size(),1);
    p.intent.insert("number",2);QCOMPARE(h.recovery.accept(p).code,QString("idempotency_conflict"));
  }
  void processCrash_data() {
    QTest::addColumn<QString>("point");QTest::addColumn<int>("versions");
    QTest::addColumn<int>("before");QTest::addColumn<int>("after");
    QTest::newRow("accepted")<<QString("accepted")<<0<<0<<1;
    QTest::newRow("running")<<QString("running")<<0<<1<<2;
    QTest::newRow("partial")<<QString("partial")<<0<<1<<2;
    QTest::newRow("prepared")<<QString("prepared")<<0<<1<<1;QTest::newRow("publishing")<<QString("publishing")<<0<<1<<1;
    QTest::newRow("catalog_committed")<<QString("catalog_committed")<<1<<1<<1;
  }
  void processCrash() {
    QFETCH(QString,point);QFETCH(int,versions);QFETCH(int,before);QFETCH(int,after);QTemporaryDir root;
    QProcess child;child.start(QCoreApplication::applicationFilePath(),{"--crash",root.path(),point});
    QVERIFY(child.waitForFinished(30000));QCOMPARE(child.exitCode(),73);
    Host h;QVERIFY(h.open(root.path()));QCOMPARE(h.catalog.versions().size(),versions);
    const auto a=h.recovery.accept(plan());QVERIFY2(a.ok,qPrintable(a.code));QCOMPARE(a.record.computeCount,before);
    if(versions)QCOMPARE(a.record.commitState,QString("committed"));
    bool done=false;ExecuteResult result;QString error;
    QVERIFY2(h.recovery.start("agent:chat1",a.record.operationId,this,"resume",[&](ExecuteResult r){result=r;done=true;},true,&error),qPrintable(error));
    QTRY_VERIFY(done);QCOMPARE(result.commitState,QString("committed"));QCOMPARE(h.catalog.versions().size(),1);
    QCOMPARE(h.recovery.get("agent:chat1",a.record.operationId).value("computeCount").toInt(),after);
    QCOMPARE(h.recovery.cancel("agent:chat1",a.record.operationId).value("code").toString(),QString("cancel_after_commit"));
  }
  void boundedEventsAndResultRead() {
    QTemporaryDir root;Host h;QVERIFY(h.open(root.path()));const auto a=h.recovery.accept(plan());QVERIFY(a.ok);
    bool done=false;QString error;QVERIFY(h.recovery.start("agent:chat1",a.record.operationId,this,"compute",[&](ExecuteResult){done=true;},false,&error));QTRY_VERIFY(done);
    const auto events=h.recovery.events("agent:chat1",a.record.operationId,0,2,1024);QVERIFY(events.value("hasMore").toBool());QCOMPARE(events.value("events").toArray().size(),2);
    QCOMPARE(events.value("contract").toString(),QString("paleo.operation.v2"));
    const auto ports=h.recovery.results("agent:chat1",a.record.operationId).value("ports").toArray();QCOMPARE(ports.size(),1);QVERIFY(!ports.first().toObject().contains("path"));
    QJsonObject page;done=false;QVERIFY(h.recovery.read("agent:chat1",a.record.operationId,"OUTPUT",0,4,this,[&](QJsonObject r){page=r;done=true;},&error));
    QTRY_VERIFY(done);QVERIFY(page.value("ok").toBool());QCOMPARE(QByteArray::fromBase64(page.value("data").toString().toLatin1()),QByteArray("real"));
    QCOMPARE(page.value("contract").toString(),QString("paleo.operation.v2"));
    QCOMPARE(page.value("nextOffset").toString(),QString("4"));QVERIFY(page.value("hasMore").toBool());
    QVERIFY(!h.recovery.read("agent:chat2",a.record.operationId,"OUTPUT",0,4,this,{},&error));QCOMPARE(error,QString("operation_not_found"));
  }
  void externalSendIsDurableAndUnknownNeverResends() {
    QTemporaryDir root;Host h;QVERIFY(h.open(root.path()));auto p=plan("remote");p.phase="compute-only";
    p.outputSpec={};p.outputPorts.clear();p.externalRequest={{"requestHash",QString(64,'a')},{"modelVersion","model-v1"}};
    QCOMPARE(h.recovery.beginExternal(p).code,QString("effect_denied"));h.allowExternal=true;
    const auto first=h.recovery.beginExternal(p);QVERIFY2(first.ok,qPrintable(first.code));QCOMPARE(first.code,QString("Send"));
    const auto repeat=h.recovery.beginExternal(p);QCOMPARE(repeat.code,QString("Unknown"));QCOMPARE(repeat.record.computeCount,1);
    QCOMPARE(h.recovery.noteRemoteAccepted(p.callerScope,first.record.operationId,"job-1"),QString("ok"));
    QCOMPARE(h.recovery.beginExternal(p).code,QString("Query"));
    QCOMPARE(h.recovery.reportExternal(p.callerScope,first.record.operationId,"wrong-job","succeeded"),QString("remote_job_conflict"));
    QCOMPARE(h.recovery.reportExternal(p.callerScope,first.record.operationId,"job-1","succeeded"),QString("ok"));
    QCOMPARE(h.recovery.beginExternal(p).code,QString("Terminal"));
    p.clientRequestId="direct";const auto direct=h.recovery.beginExternal(p);QCOMPARE(direct.code,QString("Send"));
    QJsonObject proof{{"requestHash",QString(64,'a')},{"responseHash",QString(64,'b')},{"modelVersion","wrong"}};
    QCOMPARE(h.recovery.reportExternal(p.callerScope,direct.record.operationId,{},"succeeded",proof),QString("remote_job_conflict"));
    proof["modelVersion"]="model-v1";
    QCOMPARE(h.recovery.reportExternal(p.callerScope,direct.record.operationId,{},"succeeded",proof),QString("ok"));
    QCOMPARE(h.recovery.beginExternal(p).code,QString("Terminal"));
  }
  void remoteCompletionProofPersistsForKnownJobsAndIsSetOnce() {
    QTemporaryDir root;QString id;auto p=plan("remote-proof");p.phase="compute-only";
    p.outputSpec={};p.outputPorts.clear();p.externalRequest={{"requestHash",QString(64,'a')},{"modelVersion","model-v1"}};
    QJsonObject proof{{"requestHash",QString(64,'a')},{"responseHash",QString(64,'b')},{"modelVersion","model-v1"}};
    {Host h;QVERIFY(h.open(root.path()));h.allowExternal=true;const auto accepted=h.recovery.beginExternal(p);QCOMPARE(accepted.code,QString("Send"));id=accepted.record.operationId;
      QCOMPARE(h.recovery.noteRemoteAccepted(p.callerScope,id,"job-proof"),QString("ok"));
      auto invalid=proof;invalid["requestHash"]=QString(64,'c');
      QCOMPARE(h.recovery.reportExternal(p.callerScope,id,"job-proof","succeeded",invalid),QString("remote_job_conflict"));
      QCOMPARE(h.recovery.reportExternal(p.callerScope,id,"job-proof","succeeded",proof),QString("ok"));}
    Host h;QVERIFY(h.open(root.path()));const auto accepted=h.recovery.beginExternal(p);QCOMPARE(accepted.code,QString("Terminal"));
    QCOMPARE(accepted.record.remoteCompletionProof,proof);QCOMPARE(accepted.record.remoteJobId,QString("job-proof"));
    QCOMPARE(h.recovery.reportExternal(p.callerScope,id,"job-proof","succeeded",proof),QString("ok"));
    auto changed=proof;changed["responseHash"]=QString(64,'c');
    QCOMPARE(h.recovery.reportExternal(p.callerScope,id,"job-proof","succeeded",changed),QString("external_completion_conflict"));
    QCOMPARE(h.recovery.lookup(p.callerScope,p.clientRequestId).record.remoteCompletionProof,proof);
    QCOMPARE(h.recovery.reportExternal(p.callerScope,id,"job-proof","succeeded",proof,true),QString("ok"));
    h.lock->unlock();ProjectDirLock probe(root.path());QVERIFY(!probe.tryLock());
    QCOMPARE(h.recovery.reportExternal(p.callerScope,id,"job-proof","succeeded",proof,false),QString("ok"));
    QVERIFY(probe.tryLock());probe.unlock();QVERIFY(h.lock->tryLock());
    QCOMPARE(h.recovery.reportExternal(p.callerScope,id,"job-proof","succeeded",proof,true),QString("ok"));
    h.documentId="another-document";++h.epoch;bool done=false;QString error;ExecuteResult result;
    QVERIFY2(h.recovery.attachExternalResults(p.callerScope,id,worker(),this,[&](ExecuteResult r){result=r;done=true;},&error),qPrintable(error));
    QTRY_VERIFY(done);QVERIFY2(result.ok,qPrintable(result.code));QCOMPARE(h.catalog.versions().size(),0);

  }
  void restoredRemoteQueryRetainsItsOriginalDocument() {
    QTemporaryDir root;QString id;auto p=plan("remote-query");p.phase="compute-only";
    p.outputSpec={};p.outputPorts.clear();p.externalRequest={{"requestHash",QString(64,'a')},{"modelVersion","model-v1"}};
    {Host h;QVERIFY(h.open(root.path()));h.allowExternal=true;const auto accepted=h.recovery.beginExternal(p);QCOMPARE(accepted.code,QString("Send"));id=accepted.record.operationId;
      QCOMPARE(h.recovery.noteRemoteAccepted(p.callerScope,id,"original-job"),QString("ok"));}
    Host h;QVERIFY(h.open(root.path()));h.allowExternal=true;const auto query=h.recovery.beginExternal(p);QCOMPARE(query.code,QString("Query"));
    QCOMPARE(query.record.remoteJobId,QString("original-job"));h.documentId="another-document";++h.epoch;
    const QJsonObject proof{{"requestHash",QString(64,'a')},{"responseHash",QString(64,'b')},{"modelVersion","model-v1"}};
    QCOMPARE(h.recovery.reportExternal(p.callerScope,id,"original-job","succeeded",proof),QString("ok"));
    OperationLog log(root.path()+"/.paleo/capability-ops");const auto saved=log.recordById(id);
    QCOMPARE(saved.documentId,QString("doc"));QCOMPARE(saved.execution,QString("succeeded"));QCOMPARE(saved.computeCount,1);
    QCOMPARE(saved.remoteJobId,QString("original-job"));QCOMPARE(saved.remoteCompletionProof,proof);
    h.lock->unlock();ProjectDirLock next(root.path());QTRY_VERIFY(next.tryLock());
  }
  void phaseInheritanceSurvivesRestartWithoutLivePlan() {
    QTemporaryDir root;QString sourceId;QJsonObject target;
    {Host h;QVERIFY(h.open(root.path()));auto p=plan("prepare");p.phase="prepare";
      const auto a=h.recovery.accept(p);QVERIFY(a.ok);QCOMPARE(a.record.execution,QString("prepared"));QCOMPARE(a.record.computeCount,0);
      sourceId=a.record.operationId;target=a.record.stagePlan;}
    Host h;QVERIFY(h.open(root.path()));FrozenOperation compute;
    compute.callerScope="agent:chat1";compute.clientRequestId="compute";compute.phase="compute-only";compute.sourceOperationId=sourceId;
    compute.intent={{"sourceOperationId",sourceId},{"phase","compute-only"}};
    const auto a=h.recovery.accept(compute);QVERIFY2(a.ok,qPrintable(a.code));QCOMPARE(a.record.stagePlan,target);
    bool done=false;QString error;ExecuteResult result;
    QVERIFY2(h.recovery.start(compute.callerScope,a.record.operationId,this,"compute",[&](ExecuteResult r){result=r;done=true;},false,&error),qPrintable(error));QTRY_VERIFY(done);QVERIFY2(result.ok,qPrintable(result.code));
    QCOMPARE(h.catalog.versions().size(),0);
    FrozenOperation publish;publish.callerScope=compute.callerScope;publish.clientRequestId="publish";publish.phase="publish";
    publish.sourceOperationId=a.record.operationId;publish.intent={{"sourceOperationId",a.record.operationId},{"phase","publish"}};
    const auto b=h.recovery.accept(publish);QVERIFY2(b.ok,qPrintable(b.code));QCOMPARE(b.record.stagePlan,target);done=false;
    QVERIFY(h.recovery.start(publish.callerScope,b.record.operationId,this,"publish",[&](ExecuteResult r){result=r;done=true;},false,&error));QTRY_VERIFY(done);
    QCOMPARE(result.commitState,QString("committed"));QCOMPARE(h.catalog.versions().size(),1);
    QCOMPARE(h.recovery.get(publish.callerScope,b.record.operationId).value("computeCount").toInt(),0);
  }
  void readOnlyClaimCannotCancelAnUncommittedOperation() {
    QTemporaryDir root;Host h;QVERIFY(h.open(root.path()));const auto a=h.recovery.accept(plan());QVERIFY(a.ok);
    h.writable=false;h.retainClaimReadOnly=true;
    OperationLog log(root.path()+"/.paleo/capability-ops");const auto before=log.recordById(a.record.operationId);
    QCOMPARE(h.recovery.cancel("agent:chat1",a.record.operationId).value("code").toString(),QString("project_read_only"));
    QCOMPARE(log.recordById(a.record.operationId),before);
  }
  void knownCommitSurvivesJournalWriteFailure() {
    QTemporaryDir root;Host h;QVERIFY(h.open(root.path()));
    h.recovery.setCheckpointHook([](const QString &at){if(at=="catalog_committed")OperationLog::armOperationFileWriteFault(0);});
    const auto a=h.recovery.accept(plan());QVERIFY(a.ok);bool done=false;QString error;ExecuteResult result;
    QVERIFY(h.recovery.start("agent:chat1",a.record.operationId,this,"compute",[&](ExecuteResult r){result=r;done=true;},false,&error));QTRY_VERIFY(done);
    QVERIFY(result.ok);QCOMPARE(result.commitState,QString("committed"));QCOMPARE(result.code,QString("committed_log_repair_pending"));
    QCOMPARE(h.catalog.versions().size(),1);
    h.writable=false;h.retainClaimReadOnly=true;
    OperationLog log(root.path()+"/.paleo/capability-ops");const auto before=log.recordById(a.record.operationId);
    QCOMPARE(h.recovery.get("agent:chat1",a.record.operationId).value("commitState").toString(),QString("committed"));
    QCOMPARE(log.recordById(a.record.operationId),before);
    h.writable=true;QCOMPARE(h.recovery.get("agent:chat1",a.record.operationId).value("commitState").toString(),QString("committed"));
    QCOMPARE(h.catalog.versions().size(),1);OperationLog::armOperationFileWriteFault(-1);
  }
  void presentationRetriesDoNotRecomputeOrOverwriteUserChanges() {
    QTemporaryDir root;Host h;QVERIFY(h.open(root.path()));h.presentationFails=true;
    auto p=plan();p.presentationIntent={{"target","slot"},{"expectedState","initial"}};
    const auto a=h.recovery.accept(p);QVERIFY(a.ok);bool done=false;QString error;ExecuteResult result;
    QVERIFY(h.recovery.start(p.callerScope,a.record.operationId,this,"compute",[&](ExecuteResult r){result=r;done=true;},false,&error));QTRY_VERIFY(done);
    QCOMPARE(result.commitState,QString("committed"));QCOMPARE(result.presentation,QString("pending"));QCOMPARE(h.presentationCalls,1);
    h.presentationFails=false;done=false;
    QVERIFY(h.recovery.start(p.callerScope,a.record.operationId,this,"retry presentation",[&](ExecuteResult r){result=r;done=true;},true,&error));QTRY_VERIFY(done);
    QCOMPARE(result.presentation,QString("presented"));QCOMPARE(h.presentationCalls,2);QCOMPARE(h.catalog.versions().size(),1);
    QCOMPARE(h.recovery.get(p.callerScope,a.record.operationId).value("computeCount").toInt(),1);
    QTemporaryDir other;Host changed;QVERIFY(changed.open(other.path()));auto q=plan();q.presentationIntent=p.presentationIntent;
    auto numeric=worker();q.worker=[&changed,numeric](const OperationWorkContext &c){auto out=numeric(c);
      QMetaObject::invokeMethod(&changed.recovery,[&changed]{changed.slotState="user-changed";},Qt::QueuedConnection);return out;};
    const auto b=changed.recovery.accept(q);QVERIFY(b.ok);done=false;
    QVERIFY(changed.recovery.start(q.callerScope,b.record.operationId,this,"compute",[&](ExecuteResult r){result=r;done=true;},false,&error));QTRY_VERIFY(done);
    QCOMPARE(result.commitState,QString("committed"));QCOMPARE(result.presentation,QString("superseded"));QCOMPARE(changed.presentationCalls,0);
    QCOMPARE(changed.slotState,QString("user-changed"));
  }
  void oversizedChangedArtifactIsRejectedBeforeHashing() {
    QTemporaryDir root;Host h;QVERIFY(h.open(root.path()));const auto a=h.recovery.accept(plan());QVERIFY(a.ok);
    bool done=false;QString error;QVERIFY(h.recovery.start("agent:chat1",a.record.operationId,this,"compute",[&](ExecuteResult){done=true;},false,&error));QTRY_VERIFY(done);
    OperationLog log(root.path()+"/.paleo/capability-ops");const auto record=log.recordById(a.record.operationId);
    const auto path=root.path()+'/'+h.catalog.versions().first().path;QFile file(path);
    QVERIFY(file.setPermissions(QFile::ReadOwner|QFile::WriteOwner));QVERIFY(file.open(QIODevice::WriteOnly));QVERIFY(file.resize(65));file.close();
    DerivedPreparedSet prepared;int checks=0;
    QVERIFY(!publication_detail::preparedFromJson(root.path(),record,prepared,&error,[&]{++checks;return false;}));
    QCOMPARE(error,QString("artifact_changed"));QCOMPARE(checks,0);
  }
  void preparedResumeRechecksOwnerAndWorkerInputsWithoutComputingAgain() {
    QTemporaryDir root;Host h;QVERIFY(h.open(root.path()));auto p=plan();auto numeric=worker();
    auto invalid=std::make_shared<std::atomic_bool>(false);auto computations=std::make_shared<std::atomic_int>(0);
    p.inputValidator=[invalid](const OperationWorkContext&){return invalid->load()?QString("source_bytes_changed"):QString();};
    p.worker=[&h,numeric,computations](const OperationWorkContext &c){++*computations;auto out=numeric(c);
      QMetaObject::invokeMethod(&h.recovery,[&h]{h.ownerError="source_row_changed";},Qt::QueuedConnection);return out;};
    const auto a=h.recovery.accept(p);QVERIFY(a.ok);bool done=false;QString error;ExecuteResult result;
    auto *task=h.recovery.start(p.callerScope,a.record.operationId,this,"compute",[&](ExecuteResult r){result=r;done=true;},false,&error);
    QVERIFY(task);QTRY_VERIFY(done);QCOMPARE(task->state(),PaleoTask::State::Failed);
    QVERIFY(!result.ok);QCOMPARE(result.code,QString("source_row_changed"));QCOMPARE(h.catalog.versions().size(),0);
    h.ownerError.clear();*invalid=true;done=false;
    QVERIFY(h.recovery.start(p.callerScope,a.record.operationId,this,"verify",[&](ExecuteResult r){result=r;done=true;},true,&error));QTRY_VERIFY(done);
    QVERIFY(!result.ok);QCOMPARE(result.code,QString("source_bytes_changed"));QCOMPARE(h.catalog.versions().size(),0);
    *invalid=false;done=false;
    QVERIFY(h.recovery.start(p.callerScope,a.record.operationId,this,"publish",[&](ExecuteResult r){result=r;done=true;},true,&error));QTRY_VERIFY(done);
    QCOMPARE(result.commitState,QString("committed"));QCOMPARE(h.catalog.versions().size(),1);QCOMPARE(computations->load(),1);
  }
  void retainedFinishedTaskDoesNotRetainTheProjectLock() {
    QTemporaryDir root;Host h;QVERIFY(h.open(root.path()));const auto a=h.recovery.accept(plan());QVERIFY(a.ok);
    bool done=false;QString error;QVERIFY(h.recovery.start("agent:chat1",a.record.operationId,this,"compute",[&](ExecuteResult){done=true;},false,&error));QTRY_VERIFY(done);
    QVERIFY(!h.tasks.tasks().isEmpty());h.lock->unlock();ProjectDirLock next(root.path());QTRY_VERIFY(next.tryLock());
  }
  void ownedExternalResponseAttachesAfterDocumentSwitch() {
    QTemporaryDir root;Host h;QVERIFY(h.open(root.path()));h.allowExternal=true;auto p=plan("remote-switch");p.phase="compute-only";
    p.outputSpec={};p.outputPorts.clear();p.externalRequest={{"requestHash",QString(64,'a')},{"modelVersion","v1"}};
    const auto a=h.recovery.beginExternal(p);QVERIFY(a.ok);QCOMPARE(a.code,QString("Send"));
    h.documentId="another-document";++h.epoch;
    const QJsonObject proof{{"requestHash",QString(64,'a')},{"responseHash",QString(64,'b')},{"modelVersion","v1"}};
    QCOMPARE(h.recovery.reportExternal(p.callerScope,a.record.operationId,{},"succeeded",proof,true),QString("ok"));
    QCOMPARE(h.recovery.get(p.callerScope,a.record.operationId).value("code").toString(),QString("operation_not_found"));
    bool done=false;QString error;ExecuteResult result;
    QVERIFY2(h.recovery.attachExternalResults(p.callerScope,a.record.operationId,worker(),this,[&](ExecuteResult r){result=r;done=true;},&error),qPrintable(error));
    QTRY_VERIFY(done);QVERIFY2(result.ok,qPrintable(result.code));
    OperationLog log(root.path()+"/.paleo/capability-ops");const auto record=log.recordById(a.record.operationId);
    QCOMPARE(record.documentId,QString("doc"));QCOMPARE(record.execution,QString("succeeded"));QCOMPARE(record.computeCount,1);
    QCOMPARE(record.artifactManifest.value("ports").toArray().size(),1);QCOMPARE(h.catalog.versions().size(),0);
    h.lock->unlock();ProjectDirLock next(root.path());QTRY_VERIFY(next.tryLock());
  }
};
int main(int argc,char **argv) {
  QCoreApplication application(argc,argv);const auto args=application.arguments();
  if(args.size()==4 && args[1]=="--crash")return crashChild(args[2],args[3]);
  RecoveryTests test;return QTest::qExec(&test,argc,argv);
}
#include "tst_capability_recovery.moc"
