// 层：功能
// workflow/capabilitynativeoperation.cpp — 复用原生核，外层身份/收据/发布由 Recovery 管理。
#include "capabilitynativeoperation.h"
#include "capabilityseismicjob.h"
#include "capabilitypublication_internal.h"
#include "capabilityrecovery_internal.h"
#include "services/capabilityseismicinput.h"
#include "io/sourcefingerprint.h"
#include "catalog/datacatalog.h"
#include <QDir>
#include <QFile>
#include <QJsonDocument>
#include <QSaveFile>
namespace paleo::capability {
QJsonObject nativeIntent(const ExecuteRequest &r) {
  QJsonObject object{{"capability",r.capabilityId},{"version",r.contractVersion},{"intent",r.publishIntent},
    {"parameters",QJsonObject::fromVariantMap(r.parameters)},{"inputs",QJsonObject::fromVariantMap(r.inputs)},
    {"bindingMode",r.bindingMode},{"binding",r.bindingId},{"frozenHead",r.frozenHead},{"present",r.presentOk}};
  if(r.seismicTrace)object.insert("seismicSelection",seismicTraceIdentity(*r.seismicTrace));return object;
}
QJsonObject nativeFrozenRequest(const ExecuteRequest &r) {
  auto object=nativeIntent(r);object.insert("documentId",r.documentId);object.insert("callerScope",r.callerScope);
  if(r.seismicTrace) {
    const auto &source=r.seismicTrace->source;
    object.insert("sourceVerification",QJsonObject{{"path",QDir(r.projectDir).absoluteFilePath(source.path)},
      {"fingerprint",source.fingerprint.stored()},{"versionId",source.versionId},{"assetId",source.assetId}});
  }
  return object;
}
OperationInputValidator nativeInputValidator(const QJsonObject &frozen) {
  const auto source=frozen.value("sourceVerification").toObject();if(source.isEmpty())return {};
  return [source](const OperationWorkContext &context) {
    const auto expected=document::SourceFingerprint::fromStored(source.value("fingerprint").toString());
    if(expected.kind!=document::FingerprintKind::SegyQuickV1)return QString("source_verification_budget_exceeded");
    document::FileStamp stamp;qint64 bytes=0;QString error;
    return document::verifySourceFingerprint(source.value("path").toString(),expected,stamp,bytes,&error,context.cancelled)?QString():QString("source_revision_changed");
  };
}
bool nativeRequestFromJson(const QJsonObject &j,ExecuteRequest &r) {
  r={};r.capabilityId=j.value("capability").toString();r.contractVersion=j.value("version").toInt();
  r.parameters=j.value("parameters").toObject().toVariantMap();r.inputs=j.value("inputs").toObject().toVariantMap();
  r.documentId=j.value("documentId").toString();r.callerScope=j.value("callerScope").toString();
  r.bindingMode=j.value("bindingMode").toString();r.bindingId=j.value("binding").toString();r.frozenHead=j.value("frozenHead").toString();
  if(j.contains("seismicSelection"))r.inputs.insert("seismic_selection",j.value("seismicSelection").toObject().toVariantMap());
  return !r.capabilityId.isEmpty() && r.contractVersion>0;
}
FrozenOperation freezeNativeOperation(const ExecuteRequest &r,const QJsonObject &original,QString *error) {
  FrozenOperation p;p.capabilityId=r.capabilityId;p.contractVersion=r.contractVersion;p.callerScope=r.callerScope;p.clientRequestId=r.clientRequestId;
  const auto capability=CapabilityRegistry::spec(r.capabilityId,r.contractVersion);
  p.outputSpec=AlgorithmSpecRegistry::specAtVersion(capability.algorithmId,capability.algorithmContractVersion);
  if(r.capabilityId!="depth.convert_batch" && r.capabilityId!="seismic.attributes.compute" &&
     r.capabilityId!="constraint.idw" && r.capabilityId!="singlefactor.contours") {if(error)*error="execution_adapter_unavailable";return {};}
  if(!p.outputSpec.complete){if(error)*error="algorithm_not_available";return {};}
  p.implementationVersion=p.outputSpec.implementationVersion;p.intent=original;p.frozenPlan=nativeFrozenRequest(r);
  p.phase=r.publishIntent.isEmpty()?(r.capabilityId=="constraint.idw"?"prepare":"compute-only"):r.publishIntent;
  p.sourceOperationId=r.inputs.value("source_operation_id").toString();
  if(r.seismicTrace && !r.seismicTrace->source.versionId.isEmpty()) {
    const auto &id=r.seismicTrace->source.versionId;
    if(!r.catalog || r.catalog->versionById(id).id!=id){if(error)*error="input_version_missing";return {};}
    p.parentVersionIds.append(id);
  }
  for(const auto &port:p.outputSpec.outputs) {
    if(port.kind!="file" && port.kind!="raster" && port.kind!="vector" && port.kind!="table")continue;
    if(r.capabilityId=="seismic.attributes.compute" && !r.parameters.value("ATTRIBUTES").toStringList().contains(port.id.toLower()))continue;
    p.outputPorts.append(port.id);
  }
  p.worker=nativeWorker(r);p.inputValidator=nativeInputValidator(p.frozenPlan);return p;
}
FrozenOperation freezeSourcePublication(const ExecuteRequest &r,const QJsonObject &original,
                                       const OperationLog::Record &source,QString *error) {
  FrozenOperation p;
  if(source.schemaVersion!=2 || source.documentId!=r.documentId || source.callerScope!=r.callerScope ||
      source.capabilityId!=r.capabilityId || source.contractVersion!=r.contractVersion || source.artifactManifest.isEmpty() ||
      !publication_detail::specFromJson(source.stagePlan.value("outputSpec").toObject(),p.outputSpec)) {
    if(error)*error="source_operation_mismatch";return {};
  }
  p.capabilityId=r.capabilityId;p.contractVersion=r.contractVersion;p.implementationVersion=source.implementationVersion;
  p.callerScope=r.callerScope;p.clientRequestId=r.clientRequestId;p.intent=original;p.frozenPlan=source.frozenRequest.value("plan").toObject();
  p.phase="publish";p.sourceOperationId=source.operationId;
  for(const auto &port:source.artifactManifest.value("ports").toArray())p.outputPorts.append(port.toObject().value("portId").toString());
  for(const auto &parent:source.stagePlan.value("parents").toArray())p.parentVersionIds.append(parent.toString());return p;
}
namespace {
QString writeBytes(const QString &path,QByteArrayView bytes) {
  QSaveFile file(path);if(!file.open(QIODevice::WriteOnly) || file.write(bytes.data(),bytes.size())!=bytes.size() || !file.commit())return "stage_failed";return {};
}
bool splitTrace(const ExecuteResult &result,const OperationWorkContext &context,AlgorithmRunResult &outputs) {
  QFile source(result.summary.value("result_path").toString());if(!source.open(QIODevice::ReadOnly))return false;
  const auto ports=result.summary.value("resultRef").toMap().value("ports").toList();
  for(const auto &value:ports) {
    const auto port=value.toMap();const auto id=port.value("portId").toString();
    const qint64 count=port.value("sampleCount").toLongLong(),offset=port.value("byteOffset").toLongLong();
    if(count<1 || count>16*1024*1024 || offset<0 || !source.seek(offset) || (context.cancelled && context.cancelled()))return false;
    const auto bytes=source.read(count*4);const auto path=QDir(context.privateDir).filePath(id+".f32");
    if(bytes.size()!=count*4 || !writeBytes(path,QByteArrayView(bytes)).isEmpty())return false;
    outputs.artifacts.append({id,path,{},{},{{"encoding","float32le"}}, {}});
  }
  return !ports.isEmpty();
}
QString restoreRaster(ExecuteRequest &r) {
  OperationLog log(QDir(r.projectDir).filePath(".paleo/capability-ops"));
  const auto reference=r.inputs.value("operation_result").toMap();
  const auto requested=r.inputs.value("raster_path").toString();
  auto source=reference.isEmpty()?log.artifactOwner(r.documentId,r.callerScope,QDir(r.projectDir).relativeFilePath(requested)):
      log.recordById(reference.value("operationId").toString());
  if(source.schemaVersion!=2 || source.documentId!=r.documentId || source.callerScope!=r.callerScope)return "untrusted_result_ref";
  AlgorithmRunResult artifacts;QString error;
  if(!publication_detail::outputsFromManifest(r.projectDir,source.operationId,source.artifactManifest,artifacts,&error,r.cancelled,true,
       recovery_detail::managedTargets(source,r.projectDir)))return error.isEmpty()?"result_unavailable":error;
  const auto port=reference.value("portId","OUTPUT").toString();
  for(const auto &artifact:artifacts.artifacts)if((reference.isEmpty() && artifact.absolutePath==requested) || (!reference.isEmpty() && artifact.portId==port)) {
    r.inputs.remove("operation_result");r.inputs.insert("raster_path",artifact.absolutePath);noteIssuedResult(r.documentId,artifact.absolutePath);return {};
  }
  return "result_unavailable";
}
}
QString verifyNativeRasterInput(ExecuteRequest &request) { return restoreRaster(request); }
OperationWorker nativeWorker(ExecuteRequest request) {
  request.catalog=nullptr;request.onRunning={};request.writeClaim.reset();
  return [request=std::move(request)](const OperationWorkContext &context) mutable {
    OperationWorkResult out;ExecuteRequest child=request;
    child.projectDir=context.projectDir;
    child.operationDir=context.privateDir;child.clientRequestId=context.attemptId;child.publishIntent="compute-only";child.cancelled=context.cancelled;
    if(child.capabilityId=="singlefactor.contours") {
      const auto code=restoreRaster(child);if(!code.isEmpty()){out.code=code;return out;}
    }
    child.stageBytes=[&](const QString &port,QByteArrayView bytes) {
      const auto path=QDir(context.privateDir).filePath(port+".bin");const auto code=writeBytes(path,bytes);
      if(code.isEmpty())out.outputs.artifacts.append({port,path,{},{},{},{}});return code;
    };
    const auto result=execute(child);out.ok=result.ok;out.code=result.code;out.summary=QJsonObject::fromVariantMap(result.summary);
    out.summary.remove("result_path");out.summary.remove("resultRef");
    if(!out.ok)return out;
    const auto path=result.summary.value("result_path").toString();
    if(child.seismicTrace)out.ok=splitTrace(result,context,out.outputs);
    else if(!path.isEmpty()) {
      const auto port=child.capabilityId=="singlefactor.contours"?"LINES":child.capabilityId=="seismic.attributes.compute"?"COHERENCE":"OUTPUT";
      out.outputs.artifacts.append({port,path,{},{},{},{}});
    }
    out.outputs.ok=out.ok;return out;
  };
}
} // namespace paleo::capability
