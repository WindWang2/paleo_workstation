// 层：数据
#include "artifactversioncapability.h"
#include "../catalog/datacatalog.h"
#include "../domain/seismicclusterproduct.h"
#include "../catalog/catalogversionunit.h"
#include <QJsonArray>
#include <QJsonDocument>
#include <QThread>

namespace paleo::capability {
namespace {
QJsonObject scalar(const char *type) {return {{"type",QString::fromLatin1(type)}};}
QJsonObject object(const QJsonObject &properties) {
  QJsonArray required;
  for (auto it=properties.begin();it!=properties.end();++it) required.append(it.key());
  return {{"type","object"},{"properties",properties},{"required",required},{"additionalProperties",false}};
}

QJsonObject frozenUnit(const paleo::strat::UnitSelection &unit, bool &truncated) {
  if (!unit.isValid() || unit.revision.isEmpty()) return {};
  // Never shorten an identifier into a different, seemingly valid reference.
  if (unit.schemeId.size() > 256 || unit.revision.size() > 256 ||
      unit.intervalId.size() > 256 || unit.subIntervalId.size() > 256) {
    truncated = true;
    return {};
  }
  truncated |= unit.displayName.size() > 256;
  return {{"schemeId",unit.schemeId},{"revision",unit.revision},{"intervalId",unit.intervalId},
          {"subIntervalId",unit.subIntervalId},{"aggregate",unit.aggregate},{"displayName",unit.displayName.left(256)}};
}
}
QString ArtifactVersionCapability::id() {return QStringLiteral("catalog.version.describe.v1");}
QString ArtifactVersionCapability::resultKind(const CatalogVersion &version) {
  const auto kind = version.extra.value("kind").toString();
  if (paleo::seismic::isRawClusterProduct(kind)) return kind;
  const auto result = version.extra.value("result_kind").toString();
  static const QStringList known{"well_assigned_cluster_raster", "well_assigned_cluster_patches",
                                 "manual_cluster_interpretation", "formation_prediction", "dominant_facies"};
  return known.contains(result) ? result : QString();
}
QJsonObject ArtifactVersionCapability::requestSchema() {
  return object({{"documentId",scalar("string")},{"versionId",scalar("string")},
                 {"expectedCatalogRevision",scalar("integer")}});
}
QJsonObject ArtifactVersionResult::toJson() const {
  return {{"ok",ok},{"code",code},{"error",error},{"documentId",documentId},
          {"catalogRevision",catalogRevision},{"version",version}};
}
ArtifactVersionResult ArtifactVersionCapability::describe(const DataCatalog *catalog,
    const QString &boundDocumentId,const ArtifactVersionRequest &request) {
  ArtifactVersionResult out;out.documentId=request.documentId.left(256);
  const auto fail=[&](const QString &code,const QString &error){out.code=code;out.error=error;out.version={};return out;};
  if (!catalog || !catalog->isOpen() || QThread::currentThread()!=catalog->thread())
    return fail("document_unavailable",QStringLiteral("版本查询目录未打开或线程不符"));
  if (boundDocumentId.isEmpty() || request.documentId.size()>256 || request.documentId!=boundDocumentId)
    return fail("document_mismatch",QStringLiteral("版本引用不属于当前文档"));
  out.catalogRevision=catalog->catalogRevision();
  if (request.expectedCatalogRevision!=out.catalogRevision)
    return fail("stale_revision",QStringLiteral("目录修订已变化，请重新取得查询上下文"));
  if (request.versionId.isEmpty() || request.versionId.size()>256)
    return fail("invalid_version_ref",QStringLiteral("须指定有效的精确版本标识"));
  const auto version=catalog->versionById(request.versionId);
  if (version.id!=request.versionId) return fail("version_not_found",QStringLiteral("指定版本不存在"));
  const auto asset=catalog->assetById(version.assetId);
  if (asset.id.isEmpty()) return fail("asset_not_found",QStringLiteral("版本对应的资产不存在"));
  if (asset.id.size()>256) return fail("invalid_version_ref",QStringLiteral("版本资产标识超出协议上限"));
  QJsonArray parents;
  for (qsizetype i=0;i<qMin<qsizetype>(128,version.parentVersionIds.size());++i) {
    if (version.parentVersionIds[i].size()>256) return fail("invalid_version_ref",QStringLiteral("输入版本标识超出协议上限"));
    parents.append(version.parentVersionIds[i]);
  }
  const auto horizon = version.extra.value("horizon").toString();
  bool truncated = asset.displayName.size()>512 || version.fileName.size()>512 || version.stage.size()>64 ||
      asset.type.size()>128 || horizon.size()>256;
  const auto unit = frozenUnit(analysisResultUnitSelection(catalog, version), truncated);
  out.version={{"id",version.id},{"assetId",version.assetId},{"displayName",asset.displayName.left(512)},
      {"assetType",asset.type.left(128)},{"resultKind",resultKind(version)},{"horizon",horizon.left(256)},{"unitSelection",unit},
      {"fileName",version.fileName.left(512)},{"stage",version.stage.left(64)},
      {"versionNumber",version.versionNumber},{"managed",version.managed},{"sha256",version.sha256.left(64)},
      {"stale",version.extra.value("stale").toBool()},
      {"labelsTruncated",truncated},
      {"parentCount",QString::number(version.parentVersionIds.size())},
      {"parentsTruncated",version.parentVersionIds.size()>128},{"parentVersionIds",parents}};
  out.ok=true;out.code="described";
  if (QJsonDocument(out.toJson()).toJson(QJsonDocument::Compact).size()>64*1024) {
    out.ok=false;return fail("response_budget_exceeded",QStringLiteral("版本描述超出回包预算"));
  }
  return out;
}
}
