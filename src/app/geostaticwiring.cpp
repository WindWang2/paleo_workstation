// 层：组装根
// app/geostaticwiring.cpp — 见 geostaticwiring.h。
#include "geostaticwiring.h"

#include "workflow/capabilityhost.h"
#include "workflow/documentsession.h"
#include "workflow/geostatatomoperation.h"
#include "services/geostaticapabilityspec.h"
#include "catalog/datacatalog.h"

#include <QPointer>

void bindGeostatAtomCapabilities( paleo::capability::CapabilityHost *host, DataCatalog *catalog,
                                  paleo::document::DocumentSession *document,
                                  const std::function<bool()> &currentBinding )
{
  if( !host )
    return;
  const QPointer<DataCatalog> catalogGuard( catalog );
  const QPointer<paleo::document::DocumentSession> documentGuard( document );
  paleo::geostat_atom::GeostatAtomProviders providers;
  providers.alive = [catalogGuard, documentGuard, currentBinding, catalog, document]() {
    if( !currentBinding() )
      return false;
    if( catalog && !catalogGuard )
      return false;
    if( document && !documentGuard )
      return false;
    return true;
  };
  providers.catalog = [catalogGuard]() { return catalogGuard.data(); };
  host->registerExecutionAdapter( paleo::geostat_atom::kKrigingCapabilityId(),
                                  paleo::geostat_atom::geostatAtomExecutionAdapter(
                                      paleo::geostat_atom::kKrigingCapabilityId(), providers ) );
  host->registerExecutionAdapter( paleo::geostat_atom::kSgsCapabilityId(),
                                  paleo::geostat_atom::geostatAtomExecutionAdapter(
                                      paleo::geostat_atom::kSgsCapabilityId(), providers ) );
}
