// 层：功能
#include "../catalog/datacatalog.h"
#include "../domain/faciescatalog.h"
#include "../qgis/paleomapsource.h"
#include "../services/paleomapfiles.h"
#include "paleomapimport_internal.h"
#include <QCryptographicHash>
#include <QDir>
#include <QFile>
#include <QFileInfo>
#include <QJsonDocument>
namespace
{
QVariantList schemaFor(DataCatalog *cat, const QString &horizon)
{
  for (const auto &asset : cat->assets())
    if (asset.type == "facies_schema" && asset.displayName == "facies-schema-" + horizon)
      return cat->currentVersion(asset.id).extra.value("facies").toList();
  return FaciesCatalog::defaults();
}
bool intact(const QString &project, const CatalogVersion &raw, const std::function<bool()> &cancelled)
{
  const auto members = raw.extra.value("bundle_members").toList();
  if (members.isEmpty())
    return false;
  const auto directory = QFileInfo(DataCatalog::resolvedVersionPath(project, raw)).absoluteDir();
  for (const auto &entry : members)
  {
    const auto row = entry.toMap();
    const auto name = row.value("file").toString();
    if (!DataCatalog::isSafePathSegment(name) ||
        DataCatalog::sha256FileHex(directory.filePath(name), nullptr, cancelled) !=
            row.value("sha256").toString())
      return false;
  }
  return true;
}
} // namespace
namespace PaleoMapImport
{
bool prepare(DataCatalog *catalog, const QString &projectDir, const QString &stageDir, const QString &path,
             const QString &horizon, const PaleoMapLibrary::Request &request, const std::function<bool()> &cancelled,
             Prepared *result, QString *error, const QString &existingAsset, const QSet<QString> &removedAssetIds)
{
  CatalogVersion current;
  if (!existingAsset.isEmpty())
    current = catalog->currentVersion(existingAsset);
  else
  {
    // C-10（full-audit-20261020）：文件导入按 sourceUri+horizon 匹配现有资产时，
    // 删除态（可回收清单内）资产不得被静默复活——同文件命中删除态资产即拒绝，
    // 提示先恢复或永久删除（另建新资产会在恢复时产生同源双资产）。
    bool removedMatch = false;
    for (const auto &asset : catalog->assets())
      if (asset.type == "paleogeographic_map")
      {
        const auto v = catalog->currentVersion(asset.id);
        if (v.sourceUri == path && v.extra.value("horizon") == horizon)
        {
          if (removedAssetIds.contains(asset.id))
          {
            removedMatch = true;
            break;
          }
          current = v;
          break;
        }
      }
    if (removedMatch)
    {
      if (error) *error = QObject::tr("该图件已在可回收清单中（删除状态），请先恢复或永久删除后再导入");
      return false;
    }
  }
  const auto oldRaw = current.extra.contains("source_raw_version_id")
                          ? catalog->versionById(current.extra.value("source_raw_version_id").toString())
                          : current;
  if (!existingAsset.isEmpty() &&
      (catalog->assetById(existingAsset).type != "paleogeographic_map" || oldRaw.stage != "RAW" ||
       oldRaw.assetId != existingAsset || !intact(projectDir, oldRaw, cancelled)))
  {
    if (error) *error = QObject::tr("原图或配套文件缺失／已变化，不能整理；请重新导入完整原图");
    return false;
  }
  const auto sourceUri = existingAsset.isEmpty() ? path : oldRaw.sourceUri;
  const auto assetId = current.id.isEmpty() ? catalog->nextAssetId() : current.assetId;
  const auto versionId = catalog->nextVersionId();
  const auto name = QFileInfo(path).fileName();
  const auto relative = "artifacts/" + DataCatalog::managedPath("raw", assetId, versionId, name);
  const auto copyDirectory = QFileInfo(QDir(stageDir).filePath(relative)).absolutePath();
  const auto parts = PaleoMapFiles::copyDataset(path, copyDirectory, cancelled, error);
  if (cancelled())
    return false;
  if (parts.isEmpty())
  {
    QDir(copyDirectory).removeRecursively();
    return false;
  }
  const auto fingerprint = PaleoMapFiles::fingerprint(parts);
  const auto schema = schemaFor(catalog, horizon);
  const auto policy =
      QString::fromLatin1(QCryptographicHash::hash(QJsonDocument::fromVariant(
                                                       QVariantMap{{"normalizer", 2},
                                                                   {"facies", schema},
                                                                   {"normalize", request.normalizeFacies},
                                                                   {"micro_field", request.microfaciesField}})
                                                       .toJson(QJsonDocument::Compact),
                                                   QCryptographicHash::Sha256)
                              .toHex());
  const bool reuse = !oldRaw.id.isEmpty() && oldRaw.extra.value("bundle_sha256") == fingerprint &&
                     intact(projectDir, oldRaw, cancelled);
  if (reuse && current.extra.value("normalization_policy") == policy &&
      (existingAsset.isEmpty() || current.extra.value("mapping_product").toBool()) &&
      DataCatalog::sha256FileHex(DataCatalog::resolvedVersionPath(projectDir, current), nullptr, cancelled) ==
          current.sha256)
  {
    QDir(copyDirectory).removeRecursively();
    result->unchanged = true;
    return true;
  }
  if (reuse)
    QDir(copyDirectory).removeRecursively();
  const auto rawPath =
      reuse ? DataCatalog::resolvedVersionPath(projectDir, oldRaw) : QDir(stageDir).filePath(relative);
  auto metadata = reuse ? oldRaw.extra : PaleoMapSource::inspect(rawPath, error);
  if (metadata.isEmpty())
  {
    QDir(copyDirectory).removeRecursively();
    return false;
  }
  CatalogVersion raw = oldRaw;
  if (!reuse)
  {
    raw = {};
    raw.id = versionId;
    raw.assetId = assetId;
    raw.stage = "RAW";
    raw.managed = true;
    raw.versionNumber = current.id.isEmpty() ? 1 : current.versionNumber + 1;
    raw.path = relative;
    raw.fileName = name;
    raw.sourceUri = sourceUri;
    metadata.insert("horizon", horizon);
    metadata.insert("paleomap", true);
    metadata.insert("kind", "existing_paleomap");
    metadata.insert("layer_id", "paleomap." + raw.id);
    metadata.insert("mapping_product", false);
    metadata.insert("bundle_members", parts);
    metadata.insert("bundle_sha256", fingerprint);
    raw.extra = metadata;
    for (const auto &entry : parts)
      if (entry.toMap().value("file") == name)
        raw.sha256 = entry.toMap().value("sha256").toString();
    if (!oldRaw.id.isEmpty())
      raw.parentVersionIds << oldRaw.id;
  }
  CatalogVersion derived;
  QVariantMap converted;
  QString conversionError;
  const bool faciesPolygon = request.normalizeFacies && metadata.value("source_type") == "vector" &&
                             metadata.value("polygon").toBool();
  if (faciesPolygon && !metadata.value("spatial").toBool())
    conversionError = QObject::tr("图件缺少有效坐标系，请先完成配准后重试三级相整理");
  else if (faciesPolygon)
  {
    derived.id = catalog->nextVersionId();
    derived.assetId = assetId;
    derived.stage = "DERIVED";
    derived.managed = true;
    derived.versionNumber = reuse ? current.versionNumber + 1 : raw.versionNumber + 1;
    derived.fileName = "facies.gpkg";
    derived.path = "artifacts/" + DataCatalog::managedPath("derived", assetId, derived.id, derived.fileName);
    const auto destination = QDir(stageDir).filePath(derived.path);
    QDir().mkpath(QFileInfo(destination).absolutePath());
    converted = PaleoMapSource::normalizeFacies(rawPath, destination, schema, request.microfaciesField,
                                                cancelled, &conversionError);
    if (cancelled())
      return false;
    if (converted.isEmpty())
      QDir(QFileInfo(destination).absolutePath()).removeRecursively();
  }
  if (!existingAsset.isEmpty() && converted.isEmpty())
  {
    if (error) *error = !conversionError.isEmpty() ? conversionError
        : faciesPolygon ? QObject::tr("未识别到微相字段，请指定原图中的微相字段名后重试")
                        : QObject::tr("仅已配准的矢量面图件可整理三级相；图片和 PDF 请先矢量化");
    return false;
  }
  if (!conversionError.isEmpty())
    result->warnings << QObject::tr("%1：原图已保存，仅供参考；三级相副本未生成：%2").arg(name, conversionError);
  else
    raw.extra.insert("normalization_policy", policy);
  if (!reuse)
  {
    CatalogAsset asset{assetId, "paleogeographic_map", QFileInfo(path).suffix().toLower(), name};
    if ((current.id.isEmpty() && !catalog->addAsset(asset, error)) || !catalog->addVersion(raw, error))
    {
      result->fatal = true;
      return false;
    }
    result->directories << QFileInfo(relative).path();
  }
  else if (conversionError.isEmpty() && converted.isEmpty())
  {
    if (!catalog->updateVersionExtra(raw.id, "normalization_policy", policy, error))
    {
      result->fatal = true;
      return false;
    }
  }
  if (!converted.isEmpty())
  {
    converted.insert("horizon", horizon);
    converted.insert("kind", "existing_paleomap");
    converted.insert("paleomap", true);
    converted.insert("group", "05_PaleoMap");
    converted.insert("title", QObject::tr("%1 · 三级相").arg(name));
    converted.insert("mapping_product", true);
    converted.insert("layer_id", "product." + derived.id);
    converted.insert("source_raw_version_id", raw.id);
    converted.insert("source_bundle_sha256", fingerprint);
    converted.insert("normalization_policy", policy);
    converted.insert("method", "existing-paleomap-hierarchy-v2");
    derived.extra = converted;
    derived.sourceUri = sourceUri;
    derived.parentVersionIds = {raw.id};
    derived.sha256 =
        DataCatalog::sha256FileHex(QDir(stageDir).filePath(derived.path), error, cancelled);
    if (derived.sha256.isEmpty() || !catalog->addVersion(derived, error))
    {
      result->fatal = true;
      return false;
    }
    QFile::setPermissions(QDir(stageDir).filePath(derived.path),
                          QFileDevice::ReadOwner | QFileDevice::ReadGroup | QFileDevice::ReadOther);
    result->directories << QFileInfo(derived.path).path();
  }
  result->assetId = assetId;
  return true;
}
} // namespace PaleoMapImport
