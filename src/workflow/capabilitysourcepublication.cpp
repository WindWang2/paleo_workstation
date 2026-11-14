// 层：功能
#include "capabilitysourcepublication.h"
#include "capabilitypublication_internal.h"
namespace paleo::capability {
QJsonObject capabilityIntent(const ExecuteRequest &r) {
  return {{"capability",r.capabilityId},{"version",r.contractVersion},{"intent",r.publishIntent},
    {"parameters",QJsonObject::fromVariantMap(r.parameters)},{"inputs",QJsonObject::fromVariantMap(r.inputs)},
    {"bindingMode",r.bindingMode},{"binding",r.bindingId},{"frozenHead",r.frozenHead},{"present",r.presentOk}};
}
FrozenOperation freezeSourcePublication(const ExecuteRequest &r,const QJsonObject &original,
                                       const OperationLog::Record &source,QString *error) {
  FrozenOperation p;
  if(source.artifactManifest.value("diagnosticsOnly").toBool() || source.schemaVersion!=2 || source.documentId!=r.documentId || source.callerScope!=r.callerScope ||
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
}
