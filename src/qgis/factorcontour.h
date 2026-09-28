// 层：QGIS 封装
#pragma once
#include <QString>

// qgis/factorcontour.h — 单因素等值线（m2/mapping-pages 任务 B）。
// PALEO_QGIS_PLAN §12：等值线是 GIS LineString 图层，不是画布临时线。
// 降级决议（§32/§33 评审钉死）：本机 Processing 注册表在 C++ 嵌入运行时
// 只含 paleo:*（native/gdal 等均不自动注册；gdal:contour 属 Python
// provider），因此 raster → 等值线走 GDAL C API GDALContourGenerateEx
// （gdal:contour 的同一底层引擎），产出真 LineString GPKG 图层，
// 禁止假图层。
// 层：QGIS 封装
namespace FactorContourService
{

// 从 rasterPath 的 band 1 生成等值线（间距 interval，等值线基准取 0）写入
// outputGpkg 的 "contours" 图层（字段 ID int64 / ELEV real，SRS 随栅格）。
// 输出文件已存在则覆盖（重生成幂等由调用方按 GPKG 路径 upsert 承载）。
bool generateContours( const QString &rasterPath, const QString &outputGpkg,
                       double interval, QString *error = nullptr );

} // namespace FactorContourService
