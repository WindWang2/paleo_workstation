// 层：功能
// workflow/capabilitynativeplan.cpp — 完整图是一个持久父意图；子核只写私有工件。
#include "capabilitynativeoperation.h"
#include <QDir>
#include <QJsonArray>
namespace paleo::capability {
namespace {
QJsonObject descriptor(const QVector<PlanNode> &nodes,const QVector<PlanEdge> &edges) {
  QJsonArray ns,es;
  for(const auto &n:nodes)ns.append(QJsonObject{{"id",n.id},{"capability",n.capabilityId},{"version",n.contractVersion},
    {"outputPort",n.outputPort},{"intent",n.intent},{"parameters",QJsonObject::fromVariantMap(n.parameters)},{"inputs",QJsonObject::fromVariantMap(n.inputs)}});
  for(const auto &e:edges)es.append(QJsonObject{{"from",e.from},{"fromPort",e.fromPort},{"to",e.to},{"toPort",e.toPort}});
  return {{"nodes",ns},{"edges",es}};
}
bool decode(const QJsonObject &json,QVector<PlanNode> &nodes,QVector<PlanEdge> &edges) {
  if(!json.value("nodes").isArray() || !json.value("edges").isArray())return false;
  for(const auto &entry:json.value("nodes").toArray()) {
    const auto n=entry.toObject();nodes.append({n.value("id").toString(),n.value("capability").toString(),n.value("version").toInt(),
      n.value("outputPort").toString(),n.value("intent").toString(),n.value("parameters").toObject().toVariantMap(),n.value("inputs").toObject().toVariantMap()});
  }
  for(const auto &entry:json.value("edges").toArray()) {
    const auto e=entry.toObject();edges.append({e.value("from").toString(),e.value("fromPort").toString(),e.value("to").toString(),e.value("toPort").toString()});
  }
  return true;
}
}
QJsonObject nativePlanIntent(const ExecuteRequest &r,const QVector<PlanNode> &nodes,const QVector<PlanEdge> &edges) {
  return {{"host",nativeIntent(r)},{"graph",descriptor(nodes,edges)}};
}
OperationWorker restoreNativePlan(const QJsonObject &json,QString *error) {
  QVector<PlanNode> nodes;QVector<PlanEdge> edges;ExecuteRequest host;
  if(!nativeRequestFromJson(json.value("host").toObject(),host) || !decode(json.value("graph").toObject(),nodes,edges)) {
    if(error)*error="invalid_frozen_plan";return {};
  }
  const auto plan=validatePlan(nodes,edges);if(!plan.ok){if(error)*error=plan.issues.first().code;return {};}
  QString outputPort;
  for(const auto &n:plan.nodes)if(n.node.id==plan.order.last())outputPort=n.node.outputPort;
  return [host,nodes,edges,outputPort](const OperationWorkContext &context) mutable {
    auto request=host;request.operationDir=context.privateDir;request.clientRequestId=context.attemptId;
    request.projectDir=context.projectDir;
    request.publishIntent="compute-only";request.cancelled=context.cancelled;request.catalog=nullptr;
    for(auto &node:nodes)if(node.capabilityId==QLatin1String("singlefactor.contours") && node.inputs.contains("raster_path")) {
      auto literal=request;literal.inputs=node.inputs;const auto code=verifyNativeRasterInput(literal);
      if(!code.isEmpty()){OperationWorkResult failed;failed.code=code;return failed;}node.inputs=literal.inputs;
    }
    const auto result=executePlan(request,nodes,edges);OperationWorkResult out;
    out.ok=result.ok;out.code=result.code;out.summary=QJsonObject::fromVariantMap(result.summary);out.summary.remove("result_path");
    QJsonArray children;for(const auto &entry:out.summary.value("child_operations").toArray()) {
      auto child=entry.toObject();child.insert("parentOperationId",context.operationId);children.append(child);
    }
    if(!children.isEmpty())out.summary.insert("child_operations",children);
    out.outputs.ok=out.ok;
    if(out.ok)out.outputs.artifacts.append({outputPort,result.summary.value("result_path").toString(),{},{},{},{}});
    return out;
  };
}
FrozenOperation freezeNativePlan(const ExecuteRequest &r,const QVector<PlanNode> &nodes,const QVector<PlanEdge> &edges,QString *error) {
  const auto validated=validatePlan(nodes,edges);if(!validated.ok){if(error)*error=validated.issues.first().code;return {};}
  FrozenOperation p;p.capabilityId="workflow.plan";p.implementationVersion="1";p.callerScope=r.callerScope;p.clientRequestId=r.clientRequestId;
  p.intent=nativePlanIntent(r,nodes,edges);
  QVector<PlanNode> normalized;for(const auto &n:validated.nodes)normalized.append(n.node);
  auto frozenHost=r;frozenHost.capabilityId="workflow.plan";
  p.frozenPlan={{"host",nativeFrozenRequest(frozenHost)},{"graph",descriptor(normalized,edges)}};
  p.phase=r.publishIntent.isEmpty()?"compute-only":r.publishIntent;
  for(const auto &n:validated.nodes)if(n.node.id==validated.order.last()) {p.outputSpec=n.algorithm;p.outputPorts={n.node.outputPort};}
  p.worker=restoreNativePlan(p.frozenPlan,error);return p;
}
} // namespace paleo::capability
