// 层：数据
#pragma once
#include "pythonanalysisreport.h"
namespace paleo::python {
AnalysisReportValidation validateNumericTableReport(const QByteArray &,const QJsonObject &,
  const QString &codeSha,const QString &runtimeSha,const ExecutionPolicy &);
}
