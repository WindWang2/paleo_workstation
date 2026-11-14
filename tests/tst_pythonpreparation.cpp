// 层：测试壳
#include "workflow/capabilityhost.h"
#include "services/pythonanalysiscapabilityspec.h"
#include "services/paleotaskservice.h"
#include "catalog/datacatalog.h"
#include "metadata/projectlock.h"
#include <QtTest>
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
    host->setTaskService(tasks.get());host->setCallerScope(host::Caller::Agent,"agent:test");
    host->setBindingProvider([this]{return binding();});
    host->registerExecutionAdapter(QLatin1String(python::WellQualityCapability),adapter());return true;
  }
  ExecuteRequest request() {
    ExecuteRequest out;out.capabilityId=QLatin1String(python::WellQualityCapability);out.clientRequestId="stable-request";
    out.publishIntent="compute-only";return out;
  }
  void replaceTasks() {tasks=std::make_unique<ObservedTaskService>();host->setTaskService(tasks.get());}
};
}
class TestPythonPreparation:public QObject {
  Q_OBJECT
private slots:
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
