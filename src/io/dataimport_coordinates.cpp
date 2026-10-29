// 层：数据
#include "dataimportservice.h"
#include "dataimport_internal.h"
#include "projectcoordinates.h"
#include "welltable.h"
#include "../metadata/nativereference.h"
#include <QDir>
#include <QFile>
namespace paleo::dataimport_detail {
bool prepareSeismicCoordinates(FamilyContext &ctx, const QString &sourceCrs) {
  if (!ctx.s->georeference || ctx.s->georeference->kind != "crs") return true;
  QFile file(ctx.sourcePath);
  if (!file.open(QIODevice::ReadOnly)) { *ctx.error = QStringLiteral("无法读取地震坐标声明"); return false; }
  auto from = paleo::io::wellSourceCrsHint(QString::fromLatin1(file.read(3200)));
  if (from.isEmpty()) from = sourceCrs;
  const auto target = ctx.s->georeference->sourceCrs;
  if (from.isEmpty()) return true;
  if (sameNativeCrs(from, target)) return true;
  if (!ctx.s->ensureStagingRoot(ctx.error)) return false;
  CatalogVersion derived;
  derived.id = ctx.cat->nextVersionId(); derived.assetId = ctx.assetId;
  derived.versionNumber = 2; derived.stage = "DERIVED"; derived.managed = true;
  derived.fileName = ctx.fi.fileName(); derived.parentVersionIds = {ctx.versionId};
  derived.path = "artifacts/" + DataCatalog::managedPath("derived", ctx.assetId, derived.id, derived.fileName);
  derived.sourceUri = ctx.sourcePath; derived.extra = {{"source_crs", from}, {"project_crs", target}};
  const auto staged = DataCatalog::resolvedVersionPath(ctx.s->stagingRoot, derived);
  if (staged.isEmpty() || !QDir().mkpath(QFileInfo(staged).absolutePath()) ||
      !paleo::io::projectSegyFile(ctx.sourcePath, staged, from, target, ctx.error)) return false;
  derived.sha256 = DataCatalog::sha256FileHex(staged, ctx.error);
  if (derived.sha256.isEmpty() || !ctx.cat->addVersion(derived, ctx.error)) return false;
  ctx.sourcePath = staged; // Survey geometry and all future reads use the projected version.
  return true;
}
}
