// 层：数据
#pragma once
#include <QString>

namespace paleo {
// Declared DataService CRS. An unbound legacy source returns an empty CRS;
// a bound read failure returns false rather than inventing a local reference.
bool readSeismicSourceCrs(const QString &path,QString &crs,QString *error);
}
