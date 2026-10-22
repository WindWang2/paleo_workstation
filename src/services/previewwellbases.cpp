// 层：数据
#include "previewwellbases.h"
#include "projectdata.h"
#include "../catalog/datacatalog.h"
#include "../io/welltable.h"
#include <QFileInfo>
#include <QSet>
#include <algorithm>
#include <cmath>

QVector<WellTopRecord> previewWellBases(DataCatalog *catalog, const QString &wellId,
                                      const QVector<WellTop> &current, QStringList *warnings) {
  if (!catalog) return {};
  CatalogVersion version;
  for (const auto &link : catalog->linksForEntity(wellId))
    if (link.role == "tops" && !link.unresolved && (version.id.isEmpty() || link.isPrimary)) {
      version = catalog->currentVersion(link.assetId); if (link.isPrimary) break;
    }
  // 新导入的受管标准表已保留顶/底界；不依赖外链原件或 Office 工作簿。
  const auto name = DataCatalog::normalizeWellName(catalog->entityById(wellId).name);
  const auto managed = catalog->versionFilePath(version);
  if (version.managed && QFileInfo(managed).suffix().compare("dat", Qt::CaseInsensitive) == 0) {
    auto stored = paleo::io::readWellTableBases(managed);
    stored.erase(std::remove_if(stored.begin(), stored.end(), [&](const auto &base) {
      return DataCatalog::normalizeWellName(base.wellName) != name;
    }), stored.end());
    if (!stored.isEmpty()) return stored;
  }
  QSet<QString> seen;
  while (version.stage != "RAW" && !version.parentVersionIds.isEmpty() && !seen.contains(version.id)) {
    seen.insert(version.id); version = catalog->versionById(version.parentVersionIds.first());
  }
  if (version.stage != "RAW") return {};
  const auto path = DataCatalog::resolvedVersionPath(catalog->projectDir(), version);
  if (!QStringList{"xlsx", "csv", "tsv", "txt"}.contains(QFileInfo(path).suffix().toLower())) return {};
  const auto unavailable = [&]() -> QVector<WellTopRecord> {
    if (warnings) warnings->append(QObject::tr("原始分层表缺失或内容已变化，未采用其底界；末层仅按测井显示范围截取。"));
    return {};
  };
  if (version.sha256.isEmpty() || DataCatalog::sha256FileHex(path) != version.sha256) return unavailable();
  const auto originals = paleo::io::readWellTable(path).tops;
  const auto bases = paleo::io::readWellTableBases(path);
  if (DataCatalog::sha256FileHex(path) != version.sha256) return unavailable();
  QVector<WellTopRecord> out;
  for (const auto &base : bases) {
    if (DataCatalog::normalizeWellName(base.wellName) != name) continue;
    const auto original = std::find_if(originals.cbegin(), originals.cend(), [&](const auto &top) {
      return top.topName == base.topName && DataCatalog::normalizeWellName(top.wellName) == name;
    });
    const auto top = std::find_if(current.cbegin(), current.cend(), [&](const auto &top) { return top.horizon == base.topName; });
    // 编辑过顶界的派生版本不再套用旧原件的底界。
    if (original == originals.cend() || top == current.cend()) continue;
    if (std::isfinite(top->md) != original->hasMd || std::isfinite(top->tvd) != original->hasTvd) continue;
    if (original->hasMd && std::abs(top->md - original->md) >= 0.001) continue;
    if (original->hasTvd && std::abs(top->tvd - original->tvd) >= 0.001) continue;
    out.append(base);
  }
  return out;
}
