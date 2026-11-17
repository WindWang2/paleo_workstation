// 层：数据
#include "pythongeneratedreportcontent.h"
#include "pythonruntime.h"
#include "pythonoutputjson.h"
#include <QJsonArray>
#include <QJsonDocument>
#include <QRegularExpression>
#include <QSet>
#include <cmath>

namespace paleo::python {
namespace {
bool keys(const QJsonObject &object,std::initializer_list<const char *> fields) {
  if(object.size()!=qsizetype(fields.size()))return false;
  for(const auto *field:fields)if(!object.contains(QLatin1String(field)))return false;return true;
}
bool text(const QJsonValue &value,int maximum=256) {
  return value.isString() && value.toString().size()<=maximum && !value.toString().trimmed().isEmpty() &&
    !value.toString().contains(QChar(0)) && value.toString().toUtf8().size()<=maximum;
}
bool identifier(const QJsonValue &value) {
  static const QRegularExpression pattern("^[a-zA-Z][a-zA-Z0-9_]{0,63}$");
  return value.isString() && value.toString().size()<=64 && pattern.match(value.toString()).hasMatch();
}
bool digest(const QString &value) {
  static const QRegularExpression pattern("^[0-9a-f]{64}$");return value.size()==64 && pattern.match(value).hasMatch();
}
bool number(const QJsonValue &value) {return value.isNull() || (value.isDouble() && std::isfinite(value.toDouble()));}
bool metrics(const QJsonValue &value) {
  if(!value.isArray() || value.toArray().size()>64)return false;QSet<QString> ids;
  for(const auto &entry:value.toArray()) {
    const auto item=entry.toObject();const auto id=item.value("id");
    if(!entry.isObject() || !keys(item,{"id","unit","value"}) || !identifier(id) || ids.contains(id.toString()) ||
        !text(item.value("unit")) || !number(item.value("value")))return false;
    ids.insert(id.toString());
  }
  return true;
}
bool table(const QJsonObject &value,int &cells) {
  if(!keys(value,{"id","columns","rows"}) || !identifier(value.value("id")) ||
      !value.value("columns").isArray() || !value.value("rows").isArray())return false;
  const auto columns=value.value("columns").toArray(),rows=value.value("rows").toArray();
  if(columns.isEmpty() || columns.size()>16 || rows.size()>128)return false;QSet<QString> ids;QStringList types;
  for(const auto &entry:columns) {
    const auto column=entry.toObject();const auto id=column.value("id"),type=column.value("type");
    if(!entry.isObject() || !keys(column,{"id","unit","type"}) || !identifier(id) || ids.contains(id.toString()) ||
        !text(column.value("unit")) || !QStringList{"number","string","boolean"}.contains(type.toString()))return false;
    ids.insert(id.toString());types.append(type.toString());
  }
  for(const auto &entry:rows) {
    if(!entry.isArray() || entry.toArray().size()!=columns.size())return false;const auto row=entry.toArray();
    for(int i=0;i<row.size();++i) {
      const auto cell=row[i];if(cell.isNull())continue;
      if(types[i]=="number"?!number(cell):types[i]=="string"?!text(cell):!cell.isBool())return false;
    }
  }
  cells+=int(rows.size()*columns.size());return cells<=8192;
}
bool tables(const QJsonValue &value,int &cells) {
  if(!value.isArray() || value.toArray().size()>4)return false;QSet<QString> ids;
  for(const auto &entry:value.toArray()) {
    const auto item=entry.toObject();const auto id=item.value("id").toString();
    if(!entry.isObject() || !table(item,cells) || ids.contains(id))return false;ids.insert(id);
  }
  return true;
}
bool notes(const QJsonValue &value) {
  if(!value.isArray() || value.toArray().size()>16)return false;
  for(const auto &entry:value.toArray())if(!text(entry,2048))return false;return true;
}
}
GeneratedReportContent validateGeneratedReportContent(const QByteArray &bytes,const ExecutionPolicy &policy) {
  GeneratedReportContent out;const auto fail=[&](const QString &code){out.code=code;out.report={};out.summary={};return out;};
  if(!policy.validate().isEmpty())return fail("python_policy_invalid");
  QJsonObject report;const auto invalid=parsePythonOutputObject(bytes,qMin<qint64>(policy.outputBytes,256*1024),16384,report);
  if(!invalid.isEmpty())return fail(invalid);int cells=0;
  if(!keys(report,{"contract","metrics","tables","notes"}) || report.value("contract")!="paleo.python-generated-report.v1" ||
      !metrics(report.value("metrics")) || !tables(report.value("tables"),cells) || !notes(report.value("notes")))
    return fail("python_output_contract_mismatch");
  out.report=report;out.summary={{"contract","paleo.python-generated-report.v1"},{"scope","selected_window_only"},
    {"metrics",report.value("metrics").toArray().size()},{"tables",report.value("tables").toArray().size()},
    {"tableCells",cells},{"notes",report.value("notes").toArray().size()}};out.code="ok";return out;
}
AnalysisReportValidation bindGeneratedReportContent(const GeneratedReportContent &content,const QJsonObject &metadata,
    const QString &codeSha,const QString &runtimeSha,const ExecutionPolicy &policy) {
  AnalysisReportValidation out;const auto fail=[&](const QString &code){out.code=code;out.bytes.clear();out.summary={};return out;};
  if(!policy.validate().isEmpty())return fail("python_policy_invalid");
  if(!digest(codeSha)||!digest(runtimeSha))return fail("python_generated_identity_invalid");
  if(!content.ok())return fail(content.code);
  auto accepted=content.report;accepted["provenance"]=metadata.value("provenance");accepted["sourceKind"]=metadata.value("sourceKind");
  accepted["scope"]="selected_window_only";accepted["codeSha256"]=codeSha;accepted["runtimeManifestSha256"]=runtimeSha;
  accepted["executionMode"]="generated";accepted["claims"]="agent_generated_values_not_independently_recomputed";
  accepted["inputColumns"]=metadata.value("columns");accepted["software"]=QJsonObject{{"python",QLatin1String(PythonVersion)},
    {"numpy",QLatin1String(NumpyVersion)},{"determinism","generated_program_responsibility"}};
  out.bytes=QJsonDocument(accepted).toJson(QJsonDocument::Compact);
  if(out.bytes.size()>qMin<qint64>(policy.outputBytes,256*1024))return fail("python_output_budget_exceeded");
  out.summary=content.summary;out.summary["analysis"]=metadata.value("analysis");out.summary["codeSha256"]=codeSha;
  out.summary["claims"]=accepted.value("claims");out.code="ok";return out;
}
}
