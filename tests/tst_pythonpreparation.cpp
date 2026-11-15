// 层：测试壳
#include "workflow/capabilityhost.h"
#include "services/pythonanalysiscapabilityspec.h"
#include "services/paleotaskservice.h"
#include "catalog/datacatalog.h"
#include "metadata/projectlock.h"
#include <QtTest>
#include <QFile>
#include <QTemporaryDir>
#include <QThread>
#include <atomic>

using namespace paleo;
using namespace paleo::capability;
namespace {
class ObservedTaskService final:public PaleoTaskService {
public:
  int destructionObservers() const {return receivers(SIGNAL(destroyed(QObject*)));}
};
struct Fixture {
  QTemporaryDir temp;DataCatalog catalog;QString error;
  std::unique_ptr<ProjectDirLock> lock;
  std::unique_ptr<ObservedTaskService> tasks;
  std::unique_ptr<CapabilityHost> host;
  std::atomic_bool blocked{false},entered{false};
  std::function<void()> onValidate,onBinding,onCapture;
  ~Fixture(){host.reset();tasks.reset();}
  HostBinding binding() {
    if(onBinding)onBinding();HostBinding out;out.projectDir=temp.path();out.operationDir=temp.path()+"/.paleo/capability-ops";
    out.catalog=&catalog;out.writeClaim=lock->claim();out.context.documentId="doc";out.context.epoch=1;
    out.context.open=true;out.context.persistentIdentity=true;out.context.ownsWrites=true;out.context.readOnly=false;
    out.context.allowedEffects=host::Read|host::PrivateStage|host::LocalExecute;return out;
  }
  CapabilityExecutionAdapter adapter() {
    CapabilityExecutionAdapter out;
    out.capture=[this](const ExecuteRequest &,const QJsonObject &,QString *code) {
      if(onCapture)onCapture();
      if(code)code->clear();CapabilityPreparation preparation;
      preparation.worker=[this](const std::function<bool()> &cancelled,QString *error) {
        entered=true;
        while(blocked && !cancelled())QThread::msleep(1);
        if(error)*error=cancelled()?QStringLiteral("cancelled"):QStringLiteral("test_preparation_failed");
        return FrozenOperation{};
      };
      preparation.validate=[this]{if(onValidate)onValidate();return QString();};return preparation;
    };return out;
  }
  bool open() {
    lock=std::make_unique<ProjectDirLock>(temp.path());if(!lock->tryLock(&error) || !catalog.open(temp.path(),&error))return false;
    tasks=std::make_unique<ObservedTaskService>();host=std::make_unique<CapabilityHost>();
    configureHost();
    host->registerExecutionAdapter(QLatin1String(python::WellQualityCapability),adapter());return true;
  }
  ExecuteRequest request() {
    ExecuteRequest out;out.capabilityId=QLatin1String(python::WellQualityCapability);out.clientRequestId="stable-request";
    out.publishIntent="compute-only";return out;
  }
  void configureHost() {
    host->setTaskService(tasks.get());host->setCallerScope(host::Caller::Agent,"agent:test");
    host->setBindingProvider([this]{return binding();});
  }
  FrozenOperation restorationPlan(bool registered) {
    FrozenOperation plan;plan.capabilityId=registered?QString::fromLatin1(python::WellQualityCapability):QStringLiteral("test.restore-hooks");
    plan.callerScope="agent:test";plan.clientRequestId="restore-lifetime";plan.implementationVersion="test";
    plan.phase="compute-only";plan.requiredEffects=host::LocalExecute;plan.intent={{"test",true}};plan.frozenPlan={{"test",true}};
    plan.outputSpec.id="test:restore-hooks";plan.outputSpec.complete=true;AlgoPort port;port.id="REPORT";port.kind="file";
    plan.outputSpec.outputs={port};plan.outputPorts={"REPORT"};
    plan.worker=[this](const OperationWorkContext &context) {
      entered=true;OperationWorkResult result;QFile file(context.privateDir+"/REPORT.json");
      result.ok=file.open(QIODevice::WriteOnly) && file.write("{}",2)==2;file.close();
      result.outputs.ok=result.ok;result.outputs.artifacts.append({"REPORT",file.fileName(),{},{},{},{}});return result;
    };
    plan.inputValidator=[](const OperationWorkContext &){return QString();};return plan;
  }
  void replaceTasks() {tasks=std::make_unique<ObservedTaskService>();host->setTaskService(tasks.get());}
};
}
class TestPythonPreparation:public QObject {
  Q_OBJECT
private slots:
  void restoringHooksCannotOutliveHost_data() {
    QTest::addColumn<QString>("hook");QTest::addColumn<bool>("registered");
    for(const auto &hook:{"validate","input-validator","worker"}) {
      const auto name=QByteArray(hook);QTest::newRow(("adapter-"+name).constData())<<QString::fromLatin1(hook)<<true;
      QTest::newRow(("member-"+name).constData())<<QString::fromLatin1(hook)<<false;
    }
    QTest::newRow("native-factory")<<QString("factory")<<false;
  }
  void restoringHooksCannotOutliveHost() {
    QFETCH(QString,hook);QFETCH(bool,registered);Fixture f;QVERIFY(f.open());
    if(hook=="factory") {
      f.host=std::make_unique<CapabilityHost>([&](const HostBinding &) {
        f.host.reset();return CapabilityExecutionAdapter{};
      },nullptr);
      f.configureHost();
    }
    auto plan=f.restorationPlan(registered);plan.intent={{"hook",hook}};const auto worker=plan.worker;
    const auto accepted=f.host->recovery()->accept(plan);QVERIFY2(accepted.ok,qPrintable(accepted.code));
    f.host->invalidate(); // Restore from the journal, without cached runtime callbacks.
    const auto validate=[&](const OperationLog::Record &) {if(hook=="validate")f.host.reset();return QString();};
    const auto input=[&](const OperationLog::Record &,QString *)->OperationInputValidator {
      if(hook=="input-validator"){f.host.reset();if(!registered)return {};}
      return [](const OperationWorkContext &){return QString();};
    };
    const auto restore=[&,worker](const OperationLog::Record &,QString *)->OperationWorker {
      if(hook=="worker"){f.host.reset();if(!registered)return {};}return worker;
    };
    if(registered) {
      CapabilityExecutionAdapter adapter;adapter.validateFrozen=validate;adapter.restoreInputValidator=input;adapter.restoreWorker=restore;
      f.host->registerExecutionAdapter(plan.capabilityId,std::move(adapter));
    } else {
      f.host->setFrozenValidator(validate);f.host->setInputValidatorRestorer(input);f.host->setWorkerRestorer(restore);
    }
    auto *recovery=f.host->recovery();int completions=0;
    QVERIFY(!recovery->start(plan.callerScope,accepted.record.operationId,this,"Deleted restore owner",[&](auto){++completions;},false,&f.error));
    QVERIFY(!f.host);QCOMPARE(f.error,QString("document_unavailable"));QCOMPARE(completions,0);QVERIFY(!f.entered);
    QCOMPARE(f.tasks->runningCount(),0);QVERIFY(f.catalog.versions().isEmpty());
    OperationLog log(f.temp.path()+"/.paleo/capability-ops",f.temp.path(),f.lock->claim());const auto record=log.recordById(accepted.record.operationId);
    QCOMPARE(record.execution,QString("queued"));QCOMPARE(record.computeCount,0);
  }
  void restoringHooksCannotChangeBinding_data() {
    QTest::addColumn<QString>("hook");QTest::addColumn<bool>("registered");QTest::addColumn<bool>("rebind");
    for(const auto &hook:{"validate","input-validator","worker"})for(const bool registered:{false,true})for(const bool rebind:{false,true}) {
      const auto name=QByteArray(registered?"adapter-":"member-")+hook+(rebind?"-rebind":"-invalidate");
      QTest::newRow(name.constData())<<QString::fromLatin1(hook)<<registered<<rebind;
    }
  }
  void restoringHooksCannotChangeBinding() {
    QFETCH(QString,hook);QFETCH(bool,registered);QFETCH(bool,rebind);Fixture f;QVERIFY(f.open());
    auto plan=f.restorationPlan(registered);const auto worker=plan.worker;const auto validator=plan.inputValidator;
    const auto accepted=f.host->recovery()->accept(plan);QVERIFY2(accepted.ok,qPrintable(accepted.code));f.host->invalidate();
    const auto change=[&] {if(rebind)f.host->setBindingProvider([&]{return f.binding();});else f.host->invalidate();};
    const auto validate=[&](const OperationLog::Record &){if(hook=="validate")change();return QString();};
    const auto input=[&,validator](const OperationLog::Record &,QString *){if(hook=="input-validator")change();return validator;};
    const auto restore=[&,worker](const OperationLog::Record &,QString *){if(hook=="worker")change();return worker;};
    if(registered) {
      CapabilityExecutionAdapter adapter;adapter.validateFrozen=validate;adapter.restoreInputValidator=input;adapter.restoreWorker=restore;
      f.host->registerExecutionAdapter(plan.capabilityId,std::move(adapter));
    } else {
      f.host->setFrozenValidator(validate);f.host->setInputValidatorRestorer(input);f.host->setWorkerRestorer(restore);
    }
    int completions=0;
    QVERIFY(!f.host->recovery()->start(plan.callerScope,accepted.record.operationId,this,"Changed restore binding",[&](auto){++completions;},false,&f.error));
    QVERIFY(f.host);QCOMPARE(f.error,QString("stale_session"));QCOMPARE(completions,0);QVERIFY(!f.entered);
    QCOMPARE(f.tasks->runningCount(),0);QVERIFY(f.catalog.versions().isEmpty());
    OperationLog log(f.temp.path()+"/.paleo/capability-ops",f.temp.path(),f.lock->claim());const auto record=log.recordById(accepted.record.operationId);
    QCOMPARE(record.execution,QString("queued"));QCOMPARE(record.computeCount,0);
  }
  void firstDirectStartAfterRestartAcceptsTheInitializedGeneration() {
    Fixture f;QVERIFY(f.open());const auto plan=f.restorationPlan(false);
    const auto accepted=f.host->recovery()->accept(plan);QVERIFY2(accepted.ok,qPrintable(accepted.code));
    f.host=std::make_unique<CapabilityHost>();f.configureHost();
    f.host->setFrozenValidator([](const OperationLog::Record &){return QString();});
    f.host->setInputValidatorRestorer([validator=plan.inputValidator](const OperationLog::Record &,QString *){return validator;});
    f.host->setWorkerRestorer([worker=plan.worker](const OperationLog::Record &,QString *){return worker;});
    int completions=0;ExecuteResult result;
    // No lookup/accept warms this new coordinator before its first direct start.
    QVERIFY2(f.host->recovery()->start(plan.callerScope,accepted.record.operationId,this,"Cold restore",[&](auto value){result=std::move(value);++completions;},false,&f.error),qPrintable(f.error));
    QTRY_COMPARE_WITH_TIMEOUT(completions,1,5000);QVERIFY2(result.ok,qPrintable(result.code));QVERIFY(f.entered);
    OperationLog log(f.temp.path()+"/.paleo/capability-ops",f.temp.path(),f.lock->claim());const auto record=log.recordById(accepted.record.operationId);
    QCOMPARE(record.execution,QString("succeeded"));QCOMPARE(record.computeCount,1);QVERIFY(f.catalog.versions().isEmpty());
  }
  void prepareBindingCannotOutliveHost_data() {
    QTest::addColumn<bool>("plan");QTest::addColumn<int>("deleteOnCall");
    QTest::newRow("single-initial-binding")<<false<<1;
    QTest::newRow("single-lookup-binding")<<false<<2;
    QTest::newRow("plan-initial-binding")<<true<<1;
    QTest::newRow("plan-lookup-binding")<<true<<2;
  }
  void prepareBindingCannotOutliveHost() {
    QFETCH(bool,plan);QFETCH(int,deleteOnCall);Fixture f;QVERIFY(f.open());int calls=0;
    f.onBinding=[&]{if(++calls==deleteOnCall)f.host.reset();};
    auto *owner=f.host.get();const auto request=f.request();
    const auto prepared=plan?owner->preparePlan(request,{}, {},host::Caller::Agent):owner->prepare(request,host::Caller::Agent);
    QVERIFY(!f.host);QVERIFY(!prepared.ok);QCOMPARE(prepared.code,QString("document_unavailable"));
    QCOMPARE(calls,deleteOnCall);QVERIFY(!f.entered);QVERIFY(f.catalog.versions().isEmpty());
  }
  void prepareCaptureCannotOutliveItsBinding_data() {
    QTest::addColumn<bool>("deleteOwner");QTest::newRow("deleted-owner")<<true;QTest::newRow("invalidated-binding")<<false;
  }
  void prepareCaptureCannotOutliveItsBinding() {
    QFETCH(bool,deleteOwner);Fixture f;QVERIFY(f.open());
    f.onCapture=[&]{if(deleteOwner)f.host.reset();else f.host->invalidate();};
    const auto prepared=f.host->prepare(f.request(),host::Caller::Agent);
    QVERIFY(!prepared.ok);QCOMPARE(prepared.code,deleteOwner?QString("document_unavailable"):QString("stale_session"));
    QVERIFY(!f.entered);QVERIFY(f.catalog.versions().isEmpty());
  }
  void completedPreparationsReleaseServiceLifetimeObservers() {
    Fixture f;QVERIFY(f.open());const auto observers=f.tasks->destructionObservers();
    for(int i=0;i<8;++i) {
      auto request=f.request();request.clientRequestId=QString::number(i);
      const auto prepared=f.host->prepare(request,host::Caller::Agent);QVERIFY(prepared.ok);bool done=false;ExecuteResult result;
      QVERIFY(f.host->start(prepared.handle,request,this,"Release preparation",[&](auto value){result=std::move(value);done=true;},&f.error));
      QTRY_VERIFY_WITH_TIMEOUT(done,5000);
      QVERIFY(!result.ok);
      QTRY_COMPARE_WITH_TIMEOUT(f.tasks->destructionObservers(),observers,5000);
    }
    QVERIFY(f.catalog.versions().isEmpty());
  }
  void taskAddedReentrantDeletion_data() {
    QTest::addColumn<QString>("target");
    for(const auto &name:{"host","parent","service","phase-service"})QTest::newRow(name)<<QString::fromLatin1(name);
  }
  void taskAddedReentrantDeletion() {
    QFETCH(QString,target);Fixture f;QVERIFY2(f.open(),qPrintable(f.error));const auto request=f.request();
    const auto prepared=f.host->prepare(request,host::Caller::Agent);QVERIFY2(prepared.ok,qPrintable(prepared.code));
    connect(f.tasks.get(),&PaleoTaskService::taskAdded,this,[&](PaleoTask *task) {
      if((target=="phase-service")!=task->quiet())return;
      if(target=="host")f.host.reset();else if(target=="parent")delete task;else f.tasks.reset();
    });
    int completions=0;ExecuteResult result;
    auto *task=f.host->start(prepared.handle,request,this,"Reentrant preparation",[&](auto value){++completions;result=value;},&f.error);
    if(target=="host") {QTRY_COMPARE_WITH_TIMEOUT(f.tasks->runningCount(),0,5000);QCOMPARE(completions,0);}
    else if(target=="phase-service") {QVERIFY(!task);QTRY_COMPARE_WITH_TIMEOUT(completions,1,5000);QVERIFY(!result.ok);}
    else {QVERIFY(!task);QCOMPARE(completions,0);}
    QVERIFY(!f.entered);QVERIFY(f.catalog.versions().isEmpty());
  }
  void deletingServiceDuringPreparationReleasesPendingIdentity() {
    Fixture f;QVERIFY(f.open());f.blocked=true;const auto request=f.request();const auto prepared=f.host->prepare(request,host::Caller::Agent);
    QVERIFY(prepared.ok);int completions=0;ExecuteResult result;
    QVERIFY(f.host->start(prepared.handle,request,this,"Delete service",[&](auto value){++completions;result=value;},&f.error));
    QTRY_VERIFY_WITH_TIMEOUT(f.entered.load(),5000);f.tasks.reset();
    QTRY_COMPARE_WITH_TIMEOUT(completions,1,5000);QVERIFY(!result.ok);f.blocked=false;f.replaceTasks();
    const auto again=f.host->prepare(request,host::Caller::Agent);QVERIFY2(again.ok,qPrintable(again.code));
    QVERIFY2(f.host->start(again.handle,request,this,"Identity reused",[&](auto){++completions;},&f.error),qPrintable(f.error));
    QTRY_COMPARE_WITH_TIMEOUT(completions,2,5000);QVERIFY(f.catalog.versions().isEmpty());
  }
  void validationReentrancyCannotAcceptStaleInput_data() {
    QTest::addColumn<QString>("change");for(const auto &name:{"binding","adapter","owner"})QTest::newRow(name)<<QString::fromLatin1(name);
  }
  void validationReentrancyCannotAcceptStaleInput() {
    QFETCH(QString,change);Fixture f;QVERIFY(f.open());
    f.onValidate=[&] {
      if(change=="owner")f.host.reset();
      else if(change=="binding")f.host->setBindingProvider([&]{return f.binding();});
      else f.host->registerExecutionAdapter(QLatin1String(python::WellQualityCapability),f.adapter());
    };
    const auto request=f.request();const auto prepared=f.host->prepare(request,host::Caller::Agent);QVERIFY(prepared.ok);
    int completions=0;ExecuteResult result;
    QVERIFY(f.host->start(prepared.handle,request,this,"Validate callback",[&](auto value){++completions;result=value;},&f.error));
    QTRY_COMPARE_WITH_TIMEOUT(f.tasks->runningCount(),0,5000);
    if(change=="owner")QCOMPARE(completions,0);
    else {QCOMPARE(completions,1);QCOMPARE(result.code,QString("stale_session"));}
    QVERIFY(f.catalog.versions().isEmpty());
  }
  void startCopiesPreparedBeforeCallingHostHooks() {
    Fixture f;QVERIFY(f.open());const auto request=f.request();bool invalidate=false;
    const auto prepared=f.host->prepare(request,host::Caller::Agent,{}, {},[&]{if(invalidate)f.host->invalidate();return QString();});
    QVERIFY(prepared.ok);invalidate=true;
    QVERIFY(!f.host->start(prepared.handle,request,this,"Invalidated",[](auto){},&f.error));
    QCOMPARE(f.error,QString("prepared_unknown"));QVERIFY(!f.entered);
    const auto next=f.host->prepare(request,host::Caller::Agent);QVERIFY(next.ok);
    f.onBinding=[&]{f.host.reset();};
    QVERIFY(!f.host->start(next.handle,request,this,"Deleted binding owner",[](auto){},&f.error));
    QCOMPARE(f.error,QString("document_unavailable"));
  }
};
QTEST_GUILESS_MAIN(TestPythonPreparation)
#include "tst_pythonpreparation.moc"
