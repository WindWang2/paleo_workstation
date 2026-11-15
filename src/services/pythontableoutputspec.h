// 层：数据
#pragma once
#include "pythonnumerictablespec.h"
#include <QJsonObject>
namespace paleo::python {
inline constexpr char TableOutputsCapability[] = "python.numeric_table_outputs";
inline constexpr char TableOutputsTool[] = "paleo.python_table_outputs";
AlgorithmSpec tableOutputsAlgorithmSpec();
CapabilitySpec tableOutputsCapabilitySpec();
QJsonObject tableOutputsToolRequestSchema();
QString validateTableOutputsToolArguments(const QJsonObject &);
}
