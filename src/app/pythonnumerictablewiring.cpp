// 层：组装根
#include "pythonnumerictablewiring.h"
#include "pythoninputbinding_internal.h"
#include "workflow/documentsession.h"
#include "catalog/datacatalog.h"
#include <QFileInfo>

namespace {
using namespace paleo;
using namespace paleo::python::inputbinding;
QString mappedSource(const Scope &scope,const document::SourceRef &reference,const QString &model,document::SourceRecord &out) {
  return python::inputbinding::mappedSource(scope,reference,model,out,QStringLiteral("python_table_source_mapping_unavailable"));
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
