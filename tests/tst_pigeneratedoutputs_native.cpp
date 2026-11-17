// 层：测试壳
#include "fixtures/pythongeneratedoutputsfixture.h"
#include "workflow/aichatcontroller.h"
#include "ai/chat/pythonanalysistools.h"
#include <QHostAddress>
#include <QImage>
#include <QMap>
#include <QSet>
#include <QSignalSpy>
#include <QStandardPaths>
#include <QTcpServer>
#include <QTcpSocket>
#include <QtTest>
#include <cstring>
using namespace paleo;
using namespace generated_outputs_fixture;
namespace {
// Only the remote model is controlled. The C++ LlmClient consumes real SSE;
// Node/Pi, tool authority, SDK bytes, strong Python and Catalog commit are real.
class Model:public QObject {
public:
  QTcpServer server;int requests=0;bool emitTool=false,authorizationSeen=false;
  QString error;QSet<QString> responseIds;QStringList providerCalls,advertisedTools;ChatToolCall next;
  Model() {
    connect(&server,&QTcpServer::newConnection,this,[this] {
      auto *socket=server.nextPendingConnection();auto bytes=std::make_shared<QByteArray>();auto served=std::make_shared<bool>(false);
      connect(socket,&QTcpSocket::disconnected,socket,&QObject::deleteLater);
      connect(socket,&QTcpSocket::readyRead,socket,[this,socket,bytes,served] {
        if(*served)return;bytes->append(socket->read(256*1024));
        if(bytes->size()>256*1024){*served=true;error="model_request_budget";socket->disconnectFromHost();return;}
        const auto header=bytes->indexOf("\r\n\r\n");if(header<0)return;
        const auto headers=QString::fromUtf8(bytes->left(header));
        const auto length=headers.section("Content-Length:",1,1,QString::SectionCaseInsensitiveSeps).section('\r',0,0).trimmed().toInt();
        if(length<0||length>256*1024){*served=true;error="model_request_length";socket->disconnectFromHost();return;}
        if(bytes->size()<header+4+length)return;*served=true;++requests;
        authorizationSeen=authorizationSeen||headers.contains("Authorization: Bearer fixture-only-key",Qt::CaseInsensitive);
        const auto request=QJsonDocument::fromJson(bytes->mid(header+4,length)).object();
        for(const auto &tool:request.value("tools").toArray())advertisedTools.append(tool.toObject().value("function").toObject().value("name").toString());
        const bool tool=emitTool;emitTool=false;QJsonObject delta{{"role","assistant"}};
        if(tool) {
          auto call=next;call.id=QString("provider-call-%1").arg(requests);providerCalls.append(call.id);
          auto object=call.toJson();object["index"]=0;delta["tool_calls"]=QJsonArray{object};
        } else delta["content"]="completed through the authoritative host";
        const auto id=QString("response-%1").arg(requests);responseIds.insert(id);
        const auto event=[&](const QJsonObject &value) {
          return QByteArray("data: ")+QJsonDocument(value).toJson(QJsonDocument::Compact)+"\n\n";
        };
        const QByteArray body=event({{"id",id},{"choices",QJsonArray{QJsonObject{{"index",0},{"delta",delta},{"finish_reason",QJsonValue::Null}}}}})+
          event({{"id",id},{"choices",QJsonArray{QJsonObject{{"index",0},{"delta",QJsonObject{}},{"finish_reason",tool?"tool_calls":"stop"}}}}})+"data: [DONE]\n\n";
        socket->write("HTTP/1.1 200 OK\r\nContent-Type: text/event-stream\r\nContent-Length: "+QByteArray::number(body.size())+
          "\r\nConnection: close\r\nCache-Control: no-cache\r\n\r\n"+body);socket->disconnectFromHost();
      });
    });
  }
  bool open(){return server.listen(QHostAddress::LocalHost,0);}
  LlmConfig config()const{return LlmConfig::fromParts(QUrl(QString("http://127.0.0.1:%1/v1").arg(server.serverPort())),"controlled-sse-model","fixture-only-key",true);}
  void call(const QString &tool,const QJsonObject &arguments) {
    emitTool=true;next={"model-provider-id",tool,QString::fromUtf8(QJsonDocument(arguments).toJson(QJsonDocument::Compact))};
  }
};
void bind(Fixture &fixture,AiChatController &controller,const Model &model,bool pi=true) {
  const auto selected=fixture.registry->active(python::EnvironmentMode::Generated);
  controller.toolRunner()->setTaskService(&fixture.tasks);controller.toolRunner()->setPythonEnvironments(fixture.registry);
  AiChatToolContext context;context.catalog=&fixture.catalog;context.projectDir=fixture.project;context.documentId=fixture.context.documentId;
  context.documentEpoch=fixture.context.epoch;context.capabilityHost=fixture.host.get();context.requireCapabilityHost=true;
  context.pythonRuntime=selected.runtime;context.pythonSandbox=selected.sandbox;
  context.runtimeReadiness=[registry=fixture.registry](const QString &name,const QJsonObject &arguments,bool listing) {
    if(name!=QLatin1String(python::GeneratedOutputsTool))return isPythonAnalysisTool(name)?QString("python_execution_denied"):QString();
    if(!listing&&arguments.value("publish_intent")=="publish")return registry->status().value("allowGenerated").toBool()?QString():QString("python_execution_denied");
    const auto active=registry->active(python::EnvironmentMode::Generated);if(!active.ok())return active.code;
    const auto integrity=active.runtime->snapshot();if(!integrity.ready())return integrity.code;
    const auto proof=active.sandbox->snapshot();return proof.value("ready").toBool()?QString():proof.value("code").toString("python_sandbox_unavailable");
  };
  controller.toolRunner()->setContext(context);controller.setConfig(model.config());
  controller.setPiRuntime({pi,qEnvironmentVariable("PALEO_TEST_PI_NATIVE_ROOT"),qEnvironmentVariable("PALEO_TEST_PI_NATIVE_SHA256")});
}
capability::OperationLog::Record boundRecord(Fixture &fixture,const AiChatController &controller,int index=0) {
  const auto bindings=controller.session().hostToolBindings;if(index<0||index>=bindings.size())return {};
  const auto id=bindings.at(index).toObject().value("clientRequestId").toString();
  return fixture.host->recovery()->lookup(fixture.host->callerScope(host::Caller::Agent,controller.session().id),id).record;
}
QJsonObject lastTool(const AiChatController &controller) {
  const auto messages=controller.messages();for(auto it=messages.crbegin();it!=messages.crend();++it)
    if(it->role==ChatRole::Tool)return QJsonDocument::fromJson(it->content.toUtf8()).object();return {};
}
}
class TestPiGeneratedOutputsNative:public QObject {
  Q_OBJECT
  QStringList sessions;
private slots:
  void initTestCase() {
    QStandardPaths::setTestModeEnabled(true);
    for(const auto &name:QStringList{"PALEO_TEST_AGENT_PYTHON_ROOT","PALEO_TEST_PI_NATIVE_ROOT"})QVERIFY2(!qEnvironmentVariable(name.toLatin1().constData()).isEmpty(),qPrintable(name+" required by this opt-in suite"));
    for(const auto &name:QStringList{"PALEO_TEST_AGENT_PYTHON_MANIFEST_SHA256","PALEO_TEST_PI_NATIVE_SHA256"}) {
      const auto sha=qEnvironmentVariable(name.toLatin1().constData());QCOMPARE(sha.size(),64);QCOMPARE(QByteArray::fromHex(sha.toLatin1()).toHex(),sha.toLatin1());
    }
  }
  void cleanupTestCase(){for(const auto &id:sessions)ChatSessionStore::remove(id,nullptr);}
  void realPiSseSdkStrongPythonThreePortsAndStableNativeReplay() {
    Fixture f;QVERIFY2(f.openTrace(),qPrintable(f.error));f.tasks.setMaxWorkerThreads(2);
    QVERIFY2(f.activateRuntime(),qPrintable(f.error));f.configureOutputs();const auto identity=f.original.identity;Model model;QVERIFY(model.open());
    auto controller=std::make_unique<AiChatController>();bind(f,*controller,model);const auto session=controller->session().id;sessions.append(session);
    QVERIFY(controller->piRuntime().enabled);auto *bridge=controller->findChild<PiChatBridge*>();QVERIFY(bridge);
    QSignalSpy piModels(bridge,&PiChatBridge::modelRequested),piTools(bridge,&PiChatBridge::toolRequested),errors(controller.get(),&AiChatController::errorOccurred);
    const auto arguments=f.outputArguments("the-model-does-not-own-the-request-id");model.call(QLatin1String(python::GeneratedOutputsTool),arguments);
    controller->sendUserText("Compute the exact selected SEG-Y trace window and return a declared table and nonspatial RGBA image.");
    QTRY_VERIFY_WITH_TIMEOUT(!controller->streaming(),180000);
    QVERIFY2(errors.isEmpty(),errors.isEmpty()?"":qPrintable(errors.first().first().toString()));QVERIFY2(model.error.isEmpty(),qPrintable(model.error));
    QCOMPARE(piModels.size(),2);QCOMPARE(piTools.size(),1);QCOMPARE(model.requests,2);QVERIFY(model.authorizationSeen);
    QVERIFY(model.advertisedTools.contains(QLatin1String(python::GeneratedOutputsTool)));QCOMPARE(controller->session().hostToolBindings.size(),1);
    const auto bindings=controller->session().hostToolBindings;const auto binding=bindings.first().toObject();
    QVERIFY(!binding.value("turnId").toString().isEmpty());QVERIFY(binding.value("clientRequestId").toString()!=arguments.value("client_request_id").toString());
    const auto computed=boundRecord(f,*controller);QVERIFY(!computed.operationId.isEmpty());QCOMPARE(computed.computeCount,1);QCOMPARE(computed.execution,QString("succeeded"));
    const auto plan=computed.frozenRequest.value("plan").toObject().value("plan").toObject();QCOMPARE(plan.value("inputKind").toString(),QString("seismic_trace"));
    const auto provenance=plan.value("inputMetadata").toObject().value("provenance").toObject();
    QCOMPARE(provenance.value("readCut"),QJsonValue(python::seismicTraceReadCutJson(f.descriptor.cut)));
    QCOMPARE(provenance.value("readMetrics").toObject().value("sdkRequestedSourceBytes").toString(),QString("652"));
    QCOMPARE(provenance.value("readMetrics").toObject().value("authenticatedFullHashBytes").toString(),QString::number(f.raw.size()));
    QCOMPARE(plan.value("codeSha256").toString(),QString::fromLatin1(QCryptographicHash::hash(sourceCode(),QCryptographicHash::Sha256).toHex()));
    QCOMPARE(plan.value("runtimeManifestSha256").toString(),identity.runtimeSha256);QVERIFY(f.derived().isEmpty());
    const auto scope=f.host->callerScope(host::Caller::Agent,session);const auto materializations=f.counts->materializations.load(),workers=f.counts->workerInvocations.load();
    QVERIFY(f.counts->ownerOnly.load());QVERIFY(f.counts->workerOffOwner.load());
    controller->setPiRuntime({false,{},{}});model.call(QLatin1String(python::GeneratedOutputsTool),arguments);controller->retryLast();
    QTRY_VERIFY_WITH_TIMEOUT(!controller->streaming(),180000);QVERIFY(errors.isEmpty());QCOMPARE(piModels.size(),2);
    QCOMPARE(controller->session().hostToolBindings,bindings);QCOMPARE(boundRecord(f,*controller).operationId,computed.operationId);
    QCOMPARE(f.counts->materializations.load(),materializations);QCOMPARE(f.counts->workerInvocations.load(),workers);
    controller->setPiRuntime({true,qEnvironmentVariable("PALEO_TEST_PI_NATIVE_ROOT"),qEnvironmentVariable("PALEO_TEST_PI_NATIVE_SHA256")});
    QMap<QString,QByteArray> outputs;
    for(const auto &id:QStringList{"REPORT","TABLE","IMAGE"}) {
      model.call("paleo.operation_read",{{"operation_id",computed.operationId},{"port_id",id},{"offset","0"},{"max_bytes",128}});
      controller->sendUserText("Read the first authenticated "+id+" page, with a 128-byte limit.");QTRY_VERIFY_WITH_TIMEOUT(!controller->streaming(),180000);QVERIFY(errors.isEmpty());
      const auto first=lastTool(*controller).value("data").toObject();QVERIFY2(first.value("ok").toBool(),qPrintable(QJsonDocument(first).toJson()));
      QCOMPARE(first.value("portId").toString(),id);const auto prefix=QByteArray::fromBase64(first.value("data").toString().toLatin1());QVERIFY(!prefix.isEmpty()&&prefix.size()<=128);
      const auto all=f.bytesFor(computed.operationId,id,scope);QVERIFY2(!all.isEmpty(),qPrintable(f.error));QVERIFY(all.startsWith(prefix));outputs.insert(id,all);
    }
    QCOMPARE(piModels.size(),8);QCOMPARE(outputs.value("TABLE"),expectedCsv());
    const auto image=QImage::fromData(outputs.value("IMAGE"),"PNG").convertToFormat(QImage::Format_RGBA8888);QCOMPARE(image.width(),3);QCOMPARE(image.height(),1);
    const auto rgba=expectedRgba();QCOMPARE(std::memcmp(image.constScanLine(0),rgba.constData(),size_t(rgba.size())),0);
    const auto report=QJsonDocument::fromJson(outputs.value("REPORT")).object();
    QCOMPARE(report.value("metrics").toArray().first().toObject().value("value").toDouble(),10.0);
    QCOMPARE(report.value("metrics").toArray().first().toObject().value("unit").toString(),QString("relative_amplitude"));
    QCOMPARE(report.value("provenance"),plan.value("inputMetadata").toObject().value("provenance"));
    QCOMPARE(report.value("outputProvenance"),plan.value("outputProfile"));
    QVERIFY(QFile::remove(f.source.path));controller.reset();QVERIFY2(f.restartWithAnotherActivePolicy(),qPrintable(f.error));f.configureOutputs();
    const auto historical=f.registry->resolve(identity.id,identity.runtimeSha256,identity.policy.json(),python::EnvironmentMode::Generated);
    QVERIFY2(historical.ok(),qPrintable(historical.code));QVERIFY(!historical.sandbox->snapshot().value("ready").toBool());
    // Warm only the NEW active identity so the real Pi tool snapshot advertises
    // the capability. The old computed code/identity is never executed again.
    const auto active=f.registry->active(python::EnvironmentMode::Generated);
    const auto proof=numeric_table_native_fixture::completed(QtConcurrent::run([&]{return f.registry->probe(active.identity.id,python::EnvironmentMode::Generated);}));
    QVERIFY2(proof.value("ready").toBool(),qPrintable(QJsonDocument(proof).toJson()));QVERIFY(!historical.sandbox->snapshot().value("ready").toBool());
    controller=std::make_unique<AiChatController>();sessions.append(controller->session().id);QVERIFY(controller->loadSession(session));bind(f,*controller,model);
    auto *restartBridge=controller->findChild<PiChatBridge*>();QVERIFY(restartBridge);QSignalSpy restartedPi(restartBridge,&PiChatBridge::modelRequested),restartErrors(controller.get(),&AiChatController::errorOccurred);
    const auto publication=f.publication("another-model-owned-id",computed.operationId);model.call(QLatin1String(python::GeneratedOutputsTool),publication);
    controller->sendUserText("Publish the frozen REPORT, TABLE and IMAGE once after host restart; do not rerun code or reopen the source.");
    QTRY_VERIFY_WITH_TIMEOUT(!controller->streaming(),180000);QVERIFY2(restartErrors.isEmpty(),restartErrors.isEmpty()?"":qPrintable(restartErrors.first().first().toString()));
    QCOMPARE(restartedPi.size(),2);QCOMPARE(lastTool(*controller).value("commitState").toString(),QString("committed"));QCOMPARE(f.derived().size(),3);
    for(const auto &version:f.derived())QCOMPARE(version.parentVersionIds,QStringList{f.source.versionId});
    const auto publicationRecord=boundRecord(f,*controller,controller->session().hostToolBindings.size()-1);QCOMPARE(publicationRecord.computeCount,0);
    QCOMPARE(f.recordById(computed.operationId).computeCount,1);QCOMPARE(f.counts->workerInvocations.load(),workers);QCOMPARE(f.counts->materializations.load(),materializations);
    for(const auto &id:QStringList{"REPORT","TABLE","IMAGE"})QCOMPARE(f.bytesFor(computed.operationId,id,scope),outputs.value(id));
    QVERIFY(!historical.sandbox->snapshot().value("ready").toBool());
    controller->setPiRuntime({false,{},{}});model.call(QLatin1String(python::GeneratedOutputsTool),publication);controller->retryLast();
    QTRY_VERIFY_WITH_TIMEOUT(!controller->streaming(),180000);QVERIFY(restartErrors.isEmpty());QCOMPARE(f.derived().size(),3);QCOMPARE(restartedPi.size(),2);
    QCOMPARE(f.recordById(computed.operationId).computeCount,1);QCOMPARE(model.responseIds.size(),model.requests);QVERIFY2(model.error.isEmpty(),qPrintable(model.error));
    QSet<QString> calls;for(const auto &id:model.providerCalls){QVERIFY(!calls.contains(id));calls.insert(id);}
  }
};
QTEST_GUILESS_MAIN(TestPiGeneratedOutputsNative)
#include "tst_pigeneratedoutputs_native.moc"
