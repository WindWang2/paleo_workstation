// 层：功能
#pragma once

#include "../io/timedeptool.h"
#include "../services/projectdata.h"
#include <gdal.h>
#include <cmath>
#include <limits>

// Validation and the publication gate must use identical depth/point/cell rules.
namespace MappingSamples
{
  // TD 表 → TIME(ms)：走数据底座的 TimeDepthTool（plan §3：文件顺序、不排序、
  // 不外推）。分层 TVD 有效时查 TVD 列；TVD 空（-99999）改用 MD 对 MD 列。
  // TdResult.status 区分 无时深表/超出时深表/时深表无序，文案经
  // TimeDepthTool::reasonText() 取——剖面标层与残差共用这一个结果。
  inline TimeDepthTool::TdResult timeForTop( const QVector<TdSample> &td, const WellTop &top )
  {
    TimeDepthTool::TdResult none; // status=NoTable
    if ( td.isEmpty() )
      return none;
    TimeDepthTable table;
    table.rows.reserve( td.size() );
    for ( const TdSample &s : td )
    {
      TdRow row;
      row.timeMs = s.timeMs;
      row.tvd = s.tvd;
      row.md = s.md;
      row.hasTvd = !qIsNaN( s.tvd ); // NaN 透传 -99999/缺列 → 不进插值
      row.hasMd = !qIsNaN( s.md );
      table.rows.append( row );
    }
    if ( !qIsNaN( top.tvd ) )
      return TimeDepthTool::interpolateTimeMs( table, top.tvd, /*useMd=*/false );
    if ( !qIsNaN( top.md ) ) // 分层 TVD 空 → MD 对 TD 的 MD 列兜底（plan §3）
      return TimeDepthTool::interpolateTimeMs( table, top.md, /*useMd=*/true );
    return none; // 分层两个深度都没有 → 按无时深表处理
  }

  // 井/分层的采样点：分层 X/Y 优先，缺省退井口。
  inline void pickSamplePoint( const ProjectWell &well, const WellTop *top, double *x, double *y )
  {
    if ( top && std::isfinite( top->x ) && std::isfinite( top->y ) )
    {
      *x = top->x;
      *y = top->y;
    }
    else
    {
      *x = well.surfaceX;
      *y = well.surfaceY;
    }
  }

  // 包含像元采样：左闭右开；恰在外边界上时归末像元（x==xmax→最后一列，
  // y==ymin→最后一行）。返回值区分三种结局——网外/空道/数值——
  // 前两种不算数值残差（autoplan §5C）。
  enum class SampleOutcome
  {
    Ok,
    Outside,
    Nodata,
  };

  inline SampleOutcome sampleRasterAt( GDALDatasetH ds, double x, double y, double *out )
  {
    if ( !std::isfinite( x ) || !std::isfinite( y ) )
      return SampleOutcome::Outside;
    double gt[6] = { 0, 0, 0, 0, 0, 0 };
    GDALGetGeoTransform( ds, gt );
    const int cols = GDALGetRasterXSize( ds );
    const int rows = GDALGetRasterYSize( ds );
    int col = static_cast<int>( std::floor( ( x - gt[0] ) / gt[1] ) );
    int row = static_cast<int>( std::floor( ( y - gt[3] ) / gt[5] ) );
    const double xmax = gt[0] + gt[1] * cols;
    const double ymin = gt[3] + gt[5] * rows;
    // 恰在外边界 → 末像元。不取逐位相等：调用方若由 xmin+cols*dx 反推
    // xmax，浮点可能差 1 ULP——按 1 ULP 容差归边（audit #35 附注）。
    const double ulpX =
        std::nextafter( xmax, std::numeric_limits<double>::infinity() ) - xmax;
    const double ulpY =
        std::nextafter( ymin, std::numeric_limits<double>::infinity() ) - ymin;
    if ( col == cols && qAbs( x - xmax ) <= ulpX )
      col = cols - 1; // 恰在外边界 → 最后一列
    if ( row == rows && qAbs( y - ymin ) <= ulpY )
      row = rows - 1; // 恰在外边界 → 最后一行
    if ( col < 0 || row < 0 || col >= cols || row >= rows )
      return SampleOutcome::Outside;
    if ( GDALGetRasterCount( ds ) < 1 )
      return SampleOutcome::Nodata;
    GDALRasterBandH band = GDALGetRasterBand( ds, 1 );
    if ( !band )
      return SampleOutcome::Nodata;
    float v = 0.0f;
    if ( GDALRasterIO( band, GF_Read, col, row, 1, 1, &v, 1, 1, GDT_Float32, 0, 0 ) != CE_None )
      return SampleOutcome::Nodata;
    int hasNodata = 0;
    const double nodata = GDALGetRasterNoDataValue( band, &hasNodata );
    if ( std::isnan( v ) || ( hasNodata && qAbs( static_cast<double>( v ) - nodata ) < 1e-6 ) )
      return SampleOutcome::Nodata;
    if ( out )
      *out = v;
    return SampleOutcome::Ok;
  }

} // namespace MappingSamples
