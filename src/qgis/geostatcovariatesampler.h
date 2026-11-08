// 层：QGIS 封装
#pragma once

// geostatcovariatesampler — P3：协克里金协变量的跨 CRS 局部采样。
// 井位（主样本配置点）与输出格点两条路径调用同一实现：位置先从计算 CRS
//（井/网格域）变换到协变量栅格 CRS，再经 provider 采样；返回给协克里金核
// 的 secondary 坐标始终保留计算 CRS（变换只用于采样定位，不换核内坐标域）。
//
// CRS 等价判断不做 authid 字符串猜测：QgsCoordinateTransform 短路
//（isShortCircuited）= 经验证的直采路径；变换初始化失败且两侧 WKT 不等
// = 明确整体失败（工程网格缺配准时如实报错，不套 EPSG、不当同 CRS）。
//
// 线程契约：raster 由调用线程创建/释放（worker 持有自己的受支持实例，
// 与 runGeostatCokriging 现状同款）；本组件无全局状态、无 QgsProject 依赖。

#include <qgscoordinatereferencesystem.h>
#include <qgspointxy.h>

#include <QString>

#include <functional>
#include <vector>

class QgsRasterLayer;

namespace paleo::geostat
{

// 逐点分类诊断——「有图层」不等于「协变量有效」，成功/失败数量要对得上。
struct CovariateSampleDiagnostics
{
  int requested = 0;        // 请求采样点数
  int sampled = 0;          // 成功且有限、非 nodata
  int nodata = 0;           // provider 无值或命中 nodata（含范围内不可读）
  int outOfExtent = 0;      // 变换后的位置在栅格范围外
  int nonFinite = 0;        // provider 报 ok 但值非有限（inf/NaN）
  int transformFailed = 0;  // 单点坐标变换失败（变换整体可用时的个别失败）
  QString computeCrs;       // 计算 CRS 描述
  QString rasterCrs;        // 协变量栅格 CRS 描述
  QString transformMode;    // short_circuited | projected | direct_wkt_equal
};

// 对一组计算 CRS 坐标点采样协变量栅格。成功返回 true 且 values 与 points
// 对齐（NaN = 该点采样失败，原因计入 diagnostics）。整体失败（栅格无 CRS、
// 变换初始化不可用、取消）返回 false 并给出 error 文案。
bool sampleCovariateRaster( QgsRasterLayer *raster,
                            const QgsCoordinateReferenceSystem &computeCrs,
                            const std::vector<QgsPointXY> &points,
                            const std::function<bool()> &cancelled,
                            std::vector<double> *values,
                            CovariateSampleDiagnostics *diagnostics,
                            QString *error );

} // namespace paleo::geostat
