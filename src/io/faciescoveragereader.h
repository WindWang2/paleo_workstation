// 层：数据
#pragma once
#include <QString>

#include "../algorithms/evolution/types.h"

// io/faciescoveragereader — 相多边形 GPKG → 演化对比 DTO（方向35）。
// 读取面（GDAL 只读）：facies_code + 多边形几何 + PALEO_PROVENANCE 里
// faciespolygonize 落盘的格网口径戳（grid：尺寸/像元/原点/CRS）。
// 没有格网戳的旧产物如实报错——演化对比的拒算面要求两期都能自证同源，
// 不允许拿缺口径的输入近似对比。
namespace paleo::evolution
{

struct CoverageReadResult
{
  bool ok = false;
  QString error;
  FaciesCoverage coverage;
  // provenance 原文（报告/排查透传；空 = 无戳之外仍读到了旧版无 provenance）。
  QString provenance;
};

// sourceUri：QGIS 图层源（"<gpkg>|layername=facies_polygons" 或裸路径）。
// horizon：写入 coverage.horizon 的层位名（读取面不知道声明层位）。
CoverageReadResult readFaciesCoverage( const QString &sourceUri, const QString &horizon );

} // namespace paleo::evolution
