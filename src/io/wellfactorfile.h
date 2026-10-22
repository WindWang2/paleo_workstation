// 层：数据
#pragma once
#include <QVariantList>
namespace paleo::io {
// Explicit exchange CSV: well_name,horizon,sand_thick,sand_ratio,... .
// Missing measurements remain null; malformed rows reject the whole file.
bool isWellFactorFile(const QString &path);
QVariantList readWellFactorFile(const QString &path, QString *error);
}
