// 层：测试壳
#pragma once
#include "services/pythonnumerictable.h"
#include <paleo_data/service.h>
#include <QCryptographicHash>
#include <QDir>
#include <QFile>
#include <QJsonArray>
#include <QJsonDocument>
#include <QTemporaryDir>
#include <QUuid>
namespace numeric_table_fixture {
using namespace paleo;
struct Model {
  QTemporaryDir temp;
  QString project,model,error;
  data::DataService sdk;
  data::NumericTableProjection declaration;
  data::NumericTableDescriptor descriptor;
  python::NumericTableReadPlan plan;
  bool open(const QString &projectDirectory={},const QString &documentId="numeric-doc",int unusedBytes=0,
            const std::string &firstValue="-2.5",const std::string &lastBoolean="1",bool declared=true) {
    project=projectDirectory.isEmpty()?temp.filePath("project"):projectDirectory;
    model=project+"/artifacts/metadata/area-model";std::string detail;QDir().mkpath(project);
    if(!sdk.createProject(model.toUtf8().toStdString(),"Explicit numeric table fixture",detail)){error=QString::fromStdString(detail);return false;}
    const QByteArray raw="Explicit synthetic numeric projection fixture\n";QFile file(project+"/table.data");
    if(!file.open(QIODevice::WriteOnly)||file.write(raw)!=raw.size())return false;file.close();
    const auto sha=QCryptographicHash::hash(raw,QCryptographicHash::Sha256).toHex();
    data::ParsedFile parsed;parsed.kind="text";parsed.status="parsed";data::SourceTable table;
    table.sheetName="same name";table.rowCount=4;table.columnCount=3;
    table.cells={{1,1,firstValue},{2,1,"0"},{3,1,"-999.25"},{4,1,"4.5"},{1,2,"1"},{2,2,"0"},{4,2,lastBoolean},{1,3,std::string(size_t(unusedBytes),'x')}};
    parsed.sourceTables={table,table};long long sourceId=0;
    if(!sdk.importPreparedFile(file.fileName().toUtf8().toStdString(),file.fileName().toUtf8().toStdString(),
        file.fileName().toUtf8().toStdString(),sha.toStdString(),parsed,sourceId,detail)){error=QString::fromStdString(detail);return false;}
    std::vector<data::DataEntry> tables;
    if(!sdk.listSourceData("tables",sourceId,tables,detail)||tables.size()!=2)return false;
    declaration.tableId=tables.back().id;declaration.sourceId=sourceId;declaration.sourceSha256=sha.toStdString();declaration.rowCount=4;
    data::NumericTableColumn number;number.id="amplitude";number.label="Amplitude";number.unit="mV";number.sourceColumn=1;number.nullTexts={"-999.25"};
    data::NumericTableColumn boolean;boolean.id="accepted";boolean.label="Accepted";boolean.unit="1";boolean.sourceColumn=2;boolean.dtype=data::NumericTableDtype::Boolean;
    declaration.columns={number,boolean};data::ModelChangeHead head;std::string revision;
    if(declared) {
      if(!sdk.upgradeNumericTableProjectionSchema(detail)||!sdk.readModelChangeHead(head,detail)||
          !sdk.confirmNumericTableProjection(declaration,head,{},revision,detail)||
          !sdk.describeNumericTableProjection(declaration.tableId,descriptor,detail)){error=QString::fromStdString(detail);return false;}
    } else revision=QUuid::createUuid().toString(QUuid::WithoutBraces).toStdString();
    plan.projectDir=project;plan.modelDirectory=model;plan.documentId=documentId;plan.callerScope="agent:numeric-table";
    plan.selection.source={documentId,"area-model",sourceId,QString::fromLatin1(sha)};plan.selection.tableId=declaration.tableId;
    plan.selection.projectionRevision=QString::fromStdString(revision);plan.selection.columnIds={"amplitude","accepted"};plan.selection.rowCount=4;
    plan.source.ref=plan.selection.source;plan.source.fingerprint=document::SourceFingerprint::fromStored(plan.selection.source.fileRevisionId);
    plan.source.modelDirectory=model;plan.source.assetId="fixture-source-asset";plan.source.versionId="fixture-source-version";plan.source.path=file.fileName();
    return true;
  }
};
inline python::ExecutionPolicy policy(){python::ExecutionPolicy out;out.allowGenerated=true;return out;}
inline QByteArray sourceCode() {
  return R"PY(import sys, struct, json, numpy as np
n=struct.unpack('<I',sys.stdin.buffer.read(4))[0]
m=json.loads(sys.stdin.buffer.read(n)); raw=sys.stdin.buffer.read()
axis=np.frombuffer(raw,dtype='<i8',count=m['shape'][0],offset=0)
metrics=[]
for c in m['columns']:
 v=np.frombuffer(raw,dtype=c['dtype'],count=m['shape'][0],offset=c['valueOffset'])
 null=np.frombuffer(raw,dtype='u1',count=m['shape'][0],offset=c['nullOffset'])
 x=v[null==0]
 metrics.append({'id':'mean_'+c['id'],'unit':c['unit'],'value':float(np.mean(x)) if x.size else None})
print(json.dumps({'contract':'paleo.python-output.v1','ports':{'REPORT':{'contract':'paleo.python-generated-report.v1','metrics':metrics,'tables':[],'notes':['Declared projection; selected rows only.']}}},allow_nan=False))
)PY";
}
inline QByteArray bytes(const python::InputBundle &bundle,const QString &name) {
  QFile file(bundle.root+'/'+name);return file.open(QIODevice::ReadOnly)?file.readAll():QByteArray{};
}
}
