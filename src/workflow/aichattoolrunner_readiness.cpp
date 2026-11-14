// 层：功能
#include "aichattoolrunner.h"
#include "capabilityhost.h"
#include "capabilityrecovery.h"
#include "ai/chat/domaintools.h"
#include "ai/onnxpredictionservice.h"
#include "catalog/datacatalog.h"
#include "tileoperationservice.h"
#include <QThread>

bool AiChatToolRunner::externalTransferConfigured() const {
  if(QThread::currentThread()!=thread())return false;
  return !m_faciesConfig.apiKey.trimmed().isEmpty() && !m_faciesConfig.apiKey.contains('\r') && !m_faciesConfig.apiKey.contains('\n') &&
    WellFaciesConfig::validateUrl(m_faciesConfig.baseUrl,m_faciesConfig.allowInsecureHttp).isEmpty();
}

AiChatToolRunner::ToolReadiness AiChatToolRunner::readiness(const QString &name,const QJsonObject &args,bool listing) const {
  return readinessForSpec(aiToolSpec(name),args,listing);
}
AiChatToolRunner::ToolReadiness AiChatToolRunner::readinessForSpec(const AiToolSpec &spec,const QJsonObject &args,bool listing) const {
  const auto &name=spec.name;
  ToolReadiness result;result.effects=spec.effectsForArguments(args);
  const auto fail=[&](const QString &code) { result.code=code;return result; };
  if(QThread::currentThread()!=thread())return fail("owner_thread_required");
  if(spec.name.isEmpty())return fail("tool_not_found");
  if(spec.exposure==CapabilityExposure::Internal || spec.exposure==CapabilityExposure::Unavailable)
    return fail("tool_not_exposed");
#if !PALEO_HAVE_ORT
  if(spec.requiresLocalOrt)return fail("ort_unavailable");
#endif
  // Explicitly injected legacy adapters remain available to existing internal callers.
  if(!m_context.requireCapabilityHost) {result.ready=true;return result;}
  if(spec.legacyAdapter)return fail("legacy_tool_requires_versioned_adapter");
  if(name==QLatin1String("paleo.read_result")) {result.ready=true;return result;}
  if(!m_context.capabilityHost)return fail("host_unavailable");
  const auto live=m_context.capabilityHost->context();auto expected=live;
  expected.documentId=m_context.documentId;expected.epoch=m_context.documentEpoch;
  if(name==QLatin1String("paleo.operation_cancel") && !listing && !m_resultSession.isEmpty() &&
     m_context.capabilityHost->recovery() && paleo::host::checkContext(live,expected,paleo::host::Read).isEmpty()) {
    const auto scope=m_context.capabilityHost->callerScope(paleo::host::Caller::Agent,m_resultSession);
    const auto operation=m_context.capabilityHost->recovery()->get(scope,args.value("operation_id").toString());
    if(operation.value("commitState")==QLatin1String("committed")) {
      result.effects=paleo::host::Read;result.ready=true;return result;
    }
  }
  if((isAiCapabilityTool(name) || name==QLatin1String("paleo.tile_classification_v2") ||
      name==QLatin1String("paleo.well_facies_prediction")) && !listing &&
     !args.value("client_request_id").toString().isEmpty() && m_context.capabilityHost->recovery()) {
    const auto readable=paleo::host::checkContext(live,expected,paleo::host::Read);
    if(!readable.isEmpty())return fail(readable);
    const auto scope=m_context.capabilityHost->callerScope(paleo::host::Caller::Agent,m_resultSession);
    if(m_context.capabilityHost->recovery()->lookup(scope,args.value("client_request_id").toString()).code==QLatin1String("found")) {
      result.effects=paleo::host::Read;result.ready=true;return result;
    }
  }
  if(name==QLatin1String("paleo.well_facies_prediction") ||
     (name==QLatin1String("paleo.query_project") && args.value("topic")==QLatin1String("well_facies_models"))) {
    const auto read=paleo::host::checkContext(live,expected,paleo::host::Read);if(!read.isEmpty())return fail(read);
    if(!externalTransferConfigured())return fail("well_endpoint_unavailable");
  }
  if(!result.effects)return fail("invalid_publish_intent");
  const auto denied=paleo::host::checkContext(live,expected,result.effects);
  if(!denied.isEmpty())return fail(denied);
  if((name==QLatin1String("paleo.query_project") || name==QLatin1String("paleo.asset_lineage")) &&
      (!m_context.catalog || !m_context.catalog->isOpen()))return fail("catalog_unavailable");
  if(name==QLatin1String("paleo.well_facies_prediction")) {
    const auto code=wellFaciesReadiness(args,listing);if(!code.isEmpty())return fail(code);
  }
  if(name==QLatin1String("paleo.query_project") && args.value("topic")==QLatin1String("local_models")) {
#if !PALEO_HAVE_ORT
    return fail("ort_unavailable");
#else
    if(!m_context.onnx)return fail("ort_provider_unavailable");
#endif
  }
  if(name==QLatin1String("paleo.query_project") &&
      (args.value("topic")==QLatin1String("well_facies_models") || args.value("topic")==QLatin1String("well_facies_inputs"))) {
    if(!m_taskService)return fail("task_service_unavailable");
    if(args.value("topic")==QLatin1String("well_facies_models") && !externalTransferConfigured())return fail("well_endpoint_unavailable");
  }
  if(spec.requiresLocalOrt) {
#if PALEO_HAVE_ORT
    if(!m_context.onnx)return fail("ort_provider_unavailable");
    const auto models=m_context.onnx->availableModels();
    if(models.isEmpty())return fail("model_unavailable");
    if(!listing && !models.contains(args.value("model").toString()))return fail("model_unavailable");
#else
    return fail("ort_unavailable");
#endif
  }
  if(name==QLatin1String("paleo.horizon_tracking_suggestion")) {
    if(!m_workflow || !m_context.trackingInput)return fail("tracking_provider_unavailable");
    TrackingInput input;QString code;
    if(!m_context.trackingInput({},&input,&code))return fail(code.isEmpty()?QStringLiteral("tracking_semantics_unavailable"):code);
  }
  if(m_context.runtimeReadiness) {
    const auto code=m_context.runtimeReadiness(name,args,listing);
    if(!code.isEmpty())return fail(code);
  }
  if(name==QLatin1String("paleo.tile_classification_v2")) {
    if(!m_context.tileOperations)return fail("tile_provider_unavailable");
    const auto code=m_context.tileOperations->readiness(args,listing);if(!code.isEmpty())return fail(code);
  }
  result.ready=true;return result;
}
QVector<AiToolSpec> AiChatToolRunner::availableTools() const {
  QVector<AiToolSpec> tools;
  for(const auto &spec:builtinAiToolSpecs())if(readinessForSpec(spec,{},true).ready)tools.append(spec);
  return tools;
}
