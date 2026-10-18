// 层：数据
#pragma once
#include <gdal.h>

#include <QString>
#include <QVariantMap>

#include <memory>

class QgsCoordinateReferenceSystem;

// algorithms/ — 单波段 Float32 GeoTIFF 写口（审计 02 M-2）。原先在
// paleoalgorithms/mincurvature/distancetransform/welldist 各复制一份且已分叉：
// 后两份不写 CRS，距离面输出的 GeoTIFF 丢了坐标系。现收敛为唯一实现，
// 恒写 GeoTransform；crs 有效时写投影（工程坐标系保 EDATUM）与 PALEO_CRS_WKT
// 元数据；nodata 写到波段。失败返回 nullptr，句柄所有权交调用方——接进
// GdalDatasetPtr 即 RAII 关闭，不再手工 GDALClose（#553）。
namespace PaleoRasterOut
{
  // #553：GDAL 数据集 RAII 守卫——unique_ptr + GDALClose 删除器，析构即关。
  // 算法里 throw / 提前返回路径不再逐点手工 close（新增退出路径漏写即泄漏）；
  // 仍持裸句柄契约的交接（如 create*Raster 的返回值）用 release() 取回。
  // 元素类型用 void（GDALDatasetH 本身就是 void*；services 层同款先例）：
  // unique_ptr<void*, D> 的 pointer 会被解析成 void**，构造/reset 反而不编。
  struct GdalDatasetCloser
  {
    void operator()( GDALDatasetH ds ) const { GDALClose( ds ); }
  };
  using GdalDatasetPtr = std::unique_ptr<void, GdalDatasetCloser>;

  // canonicalCrsWkt（ARCH-05 参数化）：调用方传入的规范局部网格 WKT。
  // crs 与该 WKT 表示同一坐标系时按原串写出——QGIS 工程坐标系的再导出会
  // 丢 EDATUM，丢失后 GeoTIFF 与工程网格不再判等。空串 = 无规范覆盖，
  // 一律走 QGIS PreferredGdal 导出（非局部网格 CRS 与旧行为逐字节一致）。
  // 算法核不问 catalog：规范 WKT 由 workflow 侧以 LOCAL_GRID_WKT 处理参数
  // 注入（见 canonicalWktFromParameters）。
  // BIZ-11（方向58）：crs 无效（输入无 CRS）时，canonicalCrsWkt 非空 → 按规范
  // 串写投影 + PALEO_CRS_WKT；两者皆空 → 不写投影（不编造 CRS；仅直调路径，
  // workflow 路径恒注入 LOCAL_GRID_WKT）。realizationworkflow 传空 crs 后自行
  // GDALSetProjection 沿用成员投影，不受影响。
  GDALDatasetH createFloatRaster( const QString &outPath, int nCols, int nRows,
                                  const double geoTransform[6],
                                  const QgsCoordinateReferenceSystem &crs,
                                  double nodata,
                                  const QString &canonicalCrsWkt = QString() );

  // 与 createFloatRaster 同一 GeoTransform / CRS 写口。不写 nodata，
  // 像元值 0 会保留（支撑标记 0 是域外缺失，不是文件空值）。
  GDALDatasetH createByteRaster( const QString &outPath, int nCols, int nRows,
                                 const double geoTransform[6],
                                 const QgsCoordinateReferenceSystem &crs,
                                 const QString &canonicalCrsWkt = QString() );

  // 结构面引擎的 Float64 写口（与上游 trend grid 精度对齐）；nodata 写波段。
  GDALDatasetH createDoubleRaster( const QString &outPath, int nCols, int nRows,
                                   const double geoTransform[6],
                                   const QgsCoordinateReferenceSystem &crs,
                                   double nodata,
                                   const QString &canonicalCrsWkt = QString() );

  // 处理算法侧统一取参口：LOCAL_GRID_WKT（隐藏可选参数，缺省空）。
  // workflow 构造 runParams 时注入 DataCatalog::localGridCrsWkt()；直接
  // 调算法的测试不传即无规范覆盖。
  inline QString canonicalWktFromParameters( const QVariantMap &parameters )
  {
    return parameters.value( QStringLiteral( "LOCAL_GRID_WKT" ) ).toString();
  }
}
