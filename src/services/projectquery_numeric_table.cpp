// 层：数据
#include "projectquery_internal.h"
#include "pythonnumerictable.h"
#include "pythonnumerictablequery.h"
#include <paleo_data/service.h>
#include <QDir>
#include <QFileInfo>
#include <QJsonDocument>

namespace paleo::query {
namespace {
bool samePath(const QString &left,const QString &right) {
#ifdef Q_OS_WIN
  return left.compare(right,Qt::CaseInsensitive)==0;
#else
  return left==right;
#endif
}
QString modelRoot(const DataCatalog *catalog) {
  const auto project=QFileInfo(catalog->projectDir()).canonicalFilePath();if(project.isEmpty())return {};
  const auto root=QDir(project).filePath("artifacts/metadata/area-model");const QFileInfo directory(root),database(root+"/data.sqlite");
  if(!directory.isDir()||directory.isSymLink()||directory.isJunction()||!samePath(directory.canonicalFilePath(),root)||
      !database.isFile()||database.isSymLink()||database.isJunction()||!samePath(database.canonicalFilePath(),root+"/data.sqlite"))return {};
  return directory.canonicalFilePath();
}
}
QJsonObject ProjectQueryCapability::Impl::numericTableProjection(QueryState &state,const QJsonObject &arguments,int bytes,bool &more) {
  more=false;document::SourceRef reference;qlonglong tableId=0;
  const auto invalid=python::numericTableProjectionQueryArguments(arguments,reference,tableId);if(!invalid.isEmpty())return queryFailure(invalid);
  if(reference.documentId!=state.scope.documentId)return queryFailure("source_document_mismatch");
  const auto owner=catalog;const auto resolver=sourceResolver;const auto root=modelRoot(owner);if(root.isEmpty())return queryFailure("python_table_model_unavailable");
  if(!resolver)return queryFailure("source_resolver_unavailable");document::SourceRecord source;
  try{source=resolver(reference);}catch(...){return queryFailure("source_resolver_failed");}
  if(!owner||!owner->isOpen())return queryFailure("document_unavailable");
  if(scope!=state.scope||catalog!=owner||bindingEpoch!=state.bindingEpoch)return queryFailure("cursor_scope_mismatch");
  const auto version=owner->versionById(source.versionId);const auto asset=owner->assetById(source.assetId);
  if(source.ref!=reference||source.versionId.isEmpty()||source.assetId.isEmpty()||version.id!=source.versionId||version.assetId!=source.assetId||asset.id!=source.assetId||
      version.extra.value("areaModelSourceId").toLongLong()!=reference.sourceId||source.fingerprint.stored()!=reference.fileRevisionId||
      document::SourceFingerprint::fromStored(version.sha256).stored()!=reference.fileRevisionId||!samePath(QFileInfo(source.modelDirectory).canonicalFilePath(),root))
    return queryFailure("python_table_source_mapping_unavailable");
  data::DataService sdk;std::string detail;if(!sdk.openProjectReadOnly(root.toUtf8().toStdString(),detail))return queryFailure("python_table_model_unavailable");
  data::SourceIndexReadOptions sourceBudget;sourceBudget.maxFieldBytes=qMin(8192,bytes);sourceBudget.maxTextBytes=sourceBudget.maxFieldBytes;
  data::SourceIndexReadStats sourceStats;data::SourceIndexEntry registered;
  if(!sdk.readSourceIndexEntry(reference.sourceId,registered,detail,sourceBudget,&sourceStats))return queryFailure(
    detail.find("Budget")!=std::string::npos?"query_response_budget_exceeded":"python_table_source_metadata_unavailable");
  if(!registered.active||registered.id!=reference.sourceId||QString::fromStdString(registered.sha256)!=reference.fileRevisionId)return queryFailure("python_table_source_changed");
  data::NumericTableReadOptions budget;budget.metadata.maxFieldBytes=qMin(8192,bytes);budget.metadata.maxTextBytes=qMin(32768,bytes);
  data::NumericTableDescriptor descriptor;data::NumericTableReadStats read;
  if(!sdk.describeNumericTableProjection(tableId,descriptor,detail,budget,&read)) {
    return queryFailure(detail=="NumericTableSchemaUnavailable"?"python_table_schema_unavailable":
      detail=="NumericTableProjectionUnavailable"?"python_table_projection_unavailable":
      detail.find("Budget")!=std::string::npos?"query_response_budget_exceeded":"python_table_metadata_unavailable");
  }
  if(read.cellsRead||read.cellTextBytesRead||read.outputBytes||sourceStats.textBytesRead<0||sourceStats.textBytesRead>sourceBudget.maxTextBytes||
      read.metadataBytesRead<0||read.metadataBytesRead>budget.metadata.maxTextBytes)return queryFailure("python_table_metadata_payload_violation");
  const auto &projection=descriptor.projection;
  if(projection.tableId!=tableId||projection.sourceId!=reference.sourceId||QString::fromStdString(projection.sourceSha256)!=reference.fileRevisionId)
    return queryFailure("python_table_source_mismatch");
  const auto cut=python::numericTableReadCutJson(descriptor.cut);const auto model=cut.value("model").toObject();
  state.modelToken=QString::fromUtf8(QJsonDocument(QJsonObject{{"store",model.value("storeId")},{"generation",model.value("generation")},
    {"sequence",model.value("sequence")}}).toJson(QJsonDocument::Compact));
  QJsonArray fields;QStringList ids;
  for(const auto &column:projection.columns) {
    ids.append(QString::fromStdString(column.id));QJsonArray nulls;for(const auto &text:column.nullTexts)nulls.append(QString::fromStdString(text));
    fields.append(QJsonObject{{"id",QString::fromStdString(column.id)},{"label",QString::fromStdString(column.label)},{"unit",QString::fromStdString(column.unit)},
      {"dtype",column.dtype==data::NumericTableDtype::Float64?"Float64":"Boolean"},{"source_column",column.sourceColumn},
      {"null_texts",nulls},{"missing_is_null",column.missingIsNull}});
  }
  python::NumericTableSelection selected;selected.source=reference;selected.tableId=tableId;selected.projectionRevision=QString::fromStdString(descriptor.revision);
  selected.columnIds=ids;selected.rowCount=int(qMin<std::int64_t>(data::NumericTableMaxRows,projection.rowCount));
  return {{"ok",true},{"code","ok"},{"contract","paleo.project.query.page.v1"},{"tool","paleo.query_project"},{"topic","numeric_table_projection"},
    {"metadataOnly",true},{"count",1},{"returnedCount",1},{"numericPayloadBytes",0},{"input_ref",python::numericTableSelectionJson(selected)},
    {"projection",QJsonObject{{"table_id",QString::number(tableId)},{"projection_revision",QString::fromStdString(descriptor.revision)},
      {"first_source_row",QString::number(projection.firstSourceRow)},{"row_count",QString::number(projection.rowCount)},
      {"axis","one_based_physical_source_row_i64"},{"columns",fields}}},{"readCut",cut},{"source_version_id",source.versionId},
    {"metadataTextBytes",QString::number(sourceStats.textBytesRead+read.metadataBytesRead)}};
}
}
