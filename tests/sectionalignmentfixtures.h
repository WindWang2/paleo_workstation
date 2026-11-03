#pragma once
#include "catalog/datacatalog.h"
#include <QDir>
#include <QFile>

// Consumer tests supply curve and time-depth prerequisites explicitly.
inline bool addSectionInputFixture(DataCatalog &catalog, const QString &well,
                                  const QString &role, const QByteArray &body) {
  const auto id = well + QStringLiteral("-section-fixture-") + role;
  const auto path = QDir(catalog.projectDir()).filePath(id + QStringLiteral(".dat"));
  QFile file(path);
  if (!file.open(QIODevice::WriteOnly) || file.write(body) != body.size()) return false;
  file.close();
  CatalogAsset asset; asset.id = id; asset.type = role; asset.displayName = id;
  if (!catalog.addAsset(asset)) return false;
  CatalogVersion version; version.id = id + QStringLiteral("-v"); version.assetId = id;
  version.path = path; version.managed = false; version.stage = QStringLiteral("RAW");
  if (!catalog.addVersion(version)) return false;
  EntityAssetLink link; link.entityType = QStringLiteral("well"); link.entityId = well;
  link.assetId = id; link.role = role; link.isPrimary = true;
  return catalog.addLink(link);
}
inline bool addSectionLogFixture(DataCatalog &catalog, const QString &well) {
  return addSectionInputFixture(catalog,well,QStringLiteral("well_log"),
    "~Version\nVERS. 2.0:\nWRAP. NO:\n~Well\nNULL. -999.25:\n"
    "~Curve\nDEPT.M :\nGR.API :\n~ASCII\n0 10\n350 20\n");
}
inline bool addSectionTimeDepthFixture(DataCatalog &catalog, const QString &well) {
  return addSectionInputFixture(catalog,well,QStringLiteral("time_depth"),
    "# TIME TVDSS TVD MD\n0 0 0 0\n160000 0 200000 200000\n");
}
