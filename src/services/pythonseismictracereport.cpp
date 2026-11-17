// 层：数据
#include "pythonseismictracereport.h"
#include "pythonseismictrace.h"
#include "pythongeneratedreportcontent.h"
#include "pythonoutputjson.h"
#include <QJsonDocument>
namespace paleo::python {
AnalysisReportValidation validateSeismicTraceReport(const QByteArray &bytes,const QJsonObject &metadata,
    const QString &codeSha,const QString &runtimeSha,const ExecutionPolicy &policy) {
  AnalysisReportValidation out;const auto fail=[&](const QString &code){out.code=code;return out;};
  if(!validateSeismicTraceInputMetadata(metadata))return fail("python_generated_identity_invalid");
  if(!policy.validate().isEmpty())return fail("python_policy_invalid");
  QJsonObject envelope;const auto invalid=parsePythonOutputObject(bytes,qMin<qint64>(policy.outputBytes,256*1024),16384,envelope);
  if(!invalid.isEmpty())return fail(invalid);const auto ports=envelope.value("ports").toObject();
  if(envelope.size()!=2||!envelope.contains("contract")||!envelope.contains("ports")||envelope.value("contract")!="paleo.python-output.v1"||
      ports.keys()!=QStringList{"REPORT"}||!ports.value("REPORT").isObject())return fail("python_output_contract_mismatch");
  const auto content=validateGeneratedReportContent(QJsonDocument(ports.value("REPORT").toObject()).toJson(QJsonDocument::Compact),policy);
  return bindGeneratedReportContent(content,metadata,codeSha,runtimeSha,policy);
}
}
