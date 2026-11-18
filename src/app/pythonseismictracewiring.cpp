// 层：组装根
#include "pythonseismictracewiring.h"
#include "pythoninputbinding_internal.h"
#include "workflow/documentsession.h"
#include "catalog/datacatalog.h"
#include <QFileInfo>
#include <QJsonArray>
#include <QJsonDocument>
namespace {
using namespace paleo;
using namespace paleo::python::inputbinding;
QString mappedSource(const Scope &scope,const document::SourceRef &reference,const QString &model,document::SourceRecord &out) {
  const auto code=python::inputbinding::mappedSource(scope,reference,model,out,QStringLiteral("python_seismic_source_mapping_unavailable"));
  if(!code.isEmpty())return code;
  if(out.fingerprint.kind!=document::FingerprintKind::Sha256)return "python_seismic_source_mapping_unavailable";
  const QFileInfo file(out.path);
  if(!file.isAbsolute()||!file.isFile()||file.isSymLink()||file.isJunction()||file.size()<3844||
      file.size()>artifacts::AuthenticatedArtifactReader::MaxSeismicSourceBytes)return "python_seismic_source_unavailable";
  return {};
}
}
paleo::python::SeismicTracePlanProvider assemblePythonSeismicTraceProvider(DataCatalog *catalog,
    paleo::document::DocumentSession *document,paleo::capability::CapabilityHost *host) {
  const Scope scope{catalog,document,host};
  return [scope](const capability::ExecuteRequest &request,python::SeismicTraceReadPlan &plan,std::function<QString()> &validate) {
    plan={};validate={};if(!live(scope))return QStringLiteral("document_unavailable");
    const auto expected=scope.host->context();if(!live(scope))return QStringLiteral("document_unavailable");
    if(request.capabilityId!=QLatin1String(python::SeismicTraceCapability)||request.contractVersion!=1)return QStringLiteral("execution_adapter_unavailable");
    if(request.bindingMode!="pinned-value"||!request.parameters.isEmpty())return QStringLiteral("invalid_python_seismic_request");
    python::SeismicTraceSelection selected;QString code;
    if(!python::seismicTraceSelectionFromJson(QJsonObject::fromVariantMap(request.inputs),selected,&code))return code;
    if(request.documentId!=expected.documentId||request.documentId!=selected.source.documentId||request.callerScope.isEmpty())return QStringLiteral("python_scope_required");
    code=checkScope(scope,expected);if(!code.isEmpty())return code;
    const auto model=modelRoot(scope);if(model.isEmpty())return QStringLiteral("python_seismic_model_unavailable");
    document::SourceRecord source;code=mappedSource(scope,selected.source,model,source);if(!code.isEmpty())return code;
    plan.reader=scope.document->seismicSourceReader();if(!plan.reader)return QStringLiteral("python_seismic_reader_unavailable");
    plan.selection=selected;plan.source=source;plan.modelDirectory=model;plan.projectDir=QFileInfo(scope.catalog->projectDir()).canonicalFilePath();
    plan.documentId=request.documentId;plan.callerScope=request.callerScope;
    // Stable binding/source/caller scope, independent of operation/client IDs.
    const QJsonArray identity{request.documentId,QString::number(scope.document->epoch()),source.versionId,request.callerScope,source.ref.key()};
    plan.authenticatedSource={QString::fromUtf8(QJsonDocument(identity).toJson(QJsonDocument::Compact)),source.path,source.ref.fileRevisionId,QFileInfo(source.path).size()};
    const auto reader=plan.reader;const auto size=plan.authenticatedSource.byteSize;
    validate=[scope,expected,source,model,reader,size] {
      auto code=checkScope(scope,expected);if(!code.isEmpty())return code;
      if(!samePath(modelRoot(scope),model))return QStringLiteral("python_seismic_model_changed");
      if(scope.document->seismicSourceReader()!=reader)return QStringLiteral("source_revision_changed");
      document::SourceRecord current;code=mappedSource(scope,source.ref,model,current);if(!code.isEmpty())return code;
      if(current.versionId!=source.versionId||current.assetId!=source.assetId||current.path!=source.path||QFileInfo(current.path).size()!=size)
        return QStringLiteral("source_revision_changed");return QString();
    };
    return validate();
  };
}
