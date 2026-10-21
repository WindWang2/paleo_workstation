// 层：功能
#pragma once
#include "paleomaplibrary.h"
#include <QSet>
class DataCatalog;
namespace PaleoMapImport
{
struct Prepared
{
  QString assetId;
  QStringList directories, warnings;
  bool unchanged = false, fatal = false;
};
// removedAssetIds：可回收清单内处于删除态的资产——文件导入按 sourceUri 匹配
// 到删除态资产时拒绝（C-10，full-audit-20261020：不得静默复活软删资产）。
bool prepare(DataCatalog *catalog, const QString &projectDir, const QString &stageDir, const QString &path,
             const QString &horizon, const PaleoMapLibrary::Request &request,
             const std::function<bool()> &cancelled, Prepared *result, QString *error,
             const QString &existingAsset = {}, const QSet<QString> &removedAssetIds = {});
} // namespace PaleoMapImport
