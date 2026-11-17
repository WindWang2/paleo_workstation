// 层：数据
#include "pythongenerated.h"
#include "pythonanalysis.h"
#include "pythonoutputjson.h"
#include "pythongeneratedreportcontent.h"
#include <QJsonDocument>
#include <QRegularExpression>

namespace paleo::python {
AnalysisReportValidation validateGeneratedReport(const QByteArray &bytes,const QJsonObject &metadata,
    const QString &codeSha,const QString &runtimeSha,const ExecutionPolicy &policy) {
  AnalysisReportValidation out;const auto fail=[&](const QString &code){out.code=code;return out;};
  if(!policy.validate().isEmpty())return fail("python_policy_invalid");
  static const QRegularExpression digest("^[0-9a-f]{64}$");
  if(codeSha.size()!=64 || runtimeSha.size()!=64 || !digest.match(codeSha).hasMatch() || !digest.match(runtimeSha).hasMatch() ||
      !validateAnalysisInputMetadata(metadata,AnalysisKind::Generated))return fail("python_generated_identity_invalid");
  QJsonObject envelope;const auto invalid=parsePythonOutputObject(bytes,qMin<qint64>(policy.outputBytes,256*1024),16384,envelope);
  if(!invalid.isEmpty())return fail(invalid);const auto ports=envelope.value("ports").toObject();
  if(envelope.size()!=2 || !envelope.contains("contract") || !envelope.contains("ports") || envelope.value("contract")!="paleo.python-output.v1" ||
      ports.keys()!=QStringList{"REPORT"} || !ports.value("REPORT").isObject())return fail("python_output_contract_mismatch");
  const auto content=validateGeneratedReportContent(QJsonDocument(ports.value("REPORT").toObject()).toJson(QJsonDocument::Compact),policy);
  return bindGeneratedReportContent(content,metadata,codeSha,runtimeSha,policy);
}
}
