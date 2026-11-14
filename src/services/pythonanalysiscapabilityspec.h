// 层：数据
#pragma once
#include "capabilityspec.h"

namespace paleo::python {
enum class AnalysisKind;
inline constexpr char WellQualityCapability[]="python.well_curve_quality";
inline constexpr char CorrelationCapability[]="python.attribute_correlation";
QString analysisCapabilityId(AnalysisKind kind);
AlgorithmSpec analysisAlgorithmSpec(AnalysisKind kind);
CapabilitySpec analysisCapabilitySpec(AnalysisKind kind);
} // namespace paleo::python
