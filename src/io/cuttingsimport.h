// 层：数据
#pragma once
#include <QString>
struct ImportSession;
struct CatalogVersion;
namespace paleo::io {
// Validate once on import and preserve an independent, managed interpretation table.
bool storeImportedCuttings(ImportSession &session, const CatalogVersion &raw,
                           const QString &path, QString *error);
}
