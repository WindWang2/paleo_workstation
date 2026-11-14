// 层：数据
#pragma once
#include <QByteArray>
#include <QJsonObject>
#include <QString>

namespace paleo::python {
enum class AnalysisKind;
struct ExecutionPolicy;
struct AnalysisReportValidation {
  QString code="python_output_invalid";
  QByteArray bytes; // Host-repacked REPORT object, without the script envelope.
  QJsonObject summary; // Bounded counts/status only; never the complete report.
  bool ok() const {return code==QLatin1String("ok");}
};
AnalysisReportValidation validateAnalysisReport(const QByteArray &bytes,AnalysisKind kind,
    const QJsonObject &expectedInputMetadata,const ExecutionPolicy &policy);
} // namespace paleo::python
