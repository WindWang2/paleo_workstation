// 层：数据
#pragma once
#include "pythonanalysisreport.h"
namespace paleo::python {
AnalysisReportValidation validateSeismicTraceReport(const QByteArray &,const QJsonObject &,
  const QString &codeSha256,const QString &runtimeManifestSha256,const ExecutionPolicy &);
}
