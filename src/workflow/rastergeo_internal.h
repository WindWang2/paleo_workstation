#pragma once
// 层：功能
// PW-010/PW-011：工作流栅格读取的地理参考校验，唯一实现。
// 拒绝：无地理变换（CE_Failure，GDAL 默认 0,1,0,0,0,1 会把像素当米）、
// 旋转/斜切（gt[2]/gt[4] ≠ 0，北向上采样会错位）、dx ≤ 0 或 dy ≥ 0（非北向上）。
// 不依赖 Qt GUI / QGIS，便于独立测试。
#include <QByteArray>
#include <QString>
#include <cmath>
#include <gdal.h>
#include <ogr_srs_api.h>

namespace paleo::workflow_detail {

// 通用仿射校验：有限且非退化（|det| > 0）。像元级运算（集合统计、时深逐
// 像元换算）可接受旋转网格，但写出时必须原样带回旋转项。
inline bool validAffineGeoTransform( const double gt[6], QString *why = nullptr )
{
  auto fail = [why]( const QString &m ) { if ( why ) *why = m; return false; };
  for ( int i = 0; i < 6; ++i )
    if ( !std::isfinite( gt[i] ) )
      return fail( QStringLiteral( "地理变换含非有限值" ) );
  if ( !( std::abs( gt[1] * gt[5] - gt[2] * gt[4] ) > 0.0 ) )
    return fail( QStringLiteral( "地理变换退化（像元面积为 0）" ) );
  return true;
}

inline bool readAffineGeoTransform( GDALDatasetH ds, double gt[6], QString *why = nullptr )
{
  if ( !ds || GDALGetGeoTransform( ds, gt ) != CE_None )
  {
    if ( why )
      *why = QStringLiteral( "栅格没有地理参考（无地理变换）" );
    return false;
  }
  return validAffineGeoTransform( gt, why );
}

inline bool validNorthUpGeoTransform( const double gt[6], QString *why = nullptr )
{
  auto fail = [why]( const QString &m ) { if ( why ) *why = m; return false; };
  if ( !validAffineGeoTransform( gt, why ) )
    return false;
  if ( gt[2] != 0.0 || gt[4] != 0.0 )
    return fail( QStringLiteral( "栅格有旋转/斜切（gt[2]=%1, gt[4]=%2），不支持" ).arg( gt[2] ).arg( gt[4] ) );
  if ( !( gt[1] > 0.0 ) )
    return fail( QStringLiteral( "栅格像元宽度非正（dx=%1）" ).arg( gt[1] ) );
  if ( !( gt[5] < 0.0 ) )
    return fail( QStringLiteral( "栅格不是北向上（dy=%1）" ).arg( gt[5] ) );
  return true;
}

// 读取并校验；失败时 gt 内容未定义，调用方必须拒绝该栅格。
inline bool readNorthUpGeoTransform( GDALDatasetH ds, double gt[6], QString *why = nullptr )
{
  if ( !ds || GDALGetGeoTransform( ds, gt ) != CE_None )
  {
    if ( why )
      *why = QStringLiteral( "栅格没有地理参考（无地理变换）" );
    return false;
  }
  return validNorthUpGeoTransform( gt, why );
}

// 两张栅格是否同网格：尺寸相同、地理变换在半个微像元内一致、CRS 等价（都空也算相同）。
inline bool sameGeoGrid( int colsA, int rowsA, const double gtA[6], const QByteArray &wktA,
                         int colsB, int rowsB, const double gtB[6], const QByteArray &wktB,
                         QString *why = nullptr )
{
  auto fail = [why]( const QString &m ) { if ( why ) *why = m; return false; };
  if ( colsA != colsB || rowsA != rowsB )
    return fail( QStringLiteral( "尺寸不同（%1×%2 vs %3×%4）" ).arg( colsA ).arg( rowsA ).arg( colsB ).arg( rowsB ) );
  const double tol = 1e-6 * std::max( std::abs( gtA[1] ), std::abs( gtA[5] ) );
  for ( int i = 0; i < 6; ++i )
    if ( std::abs( gtA[i] - gtB[i] ) > tol )
      return fail( QStringLiteral( "地理变换不同（原点或像元大小不一致）" ) );
  if ( wktA.isEmpty() != wktB.isEmpty() )
    return fail( QStringLiteral( "坐标系不同（一方缺少 CRS）" ) );
  if ( !wktA.isEmpty() && wktA != wktB )
  {
    OGRSpatialReferenceH a = OSRNewSpatialReference( wktA.constData() );
    OGRSpatialReferenceH b = OSRNewSpatialReference( wktB.constData() );
    const bool same = a && b && OSRIsSame( a, b );
    if ( a ) OSRDestroySpatialReference( a );
    if ( b ) OSRDestroySpatialReference( b );
    if ( !same )
      return fail( QStringLiteral( "坐标系不同" ) );
  }
  return true;
}

} // namespace paleo::workflow_detail
