// 层：功能
#include "aichattoolrunner.h"
#include "capabilityhost.h"
#include "capabilityrecovery.h"
#include "ai/chat/domaintools.h"
#include "ai/chat/planpreviewtool.h"
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

AiChatToolRunner::ToolReadiness AiChatToolRunner::readiness(const QString &name,const QJsonObject &args,bool listing) const {
  return readinessForSpec(aiToolSpec(name),args,listing);
}
AiChatToolRunner::ToolReadiness AiChatToolRunner::readinessForSpec(const AiToolSpec &spec,const QJsonObject &args,bool listing) const {
  if (QThread::currentThread() != thread()) return {false, QStringLiteral("owner_thread_required")};
  // Keep the callable alive even if it replaces its own binding synchronously.
  const Context snapshot = m_context;
  return readinessForSpecInContext(spec, args, listing, snapshot, m_bindingRevision);
}
AiChatToolRunner::ToolReadiness AiChatToolRunner::readinessForSpecInContext(const AiToolSpec &spec,const QJsonObject &args,bool listing,
                                                                           const Context &context,quint64 bindingRevision) const {
  const auto &name=spec.name;
  ToolReadiness result;result.effects=spec.effectsForArguments(args);
  const auto fail=[&](const QString &code) { result.code=code;return result; };
  if(spec.name.isEmpty())return fail("tool_not_found");
  if(spec.exposure==CapabilityExposure::Internal || spec.exposure==CapabilityExposure::Unavailable)
    return fail("tool_not_exposed");
  if (name == QLatin1String("paleo.read_result")) { result.ready = true; return result; }
  // H5：计划提案纯预检零宿主依赖（decode+validatePlan 无 I/O、无副作用）——
  // 与会话级 read_result 同口径；采纳（重新预检）与显式执行仍由
  // aiplanpresentation 走宿主 Read/effects 门，不因本分支放宽。
  if (name == QLatin1String(paleo::chat::kPlanPreviewTool)) { result.ready = true; return result; }

  const QPointer<const AiChatToolRunner> owner(this);
  const QPointer<const DataCatalog> catalog(context.catalog);
  const bool hadHost = bool(context.capabilityHost);
  const bool hadOnnx = bool(context.onnx);
  const bool hadTileOperations = bool(context.tileOperations);
  const bool hadTasks = bool(m_taskService);
  const auto rebound = [&] {
    return !owner || m_bindingRevision != bindingRevision ||
        (hadHost && !context.capabilityHost) || (hadOnnx && !context.onnx) ||
        (hadTileOperations && !context.tileOperations) || (hadTasks && !m_taskService) ||
        (context.catalog && !catalog);
  };
  if (rebound()) return fail("context_rebound");
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
  // H4：operation_list 依赖 Host/Recovery/TaskService，无宿主即不可执行——不经
  // 该无条件放行，恒走下方 hosted 资格路径（不向模型广播必然失败的调用）。
  const bool operationList=name==QLatin1String("paleo.operation_list");
  if(!context.requireCapabilityHost && !python && !operationList) {result.ready=true;return result;}
  if(spec.legacyAdapter)return fail("legacy_tool_requires_versioned_adapter");
  if(!context.capabilityHost)return fail("host_unavailable");
  const auto live = context.capabilityHost->context();
  if (rebound()) return fail("context_rebound");
  auto expected = live;
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
  // H4：operation_list 只需 Read（写权限不足不阻止只读发现）；扫描走
  // TaskService worker，资格在此精确声明，不假设调用方替它检查。
  if(operationList) {
    if(!context.capabilityHost->recovery())return fail("host_unavailable");
    if(!m_taskService)return fail("task_service_unavailable");
    result.ready=true;return result;
  }
  if((name==QLatin1String("paleo.query_project") || name==QLatin1String("paleo.asset_lineage")) &&
      (!context.catalog || !context.catalog->isOpen()))return fail("catalog_unavailable");
  if(name==QLatin1String("paleo.well_facies_prediction")) {
    const auto code = wellFaciesReadiness(args, listing);
    if (rebound()) return fail("context_rebound");
    if (!code.isEmpty()) return fail(code);
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
  if (QThread::currentThread() != thread()) return {};
  const QPointer<const AiChatToolRunner> owner(this);
  const Context snapshot = m_context;
  const QPointer<const DataCatalog> catalog(snapshot.catalog);
  const auto lostOwner = [&] { return !owner || (snapshot.catalog && !catalog); };
  const quint64 bindingRevision = m_bindingRevision;
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
      if (topicIndex >= 0) {
        QStringList narrowed;
        for (const auto &topic : spec.parameters[topicIndex].enumValues) {
          if (readinessForSpecInContext(spec, {{"topic", topic}}, true, snapshot, bindingRevision).ready)
            narrowed.append(topic);
          if (lostOwner()) return {};
        }
        if (!narrowed.isEmpty()) {
          candidate.parameters[topicIndex].enumValues = narrowed;
          tools.append(candidate);
        }
        continue;
      }
    }
    if (readinessForSpecInContext(candidate, {}, true, snapshot, bindingRevision).ready) tools.append(candidate);
    if (lostOwner()) return {};
  }
  if (m_bindingRevision != bindingRevision)
    tools.removeIf([](const auto &spec) { return spec.name != QLatin1String("paleo.read_result"); });
  return tools;
}
