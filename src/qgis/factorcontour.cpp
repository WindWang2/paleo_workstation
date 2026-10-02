// 层：QGIS 封装
#include "factorcontour.h"

#include <QByteArray>
#include <QFile>
#include <QStringList>

#include <cmath>

#include <cpl_conv.h>
#include <cpl_string.h>
#include <gdal.h>
#include <gdal_alg.h>
#include <ogr_api.h>
#include <ogr_srs_api.h>

// 层：QGIS 封装

namespace
{
  void setError( QString *error, const QString &text )
  {
    if ( error )
      *error = text;
  }
} // namespace

namespace FactorContourService
{

namespace
{
bool generateWithOption( const QString &rasterPath, const QString &outputGpkg,
                         const QByteArray &levelOption, QString *error )
{
  if ( rasterPath.isEmpty() || !QFile::exists( rasterPath ) )
  {
    setError( error, QStringLiteral( "contour input raster not found: %1" ).arg( rasterPath ) );
    return false;
  }
  if ( outputGpkg.isEmpty() )
  {
    setError( error, QStringLiteral( "contour output path is empty" ) );
    return false;
  }

  GDALAllRegister();

  GDALDatasetH src = GDALOpen( rasterPath.toUtf8().constData(), GA_ReadOnly );
  if ( !src )
  {
    setError( error, QStringLiteral( "cannot open raster '%1' for contouring" ).arg( rasterPath ) );
    return false;
  }
  GDALRasterBandH band = GDALGetRasterBand( src, 1 );
  if ( !band )
  {
    setError( error, QStringLiteral( "raster '%1' has no band 1" ).arg( rasterPath ) );
    GDALClose( src );
    return false;
  }

  // GPKG 输出（OGR 面）；同路径重生成 → 先移除旧文件保证幂等。
  QFile::remove( outputGpkg );
  GDALDriverH drv = GDALGetDriverByName( "GPKG" );
  if ( !drv )
  {
    setError( error, QStringLiteral( "GDAL GPKG driver unavailable" ) );
    GDALClose( src );
    return false;
  }
  GDALDatasetH ds = GDALCreate( drv, outputGpkg.toUtf8().constData(), 0, 0, 0, GDT_Unknown, nullptr );
  if ( !ds )
  {
    setError( error, QStringLiteral( "cannot create contour output '%1'" ).arg( outputGpkg ) );
    GDALClose( src );
    return false;
  }

  OGRSpatialReferenceH srs = const_cast<OGRSpatialReferenceH>( GDALGetSpatialRef( src ) );
  OGRSpatialReferenceH preserved = nullptr;
  if(const char *wkt=GDALGetMetadataItem(src,"PALEO_CRS_WKT",nullptr)) {
    preserved=OSRNewSpatialReference(wkt);if(preserved)srs=preserved;
  }
  OGRLayerH layer = GDALDatasetCreateLayer( ds, "contours", srs, wkbLineString, nullptr );
  if(preserved)OSRDestroySpatialReference(preserved);
  if ( !layer )
  {
    setError( error, QStringLiteral( "cannot create layer 'contours' in '%1'" ).arg( outputGpkg ) );
    GDALClose( ds );
    GDALClose( src );
    return false;
  }
  OGRFieldDefnH idField = OGR_Fld_Create( "ID", OFTInteger64 );
  OGR_L_CreateField( layer, idField, true );
  OGR_Fld_Destroy( idField );
  OGRFieldDefnH elevField = OGR_Fld_Create( "ELEV", OFTReal );
  OGR_L_CreateField( layer, elevField, true );
  OGR_Fld_Destroy( elevField );

  // 与 gdal:contour 工具同参（LEVEL_INTERVAL/ID_FIELD/ELEV_FIELD 是
  // GDALContourGenerateEx 认的 option 名——utility 与 C API 共用底层）。
  const char *options[] = { levelOption.constData(), "ID_FIELD=0", "ELEV_FIELD=1", nullptr };

  const CPLErr rc = GDALContourGenerateEx( band, layer, options, nullptr, nullptr );

  GDALClose( ds );
  GDALClose( src );

  if ( rc != CE_None )
  {
    setError( error, QStringLiteral( "GDALContourGenerateEx failed (rc=%1)" ).arg( int( rc ) ) );
    return false;
  }
  if ( !QFile::exists( outputGpkg ) )
  {
    setError( error, QStringLiteral( "contour run reported success but '%1' is missing" ).arg( outputGpkg ) );
    return false;
  }
  return true;
}
} // namespace

bool generateContours( const QString &rasterPath, const QString &outputGpkg,
                       double interval, QString *error )
{
  if ( !( interval > 0.0 ) )
  {
    setError( error, QStringLiteral( "contour interval must be positive (got %1)" ).arg( interval ) );
    return false;
  }
  const QByteArray levelOption = QByteArray( "LEVEL_INTERVAL=" ) + QByteArray::number( interval );
  return generateWithOption( rasterPath, outputGpkg, levelOption, error );
}

bool generateFixedContours( const QString &rasterPath, const QString &outputGpkg,
                            const QVector<double> &levels, QString *error )
{
  if ( levels.isEmpty() )
  {
    setError( error, QStringLiteral( "contour levels are empty" ) );
    return false;
  }
  QStringList text;
  for ( double level : levels )
  {
    if ( !std::isfinite( level ) )
    {
      setError( error, QStringLiteral( "contour level must be finite" ) );
      return false;
    }
    text << QString::number( level, 'g', 17 );
  }
  const QByteArray levelOption = QByteArray( "FIXED_LEVELS=" ) + text.join( QLatin1Char( ',' ) ).toUtf8();
  return generateWithOption( rasterPath, outputGpkg, levelOption, error );
}

} // namespace FactorContourService
