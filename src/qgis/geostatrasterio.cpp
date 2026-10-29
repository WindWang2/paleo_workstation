// 层：QGIS 封装
#include "geostatrasterio.h"

#include "../algorithms/rasterout.h"

#include <gdal.h>
#include <qgscoordinatereferencesystem.h>

#include <QFile>

#include <cmath>
#include <vector>

namespace paleo
{
namespace
{
// 原子替换（#73）：staging 就位后，旧产物先改名 .bak 再 rename——
// 直接 QFile::remove + rename 有个「remove 成功而 rename 失败」的窗口，新旧
// 两失。rename 失败时把 .bak 改回原名，旧产物原样保留。
bool replacePath( const QString &finalPath, const QString &staging )
{
  const QString backup = finalPath + QStringLiteral( ".bak" );
  QFile::remove( backup );
  if ( QFile::exists( finalPath ) && !QFile::rename( finalPath, backup ) )
    return false;
  if ( !QFile::rename( staging, finalPath ) )
  {
    QFile::rename( backup, finalPath ); // 回滚：旧产物还原
    return false;
  }
  QFile::remove( backup );
  return true;
}
} // namespace

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
  // 不能在正式路径上直接 GDALCreate：它会截断旧文件，行写中途失败（磁盘满 /
  // IO 错）时半截 GeoTIFF 留在正式路径，而旧的好产物已被毁（#73）。先写
  // .tmp，全部行写成功并关闭后才替换；失败删 tmp，正式路径保持调用前状态。
  const QString staging = path + QStringLiteral( ".tmp" );
  QFile::remove( staging );
  GDALDatasetH ds = PaleoRasterOut::createFloatRaster( staging, cols, rows, geoTransform, crs.get(), -9999.0,
                                                       canonicalCrsWkt );
  if ( !ds )
  {
    QFile::remove( staging );
    return false;
  }
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
  // GDALClose 才真正落盘（缓冲刷新/目录写入）；关失败同样算失败。
  if ( GDALClose( ds ) != CE_None )
    ok = false;
  if ( !ok || !replacePath( path, staging ) )
  {
    QFile::remove( staging );
    return false;
  }
  return true;
}

} // namespace paleo
