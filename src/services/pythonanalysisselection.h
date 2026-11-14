// 层：数据
#pragma once
#include "pythonanalysis.h"
#include "domain/wellref.h"

namespace paleo::python {
struct AnalysisSelectionRequest {
  document::WellRef well;
  QVector<AnalysisCurveSelection> curves; // Public references only; the host replaces SourceRecord before I/O.
  QString alignment,sampling;
  quint32 seed=0;
};
bool analysisSelectionFromJson(const QJsonObject &inputs,AnalysisKind kind,AnalysisSelectionRequest &out,QString *error=nullptr);
QJsonObject analysisToolRequestSchema(AnalysisKind kind);
QString validateAnalysisToolArguments(const QJsonObject &arguments,AnalysisKind kind);
} // namespace paleo::python
