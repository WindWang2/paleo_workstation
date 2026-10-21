// 层：QGIS 封装
// qgis/geostatrasterio.h — algo-atom P2：NativeCpp geostat 适配器的 CRS 句柄与
// Float32 栅格写口。功能层（nativeinputs_geostat.h / algorithmadapters.cpp）
// 只经此头持有 CRS、写栅格，不直连 qgs* 头（qgs-audit / qgis-spread 闸）。
#pragma once

#include <QString>
#include <memory>
#include <vector>

class QgsCoordinateReferenceSystem;

namespace paleo
{

// 不透明 CRS 句柄：值语义（共享不可变），默认 = 无效 CRS。
class RasterCrs
{
public:
  RasterCrs();
  // 有意隐式：调用方可直接 `inputs.crs = crs;`。
  RasterCrs( const QgsCoordinateReferenceSystem &crs ); // NOLINT(google-explicit-constructor)
  const QgsCoordinateReferenceSystem &get() const;

private:
  std::shared_ptr<const QgsCoordinateReferenceSystem> m_crs;
};

// 行主序 double 网格 → Float32 GeoTIFF（非有限值写 nodata -9999）。
bool writeFloatRasterGrid( const QString &path, const std::vector<double> &values, int cols, int rows,
                           const double geoTransform[6], const RasterCrs &crs,
                           const QString &canonicalCrsWkt );

} // namespace paleo
