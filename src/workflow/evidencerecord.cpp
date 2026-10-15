// 层：功能
#include "evidencerecord.h"
#include "../catalog/datacatalog.h"
#include "../domain/predictionevidence.h"
#include <QCoreApplication>
#include <QSet>
#include <QHash>
#include <qgis.h>

namespace paleo::workflow_detail
{
QVariantMap evidenceRecord( const DataCatalog *catalog, const QStringList &parentVersionIds,
                            const QVariantMap &extra )
{
  QVector<paleo::evidence::InputRef> inputs;
  QVariantList missing;
  QSet<QString> seen;
  QHash<QString, QVariantList> referencesByVersion;
  QHash<QString, QSet<QString>> activeRoles;
  QSet<QString> referenceKeys;
  QVariantList interpretationReferences;
  for (const auto &entry : extra.value("interpretation_reference_versions").toList()) {
    auto ref = entry.toMap(); const auto version = ref.value("version_id").toString();
    if (version.isEmpty()) continue;
    const auto role = ref.value("role", "REFERENCES").toString();
    const bool tombstone = ref.value("tombstone", ref.value("unlinked", false)).toBool();
    ref.insert("role", role); ref.insert("tombstone", tombstone);
    const QString key = version + QChar(0x1f) + role + QChar(0x1f) + QString::number(tombstone);
    if (referenceKeys.contains(key)) continue;
    referenceKeys.insert(key); referencesByVersion[version] << ref;
    interpretationReferences << ref;
    if (!tombstone) activeRoles[version].insert(role);
  }
  QSet<QString> computationalParents;
  QStringList computationalList;
  if (extra.contains("computational_parent_versions")) {
    computationalList = extra.value("computational_parent_versions").toStringList();
  } else {
    for (const auto &parent : parentVersionIds)
      if (!referencesByVersion.contains(parent)) computationalList << parent;
  }
  computationalList.removeDuplicates();
  for (const auto &parent : computationalList) computationalParents.insert(parent);
  if ( catalog )
    for ( const QString &parent : parentVersionIds )
    {
      if (seen.contains(parent)) continue;
      seen.insert(parent);
      const CatalogVersion pv = catalog->versionById( parent );
      if ( pv.id.isEmpty() ) {
        paleo::evidence::InputRef absent; absent.versionId = parent; absent.missing = true;
        inputs << absent;
        missing << QVariantMap{{QStringLiteral("version_id"), parent},
                               {QStringLiteral("severity"), QStringLiteral("Blocking")},
                               {QStringLiteral("code"), QStringLiteral("missing_input_version")}};
        continue;
      }
      const CatalogAsset asset = catalog->assetById( pv.assetId );
      inputs << paleo::evidence::InputRef{ pv.id,
                                           pv.assetId,
                                           pv.extra.value( QStringLiteral( "title" ), asset.displayName ).toString(),
                                           asset.type,
                                           pv.extra.value( QStringLiteral( "kind" ) ).toString(),
                                           pv.versionNumber,
                                           pv.sha256,
                                           pv.stage,
                                           pv.extra.value( QStringLiteral( "mock" ) ).toBool() };
      auto &input = inputs.last();
      input.horizon = pv.extra.value(QStringLiteral("horizon")).toString();
      if (!computationalParents.contains(parent) && referencesByVersion.contains(parent)) {
        const auto roles = activeRoles.value(parent);
        input.role = roles.size() == 1 ? *roles.cbegin() : QStringLiteral("REFERENCES");
      }
    }
  QVariantMap generation = extra;
  // Append-only audit and display state do not change a scientific parameter hash.
  for (const auto &key : {QStringLiteral("interpretation_history"), QStringLiteral("review_events"),
                         QStringLiteral("actor_id"), QStringLiteral("feature_summary"),
                         QStringLiteral("evidence_quality"), QStringLiteral("evidence_snapshot"),
                         QStringLiteral("spatial_edit_events")})
    generation.remove(key);
  QVariantList wellRows = extra.value(QStringLiteral("rows")).toList();
  if (wellRows.isEmpty() && catalog)
    for (const auto &parent : parentVersionIds) {
      const auto source = catalog->versionById(parent);
      if (source.extra.value(QStringLiteral("kind")).toString() == QStringLiteral("well_factor_points"))
        wellRows += source.extra.value(QStringLiteral("rows")).toList();
    }
  if (!generation.contains(QStringLiteral("supporting_wells")) && !wellRows.isEmpty()) {
    QVariantList supporting, skipped;
    for (const auto &entry : wellRows) {
      const auto row = entry.toMap();
      (row.value(QStringLiteral("contributing")).toBool() ? supporting : skipped) << row;
    }
    generation.insert(QStringLiteral("supporting_wells"), supporting);
    generation.insert(QStringLiteral("skipped_wells"), skipped);
  }
  auto result = paleo::evidence::build(
      inputs, generation,
      { { QStringLiteral( "application" ), QCoreApplication::applicationName() },
        { QStringLiteral( "application_version" ), QCoreApplication::applicationVersion() },
        { QStringLiteral( "qt" ), QString::fromLatin1( qVersion() ) },
        { QStringLiteral( "qgis" ), Qgis::version() } } );
  result.insert(QStringLiteral("interpretation_references"), interpretationReferences);
  result.insert(QStringLiteral("computational_parent_versions"), computationalList);
  if (!missing.isEmpty())
    result.insert(QStringLiteral("quality_issues"), missing);
  return result;
}
} // namespace paleo::workflow_detail
