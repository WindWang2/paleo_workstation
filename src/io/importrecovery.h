// 层：数据
#pragma once
#include <QSet>
#include <QString>
class DataCatalog;
struct ImportSession;
namespace paleo::dataimport_detail
{
QSet<QString> stagedManagedVersionPaths(const ImportSession &session);
bool isImportScratchPath(const QString &relativePath, const QSet<QString> &versionPaths);
bool writeImportIntent(ImportSession &session, QString *error);
bool recoverImportIntents(const QString &projectDir, const DataCatalog &catalog, QString *error);
}
