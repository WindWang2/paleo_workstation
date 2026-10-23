// 层：数据
#pragma once
#include "lasdoc.h"
#include "../domain/importrows.h"

class DataCatalog;
namespace paleo::dataimport_detail
{
struct FamilyContext;
struct LogInspection
{
  LasHeaderInfo header;
  QString summary;
  QStringList issues;
  QString preview;
  QStringList sourceAnnotations;
};
bool inspectLog(const QString &path, LogInspection &out, QString *error);
void fillLogPreview(const DataCatalog &cat, FolderPreviewRow &row);
QString importInspectedLog(FamilyContext &ctx);
QString ensureNamedImportWell(DataCatalog &cat, const QString &name, QString *error);
QString logFileDigest(const QString &path);
bool validateDuplicateLogTarget(DataCatalog &cat, const QString &assetId,
                               const QString &choice, QString *error);
}
