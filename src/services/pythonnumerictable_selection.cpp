// 层：数据
#include "pythonnumerictable.h"
#include "pythongenerated.h"
#include <QJsonArray>
#include <QRegularExpression>
#include <QSet>
#include <QUuid>
#include <cmath>

namespace paleo::python {
namespace {
bool keys(const QJsonObject &value,std::initializer_list<const char*> names) {
  if(value.size()!=qsizetype(names.size()))return false;for(const auto *name:names)if(!value.contains(QLatin1String(name)))return false;return true;
}
bool decimal(const QJsonValue &value,qlonglong &number,bool positive=false) {
  bool valid=false;if(!value.isString())return false;number=value.toString().toLongLong(&valid);
  return valid&&number>=(positive?1:0)&&QString::number(number)==value.toString();
}
bool token(const QJsonValue &value) {
  if(!value.isString())return false;const auto text=value.toString();const auto bytes=text.toUtf8();
  return !text.trimmed().isEmpty()&&bytes.size()<=256&&!text.contains(QChar(0))&&QString::fromUtf8(bytes)==text;
}
QJsonObject object(const QJsonObject &properties) {
  QJsonArray required;for(const auto &key:properties.keys())required.append(key);
  return {{"type","object"},{"properties",properties},{"required",required},{"additionalProperties",false}};
}
QJsonObject text(int maximum=256){return {{"type","string"},{"minLength",1},{"maxLength",maximum}};}
QJsonObject integerString(bool positive=false){return {{"type","string"},{"pattern",positive?"^[1-9][0-9]{0,18}$":"^(0|[1-9][0-9]{0,18})$"}};}
QJsonObject inputs() {
  const auto source=object({{"documentId",text()},{"modelStoreId",QJsonObject{{"const","area-model"}}},
    {"sourceId",integerString(true)},{"fileRevisionId",QJsonObject{{"type","string"},{"pattern","^[0-9a-f]{64}$"}}}});
  return object({{"source_ref",source},{"table_id",integerString(true)},{"projection_revision",text(36)},
    {"column_ids",QJsonObject{{"type","array"},{"minItems",1},{"maxItems",data::NumericTableMaxColumns},{"uniqueItems",true},
      {"items",QJsonObject{{"type","string"},{"pattern","^[a-z][a-z0-9_]{0,63}$"}}}}},
    {"first_row",integerString()},{"row_count",QJsonObject{{"type","integer"},{"minimum",0},{"maximum",data::NumericTableMaxRows}}}});
}
}
QJsonObject numericTableSelectionJson(const NumericTableSelection &value) {
  return {{"source_ref",document::sourceRefJson(value.source)},{"table_id",QString::number(value.tableId)},
    {"projection_revision",value.projectionRevision},{"column_ids",QJsonArray::fromStringList(value.columnIds)},
    {"first_row",QString::number(value.firstRow)},{"row_count",value.rowCount}};
}
bool numericTableSelectionFromJson(const QJsonObject &value,NumericTableSelection &out,QString *error) {
  out={};const auto fail=[&](const char *code){out={};if(error)*error=QLatin1String(code);return false;};
  if(!keys(value,{"source_ref","table_id","projection_revision","column_ids","first_row","row_count"}))return fail("invalid_python_table_selection");
  const auto reference=value.value("source_ref").toObject();const auto digest=reference.value("fileRevisionId").toString();
  static const QRegularExpression sha("^[0-9a-f]{64}$"),identifier("^[a-z][a-z0-9_]{0,63}$");
  if(!value.value("source_ref").isObject()||!document::sourceRefFromJson(reference,out.source)||
      document::sourceRefJson(out.source)!=reference||out.source.modelStoreId!="area-model"||
      out.source.sourceId<=0||out.source.documentId.isEmpty()||out.source.documentId.toUtf8().size()>256||
      out.source.documentId.contains(QChar(0))||QString::fromUtf8(out.source.documentId.toUtf8())!=out.source.documentId||
      digest.size()!=64||!sha.match(digest).hasMatch())return fail("python_table_source_mismatch");
  if(!decimal(value.value("table_id"),out.tableId,true)||!decimal(value.value("first_row"),out.firstRow))return fail("python_table_window_out_of_range");
  const auto count=value.value("row_count");if(!count.isDouble()||!std::isfinite(count.toDouble())||count.toDouble()!=std::floor(count.toDouble())||
      count.toDouble()<0||count.toDouble()>data::NumericTableMaxRows)return fail("python_table_window_out_of_range");
  out.rowCount=count.toInt();const auto revision=value.value("projection_revision");
  if(!revision.isString()||revision.toString().size()!=36||QUuid(revision.toString()).isNull()||
      QUuid(revision.toString()).toString(QUuid::WithoutBraces)!=revision.toString())return fail("python_table_projection_required");
  out.projectionRevision=revision.toString();const auto columns=value.value("column_ids");
  if(!columns.isArray()||columns.toArray().isEmpty()||columns.toArray().size()>data::NumericTableMaxColumns)return fail("python_table_column_budget");
  QSet<QString> seen;
  for(const auto &column:columns.toArray()) {
    if(!column.isString()||column.toString()=="source_row"||column.toString().size()>64||
        !identifier.match(column.toString()).hasMatch()||seen.contains(column.toString()))return fail("python_table_column_invalid");
    seen.insert(column.toString());out.columnIds.append(column.toString());
  }
  if(error)error->clear();return true;
}
QJsonObject numericTableToolRequestSchema() {
  auto schema=generatedToolRequestSchema();auto properties=schema.value("properties").toObject();
  const auto publish=object({{"source_operation_id",text()}});properties["inputs"]=QJsonObject{{"oneOf",QJsonArray{inputs(),publish}}};
  auto parameters=properties.value("parameters").toObject();auto choices=parameters.value("oneOf").toArray();
  auto source=choices[0].toObject();auto fields=source.value("properties").toObject();auto code=fields.value("code").toObject();
  code["description"]="UTF-8 Python source <=16KiB. stdin: uint32LE JSON length + paleo.python-numeric-table-input.v1 metadata + binary. axis is <i8 source_row[n], then each selected column's <f8 or u1 values and u1 null mask (1=null); layout gives exact offsets/bytes. Preserve int64 physical rows, units, types and declared nulls; do not infer an MD axis. Output exactly paleo.python-output.v1 with REPORT paleo.python-generated-report.v1, bounded metrics/tables/notes. No paths, network, subprocesses or installation; host supplies source/ReadCut/code/runtime provenance.";
  fields["code"]=code;source["properties"]=fields;choices[0]=source;parameters["oneOf"]=choices;properties["parameters"]=parameters;schema["properties"]=properties;
  auto conditions=schema.value("allOf").toArray();auto phase=conditions[0].toObject();
  for(const auto &branch:{QString("then"),QString("else")}) {
    auto rules=phase.value(branch).toObject();auto selected=rules.value("properties").toObject();selected["inputs"]=branch=="then"?publish:inputs();
    if(branch=="else")selected["parameters"]=source;rules["properties"]=selected;phase[branch]=rules;
  }
  conditions[0]=phase;schema["allOf"]=conditions;return schema;
}
QString validateNumericTableToolArguments(const QJsonObject &arguments) {
  const QStringList allowed{"client_request_id","contract_version","binding_mode","publish_intent","inputs","parameters"};
  for(auto it=arguments.begin();it!=arguments.end();++it)if(!allowed.contains(it.key()))return "invalid_python_table_request";
  if(!token(arguments.value("client_request_id"))||arguments.value("binding_mode")!="pinned-value"||!arguments.value("parameters").isObject()||
      !arguments.value("inputs").isObject())return "invalid_python_table_request";
  if(arguments.contains("contract_version")&&arguments.value("contract_version")!=1)return "unsupported_contract_version";
  const auto phase=arguments.value("publish_intent").toString("compute-only");
  if(arguments.contains("publish_intent")&&!arguments.value("publish_intent").isString())return "unsupported_intent";
  if(phase=="publish") {
    const auto selection=arguments.value("inputs").toObject();const auto operation=selection.value("source_operation_id");
    return keys(selection,{"source_operation_id"})&&token(operation)&&arguments.value("parameters").toObject().isEmpty()?
      QString():QStringLiteral("source_operation_required");
  }
  if(phase!="prepare"&&phase!="compute-only")return "unsupported_intent";
  QByteArray code;auto invalid=generatedSourceFromArguments(arguments,code);if(!invalid.isEmpty())return invalid;
  NumericTableSelection selection;if(!numericTableSelectionFromJson(arguments.value("inputs").toObject(),selection,&invalid))return invalid;
  return {};
}
}
