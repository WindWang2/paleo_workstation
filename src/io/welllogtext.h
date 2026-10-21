// 层：数据
#pragma once

#include "lasdoc.h"
#include <QByteArray>

// Strict adapters into the existing, double precision curve contract.
namespace WellLogText
{
bool sniffText(const QByteArray &content);
bool sniffSpreadsheetXml(const QByteArray &content);
bool parse(const QString &path, bool spreadsheetXml, LasHeaderInfo &header,
           QList<LasCurve> &curves, QString *error = nullptr,
           QList<LasIssue> *issues = nullptr);
}
