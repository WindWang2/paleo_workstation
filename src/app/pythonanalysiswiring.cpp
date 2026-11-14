// 层：组装根
#include "pythonanalysiswiring.h"
#include "catalog/datacatalog.h"
#include "domain/wellref.h"
#include "services/pythonanalysisselection.h"
#include "services/pythonanalysiscapabilityspec.h"
#include "services/pythontrustedreadiness.h"
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
