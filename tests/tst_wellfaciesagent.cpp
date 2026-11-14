// 层：测试壳
#include "wellfaciesagentfixture.h"
#include "wellfacieshttpfixture.h"
#include "app/aiwiring.h"
#include "app/capabilityhostwiring.h"
#include "workflow/aichatcontroller.h"
#include "workflow/capabilityrecovery.h"
#include "workflow/documentsession.h"
#include "catalog/areawellprojection.h"
#include "metadata/projectlock.h"
#include "metadata/paleoprojectstore.h"
#include <QProcess>
#include <cstdlib>

namespace {
QByteArray encoded(const QJsonObject &json){return QJsonDocument(json).toJson(QJsonDocument::Compact);}
QJsonObject exactModel() {
  return {{"id","model"},{"name","Synthetic model"},{"version","v1"},
    {"inputSchema",QJsonObject{{"curves",QJsonArray{"GR"}},{"curveUnits",QJsonObject{{"GR","API"}}},
      {"window",2},{"labels",QJsonArray{QStringLiteral("河口坝")}},{"categoricalFields",QJsonArray{QStringLiteral("段")}}}}};
}
QJsonObject response(const QJsonArray &rows,QString job={}) {
  QJsonArray predictions;
  for(const auto &value:rows)predictions.append(QJsonObject{{"wellName","SYNTH-1"},
    {"depth",value.toObject().value(QStringLiteral("深度"))},{"label",QStringLiteral("河口坝")},{"confidence",.9}});
  QJsonObject json{{"status","completed"},{"model",QJsonObject{{"id","model"},{"version","v1"},{"name","Synthetic model"}}},
    {"predictions",predictions}};
  if(!job.isEmpty())json.insert("jobId",job);return json;
}
QJsonArray bodyRows(const QJsonObject &body){return body.value("wells").toArray().first().toObject().value("rows").toArray();}
struct AgentHost {
  Fixture data;
  explicit AgentHost(const QString &root={}):data(root){}
  DataCatalog catalog;
  PaleoProjectStore store;
  PaleoTaskService tasks{&store};
  paleo::document::DocumentSession session{&store,&tasks};
  paleo::capability::CapabilityHost host;
  AiChatController chat;
  std::unique_ptr<ProjectDirLock> lock;
  QString error;
  bool allowExternal=true;
  bool readOnly=false;
  bool open(const WellFaciesConfig &config) {
    if(!data.init()){error=QString::fromStdString(data.error);return false;}
    data.service.closeProject();
    return bind(config);
  }
  bool openExisting(const WellFaciesConfig &config,const QString &project,const QJsonObject &args) {
    data.job.projectDir=project;
    if(!wf::requestFromJson(args,data.job.request,&error))return false;
    return bind(config);
  }
  bool bind(const WellFaciesConfig &config) {
    const auto project=data.job.projectDir;
    store.setProjectPaths(project+"/project.qgz",project+"/project.gpkg",project+"/project.sqlite");
    if(!catalog.open(project,&error)||!registerAreaWells(catalog,&error))return false;
    if(!session.bind(&catalog,project+"/project.sqlite","doc",false,&error))return false;
    lock=std::make_unique<ProjectDirLock>(project);if(!lock->tryLock(&error))return false;
    host.setTaskService(&tasks);host.setCallerScope(paleo::host::Caller::Agent,"agent:well-tests");
    host.setBindingProvider([this] {
      auto binding=captureCapabilityBinding(&session,&catalog,lock.get(),nullptr);
      if(allowExternal)binding.context.allowedEffects|=paleo::host::ExternalTransfer;
      else binding.context.allowedEffects&=~paleo::host::ExternalTransfer;
      if(readOnly){binding.context.readOnly=true;binding.context.allowedEffects=paleo::host::Read;}
      return binding;
    });
    session.setHostContextProvider([this]{return host.context();});
    bindChatToolRunner(&chat,nullptr,nullptr,&catalog,project,session.documentId(),&host,&session);
    chat.toolRunner()->setTaskService(&tasks);chat.toolRunner()->setFaciesConfig(config);
    const auto endpoint=config.baseUrl.adjusted(QUrl::NormalizePathSegments|QUrl::StripTrailingSlash).toEncoded();
    data.job.request.endpointId=QString::fromLatin1(QCryptographicHash::hash(endpoint,QCryptographicHash::Sha256).toHex());
    chat.toolRunner()->setResultSession("well-test");
    data.job.request.well.catalogEntityId=catalog.entities("well").front().id;
    data.job.request.well.documentId=session.documentId();data.job.request.context.documentId=session.documentId();
    return true;
  }
  QJsonObject args(QString requestId="request") const {
    return {{"contract_version",2},{"client_request_id",requestId},{"model","model"},{"model_version","v1"},
      {"endpoint_id",data.job.request.endpointId},
      {"well_ref",paleo::document::wellRefJson(data.job.request.well)},
      {"context",paleo::document::interpretationContextJson(data.job.request.context)},
      {"unit_scope",wf::unitScopeJson(data.job.request.scope)}};
  }
  void run(const QJsonObject &args,QString id="call") {
    chat.toolRunner()->run({id,"paleo.well_facies_prediction",QString::fromUtf8(encoded(args))});
  }
  QString scope() const {return host.callerScope(paleo::host::Caller::Agent,"well-test");}
};
int crashAfterCompletion(QCoreApplication &application,const QStringList &arguments) {
  if(arguments.size()!=4)return 70;
  AgentHost f(QFileInfo(arguments[2]).absolutePath());f.data.temp.setAutoRemove(false);
  WellFaciesConfig config;config.baseUrl=QUrl(arguments[3]);config.apiKey=kKey;
  if(!f.open(config))return 71;
  const auto metadata=wf::inspectInput({f.data.job.projectDir,f.data.job.request.well,f.data.job.request.context});
  const auto occurrences=metadata.value("occurrences").toArray();if(occurrences.isEmpty())return 72;
  f.data.job.request.scope.occurrenceKeys={occurrences.first().toObject().value("key").toString()};
  const auto args=f.args();QFile state(arguments[2]);
  if(!state.open(QIODevice::WriteOnly)||state.write(encoded({{"project",f.data.job.projectDir},{"args",args}}))<=0)return 74;
  state.close();
  QObject::connect(&f.tasks,&PaleoTaskService::taskAdded,&application,[](PaleoTask *task) {
    if(task->title()=="Store external result")std::_Exit(73); // Durable remote proof, before attachment worker starts.
  });
  QObject::connect(f.chat.toolRunner(),&AiChatToolRunner::toolFinished,&application,[&application]{application.exit(75);});
  f.run(args);return application.exec();
}
}
class WellFaciesAgentTest:public QObject {
  Q_OBJECT
private slots:
  void processCrashAfterJoblessSuccessRestoresCachedResultWithoutHttp() {
    QTemporaryDir root;QVERIFY(root.isValid());const auto statePath=root.filePath("state.json");
    ScriptServer server;server.handler=[](const QByteArray &path,const QJsonObject &body) {
      if(path.endsWith("/models"))return Reply{200,encoded({{"models",QJsonArray{exactModel()}}})};
      return Reply{200,encoded(response(bodyRows(body)))};
    };
    QProcess child;child.start(QCoreApplication::applicationFilePath(),
      {"--crash-after-completion",statePath,server.config().baseUrl.toString()});
    QTRY_VERIFY_WITH_TIMEOUT(child.state()==QProcess::NotRunning,30000);
    QCOMPARE(child.exitCode(),73);QFile stateFile(statePath);QVERIFY(stateFile.open(QIODevice::ReadOnly));
    const auto state=QJsonDocument::fromJson(stateFile.readAll()).object(),args=state.value("args").toObject();
    QCOMPARE(server.count("/predict"),1);QCOMPARE(server.count("/models"),1);
    AgentHost f;QVERIFY2(f.openExisting(server.config(),state.value("project").toString(),args),qPrintable(f.error));
    const auto childRequest="well-segment-"+paleo::capability::operationDigest({{"parentRequestId","request"},{"segment",0}});
    auto record=f.host.recovery()->lookup(f.scope(),childRequest).record;
    QCOMPARE(record.execution,"succeeded");QVERIFY(record.remoteJobId.isEmpty());QVERIFY(record.artifactManifest.isEmpty());
    QVERIFY(!record.remoteCompletionProof.value("responseHash").toString().isEmpty());
    QSignalSpy finished(f.chat.toolRunner(),&AiChatToolRunner::toolFinished);f.run(args,"after-crash");
    QTRY_COMPARE_WITH_TIMEOUT(finished.size(),1,20000);QVERIFY2(finished.front()[1].toBool(),qPrintable(finished.front()[2].toString()));
    record=f.host.recovery()->lookup(f.scope(),childRequest).record;QVERIFY(!record.artifactManifest.isEmpty());
    QCOMPARE(record.computeCount,1);QCOMPARE(server.count("/predict"),1);QCOMPARE(server.count("/models"),1);
    QVERIFY(f.catalog.versions().isEmpty());
  }
  void productionAssemblyFreezesActualModelAndAllOccurrences() {
    ScriptServer server;server.handler=[](const QByteArray &path,const QJsonObject &body){
      if(path.endsWith("/models"))return Reply{200,encoded({{"models",QJsonArray{exactModel()}}})};
      return Reply{200,encoded(response(bodyRows(body)))};
    };
    AgentHost f;QVERIFY2(f.open(server.config()),qPrintable(f.error));
    QCOMPARE(f.chat.toolRunner()->wellFaciesReadiness({},true),QString());
    QSignalSpy finished(f.chat.toolRunner(),&AiChatToolRunner::toolFinished);const auto args=f.args();f.run(args);
    QTRY_COMPARE_WITH_TIMEOUT(finished.size(),1,20000);
    const auto result=QJsonDocument::fromJson(finished.front()[2].toString().toUtf8()).object();
    QVERIFY2(finished.front()[1].toBool(),qPrintable(finished.front()[2].toString()));
    QCOMPARE(result.value("code").toString(),"succeeded");QCOMPARE(result.value("segments").toArray().size(),2);
    QVERIFY(!result.contains("operationId"));QVERIFY(!result.value("inputOperationId").toString().isEmpty());
    QCOMPARE(result.value("executionScope").toString(),"batch_summary");
    QCOMPARE(result.value("inputOperation").toObject().value("execution").toString(),"prepared");
    QCOMPARE(server.count("/models"),1);QCOMPARE(server.count("/predict"),2);QVERIFY(f.catalog.versions().isEmpty());
    // A replay returns original operation results, never another model resolution or POST.
    finished.clear();f.run(args,"replay");QTRY_COMPARE_WITH_TIMEOUT(finished.size(),1,20000);
    QVERIFY2(finished.front()[1].toBool(),qPrintable(finished.front()[2].toString()));QCOMPARE(server.count("/predict"),2);
    QCOMPARE(server.count("/models"),1);
    const auto parent=f.host.recovery()->lookup(f.scope(),"request");QCOMPARE(parent.code,"found");QCOMPARE(parent.record.computeCount,0);
    QCOMPARE(parent.record.execution,"prepared");QVERIFY(!parent.record.frozenRequest.value("plan").toObject().value("storedInput").toObject().isEmpty());
    const auto childId=result.value("segments").toArray().first().toObject().value("operationId").toString();
    QJsonObject page;QString error;QByteArray artifactBytes;qint64 offset=0;
    do {
      bool read=false;
      QVERIFY2(f.host.recovery()->read(f.scope(),childId,"result",offset,24*1024,&f.chat,
        [&](QJsonObject value){page=value;read=true;},&error),qPrintable(error));
      QTRY_VERIFY(read);QVERIFY(page.value("ok").toBool());
      artifactBytes+=QByteArray::fromBase64(page.value("data").toString().toLatin1());
      offset=page.value("nextOffset").toString().toLongLong();QVERIFY(offset<=32*1024*1024);
    }while(page.value("hasMore").toBool());
    const auto artifact=QJsonDocument::fromJson(artifactBytes).object();
    QCOMPARE(artifact.value("contract").toString(),"paleo.well-facies.output.v2");QCOMPARE(artifact.value("depthAxis").toString(),"MD");
    QCOMPARE(artifact.value("predictions").toArray().size(),10);
    QCOMPARE(artifact.value("predictions").toArray().first().toObject().value("confidence").toDouble(),.9);
    const auto otherScope=f.host.callerScope(paleo::host::Caller::Agent,"other-chat");
    QCOMPARE(f.host.recovery()->lookup(otherScope,"request").code,"not_found");
  }
  void completedReplayNeedsOnlyReadWithoutProviderOrEndpoint() {
    ScriptServer server;server.handler=[](const QByteArray &path,const QJsonObject &body) {
      if(path.endsWith("/models"))return Reply{200,encoded({{"models",QJsonArray{exactModel()}}})};
      return Reply{200,encoded(response(bodyRows(body)))};
    };
    AgentHost f;QVERIFY2(f.open(server.config()),qPrintable(f.error));const auto args=f.args();
    QSignalSpy finished(f.chat.toolRunner(),&AiChatToolRunner::toolFinished);f.run(args);
    QTRY_COMPARE_WITH_TIMEOUT(finished.size(),1,20000);QVERIFY2(finished.front()[1].toBool(),qPrintable(finished.front()[2].toString()));
    const auto original=QJsonDocument::fromJson(finished.front()[2].toString().toUtf8()).object();
    f.allowExternal=false;f.readOnly=true;
    AiChatToolRunner::Context context;context.catalog=&f.catalog;context.capabilityHost=&f.host;
    context.documentId=f.session.documentId();context.documentEpoch=f.session.epoch();context.requireCapabilityHost=true;
    context.projectDir=f.data.job.projectDir;f.chat.toolRunner()->setContext(context);
    f.chat.toolRunner()->setFaciesConfig({});QCOMPARE(f.chat.toolRunner()->wellFaciesReadiness(args),QString());
    finished.clear();f.run(args,"read-only-replay");QTRY_COMPARE_WITH_TIMEOUT(finished.size(),1,10000);
    QVERIFY2(finished.front()[1].toBool(),qPrintable(finished.front()[2].toString()));
    const auto replay=QJsonDocument::fromJson(finished.front()[2].toString().toUtf8()).object();
    QCOMPARE(replay.value("inputOperationId"),original.value("inputOperationId"));QVERIFY(replay.value("replayed").toBool());
    QCOMPARE(replay.value("segments").toArray().size(),2);QCOMPARE(server.count("/predict"),2);QCOMPARE(server.count("/models"),1);
    for(int index=0;index<2;++index){const auto request="well-segment-"+paleo::capability::operationDigest({{"parentRequestId","request"},{"segment",index}});
      QCOMPARE(f.host.recovery()->lookup(f.scope(),request).record.computeCount,1);}
    f.chat.toolRunner()->setFaciesConfig(server.config());
    const auto change=connect(f.chat.toolRunner(),&AiChatToolRunner::toolStarted,&f.chat,[&](const ChatToolCall &){
      f.chat.toolRunner()->setFaciesConfig({});
    });
    finished.clear();f.run(args,"queued-epoch-replay");QTRY_COMPARE(finished.size(),1);disconnect(change);
    QVERIFY2(finished.front()[1].toBool(),qPrintable(finished.front()[2].toString()));
    QCOMPARE(QJsonDocument::fromJson(finished.front()[2].toString().toUtf8()).object().value("inputOperationId"),original.value("inputOperationId"));
    auto conflict=args;conflict.insert("model_version","v2");finished.clear();f.run(conflict,"conflict");
    QTRY_COMPARE(finished.size(),1);QVERIFY(!finished.front()[1].toBool());
    QCOMPARE(QJsonDocument::fromJson(finished.front()[2].toString().toUtf8()).object().value("code").toString(),"idempotency_conflict");
    QCOMPARE(server.count("/predict"),2);QCOMPARE(server.count("/models"),1);
  }
  void missingLocalBytesReplayOnlyExecutionReceiptsAndNeverPost() {
    ScriptServer server;server.handler=[](const QByteArray &path,const QJsonObject &body) {
      if(path.endsWith("/models"))return Reply{200,encoded({{"models",QJsonArray{exactModel()}}})};
      return Reply{200,encoded(response(bodyRows(body)))};
    };
    AgentHost f;QVERIFY2(f.open(server.config()),qPrintable(f.error));const auto args=f.args();
    QSignalSpy finished(f.chat.toolRunner(),&AiChatToolRunner::toolFinished);f.run(args);
    QTRY_COMPARE_WITH_TIMEOUT(finished.size(),1,20000);QVERIFY2(finished.front()[1].toBool(),qPrintable(finished.front()[2].toString()));
    const auto childRequest="well-segment-"+paleo::capability::operationDigest({{"parentRequestId","request"},{"segment",0}});
    const auto child=f.host.recovery()->lookup(f.scope(),childRequest).record;
    const auto port=child.artifactManifest.value("ports").toArray().first().toObject();
    QVERIFY(QFile::remove(QDir(f.data.job.projectDir).filePath(port.value("path").toString())));
    const auto parent=f.host.recovery()->lookup(f.scope(),"request").record;
    const auto directory=parent.frozenRequest.value("plan").toObject().value("storedInput").toObject().value("directory").toString();
    QDir inputs(QDir(f.data.job.projectDir).filePath(directory));
    for(const auto &name:inputs.entryList({"result-*.json"},QDir::Files))QVERIFY(QFile::remove(inputs.filePath(name)));
    finished.clear();f.run(args,"deleted-result");QTRY_COMPARE(finished.size(),1);QVERIFY(finished.front()[1].toBool());
    const auto replay=QJsonDocument::fromJson(finished.front()[2].toString().toUtf8()).object();
    QCOMPARE(replay.value("resultAvailability").toString(),"reference_unverified");
    QCOMPARE(replay.value("coverage").toObject().value("subject").toString(),"durable_execution_receipts");
    QVERIFY(!replay.value("coverage").toObject().value("resultBytesVerified").toBool());
    bool read=false;QJsonObject page;QString error;
    QVERIFY(f.host.recovery()->read(f.scope(),child.operationId,"result",0,1024,&f.chat,[&](QJsonObject value){page=value;read=true;},&error));
    QTRY_VERIFY(read);QVERIFY(!page.value("ok").toBool());
    QCOMPARE(server.count("/predict"),2);QCOMPARE(server.count("/models"),1);
    QCOMPARE(f.host.recovery()->lookup(f.scope(),childRequest).record.computeCount,1);
  }
  void missingExactModelMetadataReadsNoCurvesAndSendsNoData_data() {
    QTest::addColumn<QString>("missing");
    QTest::newRow("version")<<"version";QTest::newRow("units")<<"curveUnits";QTest::newRow("labels")<<"labels";
    QTest::newRow("window")<<"window";
  }
  void missingExactModelMetadataReadsNoCurvesAndSendsNoData() {
    QFETCH(QString,missing);ScriptServer server;
    server.handler=[missing](const QByteArray &,const QJsonObject &){auto model=exactModel();
      if(missing=="version")model.remove(missing);else{auto schema=model.value("inputSchema").toObject();schema.remove(missing);model.insert("inputSchema",schema);}
      return Reply{200,encoded({{"models",QJsonArray{model}}})};};
    AgentHost f;QVERIFY2(f.open(server.config()),qPrintable(f.error));QSignalSpy finished(f.chat.toolRunner(),&AiChatToolRunner::toolFinished);
    f.run(f.args());QTRY_COMPARE_WITH_TIMEOUT(finished.size(),1,10000);QVERIFY(!finished.front()[1].toBool());
    QCOMPARE(server.count("/models"),1);QCOMPARE(server.count("/predict"),0);
    QVERIFY(!QFileInfo::exists(f.data.job.projectDir+"/.paleo/capability-ops/well-inputs"));
    QCOMPARE(f.host.recovery()->lookup(f.scope(),"request").code,"not_found");
  }
  void missingExternalPermissionRejectsBeforeEndpoint() {
    ScriptServer server;AgentHost f;QVERIFY2(f.open(server.config()),qPrintable(f.error));f.allowExternal=false;
    QSignalSpy finished(f.chat.toolRunner(),&AiChatToolRunner::toolFinished);f.run(f.args());
    QTRY_COMPARE(finished.size(),1);QVERIFY(!finished.front()[1].toBool());QVERIFY(server.paths.isEmpty());
  }
  void modelNamespaceMustMatchConfiguredEndpoint() {
    ScriptServer server;AgentHost f;QVERIFY2(f.open(server.config()),qPrintable(f.error));auto args=f.args();
    auto endpoint=args.value("endpoint_id").toString();endpoint[0]=endpoint[0]=='0'?'1':'0';args.insert("endpoint_id",endpoint);
    QSignalSpy finished(f.chat.toolRunner(),&AiChatToolRunner::toolFinished);f.run(args);
    QTRY_COMPARE(finished.size(),1);QVERIFY(!finished.front()[1].toBool());QVERIFY(server.paths.isEmpty());
    const auto result=QJsonDocument::fromJson(finished.front()[2].toString().toUtf8()).object();QCOMPARE(result.value("code").toString(),"frozen_endpoint_unavailable");
  }
  void providerConfigurationDoesNotExpireImmutableChatResults() {
    ScriptServer server;AgentHost f;QVERIFY2(f.open(server.config()),qPrintable(f.error));QJsonArray rows;
    for(int i=0;i<200;++i)rows.append(QJsonObject{{"id",i},{"label",QString(100,'x')}});
    const auto envelope=f.chat.toolRunner()->modelResult({"large","paleo.query_project","{}"},
      QString::fromUtf8(encoded({{"rows",rows}})),true,1024);
    const auto ref=envelope.value("resultRefs").toArray().first().toObject().value("ref").toString();QVERIFY(!ref.isEmpty());
    auto config=server.config();config.apiKey="another-mock-key";f.chat.toolRunner()->setFaciesConfig(config);
    QSignalSpy finished(f.chat.toolRunner(),&AiChatToolRunner::toolFinished);
    f.chat.toolRunner()->run({"read","paleo.read_result",QString::fromUtf8(encoded({{"result_ref",ref},{"selector","/rows"},{"page_size",1},{"max_bytes",1024}}))});
    QTRY_COMPARE(finished.size(),1);QVERIFY2(finished.front()[1].toBool(),qPrintable(finished.front()[2].toString()));
    const auto result=QJsonDocument::fromJson(finished.front()[2].toString().toUtf8()).object();QCOMPARE(result.value("code").toString(),"ok");
    QVERIFY(server.paths.isEmpty());
  }
  void uncertainSubmissionDoesNotPostAgain() {
    ScriptServer server;server.handler=[](const QByteArray &path,const QJsonObject &){
      if(path.endsWith("/models"))return Reply{200,encoded({{"models",QJsonArray{exactModel()}}})};
      return Reply{200,"{}",{},true};
    };
    AgentHost f;QVERIFY2(f.open(server.config()),qPrintable(f.error));const auto args=f.args();
    f.run(args);QTRY_COMPARE_WITH_TIMEOUT(server.count("/predict"),1,20000);f.chat.toolRunner()->cancel();
    QSignalSpy finished(f.chat.toolRunner(),&AiChatToolRunner::toolFinished);f.run(args,"retry");
    QTRY_COMPARE_WITH_TIMEOUT(finished.size(),1,20000);QVERIFY(!finished.front()[1].toBool());
    const auto result=QJsonDocument::fromJson(finished.front()[2].toString().toUtf8()).object();
    QCOMPARE(result.value("code").toString(),"ExternalOutcomeUnknown");QCOMPARE(server.count("/predict"),1);QCOMPARE(server.count("/models"),1);
  }
  void authoritativeRemoteFailureIsTerminalAndNeverResubmitted() {
    ScriptServer server;server.handler=[](const QByteArray &path,const QJsonObject &) {
      if(path.endsWith("/models"))return Reply{200,encoded({{"models",QJsonArray{exactModel()}}})};
      return Reply{200,encoded({{"status","failed"},{"jobId","failed-original"},{"error","fixture terminal failure"}})};
    };
    AgentHost f;QVERIFY2(f.open(server.config()),qPrintable(f.error));const auto args=f.args();
    QSignalSpy finished(f.chat.toolRunner(),&AiChatToolRunner::toolFinished);f.run(args);
    QTRY_COMPARE_WITH_TIMEOUT(finished.size(),1,20000);QVERIFY(!finished.front()[1].toBool());
    const auto result=QJsonDocument::fromJson(finished.front()[2].toString().toUtf8()).object();QCOMPARE(result.value("code").toString(),"remote_failed");
    const auto childRequest="well-segment-"+paleo::capability::operationDigest({{"parentRequestId","request"},{"segment",0}});
    const auto child=f.host.recovery()->lookup(f.scope(),childRequest);QCOMPARE(child.record.execution,"failed");
    QCOMPARE(child.record.remoteJobId,"failed-original");QCOMPARE(child.record.computeCount,1);
    finished.clear();f.run(args,"replay");QTRY_COMPARE_WITH_TIMEOUT(finished.size(),1,20000);
    QVERIFY(!finished.front()[1].toBool());QCOMPARE(server.count("/predict"),1);QCOMPARE(server.count("/models"),1);
  }
  void cancellationDuringTaskAdmissionSendsNothing() {
    ScriptServer server;AgentHost f;QVERIFY2(f.open(server.config()),qPrintable(f.error));bool triggered=false;
    connect(&f.tasks,&PaleoTaskService::taskAdded,&f.chat,[&](PaleoTask *) {
      if(triggered)return;triggered=true;f.chat.toolRunner()->cancel();
    });
    f.run(f.args());QTRY_VERIFY(triggered);QTest::qWait(50);QVERIFY(server.paths.isEmpty());QVERIFY(!f.chat.toolRunner()->busy());
    QCOMPARE(f.host.recovery()->lookup(f.scope(),"request").code,"not_found");
  }
  void knownCompletionPersistsAfterChatCancelsReceiptTask() {
    ScriptServer server;server.handler=[](const QByteArray &path,const QJsonObject &body) {
      if(path.endsWith("/models"))return Reply{200,encoded({{"models",QJsonArray{exactModel()}}})};
      return Reply{200,encoded(response(bodyRows(body)))};
    };
    AgentHost f;QVERIFY2(f.open(server.config()),qPrintable(f.error));bool cancelled=false;
    connect(&f.tasks,&PaleoTaskService::taskAdded,&f.chat,[&](PaleoTask *task) {
      if(task->title()!=QStringLiteral("冻结测井相结果")||cancelled)return;
      cancelled=true;f.chat.toolRunner()->cancel();
    });
    f.run(f.args());QTRY_VERIFY_WITH_TIMEOUT(cancelled,20000);
    const auto childRequest="well-segment-"+paleo::capability::operationDigest({{"parentRequestId","request"},{"segment",0}});
    QTRY_VERIFY_WITH_TIMEOUT(!f.host.recovery()->lookup(f.scope(),childRequest).record.artifactManifest.isEmpty(),20000);
    const auto child=f.host.recovery()->lookup(f.scope(),childRequest);QCOMPARE(child.record.execution,"succeeded");
    QCOMPARE(child.record.computeCount,1);QCOMPARE(server.count("/predict"),1);QVERIFY(!f.chat.toolRunner()->busy());
  }
  void parentCancellationPreservesAcceptedJobFactsAndStopsNewPosts() {
    AgentHost f;ScriptServer server;QJsonArray acceptedRows;
    server.handler=[&](const QByteArray &path,const QJsonObject &body) {
      if(path.endsWith("/models"))return Reply{200,encoded({{"models",QJsonArray{exactModel()}}})};
      if(path.contains("/predictions/"))return Reply{200,encoded(response(acceptedRows,"cancel-original"))};
      acceptedRows=bodyRows(body);return Reply{202,encoded({{"jobId","cancel-original"},{"status","predicting"},{"pollAfterMs",30000}})};
    };
    QVERIFY2(f.open(server.config()),qPrintable(f.error));const auto args=f.args();f.run(args);
    const auto childRequest="well-segment-"+paleo::capability::operationDigest({{"parentRequestId","request"},{"segment",0}});
    QTRY_VERIFY_WITH_TIMEOUT(!f.host.recovery()->lookup(f.scope(),childRequest).record.remoteJobId.isEmpty(),20000);
    const auto input=f.host.recovery()->lookup(f.scope(),"request").record;
    const auto cancelled=f.host.recovery()->cancel(f.scope(),input.operationId);QVERIFY(cancelled.value("cancelRequested").toBool());
    f.chat.toolRunner()->cancel();
    QSignalSpy finished(f.chat.toolRunner(),&AiChatToolRunner::toolFinished);f.run(args,"resume-known-child");
    QTRY_COMPARE_WITH_TIMEOUT(finished.size(),1,20000);QVERIFY(!finished.front()[1].toBool());
    const auto result=QJsonDocument::fromJson(finished.front()[2].toString().toUtf8()).object();
    QCOMPARE(result.value("code").toString(),"cancelled_before_next_segment");QCOMPARE(result.value("executionScope").toString(),"batch_summary");
    QCOMPARE(result.value("inputOperationId").toString(),input.operationId);QVERIFY(!result.contains("operationId"));
    const auto child=f.host.recovery()->lookup(f.scope(),childRequest).record;
    QCOMPARE(child.execution,"succeeded");QVERIFY(!child.artifactManifest.isEmpty());QCOMPARE(child.computeCount,1);
    QCOMPARE(server.count("/predict"),1);QCOMPARE(server.count("/predictions/cancel-original"),1);QCOMPARE(server.count("/models"),1);
    const auto second="well-segment-"+paleo::capability::operationDigest({{"parentRequestId","request"},{"segment",1}});
    QCOMPARE(f.host.recovery()->lookup(f.scope(),second).code,"not_found");
    finished.clear();f.run(args,"no-more-posts");QTRY_COMPARE(finished.size(),1);QVERIFY(!finished.front()[1].toBool());
    QCOMPARE(server.count("/predict"),1);QCOMPARE(server.count("/predictions/cancel-original"),1);
    const auto snapshot=f.host.recovery()->lookup(f.scope(),"request").record;QCOMPARE(snapshot.execution,"prepared");QCOMPARE(snapshot.computeCount,0);
  }
  void queryAcceptedJobUsesPinnedInputAfterLiveCurveEdits() {
    ScriptServer server;QJsonArray acceptedRows;int posts=0;
    server.handler=[&](const QByteArray &path,const QJsonObject &body) {
      if(path.endsWith("/models"))return Reply{200,encoded({{"models",QJsonArray{exactModel()}}})};
      if(path.contains("/predictions/"))return Reply{200,encoded(response(acceptedRows,"remote-original"))};
      if(++posts==1){acceptedRows=bodyRows(body);return Reply{202,encoded({{"jobId","remote-original"},{"status","predicting"},{"pollAfterMs",30000}})};}
      return Reply{200,encoded(response(bodyRows(body)))};
    };
    AgentHost f;QVERIFY2(f.open(server.config()),qPrintable(f.error));const auto args=f.args();f.run(args);
    const auto childRequest="well-segment-"+paleo::capability::operationDigest({{"parentRequestId","request"},{"segment",0}});
    QTRY_VERIFY_WITH_TIMEOUT(!f.host.recovery()->lookup(f.scope(),childRequest).record.remoteJobId.isEmpty(),20000);
    f.chat.toolRunner()->cancel();
    data::DataService edit;std::string error;QVERIFY(edit.openProject(paleo::area_model::directory(f.data.job.projectDir).toStdString(),error));
    std::vector<data::DataEntry>curves;QVERIFY(edit.listData("curves",f.data.job.request.well.modelWellId,curves,error));
    QVERIFY(edit.updateCurveValue(curves.front().id,0,777.,error));edit.closeProject();
    QSignalSpy finished(f.chat.toolRunner(),&AiChatToolRunner::toolFinished);f.run(args,"resume");
    QTRY_COMPARE_WITH_TIMEOUT(finished.size(),1,20000);QVERIFY2(finished.front()[1].toBool(),qPrintable(finished.front()[2].toString()));
    QCOMPARE(posts,2);QCOMPARE(server.count("/models"),2);QCOMPARE(server.count("/predictions/remote-original"),1);
    QCOMPARE(acceptedRows.first().toObject().value("GR").toDouble(),20.); // Frozen original values, not the edited live row.
  }
  void modelCatalogQueryDisclosesReadinessWithoutWellRead() {
    ScriptServer server;server.handler=[](const QByteArray &,const QJsonObject &){auto model=exactModel();
      auto schema=model.value("inputSchema").toObject();schema.remove("curveUnits");model.insert("inputSchema",schema);
      return Reply{200,encoded({{"models",QJsonArray{model}}})};};
    AgentHost f;QVERIFY2(f.open(server.config()),qPrintable(f.error));QSignalSpy finished(f.chat.toolRunner(),&AiChatToolRunner::toolFinished);
    f.chat.toolRunner()->run({"models","paleo.query_project",QString::fromUtf8(encoded({{"topic","well_facies_models"},{"model_id","model"}}))});
    QTRY_COMPARE(finished.size(),1);QVERIFY(finished.front()[1].toBool());
    const auto result=QJsonDocument::fromJson(finished.front()[2].toString().toUtf8()).object();
    const auto model=result.value("models").toArray().first().toObject();QVERIFY(!model.value("ready").toBool());
    QCOMPARE(model.value("readinessCode").toString(),"model_feature_units_unavailable");QCOMPARE(server.count("/predict"),0);
  }
};
int main(int argc,char **argv) {
  QCoreApplication application(argc,argv);const auto arguments=application.arguments();
  if(arguments.size()>1&&arguments[1]=="--crash-after-completion")return crashAfterCompletion(application,arguments);
  WellFaciesAgentTest test;return QTest::qExec(&test,argc,argv);
}
#include "tst_wellfaciesagent.moc"
