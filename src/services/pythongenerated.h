// 层：数据
#pragma once
#include "pythonanalysisreport.h"
#include "pythonruntime.h"
namespace paleo::python {
inline constexpr char GeneratedCapability[]="python.generated_compute";
inline constexpr char GeneratedTool[]="paleo.python_compute";
inline constexpr int GeneratedCodeBytes=16*1024;
QString validateGeneratedCode(const QByteArray &source,const ExecutionPolicy &policy);
QString generatedSourceFromArguments(const QJsonObject &arguments,QByteArray &source);
QJsonObject generatedToolRequestSchema();
QString validateGeneratedToolArguments(const QJsonObject &arguments);
AnalysisReportValidation validateGeneratedReport(const QByteArray &output,const QJsonObject &inputMetadata,
    const QString &codeSha256,const QString &runtimeManifestSha256,const ExecutionPolicy &policy);
}
