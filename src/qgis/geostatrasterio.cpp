// 层：QGIS 封装
#include "geostatrasterio.h"

#include "../algorithms/rasterout.h"

#include <gdal.h>
#include <qgscoordinatereferencesystem.h>

#include <cmath>

namespace paleo
{

RasterCrs::RasterCrs()
  : m_crs( std::make_shared<const QgsCoordinateReferenceSystem>() )
{
}

RasterCrs::RasterCrs( const QgsCoordinateReferenceSystem &crs )
  : m_crs( std::make_shared<const QgsCoordinateReferenceSystem>( crs ) )
{
}

const QgsCoordinateReferenceSystem &RasterCrs::get() const
{
  return *m_crs;
}

bool writeFloatRasterGrid( const QString &path, const std::vector<double> &values, int cols, int rows,
                           const double geoTransform[6], const RasterCrs &crs,
                           const QString &canonicalCrsWkt )
{
  GDALDatasetH ds = PaleoRasterOut::createFloatRaster( path, cols, rows, geoTransform, crs.get(), -9999.0,
                                                       canonicalCrsWkt );
  if ( !ds )
    return false;
  std::vector<float> row( static_cast<std::size_t>( cols ) );
  bool ok = true;
  for ( int r = 0; r < rows && ok; ++r )
  {
    for ( int c = 0; c < cols; ++c )
    {
      const double value = values[static_cast<std::size_t>( r ) * cols + c];
      row[static_cast<std::size_t>( c )] =
          std::isfinite( value ) ? static_cast<float>( value ) : -9999.0f;
    }
    if ( GDALRasterIO( GDALGetRasterBand( ds, 1 ), GF_Write, 0, r, cols, 1, row.data(), cols, 1,
                       GDT_Float32, 0, 0 ) != CE_None )
      ok = false;
  }
  GDALClose( ds );
  return ok;
}

} // namespace paleo
