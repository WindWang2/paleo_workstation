// 层：测试壳
#pragma once
#include "pythonnumerictablehostfixture.h"
#include "app/pythonanalysiswiring.h"
#include "services/projectquerycapability.h"
#include "services/pythonenvironmentregistry.h"
#include "services/pythongenerated.h"
#include "workflow/pythonenvironmentoperation.h"
#include "workflow/pythongeneratedoperation.h"
#include <QEventLoop>
#include <QFutureWatcher>
#include <QTimer>
#include <QtConcurrent>

// Shared by opt-in business acceptance suites. All source data/Catalog mappings
// and managed identities are real; no fake runner or script result is installed.
namespace numeric_table_native_fixture {
using namespace paleo;
template<class T> T completed(QFuture<T> future) {
  QFutureWatcher<T> watcher;QEventLoop loop;
  QObject::connect(&watcher,&QFutureWatcherBase::finished,&loop,&QEventLoop::quit);
  watcher.setFuture(future);if(!watcher.isFinished())loop.exec();return watcher.result();
}
struct Reply {capability::ExecuteResult result;QString error;};
struct Fixture:numeric_table_host_fixture::Fixture {
  std::shared_ptr<python::PythonEnvironmentRegistry> registry;
  python::EnvironmentSelection original;
  python::EnvironmentCandidate candidate;
  QJsonObject legacySelection;
  QString configPath()const{return temp.filePath("python-environment.json");}
  bool activateRuntime() {
    candidate.runtimeRoot=qEnvironmentVariable("PALEO_TEST_AGENT_PYTHON_ROOT");
    candidate.runtimeSha256=qEnvironmentVariable("PALEO_TEST_AGENT_PYTHON_MANIFEST_SHA256");
    candidate.policy=numeric_table_fixture::policy();candidate.policy.cpuSeconds=59;
    if(candidate.runtimeRoot.isEmpty()||candidate.runtimeSha256.size()!=64){error="explicit locked runtime fixture required";return false;}
    registry=std::make_shared<python::PythonEnvironmentRegistry>(configPath());
    const auto activated=completed(QtConcurrent::run([this]{return registry->activate(candidate,0);}));
    if(!activated.ok()){error=activated.code;return false;}original=registry->active(python::EnvironmentMode::Generated);
    const auto proof=completed(QtConcurrent::run([this]{return registry->probe(original.identity.id,python::EnvironmentMode::Generated);}));
    if(!original.ok()||!proof.value("ready").toBool()||!proof.value("runtimeProbeVerified").toBool()) {
      error=QString::fromUtf8(QJsonDocument(proof).toJson(QJsonDocument::Compact));return false;
    }
    return !original.runtime->policy().allowTrusted;
  }
  void configureManaged() {
    host->registerExecutionAdapter(QLatin1String(python::NumericTableCapability),python::environmentNumericTableAdapter(registry,
      assemblePythonNumericTableProvider(&catalog,&session,host.get())));
    host->registerExecutionAdapter(QLatin1String(python::GeneratedCapability),python::environmentAnalysisAdapter(python::AnalysisKind::Generated,registry,
      assemblePythonAnalysisProvider(&catalog,&session,host.get())));
  }
  void configureLegacyBare() {
    host->registerExecutionAdapter(QLatin1String(python::GeneratedCapability),python::generatedScriptAdapter(original.runtime,original.sandbox,
      assemblePythonAnalysisProvider(&catalog,&session,host.get())));
  }
  bool selectLegacyCurve() {
    std::string detail;data::ModelChangeHead head;std::vector<data::DataEntry> curves;bool more=false;
    data::MetadataReadOptions budget;data::MetadataReadStats stats;
    if(!table.sdk.readModelChangeHead(head,detail)||!table.sdk.listDataPage("curves",0,0,1,curves,more,detail,budget,&stats)||more||curves.size()!=1)return false;
    document::WellRef well;well.documentId=session.documentId();well.modelStoreId="area-model";
    well.modelStoreIdentity=QString::fromStdString(head.storeId);well.modelGeneration=QString::fromStdString(head.generation);
    well.modelWellId=curves.front().wellId;well.queryHead=head.sequence;well.modelAvailable=true;
    for(const auto &entity:catalog.entities("well"))if(entity.extra.value("areaModelWellId").toLongLong()==well.modelWellId)well.catalogEntityId=entity.id;
    if(!well.valid())return false;query::ProjectQueryCapability query;query.bind(&catalog,{session.documentId(),1,0,"native-legacy"});
    query.setSourceResolver([this](const auto &ref){return session.source(ref);});
    const auto page=query.query({{"topic","well_curves"},{"well_ref",document::wellRefJson(well)},{"max_bytes",65536}});
    if(!page.value("ok").toBool()||page.value("curves").toArray().size()!=1){error=QString::fromUtf8(QJsonDocument(page).toJson());return false;}
    const auto curve=page.value("curves").toArray().first().toObject();if(!curve.value("metadataReady").toBool())return false;
    legacySelection={{"well_ref",document::wellRefJson(well)},{"curves",QJsonArray{curve.value("input_ref")}},{"first_sample","0"},
      {"sample_count",3},{"processed",true},{"alignment","exact_shared_axis_v1"},{"sampling",QJsonObject{{"method","all_selected_window_v1"},{"seed",0}}}};
    return true;
  }
  QJsonObject arguments(const QString &id,const QByteArray &code=numeric_table_fixture::sourceCode(),bool legacy=false)const {
    return {{"client_request_id",id},{"binding_mode","pinned-value"},{"parameters",QJsonObject{{"code",QString::fromUtf8(code)}}},
      {"inputs",legacy?legacySelection:python::numericTableSelectionJson(table.plan.selection)},{"publish_intent","compute-only"}};
  }
  QJsonObject publication(const QString &id,const QString &operation)const {
    return {{"client_request_id",id},{"binding_mode","pinned-value"},{"parameters",QJsonObject{}},
      {"inputs",QJsonObject{{"source_operation_id",operation}}},{"publish_intent","publish"}};
  }
  Reply call(const QString &tool,const QJsonObject &arguments) {
    Reply out;capability::ExecuteRequest request;
    out.error=capability::prepareHostedChatToolRequest(request,tool,arguments).value("code").toString();
    if(!out.error.isEmpty()){out.result.code=out.error;return out;}
    const auto prepared=host->prepare(request,host::Caller::Agent);
    if(!prepared.ok){out.error=prepared.code;out.result.code=prepared.code;return out;}
    QObject subscriber;QEventLoop loop;QTimer timer;timer.setSingleShot(true);bool done=false;
    QObject::connect(&timer,&QTimer::timeout,&loop,&QEventLoop::quit);
    auto *task=host->start(prepared.handle,request,&subscriber,"Numeric table business acceptance",
      [&](capability::ExecuteResult value){out.result=std::move(value);done=true;loop.quit();},&out.error);
    if(!task){out.result.code=out.error;return out;}timer.start(180000);if(!done)loop.exec();
    if(!done){task->requestCancel();out.error="test_timeout";out.result.code=out.error;}return out;
  }
  capability::OperationLog::Record record(const QString &id)const {
    capability::OperationLog log(project+"/.paleo/capability-ops",project,lock->claim());
    return log.lookup(context.documentId,table.plan.callerScope,id).record;
  }
  QJsonObject page(const QString &operation,qint64 offset,const QString &port="REPORT") {
    QJsonObject out;QObject subscriber;QEventLoop loop;QTimer timer;timer.setSingleShot(true);bool done=false;QString why;
    QObject::connect(&timer,&QTimer::timeout,&loop,&QEventLoop::quit);
    auto *task=host->recovery()->read(table.plan.callerScope,operation,port,offset,128,&subscriber,
      [&](QJsonObject value){out=value;done=true;loop.quit();},&why);
    if(!task)return {{"ok",false},{"code",why}};timer.start(180000);if(!done)loop.exec();
    if(!done){task->requestCancel();return {{"ok",false},{"code","test_timeout"}};}return out;
  }
  QJsonObject report(const QString &operation) {
    QByteArray bytes;qint64 offset=0;
    for(int i=0;i<2049;++i) {
      const auto result=page(operation,offset);if(!result.value("ok").toBool()){error=result.value("code").toString();return {};}
      const auto chunk=QByteArray::fromBase64(result.value("data").toString().toLatin1());
      if(chunk.isEmpty()||chunk.size()>128||bytes.size()+chunk.size()>256*1024){error="invalid report page";return {};}bytes+=chunk;
      if(!result.value("hasMore").toBool())return QJsonDocument::fromJson(bytes).object();
      const auto next=result.value("nextOffset").toString().toLongLong();if(next<=offset){error="invalid report continuation";return {};}offset=next;
    }
    error="report page budget exceeded";return {};
  }
  bool restartWithAnotherActivePolicy() {
    // The new permission ceiling must still authorize the historical policy.
    // A 60 -> 59 change would correctly deny an old 60-second operation.
    auto changed=candidate;changed.policy.cpuSeconds++;
    const auto activated=completed(QtConcurrent::run([this,changed]{return registry->activate(changed,1);}));
    if(!activated.ok()||activated.configuration.activeId==original.identity.id){error=activated.code;return false;}
    host.reset();original={};registry.reset();registry=std::make_shared<python::PythonEnvironmentRegistry>(configPath());
    const auto revision=registry->beginRefresh();const auto code=completed(QtConcurrent::run([this,revision]{return registry->refresh({},revision,{},true);}));
    if(!code.isEmpty()){error=code;return false;}resetHost();configureManaged();return true;
  }
  QVector<CatalogVersion> derived()const {
    QVector<CatalogVersion> out;for(const auto &version:catalog.versions())if(version.stage=="DERIVED")out.append(version);return out;
  }
};
inline QByteArray legacyCode() {
  return R"PY(import sys, struct, json, numpy as np
n=struct.unpack('<I',sys.stdin.buffer.read(4))[0]
m=json.loads(sys.stdin.buffer.read(n));raw=sys.stdin.buffer.read();r,c=m['shape']
v=np.frombuffer(raw,dtype='<f8',count=r*c,offset=r*8).reshape(r,c)
print(json.dumps({'contract':'paleo.python-output.v1','ports':{'REPORT':{'contract':'paleo.python-generated-report.v1','metrics':[{'id':'legacy_mean','unit':m['columns'][0]['unit'],'value':float(np.mean(v[:,0]))}],'tables':[],'notes':['Legacy v1 selected window.']}}},allow_nan=False))
)PY";
}
}
