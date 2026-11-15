// 层：数据
#pragma once
#include "domain/documentsource.h"
namespace paleo::python {
QString numericTableProjectionQueryArguments(const QJsonObject &,document::SourceRef &,qlonglong &tableId);
QJsonObject numericTableProjectionSourceSchema();
}
