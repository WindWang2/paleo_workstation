// 层：数据
#pragma once
#include "pythonseismictrace.h"
#include <initializer_list>
namespace paleo::python::seismic_trace_detail {
bool digest(const QJsonValue &);
bool text(const QJsonValue &,int maximum,bool declared=false);
bool decimal(const QJsonValue &,qlonglong &,bool positive=false);
bool keys(const QJsonObject &,std::initializer_list<const char*>);
QString hash(QByteArrayView);
QString sdkFailure(const std::string &);
QByteArray payload(const data::SeismicWindow &,QString &error);
QJsonObject metadata(const SeismicTraceReadPlan &,const data::SeismicWindow &,const QJsonObject &metrics,const QByteArray &);
}
