// 层：QGIS 封装
// qgis/geostatsamplereader.h — I1 地统计原子的受管点样本有界读取口。
//
// workflow 层（geostatatominputs）不直连 qgs* 头（qgs-audit / qgis-spread
// 闸：新增散布文件即红）——本封装按 geostatrasterio 同款口径提供纯值出口：
// worker 线程自建 QgsVectorLayer 实例（与既有 sampleGeostatWells 任务线程
// 用法同款），读完即弃，不触碰 DataCatalog、活图层或 GUI 连接。
//
// 读取边界（与 UI 链一致的科学口径）：
//   · 图层 CRS 缺失、地理坐标系（经纬度）或非米制单位 → 明确拒绝；
//   · 字段必须存在且为数值类型；空几何/NULL 缺测（QGIS 表示为值 0 且
//     isNull 的 QVariant）/非有限值行计入 skipped，不静默丢弃也不臆造 0 值；
//   · 读取量有硬上限（maxSamples），超限先于任何栅格分配失败。
#pragma once

#include "geostatrasterio.h"
#include "algorithms/geostat/types.h"

#include <QString>
#include <functional>
#include <vector>

namespace paleo::geostat
{

struct GeostatSampleReadResult
{
  bool ok = false;
  QString code;                 // 机器错误码（geostat_reader_*）
  QString message;              // 面向用户的补充说明（可空）
  std::vector<Sample> samples;  // 有限值样本（x, y, value）
  int skippedRows = 0;          // 空几何 / 非有限坐标 / 缺测值的行
  QString crsWkt;               // 规范 WKT（样本自身 CRS）
  QString crsAuthId;            // 如 EPSG:3857；本地工程网格可空
  RasterCrs crs;                // 不透明句柄，供 NativeCppAdapter 直接消费
  double minX = 0, maxX = 0, minY = 0, maxY = 0;
  bool hasBounds = false;       // ≥1 个有效样本时成立
};

// uri：OGR 可读的点图层 URI（受管版本文件的绝对路径）。field：数值字段名。
// cancelled：协作取消（在要素迭代间隔检查）。
GeostatSampleReadResult readGeostatPointSamples( const QString &uri, const QString &field,
                                                 int maxSamples,
                                                 const std::function<bool()> &cancelled );

// 从规范 WKT 重建不透明 CRS 句柄（快照恢复路径；无效 WKT 返回无效句柄）。
RasterCrs geostatRasterCrsFromWkt( const QString &wkt );

} // namespace paleo::geostat
