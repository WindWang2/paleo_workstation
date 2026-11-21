// 层：功能
#include "aichattoolrunner.h"
#include "capabilityhost.h"
#include "capabilityrecovery.h"
#include "ai/chat/domaintools.h"
#include "ai/onnxpredictionservice.h"
#include "catalog/datacatalog.h"
#include "tileoperationservice.h"
#include "services/pythonanalysiscapabilityspec.h"
#include "services/pythongenerated.h"
#include "services/pythonnumerictable.h"
#include "services/pythonseismictrace.h"
#include "services/pythontableoutputspec.h"
#include "services/pythongeneratedoutputspec.h"
#include <QThread>

bool AiChatToolRunner::externalTransferConfigured() const {
  if(QThread::currentThread()!=thread())return false;
  return !m_faciesConfig.apiKey.trimmed().isEmpty() && !m_faciesConfig.apiKey.contains('\r') && !m_faciesConfig.apiKey.contains('\n') &&
    WellFaciesConfig::validateUrl(m_faciesConfig.baseUrl,m_faciesConfig.allowInsecureHttp).isEmpty();
}

// G3/W6：快照身份复核——catalog/宿主/provider 指针、文档身份与世代一致
// 才算「同一有效上下文」。std::function 同布尔值替换不可值比较（已知边界，
// 见头文件注释）；世代号覆盖 setContext/setTaskService 判定的全部变化。
bool AiChatToolRunner::contextMatchesLive(const Context &context, quint64 contextGeneration) const {
  if (m_contextGeneration != contextGeneration)
    return false;
  return m_context.catalog == context.catalog &&
      m_context.documentId == context.documentId &&
      m_context.documentEpoch == context.documentEpoch &&
      m_context.projectDir == context.projectDir &&
      m_context.capabilityHost == context.capabilityHost &&
      m_context.requireCapabilityHost == context.requireCapabilityHost &&
      m_context.onnx == context.onnx &&
      m_context.tileOperations == context.tileOperations &&
      m_context.pythonRuntime == context.pythonRuntime &&
      m_context.pythonTrusted == context.pythonTrusted &&
      m_context.pythonSandbox == context.pythonSandbox &&
      bool(m_context.trackingInput) == bool(context.trackingInput) &&
      bool(m_context.traceFetch) == bool(context.traceFetch) &&
      bool(m_context.gridFetch) == bool(context.gridFetch) &&
      bool(m_context.frozenFaciesInput) == bool(context.frozenFaciesInput) &&
      bool(m_context.faciesContext) == bool(context.faciesContext) &&
      bool(m_context.sourceResolver) == bool(context.sourceResolver) &&
      bool(m_context.runtimeReadiness) == bool(context.runtimeReadiness);
}

AiChatToolRunner::ToolReadiness AiChatToolRunner::readiness(const QString &name,const QJsonObject &args,bool listing) const {
  return readinessForSpec(aiToolSpec(name),args,listing);
}
AiChatToolRunner::ToolReadiness AiChatToolRunner::readinessForSpec(const AiToolSpec &spec,const QJsonObject &args,bool listing) const {
  // G3/W6：单次判断绑定一份快照——判断途中（含注入回调内）发生重绑时，
  // 由 readinessForSpecInContext 的回调后复核按 context_rebound 失败，
  // 不混用新旧上下文成员。
  return readinessForSpecInContext(spec,args,listing,m_context,m_contextGeneration);
}
AiChatToolRunner::ToolReadiness AiChatToolRunner::readinessForSpecInContext(const AiToolSpec &spec,const QJsonObject &args,bool listing,
                                                                           const Context &context,quint64 contextGeneration) const {
  const auto &name=spec.name;
  ToolReadiness result;result.effects=spec.effectsForArguments(args);
  const auto fail=[&](const QString &code) { result.code=code;return result; };
  // 注入回调后复核：回调同步重绑 setContext/换 provider/清空绑定时，
  // 快照即刻失效——本次判断失败，不投递旧绑定的 ready=true。
  const auto rebound=[&]() { return !contextMatchesLive(context,contextGeneration); };
  if(QThread::currentThread()!=thread())return fail("owner_thread_required");
  if(spec.name.isEmpty())return fail("tool_not_found");
  if(spec.exposure==CapabilityExposure::Internal || spec.exposure==CapabilityExposure::Unavailable)
    return fail("tool_not_exposed");
  const bool python=name==QLatin1String(paleo::python::WellQualityTool) || name==QLatin1String(paleo::python::CorrelationTool) ||
    name==QLatin1String(paleo::python::GeneratedTool) || name==QLatin1String(paleo::python::NumericTableTool) ||
    name==QLatin1String(paleo::python::SeismicTraceTool) || name==QLatin1String(paleo::python::TableOutputsTool) ||
    name==QLatin1String(paleo::python::GeneratedOutputsTool);
  if(name==QLatin1String("paleo.python_environment")) {
    if(!m_taskService)return fail("task_service_unavailable");result.ready=true;return result;
  }
#if !PALEO_HAVE_ORT
  if(spec.requiresLocalOrt)return fail("ort_unavailable");
#endif
  // Explicitly injected legacy adapters remain available to existing internal callers.
  if(!context.requireCapabilityHost && !python) {result.ready=true;return result;}
  if(spec.legacyAdapter)return fail("legacy_tool_requires_versioned_adapter");
  if(name==QLatin1String("paleo.read_result")) {result.ready=true;return result;}
  if(name==QLatin1String("paleo.python_environment") && !m_taskService)return fail("task_service_unavailable");
  if(!context.capabilityHost)return fail("host_unavailable");
  const auto live=context.capabilityHost->context();auto expected=live;
  expected.documentId=context.documentId;expected.epoch=context.documentEpoch;
  if(name==QLatin1String("paleo.operation_cancel") && !listing && !m_resultSession.isEmpty() &&
      context.capabilityHost->recovery() && paleo::host::checkContext(live,expected,paleo::host::Read).isEmpty()) {
    const auto scope=context.capabilityHost->callerScope(paleo::host::Caller::Agent,m_resultSession);
    const auto operation=context.capabilityHost->recovery()->get(scope,args.value("operation_id").toString());
    if(operation.value("commitState")==QLatin1String("committed")) {
      result.effects=paleo::host::Read;result.ready=true;return result;
    }
  }
  if((isAiCapabilityTool(name) || name==QLatin1String("paleo.tile_classification_v2") ||
      name==QLatin1String("paleo.well_facies_prediction")) && !listing &&
     !args.value("client_request_id").toString().isEmpty() && context.capabilityHost->recovery()) {
    const auto readable=paleo::host::checkContext(live,expected,paleo::host::Read);
    if(!readable.isEmpty())return fail(readable);
    const auto scope=context.capabilityHost->callerScope(paleo::host::Caller::Agent,m_resultSession);
    if(context.capabilityHost->recovery()->lookup(scope,args.value("client_request_id").toString()).code==QLatin1String("found")) {
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
      (!context.catalog || !context.catalog->isOpen()))return fail("catalog_unavailable");
  if(name==QLatin1String("paleo.well_facies_prediction")) {
    const auto code=wellFaciesReadiness(args,listing);if(!code.isEmpty())return fail(code);
  }
  if(name==QLatin1String("paleo.query_project") && args.value("topic")==QLatin1String("local_models")) {
#if !PALEO_HAVE_ORT
    return fail("ort_unavailable");
#else
    if(!context.onnx)return fail("ort_provider_unavailable");
#endif
  }
  if(name==QLatin1String("paleo.query_project") &&
      (args.value("topic")==QLatin1String("well_facies_models") || args.value("topic")==QLatin1String("well_facies_inputs"))) {
    if(!m_taskService)return fail("task_service_unavailable");
    if(args.value("topic")==QLatin1String("well_facies_models") && !externalTransferConfigured())return fail("well_endpoint_unavailable");
  }
  if(spec.requiresLocalOrt) {
#if PALEO_HAVE_ORT
    if(!context.onnx)return fail("ort_provider_unavailable");
    const auto models=context.onnx->availableModels();
    if(models.isEmpty())return fail("model_unavailable");
    if(!listing && !models.contains(args.value("model").toString()))return fail("model_unavailable");
#else
    return fail("ort_unavailable");
#endif
  }
  if(name==QLatin1String("paleo.horizon_tracking_suggestion")) {
    if(!m_workflow || !context.trackingInput)return fail("tracking_provider_unavailable");
    TrackingInput input;QString code;
    const bool available=context.trackingInput({},&input,&code);
    // 复核先于回调结论：回调内重绑/销毁绑定（无论回调自身成败）都按
    // context_rebound 失败——不把旧 provider 的结论与新上下文组合。
    if(rebound())return fail("context_rebound");
    if(!available)return fail(code.isEmpty()?QStringLiteral("tracking_semantics_unavailable"):code);
  }
  if(context.runtimeReadiness) {
    const auto code=context.runtimeReadiness(name,args,listing);
    if(rebound())return fail("context_rebound");
    if(!code.isEmpty())return fail(code);
  }
  if(name==QLatin1String("paleo.tile_classification_v2")) {
    if(!context.tileOperations)return fail("tile_provider_unavailable");
    const auto code=context.tileOperations->readiness(args,listing);
    if(rebound())return fail("context_rebound");
    if(!code.isEmpty())return fail(code);
  }
  if(python && (!m_taskService || !context.pythonRuntime || !context.runtimeReadiness))
    return fail("python_analysis_provider_unavailable");
  result.ready=true;return result;
}
QVector<AiToolSpec> AiChatToolRunner::availableTools() const {
  // G3/W6：一次枚举绑定同一份快照——途中注入回调重绑/销毁时，受影响
  // 工具按 context_rebound 排除，不返回一半旧一半新的清单；下一次枚举
  // 自然使用新上下文（有界策略：不重试、不阻塞事件循环）。
  const Context snapshot=m_context;
  const quint64 generation=m_contextGeneration;
  QVector<AiToolSpec> tools;
  for(const auto &spec:builtinAiToolSpecs()) {
    auto candidate=spec;
    // G3/W7：query_project 主题差异收敛进本次返回的 spec 副本——依赖
    // endpoint/ORT/TaskService 的主题就绪失败时从 enum 收窄（描述不动），
    // 单个不可用主题不抹掉仍可用的目录查询，也不改全局注册表。
    if(spec.name==QLatin1String("paleo.query_project") && !spec.parameters.isEmpty()) {
      const int topicIndex=[&]() {
        for(int i=0;i<spec.parameters.size();++i)
          if(spec.parameters[i].name==QLatin1String("topic"))return i;
        return -1;
      }();
      if(topicIndex>=0) {
        QStringList narrowed;
        for(const auto &topic:spec.parameters[topicIndex].enumValues)
          if(readinessForSpecInContext(spec,QJsonObject{{"topic",topic}},true,snapshot,generation).ready)
            narrowed.append(topic);
        if(!narrowed.isEmpty() && narrowed.size()<spec.parameters[topicIndex].enumValues.size())
          candidate.parameters[topicIndex].enumValues=narrowed;
      }
    }
    if(readinessForSpecInContext(candidate,{},true,snapshot,generation).ready)tools.append(candidate);
  }
  return tools;
}
