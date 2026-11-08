// 层：QGIS 封装
#include "geostatcovariatesampler.h"

#include <qgscoordinatetransform.h>
#include <qgsrasterdataprovider.h>
#include <qgsrasterlayer.h>

#include <qgsexception.h>

#include <cmath>

namespace paleo::geostat
{

namespace
{
QString crsLabel( const QgsCoordinateReferenceSystem &crs )
{
  return crs.authid().isEmpty() ? QObject::tr( "自定义 CRS(%1…)" )
                                      .arg( crs.toWkt().left( 24 ) )
                                : crs.authid();
}
} // namespace

bool sampleCovariateRaster( QgsRasterLayer *raster,
                            const QgsCoordinateReferenceSystem &computeCrs,
                            const std::vector<QgsPointXY> &points,
                            const std::function<bool()> &cancelled,
                            std::vector<double> *values,
                            CovariateSampleDiagnostics *diagnostics,
                            QString *error )
{
  if ( error )
    error->clear();
  if ( !raster || !raster->isValid() || !raster->dataProvider() )
  {
    if ( error )
      *error = QObject::tr( "协变量栅格无效" );
    return false;
  }
  const QgsCoordinateReferenceSystem rasterCrs = raster->crs();
  if ( !rasterCrs.isValid() )
  {
    // 不能把 CRS 未知当与计算域相同——采样会静默错位。本地工程网格栅格
    // 声明 canonical WKT 后与计算 CRS 相等，走下方验证过的直采路径。
    if ( error )
      *error = QObject::tr( "协变量栅格缺少坐标系：无法把井位/格点变换到栅格域采样，"
                            "请为协变量声明 CRS（本地工程网格或投影坐标系）" );
    return false;
  }
  if ( diagnostics )
  {
    diagnostics->computeCrs = crsLabel( computeCrs );
    diagnostics->rasterCrs = crsLabel( rasterCrs );
  }

  const QgsCoordinateTransform transform( computeCrs, rasterCrs,
                                          QgsCoordinateTransformContext() );
  bool direct = false; // 不经变换（位置已在栅格 CRS）
  if ( transform.isValid() && transform.isShortCircuited() )
  {
    direct = true;
    if ( diagnostics )
      diagnostics->transformMode = QStringLiteral( "short_circuited" );
  }
  else if ( transform.isValid() )
  {
    if ( diagnostics )
      diagnostics->transformMode = QStringLiteral( "projected" );
  }
  else if ( computeCrs.toWkt() == rasterCrs.toWkt() )
  {
    // 变换初始化失败但两侧 WKT 逐字相等（如同一自定义工程网格 WKT）——
    // 坐标本来就同域，直采是可验证的等价，不是猜测。
    direct = true;
    if ( diagnostics )
      diagnostics->transformMode = QStringLiteral( "direct_wkt_equal" );
  }
  else
  {
    if ( error )
      *error = QObject::tr( "协变量栅格 CRS（%1）无法变换到计算 CRS（%2）："
                            "坐标变换不可用" )
                   .arg( crsLabel( rasterCrs ), crsLabel( computeCrs ) );
    return false;
  }

  QgsRasterDataProvider *provider = raster->dataProvider();
  const bool hasNoData = provider->sourceHasNoDataValue( 1 ) && provider->useSourceNoDataValue( 1 );
  const double noDataValue = hasNoData ? provider->sourceNoDataValue( 1 ) : 0.0;
  const QgsRectangle extent = raster->extent();
  values->clear();
  values->reserve( points.size() );
  if ( diagnostics )
    diagnostics->requested = static_cast<int>( points.size() );
  int sinceCancelCheck = 0;
  for ( const QgsPointXY &point : points )
  {
    if ( cancelled && ( ++sinceCancelCheck & 0xFF ) == 0 && cancelled() )
    {
      if ( error )
        *error = QObject::tr( "已取消" );
      return false;
    }
    QgsPointXY location = point;
    if ( !direct )
    {
      try
      {
        location = transform.transform( point );
      }
      catch ( const QgsException & )
      {
        if ( diagnostics )
          ++diagnostics->transformFailed;
        values->push_back( std::numeric_limits<double>::quiet_NaN() );
        continue;
      }
    }
    if ( !extent.contains( location ) )
    {
      if ( diagnostics )
        ++diagnostics->outOfExtent;
      values->push_back( std::numeric_limits<double>::quiet_NaN() );
      continue;
    }
    bool ok = false;
    const double value = provider->sample( location, 1, &ok );
    if ( !ok )
    {
      // 范围内但 provider 无值：nodata 像元（GDAL sample 对 nodata 报 !ok）。
      if ( diagnostics )
        ++diagnostics->nodata;
      values->push_back( std::numeric_limits<double>::quiet_NaN() );
      continue;
    }
    if ( !std::isfinite( value ) )
    {
      if ( diagnostics )
        ++diagnostics->nonFinite;
      values->push_back( std::numeric_limits<double>::quiet_NaN() );
      continue;
    }
    if ( hasNoData && value == noDataValue )
    {
      if ( diagnostics )
        ++diagnostics->nodata;
      values->push_back( std::numeric_limits<double>::quiet_NaN() );
      continue;
    }
    if ( diagnostics )
      ++diagnostics->sampled;
    values->push_back( value );
  }
  return true;
}

} // namespace paleo::geostat
