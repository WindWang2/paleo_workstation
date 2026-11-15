// 层：数据
#pragma once
#include "domain/documentsource.h"
namespace paleo::python {
QString seismicTraceDescriptorQueryArguments(const QJsonObject &,document::SourceRef &,qlonglong &volumeId);
}
