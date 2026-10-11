// 层：功能
#include "evidencerecord.h"
#include "../catalog/datacatalog.h"
#include "../domain/predictionevidence.h"
#include <QCoreApplication>
#include <qgis.h>

namespace paleo::workflow_detail
{
QVariantMap evidenceRecord( const DataCatalog *catalog, const QStringList &parentVersionIds,
                            const QVariantMap &extra )
{
  QVector<paleo::evidence::InputRef> inputs;
  if ( catalog )
    for ( const QString &parent : parentVersionIds )
    {
      const CatalogVersion pv = catalog->versionById( parent );
      if ( pv.id.isEmpty() )
        continue;
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
    }
  return paleo::evidence::build(
      inputs, extra,
      { { QStringLiteral( "application" ), QCoreApplication::applicationName() },
        { QStringLiteral( "application_version" ), QCoreApplication::applicationVersion() },
        { QStringLiteral( "qt" ), QString::fromLatin1( qVersion() ) },
        { QStringLiteral( "qgis" ), Qgis::version() } } );
}
} // namespace paleo::workflow_detail
