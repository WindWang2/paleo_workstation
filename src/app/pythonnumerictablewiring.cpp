// 层：组装根
#include "pythonnumerictablewiring.h"
#include "workflow/documentsession.h"
#include "catalog/datacatalog.h"
#include <QDir>
#include <QFileInfo>

namespace {
using namespace paleo;
struct Scope {QPointer<DataCatalog> catalog;QPointer<document::DocumentSession> document;QPointer<capability::CapabilityHost> host;};
bool live(const Scope &scope){return scope.catalog&&scope.document&&scope.host&&scope.catalog->isOpen()&&scope.document->isOpen();}
bool samePath(const QString &left,const QString &right) {
#ifdef Q_OS_WIN
  return left.compare(right,Qt::CaseInsensitive)==0;
#else
  return left==right;
#endif
}
QString checkScope(const Scope &scope,const host::CallContext &expected) {
  if(!live(scope))return "document_unavailable";const auto current=scope.host->context();
  if(!live(scope))return "document_unavailable";
  const auto code=host::checkContext(current,expected,host::Read|host::PrivateStage|host::LocalExecute);
  if(!code.isEmpty())return code;
  if(scope.document->documentId()!=expected.documentId)return "source_document_mismatch";
  return {};
}
QString modelRoot(const Scope &scope) {
  const auto project=QFileInfo(scope.catalog->projectDir()).canonicalFilePath();if(project.isEmpty())return {};
  const auto model=QDir(project).filePath("artifacts/metadata/area-model");const QFileInfo directory(model),database(model+"/data.sqlite");
  if(!directory.isDir()||directory.isSymLink()||directory.isJunction()||!samePath(directory.canonicalFilePath(),model)||
      !database.isFile()||database.isSymLink()||database.isJunction()||!samePath(database.canonicalFilePath(),model+"/data.sqlite"))return {};
  return directory.canonicalFilePath();
}
QString mappedSource(const Scope &scope,const document::SourceRef &reference,const QString &model,document::SourceRecord &out) {
  if(!live(scope))return "document_unavailable";out=scope.document->source(reference);
  if(!live(scope))return "document_unavailable";
  const auto version=scope.catalog->versionById(out.versionId);const auto asset=scope.catalog->assetById(out.assetId);
  if(out.ref!=reference||out.versionId.isEmpty()||out.assetId.isEmpty()||version.id!=out.versionId||version.assetId!=out.assetId||asset.id!=out.assetId||
      version.extra.value("areaModelSourceId").toLongLong()!=reference.sourceId||
      document::SourceFingerprint::fromStored(version.sha256).stored()!=reference.fileRevisionId||
      out.fingerprint.stored()!=reference.fileRevisionId||!samePath(QFileInfo(out.modelDirectory).canonicalFilePath(),model))
    return "python_table_source_mapping_unavailable";
  return {};
}
}
paleo::python::NumericTablePlanProvider assemblePythonNumericTableProvider(DataCatalog *catalog,
    paleo::document::DocumentSession *document,paleo::capability::CapabilityHost *host) {
  const Scope scope{catalog,document,host};
  return [scope](const capability::ExecuteRequest &request,python::NumericTableReadPlan &plan,std::function<QString()> &validate) {
    plan={};validate={};if(!live(scope))return QStringLiteral("document_unavailable");
    const auto expected=scope.host->context();if(!live(scope))return QStringLiteral("document_unavailable");
    if(request.capabilityId!=QLatin1String(python::NumericTableCapability)||request.contractVersion!=1)return QStringLiteral("execution_adapter_unavailable");
    if(request.bindingMode!="pinned-value"||!request.parameters.isEmpty())return QStringLiteral("invalid_python_table_request");
    python::NumericTableSelection selected;QString code;
    if(!python::numericTableSelectionFromJson(QJsonObject::fromVariantMap(request.inputs),selected,&code))return code;
    if(request.documentId!=expected.documentId||request.documentId!=selected.source.documentId||request.callerScope.isEmpty())return QStringLiteral("python_scope_required");
    code=checkScope(scope,expected);if(!code.isEmpty())return code;
    const auto model=modelRoot(scope);if(model.isEmpty())return QStringLiteral("python_table_model_unavailable");
    document::SourceRecord source;code=mappedSource(scope,selected.source,model,source);if(!code.isEmpty())return code;
    plan.selection=selected;plan.source=source;plan.modelDirectory=model;plan.projectDir=QFileInfo(scope.catalog->projectDir()).canonicalFilePath();
    plan.documentId=request.documentId;plan.callerScope=request.callerScope;
    validate=[scope,expected,source,model] {
      auto code=checkScope(scope,expected);if(!code.isEmpty())return code;
      if(!samePath(modelRoot(scope),model))return QStringLiteral("python_table_model_changed");
      document::SourceRecord current;code=mappedSource(scope,source.ref,model,current);if(!code.isEmpty())return code;
      if(current.versionId!=source.versionId||current.assetId!=source.assetId||current.path!=source.path)return QStringLiteral("source_revision_changed");
      return QString();
    };
    return validate();
  };
}
