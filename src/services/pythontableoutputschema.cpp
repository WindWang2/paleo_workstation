// 层：数据
#include "pythontableoutputspec.h"
#include "pythonnumerictable.h"
#include "pythonnumericoutput.h"
#include <QJsonArray>
namespace paleo::python {
namespace {
QJsonObject declarationSchema() {
  const auto fixed=[](const QString &value){return QJsonObject{{"const",value}};};
  const QJsonObject column{{"type","object"},{"additionalProperties",false},
    {"required",QJsonArray{"id","label","unit","dtype","role"}},
    {"properties",QJsonObject{{"id",QJsonObject{{"type","string"},{"pattern","^[A-Za-z][A-Za-z0-9_]{0,63}$"}}},
      {"label",QJsonObject{{"type","string"},{"minLength",1},{"maxLength",256}}},
      {"unit",QJsonObject{{"type","string"},{"minLength",1},{"maxLength",64}}},
      {"dtype",QJsonObject{{"enum",QJsonArray{"<i8","<f8","|b1"}}}},
      {"role",QJsonObject{{"enum",QJsonArray{"source_row","axis","value"}}}}}}};
  return {{"type","object"},{"additionalProperties",false},{"required",QJsonArray{"contract","columns","maxRows"}},
    {"properties",QJsonObject{{"contract",fixed("paleo.python-table-declaration.v1")},
      {"maxRows",QJsonObject{{"type","integer"},{"minimum",1},{"maximum",8192}}},
      {"columns",QJsonObject{{"type","array"},{"minItems",1},{"maxItems",16},{"items",column}}}}}};
}
}
QJsonObject tableOutputsToolRequestSchema() {
  auto schema=numericTableToolRequestSchema();auto properties=schema.value("properties").toObject();
  auto parameters=properties.value("parameters").toObject();auto choices=parameters.value("oneOf").toArray();
  auto source=choices[0].toObject();auto fields=source.value("properties").toObject();
  auto code=fields.value("code").toObject();code["description"]=
    "UTF-8 Python <=16KiB; input is the numeric-table analysis frame described by the input contract. "
    "stdout: ten bytes PALEOPYO2\\n, uint32LE JSON header length, exact header {contract:paleo.python-output-frame.v2,ports:[REPORT,TABLE]}, "
    "then REPORT UTF-8 JSON body and TABLE column-major bytes. REPORT is paleo.python-generated-report.v1. "
    "Each port declares id,encoding,size,sha256; TABLE also declares rows and columns exactly matching output_table.columns. "
    "TABLE encoding paleo.numeric-columns.v1: each <i8/<f8/|b1 values array followed by ceil(rows/8) LSB-first null mask. "
    "Null values and unused mask bits must be zero; axis non-null, source_row positive strictly increasing int64; no NaN/Inf. "
    "Use REPORT utf8-json and TABLE paleo.numeric-columns.v1; no other ports, paths or MIME. "
    "Host encodes numeric CSV and atomically publishes the complete group. Values/units are not independently recomputed.";
  fields["code"]=code;fields["output_table"]=declarationSchema();source["properties"]=fields;
  auto required=source.value("required").toArray();required.append("output_table");source["required"]=required;
  choices[0]=source;parameters["oneOf"]=choices;properties["parameters"]=parameters;schema["properties"]=properties;
  auto conditions=schema.value("allOf").toArray();auto phase=conditions[0].toObject();auto compute=phase.value("else").toObject();
  auto selected=compute.value("properties").toObject();selected["parameters"]=source;compute["properties"]=selected;
  phase["else"]=compute;conditions[0]=phase;schema["allOf"]=conditions;return schema;
}
QString validateTableOutputsToolArguments(const QJsonObject &arguments) {
  auto original=arguments;auto parameters=arguments.value("parameters").toObject();
  const bool publication=arguments.value("publish_intent")=="publish";
  if(!publication) {
    if(!parameters.value("output_table").isObject())return "python_output_declaration_invalid";
    const auto code=validateNumericOutputDeclaration(parameters.take("output_table").toObject());if(!code.isEmpty())return code;
    original["parameters"]=parameters;
  }
  return validateNumericTableToolArguments(original);
}
}
