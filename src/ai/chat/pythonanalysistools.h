// 层：功能
#pragma once
#include <QJsonObject>
#include <QString>
#include <QVector>
struct AiToolSpec;
bool isPythonAnalysisTool(const QString &name);
QJsonObject pythonAnalysisToolSchema(const QString &name);
QString validatePythonAnalysisArguments(const QString &name,const QJsonObject &arguments);
void appendPythonAnalysisTools(QVector<AiToolSpec> &specs);
