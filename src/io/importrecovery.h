// 层：数据
#pragma once
#include <QString>
class DataCatalog;
struct ImportSession;
namespace paleo::dataimport_detail
{
bool writeImportIntent(ImportSession &session, QString *error);
bool recoverImportIntents(const QString &projectDir, const DataCatalog &catalog, QString *error);
}
