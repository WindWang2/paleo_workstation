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
bool prepare(DataCatalog *cat, const QString &project, const QString &stageDir, const QString &path,
             const QString &horizon, const PaleoMapLibrary::Request &request,
             const std::function<bool()> &cancelled, Prepared *out, QString *error, const QString &existingAsset = {});
} // namespace PaleoMapImport
