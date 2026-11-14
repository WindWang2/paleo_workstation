// 层：组装根
#include "pythonanalysiswiring.h"
#include "catalog/datacatalog.h"
#include "domain/wellref.h"
#include "services/pythonanalysisselection.h"
#include "services/pythonanalysiscapabilityspec.h"
#include "services/pythontrustedreadiness.h"
#include "services/pythongenerated.h"
#include "workflow/pythongeneratedoperation.h"
#include "workflow/documentsession.h"
#include <QCryptographicHash>
#include <QDir>
#include <QFile>
#include <QFileInfo>
#include <QJsonArray>

namespace {
using namespace paleo;
struct Scope {
  QPointer<DataCatalog> catalog;
  QPointer<document::DocumentSession> document;
  QPointer<capability::CapabilityHost> host;
};
QString checkScope(const Scope &scope,const host::CallContext &expected,const document::WellRef &well) {
  if(!scope.catalog || !scope.catalog->isOpen() || !scope.document || !scope.document->isOpen() || !scope.host)
    return "document_unavailable";
  const auto code=host::checkContext(scope.host->context(),expected,host::Read|host::PrivateStage|host::LocalExecute);
  if(!scope.catalog || !scope.document || !scope.host)return "document_unavailable";
  if(!code.isEmpty())return code;
  if(scope.document->documentId()!=well.documentId || expected.documentId!=well.documentId)return "well_document_mismatch";
  const auto entity=scope.catalog->storedEntityById(well.catalogEntityId);
  if(entity.entityType!="well" || entity.id!=well.catalogEntityId || entity.extra.value("areaModelWellId").toLongLong()!=well.modelWellId)
    return "well_mapping_changed";
  return {};
}
QString templateStatus(const QString &root,python::AnalysisKind kind,const python::ExecutionPolicy &policy) {
  if(root.isEmpty() || !QFileInfo(root).isDir())return "python_template_package_missing";
  QFile file(QDir(root).filePath(python::analysisKindId(kind)+".py"));const QFileInfo info(file);
  if(!info.isFile() || info.isSymLink() || !file.open(QIODevice::ReadOnly) || file.size()<1 || file.size()>policy.codeBytes)
    return "python_template_unavailable";
  const auto bytes=file.read(policy.codeBytes+1);
  return bytes.size()<=policy.codeBytes && QString::fromLatin1(QCryptographicHash::hash(bytes,QCryptographicHash::Sha256).toHex())==python::analysisTemplateSha256(kind)?
    QString():QStringLiteral("python_template_changed");
}
}
paleo::python::AnalysisPlanProvider assemblePythonAnalysisProvider(DataCatalog *catalog,
    paleo::document::DocumentSession *document,paleo::capability::CapabilityHost *host) {
  const Scope scope{catalog,document,host};
  return [scope](const capability::ExecuteRequest &request,python::AnalysisReadPlan &plan,std::function<QString()> &validate) {
    plan={};validate={};
    if(!scope.host || !scope.document || !scope.catalog)return QStringLiteral("python_analysis_provider_unavailable");
    const auto expected=scope.host->context();
    python::AnalysisKind kind;
    if(request.capabilityId==QLatin1String(python::WellQualityCapability))kind=python::AnalysisKind::WellQuality;
    else if(request.capabilityId==QLatin1String(python::CorrelationCapability))kind=python::AnalysisKind::AttributeCorrelation;
    else if(request.capabilityId==QLatin1String(python::GeneratedCapability))kind=python::AnalysisKind::Generated;
    else return QStringLiteral("execution_adapter_unavailable");
    if(request.bindingMode!="pinned-value" || !request.parameters.isEmpty())return QStringLiteral("invalid_python_analysis_request");
    python::AnalysisSelectionRequest selection;QString code;
    if(!python::analysisSelectionFromJson(QJsonObject::fromVariantMap(request.inputs),kind,selection,&code))return code;
    code=checkScope(scope,expected,selection.well);if(!code.isEmpty())return code;
    if(request.documentId!=expected.documentId || request.callerScope.isEmpty())return QStringLiteral("python_scope_required");
    plan.kind=kind;plan.projectDir=QFileInfo(scope.catalog->projectDir()).canonicalFilePath();
    const auto model=QDir(plan.projectDir).filePath("artifacts/metadata/area-model");
    if(plan.projectDir.isEmpty() || QFileInfo(model).canonicalFilePath()!=model)return QStringLiteral("python_analysis_model_unavailable");
    plan.modelDirectory=model;plan.documentId=request.documentId;plan.callerScope=request.callerScope;
    plan.expectedHead.available=true;plan.expectedHead.storeId=selection.well.modelStoreIdentity.toStdString();
    plan.expectedHead.generation=selection.well.modelGeneration.toStdString();plan.expectedHead.sequence=selection.well.queryHead;
    plan.alignment=selection.alignment;plan.sampling=selection.sampling;plan.seed=selection.seed;
    for(auto selected:selection.curves) {
      const auto source=scope.document->source(selected.source.ref);
      if(!scope.document || !scope.host || !scope.catalog)return QStringLiteral("document_unavailable");
      if(source.ref!=selected.source.ref || source.versionId!=selected.sourceVersionId ||
          source.fingerprint.stored()!=source.ref.fileRevisionId || QFileInfo(source.modelDirectory).canonicalFilePath()!=model)
        return QStringLiteral("python_analysis_source_mismatch");
      selected.source=source;plan.curves.append(selected);
    }
    const auto sources=plan.curves;const auto well=selection.well;
    validate=[scope,expected,sources,well] {
      auto code=checkScope(scope,expected,well);if(!code.isEmpty())return code;
      for(const auto &selected:sources) {
        const auto current=scope.document->source(selected.source.ref);
        if(!scope.document || !scope.host || !scope.catalog)return QStringLiteral("document_unavailable");
        if(current.ref!=selected.source.ref || current.versionId!=selected.sourceVersionId || current.path!=selected.source.path ||
            QFileInfo(current.modelDirectory).canonicalFilePath()!=QFileInfo(selected.source.modelDirectory).canonicalFilePath())
          return QStringLiteral("source_revision_changed");
      }
      return QString();
    };
    return validate();
  };
}
QJsonObject pythonAnalysisReadiness(std::shared_ptr<paleo::python::RuntimeProvider> provider,const QString &root,
    std::shared_ptr<paleo::python::TrustedReadinessProvider> execution) {
  QJsonArray capabilities;bool ready=bool(provider);QString code;
  if(!provider)code="python_environment_disabled";
  else if(!provider->policy().allowTrusted)code="python_execution_denied";
  else {const auto runtime=provider->snapshot();if(!runtime.ready())code=runtime.code;}
  const auto backend=execution?execution->snapshot():QJsonObject{{"executionReady",false},{"code","python_trusted_execution_unprobed"}};
  for(const auto kind:{python::AnalysisKind::WellQuality,python::AnalysisKind::AttributeCorrelation}) {
    auto missing=code;if(missing.isEmpty())missing=templateStatus(root,kind,provider->policy());
    const bool templateReady=missing.isEmpty();
    if(missing.isEmpty() && !backend.value("executionReady").toBool())missing=backend.value("code").toString("python_trusted_execution_unprobed");
    ready=ready && missing.isEmpty();capabilities.append(QJsonObject{{"capabilityId",python::analysisCapabilityId(kind)},
      {"contractVersion",1},{"templateReady",templateReady},{"ready",missing.isEmpty()},{"code",missing.isEmpty()?"ok":missing}});
  }
  return {{"ready",ready},{"capabilities",capabilities},{"execution",backend}};
}
paleo::capability::CapabilityExecutionAdapter gateTrustedAnalysis(paleo::capability::CapabilityExecutionAdapter adapter,
    std::shared_ptr<paleo::python::TrustedReadinessProvider> execution) {
  const auto capture=adapter.capture;
  adapter.capture=[capture,execution](const auto &request,const auto &intent,QString *error) {
    if(!capture){if(error)*error="execution_adapter_unavailable";return paleo::capability::CapabilityPreparation{};}
    const auto state=execution?execution->snapshot():QJsonObject{};
    if(!state.value("executionReady").toBool()) {
      if(error)*error=state.value("code").toString("python_trusted_execution_unprobed");return paleo::capability::CapabilityPreparation{};
    }
    return capture(request,intent,error);
  };
  return adapter;
}

bool assembleAgentPython(AiChatToolRunner *runner,AiChatToolContext &context,DataCatalog *catalog,
    paleo::document::DocumentSession *document,paleo::capability::CapabilityHost *host,
    const std::function<bool()> &bindingIsCurrent) {
  const QPointer<AiChatToolRunner> owner(runner);
  const auto current=[&]{return owner && bindingIsCurrent && bindingIsCurrent();};
  if(!current())return false;
  paleo::python::RuntimeConfig python;
  python.root=qEnvironmentVariable("PALEO_AGENT_PYTHON_ROOT");
  python.manifestSha256=qEnvironmentVariable("PALEO_AGENT_PYTHON_MANIFEST_SHA256");
  python.enabled=!python.root.isEmpty();
  paleo::python::ExecutionPolicy policy;
  policy.allowTrusted=qEnvironmentVariable("PALEO_AGENT_PYTHON_ENABLE_TRUSTED")==QLatin1String("1");
  policy.allowGenerated=qEnvironmentVariable("PALEO_AGENT_PYTHON_ENABLE_GENERATED")==QLatin1String("1");
  const auto priorRuntime=runner->pythonRuntime();
  const bool reuseRuntime=priorRuntime && priorRuntime->configuration().root==python.root &&
    priorRuntime->configuration().manifestSha256==python.manifestSha256 && priorRuntime->policy().json()==policy.json();
  context.pythonRuntime=reuseRuntime?priorRuntime:std::make_shared<paleo::python::RuntimeProvider>(std::move(python),policy);
  const auto priorTrusted=runner->pythonTrusted();const bool reuseTrusted=reuseRuntime && bool(priorTrusted);
  context.pythonTrusted=reuseTrusted?priorTrusted:std::make_shared<paleo::python::TrustedReadinessProvider>(context.pythonRuntime);
  const auto priorSandbox=runner->pythonSandbox();const bool reuseSandbox=reuseRuntime && bool(priorSandbox);
  context.pythonSandbox=reuseSandbox?priorSandbox:std::make_shared<paleo::python::SandboxProvider>(context.pythonRuntime);
  const auto scripts=qEnvironmentVariable("PALEO_AGENT_PYTHON_SCRIPTS_ROOT");
  context.runtimeReadiness=[provider=context.pythonRuntime,execution=context.pythonTrusted,sandbox=context.pythonSandbox,scripts](const QString &name,const QJsonObject &args,bool listing) {
    if(name==QLatin1String(paleo::python::GeneratedTool)) {
      if(!provider->policy().allowGenerated)return QStringLiteral("python_execution_denied");
      const auto runtime=provider->snapshot();if(!runtime.ready())return runtime.code;
      if(!listing && args.value("publish_intent")==QLatin1String("publish"))return QString();
      const auto status=sandbox->snapshot();return status.value("ready").toBool()?QString():status.value("code").toString("python_sandbox_unavailable");
    }
    if(name!=QLatin1String(paleo::python::WellQualityTool) && name!=QLatin1String(paleo::python::CorrelationTool))return QString();
    if(!provider->policy().allowTrusted)return QStringLiteral("python_execution_denied");
    const auto runtime=provider->snapshot();if(!runtime.ready())return runtime.code;
    // Frozen result publication does not need the original template directory.
    if(!listing && args.value("publish_intent")==QLatin1String("publish"))return QString();
    const auto id=name==QLatin1String(paleo::python::WellQualityTool)?paleo::python::WellQualityCapability:paleo::python::CorrelationCapability;
    for(const auto &value:pythonAnalysisReadiness(provider,scripts,execution).value("capabilities").toArray()) {
      const auto capability=value.toObject();if(capability.value("capabilityId")==QLatin1String(id))
        return capability.value("ready").toBool()?QString():capability.value("code").toString();
    }
    return QStringLiteral("python_template_unavailable");
  };
  if(host)for(const auto kind:{paleo::python::AnalysisKind::WellQuality,paleo::python::AnalysisKind::AttributeCorrelation})
    host->registerExecutionAdapter(paleo::python::analysisCapabilityId(kind),gateTrustedAnalysis(paleo::python::analysisScriptAdapter(kind,
      context.pythonRuntime,scripts,assemblePythonAnalysisProvider(catalog,document,host)),context.pythonTrusted));
  if(host)host->registerExecutionAdapter(QLatin1String(paleo::python::GeneratedCapability),
    paleo::python::generatedScriptAdapter(context.pythonRuntime,context.pythonSandbox,assemblePythonAnalysisProvider(catalog,document,host)));
  if(!reuseRuntime && context.pythonRuntime->configuration().enabled && runner->taskService()) {
    const auto provider=context.pythonRuntime;
    runner->taskService()->start(QObject::tr("Python 环境状态"),[provider](PaleoTask *task) {
      const auto cancelled=[task]{return task->cancelRequested();};
      if(provider->policy().allowGenerated)provider->inspectIntegrity(cancelled);
      else provider->inspect(cancelled);
      return QString();
    });
  }
  if(!current())return false;
  if(!reuseTrusted && policy.allowTrusted && context.pythonRuntime->configuration().enabled && runner->taskService()) {
    const auto execution=context.pythonTrusted;
    runner->taskService()->start(QObject::tr("Python 环境状态"),[execution](PaleoTask *task) {
      execution->inspect([task]{return task->cancelRequested();});return QString();
    });
  }
  if(!current())return false;
  if(!reuseSandbox && policy.allowGenerated && context.pythonRuntime->configuration().enabled && runner->taskService()) {
    const auto sandbox=context.pythonSandbox;
    runner->taskService()->start(QObject::tr("Python 隔离状态"),[sandbox](PaleoTask *task) {
      sandbox->inspect([task]{return task->cancelRequested();});return QString();
    });
  }
  return current();
}
