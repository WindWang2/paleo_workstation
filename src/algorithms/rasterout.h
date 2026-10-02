// 层：数据
#pragma once
#include <gdal.h>

#include <QString>

class QgsCoordinateReferenceSystem;

// algorithms/ — 单波段 Float32 GeoTIFF 写口（审计 02 M-2）。原先在
// paleoalgorithms/mincurvature/distancetransform/welldist 各复制一份且已分叉：
// 后两份不写 CRS，距离面输出的 GeoTIFF 丢了坐标系。现收敛为唯一实现，
// 恒写 GeoTransform；crs 有效时写投影（工程坐标系保 EDATUM）与 PALEO_CRS_WKT
// 元数据；nodata 写到波段。失败返回 nullptr，调用方负责 GDALClose()。
namespace PaleoRasterOut
{
  GDALDatasetH createFloatRaster( const QString &outPath, int nCols, int nRows,
                                  const double geoTransform[6],
                                  const QgsCoordinateReferenceSystem &crs,
                                  double nodata );
}
