// 层：数据
#pragma once
#include "pythonanalysisreport.h"

namespace paleo::python {
struct GeneratedReportContent {
  QString code;
  QJsonObject report,summary;
  bool ok()const{return code==QLatin1String("ok");}
};
// Strict existing REPORT body only, independently reusable by a new outer output frame.
GeneratedReportContent validateGeneratedReportContent(const QByteArray &bytes,const ExecutionPolicy &);
// Caller first authenticates its explicit input contract. This binds host-owned provenance;
// it does not establish input identity or grant execution/publication authority.
AnalysisReportValidation bindGeneratedReportContent(const GeneratedReportContent &,const QJsonObject &metadata,
  const QString &codeSha,const QString &runtimeSha,const ExecutionPolicy &);
}
