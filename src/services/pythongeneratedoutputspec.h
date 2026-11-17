// 层：数据
#pragma once
#include "pythontableoutputspec.h"
#include "pythonimageoutput.h"
namespace paleo::python {
inline constexpr char GeneratedOutputsCapability[]="python.generated_outputs";
inline constexpr char GeneratedOutputsTool[]="paleo.python_outputs";
AlgorithmSpec generatedOutputsAlgorithmSpec();
CapabilitySpec generatedOutputsCapabilitySpec();
QJsonObject generatedOutputsToolRequestSchema();
QString validateGeneratedOutputsToolArguments(const QJsonObject &);
QJsonObject generatedOutputsProfile(const QJsonObject &table,const QJsonObject &image);
QString validateGeneratedOutputsProfile(const QJsonObject &,const ExecutionPolicy &,bool currentEncoder);
}
