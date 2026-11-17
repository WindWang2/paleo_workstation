// 层：数据
#pragma once
#include "pythonnumericoutput.h"
#include <QJsonArray>

namespace paleo::python::numeric_output {
bool fields(const QJsonObject &, std::initializer_list<const char *>);
bool integer(const QJsonValue &, qint64 minimum, qint64 maximum, qint64 &);
bool boundedText(const QJsonValue &, int maximum);
QString encodeCsv(QByteArrayView payload, qint64 rows, const QJsonArray &columns,
                  QByteArray &csv, const std::function<bool()> &cancelled);
}
