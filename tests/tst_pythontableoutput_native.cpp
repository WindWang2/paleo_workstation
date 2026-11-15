// 层：测试壳
#include "fixtures/pythonnumerictablenativefixture.h"
#include "services/pythontableoutputspec.h"
#include "workflow/pythontableoutputoperation.h"
#include <QFileInfo>
#include <QtTest>
using namespace paleo;
namespace {
QJsonObject declaration() {
  const auto column=[](const char *id,const char *label,const char *unit,const char *dtype,const char *role) {
    return QJsonObject{{"id",id},{"label",label},{"unit",unit},{"dtype",dtype},{"role",role}};
  };
  return {{"contract","paleo.python-table-declaration.v1"},{"maxRows",4},{"columns",QJsonArray{
    column("source_row","Physical row","1","<i8","source_row"),column("amplitude","Amplitude","mV","<f8","value"),
    column("accepted","Accepted","1","|b1","value")}}};
}
QByteArray sourceCode(const QByteArray &mode="valid") {
  QByteArray source="MODE="+QByteArray("'")+mode+"'\n";
  source+=R"PY(import sys, struct, json, hashlib, numpy as np
n=struct.unpack('<I',sys.stdin.buffer.read(4))[0]
m=json.loads(sys.stdin.buffer.read(n));raw=sys.stdin.buffer.read();r=m['shape'][0]
columns=[{'id':'source_row','label':'Physical row','unit':'1','dtype':'<i8','role':'source_row'},
 {'id':'amplitude','label':'Amplitude','unit':'mV','dtype':'<f8','role':'value'},
 {'id':'accepted','label':'Accepted','unit':'1','dtype':'|b1','role':'value'}]
axis=np.frombuffer(raw,dtype='<i8',count=r)
data=axis.tobytes()+bytes((r+7)//8)
for c in m['columns']:
 v=np.frombuffer(raw,dtype=c['dtype'],count=r,offset=c['valueOffset']).copy()
 null=np.frombuffer(raw,dtype='u1',count=r,offset=c['nullOffset'])
 if MODE=='nonfinite' and c['dtype']=='<f8':v[0]=float('nan')
 mask=bytearray((r+7)//8)
 for i in range(r):
  if null[i]:v[i]=0;mask[i//8]|=1<<(i%8)
 data+=v.tobytes()+mask
report=json.dumps({'contract':'paleo.python-generated-report.v1','metrics':[],
 'tables':[],'notes':['Real SDK values; explicit nulls.']},separators=(',',':')).encode()
ports=[{'id':'REPORT','encoding':'utf8-json','size':len(report),'sha256':hashlib.sha256(report).hexdigest()},
 {'id':'TABLE','encoding':'paleo.numeric-columns.v1','size':len(data),'sha256':hashlib.sha256(data).hexdigest(),'rows':r,'columns':columns}]
if MODE=='wrong-port':ports[1]['id']='IMAGE'
header=json.dumps({'contract':'paleo.python-output-frame.v2','ports':ports},separators=(',',':')).encode()
packet=b'PALEOPYO2\n'+struct.pack('<I',len(header))+header+report+data
if MODE=='truncated':packet=packet[:-1]
if MODE=='flood':packet=b'x'*(13*1024*1024)
sys.stdout.buffer.write(packet)
)PY";
  return source;
}
struct Fixture:numeric_table_native_fixture::Fixture {
  void configureOutputs(bool breakTableWrite=false) {
    auto adapter=python::environmentTableOutputsAdapter(registry,assemblePythonNumericTableProvider(&catalog,&session,host.get()));
    if(breakTableWrite) {
      const auto capture=adapter.capture;
      adapter.capture=[capture](const auto &request,const auto &intent,QString *error) {
        auto preparation=capture(request,intent,error);const auto inner=preparation.worker;
        if(inner)preparation.worker=[inner](const auto &cancelled,QString *error) {
          auto frozen=inner(cancelled,error);const auto worker=frozen.worker;
          if(worker)frozen.worker=[worker](const auto &context) {
            QDir().mkpath(QDir(context.privateDir).filePath("TABLE.csv"));return worker(context);
          };return frozen;
        };return preparation;
      };
    }
    host->registerExecutionAdapter(QLatin1String(python::TableOutputsCapability),std::move(adapter));
  }
  QJsonObject outputArguments(const QString &id,const QByteArray &code=sourceCode())const {
    auto args=arguments(id,code);auto parameters=args.value("parameters").toObject();parameters["output_table"]=declaration();args["parameters"]=parameters;return args;
  }
  QByteArray tableBytes(const QString &operation) {
    QByteArray bytes;qint64 offset=0;
    for(int i=0;i<32769;++i) {
      const auto chunk=page(operation,offset,"TABLE");if(!chunk.value("ok").toBool()){error=chunk.value("code").toString();return {};}
      const auto data=QByteArray::fromBase64(chunk.value("data").toString().toLatin1());
      if(data.isEmpty()||data.size()>128||bytes.size()+data.size()>4*1024*1024){error="invalid table page";return {};}
      bytes+=data;if(!chunk.value("hasMore").toBool())return bytes;
      const auto next=chunk.value("nextOffset").toString().toLongLong();if(next<=offset){error="invalid table continuation";return {};}offset=next;
    }
    error="table page budget exceeded";return {};
  }
};
}
class TestPythonTableOutputNative:public QObject {
  Q_OBJECT
private slots:
  void realTableGroupPagesHistoricalRestartAndOnePublication() {
    Fixture f;QVERIFY2(f.open(),qPrintable(f.error));QVERIFY2(f.activateRuntime(),qPrintable(f.error));f.configureOutputs();
    auto prepare=f.outputArguments("typed-prepare");prepare["publish_intent"]="prepare";
    const auto prepared=f.call(QLatin1String(python::TableOutputsTool),prepare);QVERIFY2(prepared.result.ok,qPrintable(prepared.result.code));
    QCOMPARE(f.record("typed-prepare").computeCount,0);QVERIFY(f.derived().isEmpty());
    const auto computed=f.call(QLatin1String(python::TableOutputsTool),f.outputArguments("typed-compute"));
    QVERIFY2(computed.result.ok,qPrintable(computed.result.code));QVERIFY(f.derived().isEmpty());
    const auto saved=f.record("typed-compute");QCOMPARE(saved.computeCount,1);
    const auto plan=saved.frozenRequest.value("plan").toObject().value("plan").toObject();
    QCOMPARE(plan.value("schema").toString(),QString("paleo.python-table-outputs-operation.v1"));QCOMPARE(plan.value("outputTable"),QJsonValue(declaration()));
    const auto report=f.report(saved.operationId);QVERIFY2(!report.isEmpty(),qPrintable(f.error));
    QCOMPARE(report.value("outputProvenance").toObject().value("tableDeclaration"),QJsonValue(declaration()));
    const auto csv=f.tableBytes(saved.operationId);QVERIFY2(!csv.isEmpty(),qPrintable(f.error));
    QCOMPARE(csv,QByteArray("source_row,amplitude,accepted\r\n1,-2.5,true\r\n2,0,false\r\n3,,\r\n4,4.5,true\r\n"));
    const auto summary=QJsonDocument::fromJson(saved.summaryJson).object();
    QCOMPARE(summary.value("hostStdoutLimitBytes").toString(),QString::number(12*1024*1024));
    QVERIFY(summary.value("hostOutputReservedBytes").toString().toLongLong()>0);
    const auto replay=f.call(QLatin1String(python::TableOutputsTool),f.outputArguments("typed-compute"));
    QVERIFY(replay.result.ok);QCOMPARE(replay.result.operationId,saved.operationId);QCOMPARE(f.record("typed-compute").computeCount,1);
    auto changed=f.outputArguments("typed-compute");auto parameters=changed.value("parameters").toObject();auto columns=declaration();columns["maxRows"]=3;
    parameters["output_table"]=columns;changed["parameters"]=parameters;
    QCOMPARE(f.call(QLatin1String(python::TableOutputsTool),changed).result.code,QString("idempotency_conflict"));
    auto projection=f.table.declaration;projection.columns[0].unit="uV";data::ModelChangeHead head;std::string detail,revision;
    QVERIFY(f.table.sdk.readModelChangeHead(head,detail));QVERIFY(f.table.sdk.confirmNumericTableProjection(projection,head,f.table.descriptor.revision,revision,detail));
    QVERIFY2(f.restartWithAnotherActivePolicy(),qPrintable(f.error));f.configureOutputs();
    const auto published=f.call(QLatin1String(python::TableOutputsTool),f.publication("typed-publish",saved.operationId));
    QVERIFY2(published.result.ok,qPrintable(published.result.code));QCOMPARE(published.result.commitState,QString("committed"));
    QCOMPARE(f.derived().size(),2);for(const auto &version:f.derived())QCOMPARE(version.parentVersionIds,QStringList{f.table.plan.source.versionId});
    QCOMPARE(f.record("typed-publish").computeCount,0);QCOMPARE(f.record("typed-compute").computeCount,1);
    QCOMPARE(f.tableBytes(saved.operationId),csv);QCOMPARE(f.report(saved.operationId),report);
    QVERIFY(f.call(QLatin1String(python::TableOutputsTool),f.publication("typed-publish",saved.operationId)).result.ok);QCOMPARE(f.derived().size(),2);
  }
  void invalidOrIncompleteGroupCannotPublish_data() {
    QTest::addColumn<QByteArray>("mode");QTest::addColumn<bool>("breakWrite");
    QTest::newRow("truncated")<<QByteArray("truncated")<<false;QTest::newRow("wrong-port")<<QByteArray("wrong-port")<<false;
    QTest::newRow("nonfinite")<<QByteArray("nonfinite")<<false;QTest::newRow("pipe-flood")<<QByteArray("flood")<<false;
    QTest::newRow("second-private-write-fails")<<QByteArray("valid")<<true;
  }
  void invalidOrIncompleteGroupCannotPublish() {
    QFETCH(QByteArray,mode);QFETCH(bool,breakWrite);Fixture f;QVERIFY2(f.open(),qPrintable(f.error));
    QVERIFY2(f.activateRuntime(),qPrintable(f.error));f.configureOutputs(breakWrite);
    const auto response=f.call(QLatin1String(python::TableOutputsTool),f.outputArguments("invalid-group",sourceCode(mode)));
    QVERIFY(!response.result.ok);QVERIFY(f.derived().isEmpty());const auto saved=f.record("invalid-group");
    QVERIFY(saved.artifactManifest.value("diagnosticsOnly").toBool());
    for(const auto &entry:saved.artifactManifest.value("ports").toArray()) {
      const auto port=entry.toObject().value("portId").toString();QVERIFY(port!="REPORT"&&port!="TABLE");
    }
    const auto published=f.call(QLatin1String(python::TableOutputsTool),f.publication("invalid-publish",saved.operationId));
    QVERIFY(!published.result.ok);QVERIFY(f.derived().isEmpty());
  }
  void sameLengthTableTamperFailsWarmAuthenticatedRange() {
    Fixture f;QVERIFY2(f.open(),qPrintable(f.error));QVERIFY2(f.activateRuntime(),qPrintable(f.error));f.configureOutputs();
    const auto computed=f.call(QLatin1String(python::TableOutputsTool),f.outputArguments("tamper"));QVERIFY2(computed.result.ok,qPrintable(computed.result.code));
    const auto saved=f.record("tamper");QVERIFY(f.page(saved.operationId,0,"TABLE").value("ok").toBool());QString path;
    for(const auto &entry:saved.artifactManifest.value("ports").toArray())if(entry.toObject().value("portId")=="TABLE")path=entry.toObject().value("path").toString();
    QVERIFY(!path.isEmpty());QFile file(QDir(f.project).filePath(path));const auto previous=QFileInfo(file).lastModified();
    QVERIFY(file.open(QIODevice::ReadWrite));QCOMPARE(file.write("!"),1);QVERIFY(file.setFileTime(previous,QFileDevice::FileModificationTime));file.close();
    const auto altered=f.page(saved.operationId,1,"TABLE");QVERIFY(!altered.value("ok").toBool());QVERIFY(altered.value("data").toString().isEmpty());QVERIFY(f.derived().isEmpty());
  }
};
QTEST_GUILESS_MAIN(TestPythonTableOutputNative)
#include "tst_pythontableoutput_native.moc"
