#pragma once

#include <QFile>
#include <QFileInfo>
#include <QTemporaryDir>

#include "../../src/catalog/datacatalog.h"
#include "../../src/metadata/layermanifest.h"
#include "../../src/metadata/paleoprojectstore.h"
#include "../../src/qgis/qgislayerservice.h"
#include "../../src/qgis/qgisprocessingservice.h"
#include "../../src/qgis/qgisprojectservice.h"

namespace paleo::tests {

// Declaration order is part of the fixture contract: temporary directory and
// project service outlive the manifest and all dependent services.
struct WorkflowFixture
{
  QTemporaryDir dir;
  DataCatalog catalog;
  QgisProjectService projectSvc;
  PaleoProjectStore store;
  LayerManifest manifest{ dir.filePath( QStringLiteral( "project.sqlite" ) ) };
  QgisLayerService layers{ &projectSvc, &manifest };
  QgisProcessingService proc{ &store };
  QString projectDir() const { return dir.path(); }
};

inline bool initFixture( WorkflowFixture &f )
{
  if ( !f.dir.isValid() )
    return false;
  if ( !f.catalog.open( f.dir.path() ) )
    return false;
  if ( !f.projectSvc.createProject( f.dir.filePath( QStringLiteral( "proj.qgz" ) ) ) )
    return false;
  if ( !f.manifest.open() )
    return false;
  return true;
}

inline bool writePointsGeoJson( const QString &path )
{
  QFile f( path );
  if ( !f.open( QIODevice::WriteOnly ) )
    return false;
  f.write( "{\"type\":\"FeatureCollection\",\"features\":["
           "{\"type\":\"Feature\",\"properties\":{\"z\":0.0},\"geometry\":{\"type\":\"Point\",\"coordinates\":[0.0,0.0]}},"
           "{\"type\":\"Feature\",\"properties\":{\"z\":8.0},\"geometry\":{\"type\":\"Point\",\"coordinates\":[4.0,0.0]}},"
           "{\"type\":\"Feature\",\"properties\":{\"z\":4.0},\"geometry\":{\"type\":\"Point\",\"coordinates\":[0.0,4.0]}}"
           "]}" );
  f.close();
  return QFile::exists( path ) && QFileInfo( path ).size() > 0;
}

inline bool derivedVersionRegistered( DataCatalog &catalog, const QString &assetType,
                                      const QString &absolutePath )
{
  for ( const CatalogAsset &a : catalog.assets() )
  {
    if ( a.type != assetType )
      continue;
    for ( const CatalogVersion &v : catalog.versionsForAsset( a.id ) )
      if ( absolutePath.contains( QStringLiteral( "artifacts/derived/" ) ) &&
           absolutePath.endsWith( QLatin1Char( '/' ) + v.fileName ) &&
           absolutePath.contains( v.id ) && !v.sha256.isEmpty() )
        return true;
  }
  return false;
}

} // namespace paleo::tests
