// 层：数据
#include "pythongenerated.h"
#include "pythonanalysisselection.h"
#include <QJsonArray>
#include <QStringDecoder>

namespace paleo::python {
QString validateGeneratedCode(const QByteArray &source,const ExecutionPolicy &policy) {
  if(!policy.validate().isEmpty())return "python_policy_invalid";
  if(!policy.allowGenerated)return "python_execution_denied";
  if(source.isEmpty() || source.size()>qMin<qint64>(GeneratedCodeBytes,policy.codeBytes))return "python_code_budget_exceeded";
  QStringDecoder decoder(QStringDecoder::Utf8,QStringConverter::Flag::Stateless);const QString decoded=decoder(source);Q_UNUSED(decoded);
  if(decoder.hasError() || source.contains('\0') || source.startsWith("\xef\xbb\xbf"))return "python_code_encoding_invalid";
  return {};
}
QString generatedSourceFromArguments(const QJsonObject &arguments,QByteArray &source) {
  source.clear();const auto value=arguments.value("parameters");if(!value.isObject())return "python_code_required";
  const auto parameters=value.toObject();const auto code=parameters.value("code");
  if(parameters.size()!=1 || !code.isString() || code.toString().size()>GeneratedCodeBytes)return "python_code_required";
  const auto text=code.toString();const auto bytes=text.toUtf8();
  if(QString::fromUtf8(bytes)!=text)return "python_code_encoding_invalid";
  ExecutionPolicy policy;policy.allowGenerated=true;const auto invalid=validateGeneratedCode(bytes,policy);
  if(!invalid.isEmpty())return invalid;source=bytes;return {};
}
QJsonObject generatedToolRequestSchema() {
  auto schema=analysisToolRequestSchema(AnalysisKind::Generated);auto properties=schema.value("properties").toObject();
  const QJsonObject code{{"type","object"},{"required",QJsonArray{"code"}},{"additionalProperties",false},
    {"properties",QJsonObject{{"code",QJsonObject{{"type","string"},{"minLength",1},{"maxLength",GeneratedCodeBytes},
      {"description","UTF-8 source, at most 16 KiB. stdin is uint32 little-endian JSON length, then JSON metadata, then binary: <f8 axis[n], <f8 values[n,m] row-major, u1 quality[n,m]. Metadata shape=[n,m], columns declare units; quality bits 1=missing,2=abnormal,4=edited. Use NumPy with explicit invalid-value masks. Write exactly one JSON object to stdout: {contract:'paleo.python-output.v1',ports:{REPORT:{contract:'paleo.python-generated-report.v1',metrics:[{id:'mean',unit:'API',value:1.5}],tables:[],notes:[]}}} (JSON uses double quotes). Each table has id, columns:[{id,unit,type:'number'|'string'|'boolean'}], rows:[[...]], at most 4 tables/16 columns/128 rows/8192 cells, 64 metrics, 16 notes of 2048 UTF-8 bytes. Null values are allowed. Do not emit provenance, paths or hashes; the host supplies identity. stdout is the report channel, stderr is a private bounded log. No host files, network, subprocesses or package installation."}}}}}};
  const QJsonObject empty{{"type","object"},{"properties",QJsonObject{}},{"additionalProperties",false}};
  properties["parameters"]=QJsonObject{{"oneOf",QJsonArray{code,empty}}};schema["properties"]=properties;
  auto conditions=schema.value("allOf").toArray();auto phase=conditions[0].toObject();
  for(const auto &branch:{QString("then"),QString("else")}) {
    auto rules=phase.value(branch).toObject();auto fields=rules.value("properties").toObject();
    fields["parameters"]=branch=="then"?empty:code;rules["properties"]=fields;phase[branch]=rules;
  }
  conditions[0]=phase;schema["allOf"]=conditions;return schema;
}
QString validateGeneratedToolArguments(const QJsonObject &arguments) {
  auto args=arguments;const auto phase=args.value("publish_intent").toString("compute-only");
  if(phase=="publish")return validateAnalysisToolArguments(args,AnalysisKind::Generated);
  QByteArray source;const auto code=generatedSourceFromArguments(args,source);if(!code.isEmpty())return code;
  args["parameters"]=QJsonObject{};return validateAnalysisToolArguments(args,AnalysisKind::Generated);
}
}
