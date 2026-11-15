// 层：数据
#pragma once
#include "pythonanalysisreport.h"
#include "pythonruntime.h"
#include <functional>

namespace paleo::python {
inline constexpr qint64 NumericOutputFrameBytes = 12 * 1024 * 1024;
inline constexpr qint64 NumericCsvBytes = 4 * 1024 * 1024;
// The host validates and freezes this declaration before execution. The child
// cannot replace column identity, type, unit or label in its output manifest.
QString validateNumericOutputDeclaration(const QJsonObject &declaration);
struct NumericOutputValidation {
  QString code = "python_output_invalid";
  QByteArray report, csv;
  QJsonObject tableMetadata, summary;
  bool ok() const { return code == QLatin1String("ok"); }
};
using GeneratedReportValidator = std::function<AnalysisReportValidation(const QByteArray &)>;
// Framing alone never accepts REPORT: the caller supplies the version-specific
// report validator bound to the frozen input/runtime/code identities. All ports
// are returned together only after complete validation; this function does no I/O.
NumericOutputValidation validateNumericOutput(const QByteArray &frame,
    const QJsonObject &declaration, const ExecutionPolicy &policy,
    const GeneratedReportValidator &validateReport, const std::function<bool()> &cancelled = {});
}
