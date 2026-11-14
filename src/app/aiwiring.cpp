// 层：组装根
#include "aiwiring.h"
#include "agentproviderwiring.h"
#include "ai/onnxpredictionservice.h"
#include "wellproviderwiring.h"
#include "pythonanalysiswiring.h"
#include "services/pythonanalysiscapabilityspec.h"
#include "services/pythonruntime.h"
#include "services/pythoncapabilityspec.h"
#include "workflow/pythonscriptoperation.h"
#include "workflow/capabilityhost.h"
#include "workflow/capabilityrecovery.h"
#include "workflow/tileoperationservice.h"
#include "workflow/documentsession.h"
#include "catalog/datacatalog.h"
#include <QJsonArray>
#include <QFileInfo>
#include <QCoreApplication>

#include "../ai/remotepredictrouter.h"
#include "../domain/arearules.h"
#include "../workflow/aichatcontroller.h"
#include "../workflow/aiassistworkflow.h"
#include "../workflow/mappingworkbench.h"
#include "../workflow/wellpredictionservice.h"
#include "../qgis/qgislayerservice.h"
#if PALEO_HAVE_ORT
#include "../ai/onnxlocalpredictor.h"
#endif

// app/ — AI 装配实现（方向51）。
//
// 唯一的远端预测装配点：产品（appcontext）与测试（tst_aiwiring 的装配断言，
// 以及 tst_mappingworkbench 的两条用例）都走这两个函数，因此「装配出来的是
// router 而不是替身」是一条可执行的断言，而不是靠读代码的人自觉。
RemotePredictionAssembly assembleRemotePrediction(
    const RemotePredictConfig &config, PaleoOnnxService *onnx,
    const QString &localModel, QObject *parent) {
  RemotePredictionAssembly out;
  out.config = config;
  out.statusHint = config.statusHint();

  HttpRemoteTransport *transport = nullptr;
  if (config.usable()) {
    transport = new HttpRemoteTransport(config.baseUrl);
    out.transportBound = true;
  }
  auto *router = new RemotePredictionRouter(transport, parent);
  if (transport)
    transport->setParent(router); // 生命周期挂在 router 上

  const QString model = localModel.isEmpty() ? config.model : localModel;
#if PALEO_HAVE_ORT
  if (onnx && !model.isEmpty())
    router->setLocalFallback(onnx, model); // 本地 ORT 降级（有 samples 才可用）
#else
  Q_UNUSED(onnx)
  Q_UNUSED(model)
#endif
  out.router = router;
  out.service = router;
  return out;
}

RemotePredictionAssembly installRemotePrediction(MappingWorkbench *bench,
                                                 const RemotePredictConfig &config,
                                                 PaleoOnnxService *onnx,
                                                 const QString &localModel,
                                                 QObject *parent) {
  const RemotePredictionAssembly assembled =
    assembleRemotePrediction(config, onnx, localModel, parent);
  if (bench) {
    bench->setPredictionService(assembled.service);
    bench->setPredictionStatusHint(assembled.statusHint);
    bench->setWellPredictionService(new WellPredictionService(parent ? parent : bench));
  }
  return assembled;
}

namespace {
QJsonObject layerValue(const LayerDeclaration &d) {
  if(d.layerId.isEmpty())return {{"missing",true}};
  return {{"layerId",d.layerId},{"horizon",d.horizon},{"type",d.type},{"source",d.source},
    {"styleRef",d.styleRef},{"group",d.group},{"title",d.title}};
}
QHash<QString,LayerDeclaration> layerSnapshot(QgisLayerService *layers,bool &ok) {
  QVector<LayerDeclaration> values;ok=layers && layers->tryDeclared(&values);
  QHash<QString,LayerDeclaration> out;if(ok)for(const auto &d:values)out.insert(d.layerId,d);return out;
}
LayerDeclaration desiredTileLayer(const QJsonObject &target,const QJsonArray &members,DataCatalog *catalog) {
  LayerDeclaration d;if(!catalog)return d;
  for(const auto &item:members) {
    const auto member=item.toObject();if(member.value("portId")!=target.value("portId"))continue;
    const auto version=catalog->versionById(member.value("versionId").toString());
    if(version.id.isEmpty() || version.assetId!=member.value("assetId").toString())return d;
    d.layerId=target.value("layerId").toString();d.horizon=target.value("horizon").toString();
    d.type="raster";d.source=DataCatalog::resolvedVersionPath(catalog->projectDir(),version);
    d.title=target.value("title").toString();d.group="03_Predict";if(d.source.isEmpty())return {};return d;
  }
  return d;
}
QString tilePresentationState(const QJsonObject &intent,QgisLayerService *layers,DataCatalog *catalog,
                               paleo::document::DocumentSession *document) {
  if(intent.value("schema")!="paleo.tile-presentation.v2" || !document || !document->isOpen() ||
     document->documentId()!=intent.value("documentId").toString())return "unavailable:document";
  bool ok=false;const auto current=layerSnapshot(layers,ok);if(!ok)return "unavailable:manifest";
  const auto targets=intent.value("targets").toArray();const auto members=intent.value("members").toArray();
  if(targets.size()!=3)return "unavailable:targets";int applied=0;
  for(const auto &item:targets) {
    const auto target=item.toObject();const auto id=target.value("layerId").toString();
    const auto hash=paleo::capability::operationDigest(layerValue(current.value(id)));
    const auto desired=desiredTileLayer(target,members,catalog);
    if(!layers->nativePresentationMatches(id,target.value("expectedNativeToken").toString(),desired.source))return "changed";
    if(!desired.layerId.isEmpty() && hash==paleo::capability::operationDigest(layerValue(desired))) {
      if(layers->nativePresentationSourceMatches(id,desired.source))++applied;
    } else if(hash!=target.value("expectedDeclarationHash").toString())return "changed";
  }
  return applied==3?QStringLiteral("presented"):intent.value("expectedState").toString();
}
}

paleo::TileOperationService *installTileOperationServices(AiChatToolRunner *runner,PaleoOnnxService *onnx,
    QgisLayerService *layers,DataCatalog *catalog,paleo::capability::CapabilityHost *host,paleo::document::DocumentSession *document) {
  if(!runner || !host)return nullptr;
  auto *service=runner->findChild<paleo::TileOperationService *>("agent-tile-operations",Qt::FindDirectChildrenOnly);
  if(!service) {service=new paleo::TileOperationService(runner);service->setObjectName("agent-tile-operations");}
  const QPointer<paleo::document::DocumentSession> owner(document);const QPointer<QgisLayerService> view(layers);
  service->configure(host,runner->taskService(),onnx,[owner,catalog](const QString &id,paleo::FrozenRasterInput &out,QString *error) {
    const auto fail=[&](const QString &code){if(error)*error=code;return false;};
    if(!owner || !owner->isOpen() || !catalog || !catalog->isOpen())return fail("document_unavailable");
    const auto version=catalog->versionById(id);
    if(id.isEmpty() || version.id!=id)return fail("input_version_missing");
    out={};out.versionId=id;out.sha256=version.sha256;
    out.catalogFeatureContract=QJsonObject::fromVariantMap(version.extra.value("featureContract").toMap());
    out.path=QFileInfo(DataCatalog::resolvedVersionPath(catalog->projectDir(),version)).canonicalFilePath();
    if(out.path.isEmpty())return fail("raster_source_unavailable");
    if(out.sha256.isEmpty())return fail("raster_version_fingerprint_required");return true;
  },[owner,view](const QString &model,const QString &horizon,QString *error) {
    if(!view)return QJsonObject{};bool ok=false;const auto current=layerSnapshot(view,ok);
    if(!owner || !owner->isOpen() || !ok || horizon.isEmpty()) {
      if(error)*error="tile_presentation_context_unavailable";return QJsonObject{};
    }
    QJsonArray targets;const QString primary="aifacies."+horizon+'.'+model;
    const QStringList ids{primary,primary+".masked","confidence."+horizon+'.'+model};
    const QStringList ports{"OUTPUT","MASKED","CONFIDENCE"};
    const QStringList titles{QCoreApplication::translate("AiAssistWorkflow","%1 AI 相分类 %2").arg(horizon,model),
      QCoreApplication::translate("AiAssistWorkflow","%1 AI 相分类 %2（低置信掩膜）").arg(horizon,model),
      QCoreApplication::translate("AiAssistWorkflow","%1 置信度 %2").arg(horizon,model)};
    for(int i=0;i<3;++i) {
      const auto native=view->nativePresentationToken(ids[i]);
      if(native==QLatin1String("!unavailable")){if(error)*error="tile_native_presentation_state_unavailable";return QJsonObject{};}
      targets.append(QJsonObject{{"portId",ports[i]},{"layerId",ids[i]},
        {"horizon",horizon},{"title",titles[i]},{"expectedNativeToken",native},
        {"expectedDeclarationHash",paleo::capability::operationDigest(layerValue(current.value(ids[i])))}});
    }
    return QJsonObject{{"schema","paleo.tile-presentation.v2"},{"documentId",owner->documentId()},{"targets",targets},
      {"expectedState",paleo::capability::operationDigest(QJsonObject{{"targets",targets}})}};
  });
  host->setPresentationHandlers([owner,view,catalog](const QJsonObject &intent) {
    return tilePresentationState(intent,view,catalog,owner);
  },[owner,view,catalog](const QJsonObject &intent,const PublicationReceipt &receipt) {
    if(!receipt.valid())return QStringLiteral("pending");
    const auto members=receipt.value.value("members").toArray();
    for(const auto &entry:intent.value("members").toArray()) {
      const auto expected=entry.toObject();bool found=false;
      for(const auto &actual:members) {
        const auto value=actual.toObject();if(value.value("portId")==expected.value("portId") &&
          value.value("assetId")==expected.value("assetId") && value.value("versionId")==expected.value("versionId"))found=true;
      }
      if(!found)return QStringLiteral("pending");
    }
    for(const auto &item:intent.value("targets").toArray()) {
      const auto state=tilePresentationState(intent,view,catalog,owner);
      if(state==QLatin1String("presented"))return state;
      if(state.startsWith(QLatin1String("unavailable:")))return QStringLiteral("pending");
      if(state!=intent.value("expectedState").toString())return QStringLiteral("superseded");
      const auto desired=desiredTileLayer(item.toObject(),members,catalog);QString error;
      if(view && paleo::capability::operationDigest(layerValue(view->declaration(desired.layerId)))==
          paleo::capability::operationDigest(layerValue(desired)) && view->nativePresentationSourceMatches(desired.layerId,desired.source))continue;
      if(!view || desired.layerId.isEmpty() || !view->declareForPresentation(desired,&error))return QStringLiteral("pending");
    }
    return tilePresentationState(intent,view,catalog,owner)==QLatin1String("presented")?QStringLiteral("presented"):QStringLiteral("pending");
  });
  return service;
}

void bindChatToolRunner(AiChatController *chat, AiAssistWorkflow *assist,
                        QgisLayerService *layers, DataCatalog *catalog,
                        const QString &projectDir, const QString &documentId,
                        paleo::capability::CapabilityHost *host, paleo::document::DocumentSession *document) {
  if (!chat)
    return;
  const QPointer<AiChatController> chatOwner(chat);
  const QPointer<paleo::capability::CapabilityHost> hostOwner(host);
  const QPointer<paleo::document::DocumentSession> documentOwner(document);
  const QPointer<DataCatalog> catalogOwner(catalog);
  const QPointer<QgisLayerService> layerOwner(layers);
  // assist 可为空（无 ORT 构建）：只绑只读工程工具；tile/horizon 执行面
  // 缺席由执行器按调用如实报错（不因装配期缺席而误报「未接线」）。
  if (assist)
    chat->toolRunner()->setWorkflow(assist);
  AiChatToolContext context;
  paleo::python::RuntimeConfig python;
  python.root=qEnvironmentVariable("PALEO_AGENT_PYTHON_ROOT");
  python.manifestSha256=qEnvironmentVariable("PALEO_AGENT_PYTHON_MANIFEST_SHA256");
  python.enabled=!python.root.isEmpty();
  paleo::python::ExecutionPolicy pythonPolicy;
  pythonPolicy.allowTrusted=qEnvironmentVariable("PALEO_AGENT_PYTHON_ENABLE_TRUSTED")==QLatin1String("1");
  const auto priorPython=chat->toolRunner()->pythonRuntime();
  const bool reusePython=priorPython && priorPython->configuration().root==python.root &&
    priorPython->configuration().manifestSha256==python.manifestSha256 && priorPython->policy().json()==pythonPolicy.json();
  context.pythonRuntime=reusePython?priorPython:std::make_shared<paleo::python::RuntimeProvider>(std::move(python),pythonPolicy);
  if(host)host->registerExecutionAdapter(QLatin1String(paleo::python::SummaryCapability),
    paleo::python::trustedScriptAdapter(context.pythonRuntime,qEnvironmentVariable("PALEO_AGENT_PYTHON_SCRIPTS_ROOT")));
  const auto scripts=qEnvironmentVariable("PALEO_AGENT_PYTHON_SCRIPTS_ROOT");
  context.runtimeReadiness=[provider=context.pythonRuntime,scripts](const QString &name,const QJsonObject &args,bool listing) {
    if(name!=QLatin1String(paleo::python::WellQualityTool) && name!=QLatin1String(paleo::python::CorrelationTool))return QString();
    if(!provider->policy().allowTrusted)return QStringLiteral("python_execution_denied");
    const auto runtime=provider->snapshot();if(!runtime.ready())return runtime.code;
    // Publishing an authenticated frozen result does not require its original supply directory.
    if(listing || args.value("publish_intent")==QLatin1String("publish"))return QString();
    const auto id=name==QLatin1String(paleo::python::WellQualityTool)?paleo::python::WellQualityCapability:paleo::python::CorrelationCapability;
    for(const auto &value:pythonAnalysisReadiness(provider,scripts).value("capabilities").toArray()) {
      const auto capability=value.toObject();if(capability.value("capabilityId")==QLatin1String(id))
        return capability.value("ready").toBool()?QString():capability.value("code").toString();
    }
    return QStringLiteral("python_template_unavailable");
  };
  if(host)for(const auto kind:{paleo::python::AnalysisKind::WellQuality,paleo::python::AnalysisKind::AttributeCorrelation})
    host->registerExecutionAdapter(paleo::python::analysisCapabilityId(kind),paleo::python::analysisScriptAdapter(kind,
      context.pythonRuntime,scripts,assemblePythonAnalysisProvider(catalog,document,host)));
  if(document)context.sourceResolver=[guard=QPointer<paleo::document::DocumentSession>(document)](const auto &ref) {
    return guard && guard->isOpen()?guard->source(ref):paleo::document::SourceRecord{};
  };
  if(!reusePython && context.pythonRuntime->configuration().enabled && chat->toolRunner()->taskService()) {
    const auto provider=context.pythonRuntime;
    chat->toolRunner()->taskService()->start(QObject::tr("Python 环境状态"),[provider](PaleoTask *task) {
      provider->inspect([task]{return task->cancelRequested();});return QString();
    });
  }
  if(!chatOwner || (host && !hostOwner) || (document && !documentOwner) ||
      (catalog && !catalogOwner) || (layers && !layerOwner))return;
  // 层位与栅格取数在绑定/重绑时快照；horizonGridFetch 每次取数重解析声明，
  // 换工程后随 appcontext 的重绑刷新 horizon 名。
  context.horizon = AreaRules::active().targetHorizon;
  context.onnx = assist ? assist->onnxService() : nullptr;
  context.tileOperations=installTileOperationServices(chat->toolRunner(),context.onnx,layers,catalog,host,document);
  context.gridFetch = AiAssistWorkflow::horizonGridFetch(layers);
  if (document) context.trackingInput = trackingInputProvider(document);
  // 方向77：只读工程上下文（catalog 指针稳定性由 importSvc 保证）。
  context.catalog = catalog;
  context.projectDir = projectDir;
  context.documentId = documentId;
  context.capabilityHost = host;
  context.requireCapabilityHost = host != nullptr;
  context.documentEpoch = host ? host->context().epoch : 0;
  context.frozenFaciesInput=assembleWellFaciesProvider(catalog,document,host);
  if(document)context.faciesContext=[guard=QPointer<paleo::document::DocumentSession>(document)] {
    return guard&&guard->isOpen()?guard->interpretationSelection().context:std::optional<paleo::document::InterpretationContext>{};
  };
  chat->toolRunner()->setContext(context);
  chat->setProjectBrief(AiChatToolRunner::projectBrief(catalog, projectDir));
}
