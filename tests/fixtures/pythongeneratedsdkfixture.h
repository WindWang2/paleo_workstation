// 层：测试壳
#pragma once
#include "services/pythonanalysisselection.h"
#include "catalog/datacatalog.h"
#include "metadata/projectlock.h"
#include <paleo_data/service.h>
#include <QCryptographicHash>
#include <QDir>
#include <QFile>
#include <QJsonArray>
#include <QJsonDocument>
#include <QTemporaryDir>
#include <memory>

namespace paleo::python::generated_sdk_fixture {
inline QByteArray sourceCode() {
  return R"PY(import sys, struct, json, numpy as np
n=struct.unpack('<I',sys.stdin.buffer.read(4))[0]
m=json.loads(sys.stdin.buffer.read(n))
r,c=m['shape'];raw=sys.stdin.buffer.read()
v=np.frombuffer(raw,dtype='<f8',count=r*c,offset=r*8).reshape(r,c)
q=np.frombuffer(raw,dtype='u1',count=r*c,offset=r*8+r*c*8).reshape(r,c)
metrics=[]
for i in range(c):
 x=v[:,i][np.isfinite(v[:,i]) & ((q[:,i]&3)==0)]
 metrics.append({'id':'mean_'+str(i),'unit':m['columns'][i]['unit'],'value':float(np.mean(x)) if x.size else None})
print(json.dumps({'contract':'paleo.python-output.v1','ports':{'REPORT':{'contract':'paleo.python-generated-report.v1','metrics':metrics,'tables':[],'notes':['Selected window means.']}}},allow_nan=False))
)PY";
}
// Real SDK typed windows and Catalog/source identities shared by generated and Pi acceptance.
struct Fixture {
  QTemporaryDir temp; QString project, model, error;
  data::DataService sdk; DataCatalog catalog; std::unique_ptr<ProjectDirLock> lock;
  AnalysisReadPlan plan; QJsonObject selection;
  bool open(const QString &callerScope) {
    project=temp.path()+"/project";model=project+"/artifacts/metadata/area-model";std::string why;
    if(!QDir().mkpath(project))return false;lock=std::make_unique<ProjectDirLock>(project);
    if(!lock->tryLock(&error) || !catalog.open(project,&error) || !sdk.createProject(model.toUtf8().toStdString(),"Generated fixture",why))return false;
    data::ParsedFile parsed;parsed.kind="logs";parsed.status="parsed";data::WellDataset well;well.wellInfo.wellName="GENERATED-1";
    data::LogSet logs;logs.logSetName="Declared shared MD";logs.depthInfo={"MD","m","KB","","",""};
    data::LogCurve a,b;a.curveName="A";a.curveUnit="API";b.curveName="B";b.curveUnit="m/s";
    for(int i=0;i<100;++i){logs.samplingAxis.depthValues.push_back(100+0.25*i);a.sampleValues.push_back(i);b.sampleValues.push_back(3*i+7);}
    logs.logCurves={a,b};well.wellbore.logSets={logs};parsed.wells={well};
    QFile input(project+"/source.data");const QByteArray bytes="Generated operation fixture\n";
    if(!input.open(QIODevice::WriteOnly) || input.write(bytes)!=bytes.size())return false;input.close();
    const auto sha=QCryptographicHash::hash(bytes,QCryptographicHash::Sha256).toHex();qlonglong sourceId=0;
    if(!sdk.importPreparedFile(input.fileName().toUtf8().toStdString(),input.fileName().toUtf8().toStdString(),input.fileName().toUtf8().toStdString(),sha.toStdString(),parsed,sourceId,why)){error=QString::fromStdString(why);return false;}
    CatalogAsset asset;asset.id="generated-source";asset.type="well_log";asset.format="data";asset.displayName="source.data";
    CatalogVersion raw;raw.id="generated-raw";raw.assetId=asset.id;raw.stage="RAW";raw.managed=false;raw.path=input.fileName();raw.fileName="source.data";raw.sha256=QString::fromLatin1(sha);
    if(!catalog.addAsset(asset,&error) || !catalog.addVersion(raw,&error))return false;
    std::vector<data::DataEntry> curves;data::MetadataReadOptions bounded;data::MetadataReadStats stats;bool more=false;
    if(!sdk.listDataPage("curves",0,0,16,curves,more,why,bounded,&stats) || more || curves.size()!=2)return false;
    plan.kind=python::AnalysisKind::Generated;plan.projectDir=project;plan.modelDirectory=model;plan.documentId="generated-doc";plan.callerScope=callerScope;
    if(!sdk.readModelChangeHead(plan.expectedHead,why) || !plan.expectedHead.available)return false;
    CatalogEntity entity;entity.id="generated-well";entity.entityType="well";entity.name="GENERATED-1";entity.extra["areaModelWellId"]=qlonglong(curves.front().wellId);
    if(!catalog.addEntity(entity,&error))return false;
    document::WellRef ref;ref.documentId=plan.documentId;ref.catalogEntityId=entity.id;ref.modelStoreId="area-model";
    ref.modelStoreIdentity=QString::fromStdString(plan.expectedHead.storeId);ref.modelGeneration=QString::fromStdString(plan.expectedHead.generation);
    ref.modelWellId=curves.front().wellId;ref.queryHead=plan.expectedHead.sequence;ref.modelAvailable=true;QJsonArray inputs;
    for(const auto &curve:curves) {
      python::AnalysisCurveSelection selected;selected.source.ref={plan.documentId,"area-model",sourceId,QString::fromLatin1(sha)};
      selected.source.fingerprint=document::SourceFingerprint::fromStored(selected.source.ref.fileRevisionId);
      selected.source.assetId=asset.id;selected.source.versionId=raw.id;selected.source.modelDirectory=model;selected.source.path=raw.path;
      selected.sourceVersionId=raw.id;selected.wellId=curve.wellId;selected.logSetId=curve.parentId;selected.curveId=curve.id;
      selected.sampleCount=100;selected.unit=curve.name=="A"?"API":"m/s";selected.depthType="MD";selected.depthUnit="m";selected.depthReference="KB";
      plan.curves.append(selected);inputs.append(QJsonObject{{"source_ref",document::sourceRefJson(selected.source.ref)},{"source_version_id",raw.id},
        {"well_id",QString::number(curve.wellId)},{"log_set_id",QString::number(curve.parentId)},{"curve_id",QString::number(curve.id)},
        {"unit",selected.unit},{"depth_type","MD"},{"depth_unit","m"},{"depth_reference","KB"},{"total_samples","100"}});
    }
    selection={{"well_ref",document::wellRefJson(ref)},{"curves",inputs},{"first_sample","0"},{"sample_count",100},{"processed",true},
      {"alignment","exact_shared_axis_v1"},{"sampling",QJsonObject{{"method","all_selected_window_v1"},{"seed",0}}}};return true;
  }
};
} // namespace paleo::python::generated_sdk_fixture
