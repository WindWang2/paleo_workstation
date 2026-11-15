// 层：数据
#pragma once
#include "pythonnumerictable.h"
namespace paleo::python::numeric_table_detail {
QString validatePlan(const NumericTableReadPlan &,const QByteArray &,const ExecutionPolicy &);
QString sdkFailure(const std::string &);
QString digest(QByteArrayView);
QByteArray payload(const data::NumericTableWindow &,QString &error);
QJsonObject metadata(const NumericTableReadPlan &,const data::NumericTableWindow &,
  const QJsonObject &readMetrics,const QByteArray &);
}
