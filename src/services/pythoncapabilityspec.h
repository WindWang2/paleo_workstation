// 层：数据
#pragma once
#include "capabilityspec.h"

namespace paleo::python {
inline constexpr char SummaryCapability[] = "python.trusted_summary";
inline constexpr char SummaryAlgorithm[] = "python:trusted_summary";
QString summaryScriptSha256();
AlgorithmSpec summaryAlgorithmSpec();
CapabilitySpec summaryCapabilitySpec();
} // namespace paleo::python
void appendPythonAlgorithmSpecs(QVector<AlgorithmSpec> *specs);
