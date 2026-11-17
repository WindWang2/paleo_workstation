// 层：测试壳
#pragma once
#include "services/pythonseismictrace.h"
#include <paleo_data/service.h>
#include <QCryptographicHash>
#include <QDir>
#include <QFile>
#include <QJsonArray>
#include <QTemporaryDir>
#include <QtEndian>
#include <cstring>
#include <utility>
namespace seismic_trace_fixture {
using namespace paleo;
inline QByteArray sourceBytes(int traces=2,int missing=-1) {
  constexpr int samples=6,traceBytes=240+samples*4;QByteArray bytes(3600+traces*traceBytes,0);
  qToBigEndian<quint16>(2000,bytes.data()+3216);qToBigEndian<quint16>(samples,bytes.data()+3220);
  qToBigEndian<quint16>(5,bytes.data()+3224);qToBigEndian<quint16>(0x0100,bytes.data()+3500);qToBigEndian<quint16>(1,bytes.data()+3502);
  for(int trace=0;trace<traces;++trace) {
    auto *header=bytes.data()+3600+trace*traceBytes;qToBigEndian<qint32>(trace?17:0,header+4);
    qToBigEndian<quint16>(samples,header+114);qToBigEndian<quint16>(2000,header+116);
    qToBigEndian<qint32>(101+trace,header+188);qToBigEndian<qint32>(201,header+192);
    for(int i=0;i<samples;++i) {
      const float value=float(trace*10+i-2);quint32 bits;std::memcpy(&bits,&value,4);
      qToBigEndian<quint32>(trace==1&&i==missing?0x7fc00000:bits,header+240+i*4);
    }
  }
  return bytes;
}
inline data::ParsedFile parsed(const QString &path,int traces=2) {
  data::ParsedFile out;out.kind="seismic";out.status="parsed";data::SeismicVolume volume;
  volume.volumeName="Explicit exact trace fixture";volume.dataFile=path.toUtf8().toStdString();volume.fileFormat="SEG-Y";
  volume.amplitudeUnit="relative_amplitude";volume.sampling={6,0,2,"ms","TWT","source_zero"};volume.geometry.traceCount=traces;
  out.volumes={volume};return out;
}
inline python::SeismicTraceSelection selection(const document::SourceRef &source,const data::SeismicWindowDescriptor &descriptor) {
  python::SeismicTraceSelection out;out.source=source;out.volumeId=descriptor.cut.volumeId;
  out.volumeRevision=QString::fromStdString(descriptor.cut.volumeRevision);out.traceIndex=1;out.firstSample=1;out.sampleCount=3;
  out.amplitudeUnit=QString::fromStdString(descriptor.amplitudeUnit);out.timeUnit=QString::fromStdString(descriptor.timeUnit);
  out.timeType=QString::fromStdString(descriptor.timeType);out.timeReference=QString::fromStdString(descriptor.timeReference);return out;
}
inline bool describeImportedVolume(data::DataService &sdk,long long sourceId,const QByteArray &sha,
    data::SeismicWindowDescriptor &descriptor,data::SeismicWindowReadStats &usage,QString &error) {
  descriptor={};usage={};error.clear();std::string detail;std::vector<data::DataEntry> volumes;
  // This synthetic fixture owns one volume per imported source. The SDK's
  // bounded well-data page API intentionally does not enumerate seismic volumes.
  if(!sdk.listSourceData("volumes",sourceId,volumes,detail)){error=QString::fromStdString(detail);return false;}
  if(volumes.size()!=1||volumes.front().id<1||volumes.front().sourceId!=sourceId) {
    error="fixture_imported_volume_identity_mismatch";return false;
  }
  data::SeismicWindowReadOptions budget;budget.maxReadBytes=8192;
  data::SeismicWindowDescriptor pending;data::ModelChangeHead current;
  if(!sdk.describeSeismicWindow(volumes.front().id,pending,detail,budget,&usage)||
      !sdk.readModelChangeHead(current,detail)){error=QString::fromStdString(detail);return false;}
  const auto &head=pending.cut.model;
  if(pending.cut.volumeId!=volumes.front().id||pending.cut.sourceId!=sourceId||
      pending.registeredSourceFingerprint!=sha.toStdString()||pending.cut.volumeRevision.empty()||
      !head.available||!current.available||head.storeId!=current.storeId||head.generation!=current.generation||head.sequence!=current.sequence||
      usage.metadataBytesRead<1||usage.metadataBytesRead>budget.metadata.maxTextBytes||
      usage.requestedSourceBytes!=0||usage.sourceReadCalls!=0||usage.outputBytes!=0) {
    error="fixture_seismic_descriptor_identity_or_io_mismatch";return false;
  }
  descriptor=std::move(pending);return true;
}
struct Model {
  QTemporaryDir temp;QString project,model,path,error;data::DataService sdk;QByteArray raw;
  python::SeismicTraceReadPlan plan;data::SeismicWindowDescriptor descriptor;
  data::SeismicWindowReadStats descriptorIo;
  bool open(int traces=2,int missing=-1,const std::function<void(QByteArray&)> &edit={}) {
    project=temp.filePath("project");model=project+"/artifacts/metadata/area-model";path=temp.filePath("source.sgy");
    std::string detail;if(!sdk.createProject(model.toUtf8().toStdString(),"Exact trace fixture",detail)){error=QString::fromStdString(detail);return false;}
    raw=sourceBytes(traces,missing);if(edit)edit(raw);QFile file(path);
    if(!file.open(QIODevice::WriteOnly)||file.write(raw)!=raw.size())return false;file.close();
    const auto sha=QCryptographicHash::hash(raw,QCryptographicHash::Sha256).toHex();long long source=0;
    if(!sdk.importPreparedFile(path.toUtf8().toStdString(),path.toUtf8().toStdString(),path.toUtf8().toStdString(),sha.toStdString(),parsed(path,traces),source,detail))return false;
    if(!describeImportedVolume(sdk,source,sha,descriptor,descriptorIo,error))return false;
    const document::SourceRef reference{"seismic-doc","area-model",source,QString::fromLatin1(sha)};
    plan.selection=selection(reference,descriptor);plan.projectDir=project;plan.modelDirectory=model;plan.documentId=reference.documentId;plan.callerScope="agent:trace";
    plan.source.ref=reference;plan.source.path=path;plan.source.assetId="data-only-source-asset";plan.source.versionId="data-only-source-version";
    plan.source.modelDirectory=model;plan.source.fingerprint=document::SourceFingerprint::fromStored(reference.fileRevisionId);
    plan.reader=std::make_shared<artifacts::AuthenticatedArtifactReader>(artifacts::AuthenticatedArtifactReader::Profile::SeismicSource);
    plan.authenticatedSource={"data-only-stable-document-source-caller",path,reference.fileRevisionId,raw.size()};return true;
  }
};
inline python::ExecutionPolicy policy(){python::ExecutionPolicy out;out.allowGenerated=true;return out;}
inline QByteArray sourceCode() {
  return R"PY(import sys, struct, json, numpy as np
n=struct.unpack('<I',sys.stdin.buffer.read(4))[0]
m=json.loads(sys.stdin.buffer.read(n));raw=sys.stdin.buffer.read();r=m['shape'][0]
axis=np.frombuffer(raw,dtype='<f8',count=r)
v=np.frombuffer(raw,dtype='<f8',count=r,offset=r*8)
null=np.frombuffer(raw,dtype='u1',count=r,offset=r*16)
x=v[null==0]
print(json.dumps({'contract':'paleo.python-output.v1','ports':{'REPORT':{'contract':'paleo.python-generated-report.v1','metrics':[{'id':'window_mean','unit':m['columns'][0]['unit'],'value':float(np.mean(x)) if x.size else None}],'tables':[],'notes':['One exact authenticated seismic trace window.']}}},allow_nan=False))
)PY";
}
inline QByteArray bytes(const python::InputBundle &bundle,const QString &name) {
  QFile file(bundle.root+'/'+name);return file.open(QIODevice::ReadOnly)?file.readAll():QByteArray{};
}
}
