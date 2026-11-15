// 层：测试壳
#pragma once
#include "pythonseismictracehostfixture.h"
#include "services/pythongeneratedoutputspec.h"
#include "workflow/pythongeneratedoutputoperation.h"
#include <QThread>
#include <atomic>
namespace generated_outputs_fixture {
using namespace paleo;
inline QJsonObject tableDeclaration(const QString &kind="seismic_trace") {
  const auto column=[](const char *id,const char *label,const char *unit,const char *dtype,const char *role) {
    return QJsonObject{{"id",id},{"label",label},{"unit",unit},{"dtype",dtype},{"role",role}};
  };
  const bool table=kind=="numeric_table";
  return {{"contract","paleo.python-table-declaration.v1"},{"maxRows",table?4:3},{"columns",table?QJsonArray{
    column("source_row","Physical source row","1","<i8","source_row"),column("amplitude","Amplitude","mV","<f8","value"),
    column("accepted","Accepted","1","|b1","value")}:QJsonArray{
    column("time","Declared trace time","ms","<f8","axis"),column("amplitude","Amplitude","relative_amplitude","<f8","value"),
    column("present","Present","1","|b1","value")}}};
}
inline QJsonObject imageDeclaration(const QString &kind="seismic_trace",bool pngBudget=false) {
  return {{"contract","paleo.python-image-declaration.v1"},{"width",pngBudget?1024:kind=="numeric_table"?4:3},
    {"height",pngBudget?1024:1},{"format","rgba8"}};
}
inline QByteArray sourceCode(const QByteArray &mode="valid",const QString &kind="seismic_trace") {
  QByteArray source="import sys, struct, json, hashlib, numpy as np\nMODE='"+mode+"'\nCOLUMNS=";
  source+=QJsonDocument(tableDeclaration(kind).value("columns").toArray()).toJson(QJsonDocument::Compact)+"\n";
  source+=R"PY(n=struct.unpack('<I',sys.stdin.buffer.read(4))[0]
m=json.loads(sys.stdin.buffer.read(n));raw=sys.stdin.buffer.read();r=m['shape'][0]
axis=np.frombuffer(raw,dtype=m['axis']['dtype'],count=r,offset=m['axis']['valueOffset'])
zero=np.zeros(r,dtype='u1')
if m['analysis']=='generated_numeric_table':
 c=m['columns'][0];v=np.frombuffer(raw,dtype=c['dtype'],count=r,offset=c['valueOffset'])
 null=np.frombuffer(raw,dtype='u1',count=r,offset=c['nullOffset'])
 c=m['columns'][1];b=np.frombuffer(raw,dtype=c['dtype'],count=r,offset=c['valueOffset'])
 bn=np.frombuffer(raw,dtype='u1',count=r,offset=c['nullOffset'])
 values=[axis,v,b];masks=[zero,null,bn]
else:
 v=np.frombuffer(raw,dtype='<f8',count=r,offset=r*8)
 null=np.frombuffer(raw,dtype='u1',count=r,offset=r*16)
 values=[axis,v,(null==0).astype('u1')];masks=[zero,null,zero]
data=b''
for value,mask,c in zip(values,masks,COLUMNS):
 a=np.asarray(value,dtype=c['dtype']).copy();a[mask!=0]=0
 if MODE=='nonfinite-table' and c['id']=='amplitude':a[0]=float('nan')
 data+=a.tobytes()+np.packbits(mask,bitorder='little').tobytes()
x=v[null==0]
body={'contract':'paleo.python-generated-report.v1','metrics':[{'id':'window_mean','unit':m['columns'][0]['unit'],'value':float(np.mean(x)) if x.size else None}],
 'tables':[],'notes':['Selected authenticated values only; RGBA pixels are not a GIS layer.']}
if MODE=='bad-report':body={'contract':'wrong-report'}
report=json.dumps(body,separators=(',',':'),allow_nan=False).encode()
if MODE=='png-budget':
 width=1024;height=1024;rgba=np.random.default_rng(237).integers(0,256,(height,width,4),dtype='u1').tobytes()
else:
 width=r;height=1;pixels=np.zeros((r,4),dtype='u1')
 pixels[:,0]=np.clip(v,0,255).astype('u1');pixels[:,1]=np.clip(axis,0,255).astype('u1')
 pixels[:,2]=null*255;pixels[:,3]=np.linspace(0,255,r,dtype='u1');rgba=pixels.tobytes()
def port(name,encoding,value):
 return {'id':name,'encoding':encoding,'size':len(value),'sha256':hashlib.sha256(value).hexdigest()}
ports=[port('REPORT','utf8-json',report),port('TABLE','paleo.numeric-columns.v1',data),port('IMAGE','rgba8',rgba)]
ports[1]['rows']=r;ports[1]['columns']=COLUMNS;ports[2]['width']=width;ports[2]['height']=height
if MODE=='wrong-image-port':ports[2]['id']='TABLE'
if MODE=='bad-image-sha':ports[2]['sha256']='0'*64
header=json.dumps({'contract':'paleo.python-output-frame.v3','ports':ports},separators=(',',':')).encode()
packet=b'PALEOPYO3\n'+struct.pack('<I',len(header))+header+report+data+rgba
if MODE=='truncated':packet=packet[:-1]
sys.stdout.buffer.write(packet)
)PY";
  return source;
}
inline QByteArray expectedRgba(const QString &kind="seismic_trace") {
  return QByteArray::fromHex(kind=="numeric_table"?"00010000000200550003ffaa040400ff":"090200000a04007f0b0600ff");
}
inline QByteArray expectedCsv(const QString &kind="seismic_trace") {
  return kind=="numeric_table"?QByteArray("source_row,amplitude,accepted\r\n1,-2.5,true\r\n2,0,false\r\n3,,\r\n4,4.5,true\r\n"):
    QByteArray("time,amplitude,present\r\n2,9,true\r\n4,10,true\r\n6,11,true\r\n");
}
struct Counts {std::atomic_int materializations{0},workerInvocations{0};std::atomic_bool ownerOnly{true},workerOffOwner{false};};
struct Fixture:seismic_trace_host_fixture::Fixture {
  std::shared_ptr<Counts> counts=std::make_shared<Counts>();
  void configureOutputs(bool failImageWrite=false) {
    auto adapter=python::environmentGeneratedOutputsAdapter(registry,assemblePythonNumericTableProvider(&catalog,&session,host.get()),
      assemblePythonSeismicTraceProvider(&catalog,&session,host.get()));
    const auto capture=adapter.capture;const auto counters=counts;const auto owner=QThread::currentThread();
    adapter.capture=[capture,counters,owner,failImageWrite](const auto &request,const auto &intent,QString *error) {
      if(QThread::currentThread()!=owner)counters->ownerOnly=false;
      auto prepared=capture(request,intent,error);const auto materialize=prepared.worker;
      if(materialize)prepared.worker=[materialize,counters,owner,failImageWrite](const auto &cancelled,QString *error) {
        ++counters->materializations;counters->workerOffOwner=QThread::currentThread()!=owner;
        auto operation=materialize(cancelled,error);const auto worker=operation.worker;
        if(worker)operation.worker=[worker,counters,failImageWrite](const auto &context) {
          ++counters->workerInvocations;
          if(failImageWrite)QDir().mkpath(QDir(context.privateDir).filePath("IMAGE.png"));
          return worker(context);
        };return operation;
      };return prepared;
    };
    host->registerExecutionAdapter(QLatin1String(python::GeneratedOutputsCapability),std::move(adapter));
  }
  QJsonObject outputArguments(const QString &id,const QByteArray &mode="valid",const QString &kind="seismic_trace")const {
    auto result=kind=="numeric_table"?arguments(id,sourceCode(mode,kind)):traceArguments(id,sourceCode(mode,kind));
    auto inputs=result.value("inputs").toObject();inputs["input_kind"]=kind;result["inputs"]=inputs;
    auto parameters=result.value("parameters").toObject();parameters["output_table"]=tableDeclaration(kind);
    parameters["output_image"]=imageDeclaration(kind,mode=="png-budget");result["parameters"]=parameters;return result;
  }
  QJsonObject port(const capability::OperationLog::Record &record,const QString &id)const {
    for(const auto &entry:record.artifactManifest.value("ports").toArray())if(entry.toObject().value("portId")==id)return entry.toObject();return {};
  }
  capability::OperationLog::Record recordById(const QString &id)const {
    capability::OperationLog journal(project+"/.paleo/capability-ops",project,lock->claim());return journal.recordById(id);
  }
  QJsonObject pageForScope(const QString &operation,const QString &id,qint64 offset,const QString &scope={}) {
    if(scope.isEmpty())return page(operation,offset,id);
    QJsonObject result;QObject subscriber;QEventLoop loop;QTimer timer;timer.setSingleShot(true);bool done=false;QString why;
    QObject::connect(&timer,&QTimer::timeout,&loop,&QEventLoop::quit);
    auto *task=host->recovery()->read(scope,operation,id,offset,128,&subscriber,[&](QJsonObject value){result=value;done=true;loop.quit();},&why);
    if(!task)return {{"ok",false},{"code",why}};timer.start(180000);if(!done)loop.exec();
    if(!done){task->requestInterruption();return {{"ok",false},{"code","test_timeout"}};}return result;
  }
  QByteArray bytesFor(const QString &operation,const QString &id,const QString &scope={}) {
    const qint64 maximum=id=="REPORT"?256*1024:4*1024*1024;QByteArray bytes;qint64 offset=0;
    for(qint64 index=0;index<maximum/128+1;++index) {
      const auto result=pageForScope(operation,id,offset,scope);if(!result.value("ok").toBool()){error=result.value("code").toString();return {};}
      const auto chunk=QByteArray::fromBase64(result.value("data").toString().toLatin1());
      if(chunk.isEmpty()||chunk.size()>128||bytes.size()+chunk.size()>maximum){error="invalid bounded artifact page";return {};}bytes+=chunk;
      if(!result.value("hasMore").toBool())return bytes;
      const auto next=result.value("nextOffset").toString().toLongLong();if(next<=offset){error="invalid artifact continuation";return {};}offset=next;
    }
    error="artifact page limit";return {};
  }
};
}
