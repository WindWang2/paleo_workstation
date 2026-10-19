// 层：功能
#pragma once
#include "paleomaplibrary.h"
class DataCatalog;
namespace PaleoMapImport
{
struct Prepared
{
  QString assetId;
  QStringList directories, warnings;
  bool unchanged = false, fatal = false;
};
bool prepare(DataCatalog *catalog, const QString &projectDir, const QString &stageDir, const QString &path,
             const QString &horizon, const PaleoMapLibrary::Request &request,
             const std::function<bool()> &cancelled, Prepared *result, QString *error, const QString &existingAsset = {});
} // namespace PaleoMapImport
