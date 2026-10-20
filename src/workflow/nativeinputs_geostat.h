// 层：功能
// workflow/nativeinputs_geostat.h — algo-atom P2：NativeCppAdapter geostat 输入（拆自 algorithmadapters.h，
// 只让用到该域的 TU 带入对应核头，压传递扇出）。
#pragma once

#include "../algorithms/geostat/cokriging.h"
#include "../algorithms/geostat/kriging.h"
#include "../algorithms/geostat/sgs.h"
#include <qgscoordinatereferencesystem.h>
#include <QString>
#include <array>
#include <vector>

namespace paleo
{

struct OrdinaryKrigingInputs
{
  std::vector<paleo::geostat::Sample> samples;
  paleo::geostat::GridSpec grid;
  paleo::geostat::VariogramModel model;
  paleo::geostat::KrigingParams params;
  paleo::geostat::Control control;
  QString estimatePath;
  QString variancePath; // 空 = 不写方差
  std::array<double, 6> geoTransform{ { 0, 1, 0, 0, 0, -1 } };
  QgsCoordinateReferenceSystem crs;
  QString canonicalCrsWkt;
};

struct OrdinaryCoKrigingInputs
{
  std::vector<paleo::geostat::Sample> primary;
  std::vector<paleo::geostat::Sample> secondary;
  paleo::geostat::GridSpec grid;
  paleo::geostat::CoKrigingModel model;
  paleo::geostat::CoKrigingParams params;
  paleo::geostat::Control control;
  QString estimatePath;
  QString variancePath;
  std::array<double, 6> geoTransform{ { 0, 1, 0, 0, 0, -1 } };
  QgsCoordinateReferenceSystem crs;
  QString canonicalCrsWkt;
};

struct SgsInputs
{
  std::vector<paleo::geostat::Sample> samples;
  paleo::geostat::GridSpec grid;
  paleo::geostat::VariogramModel model;
  paleo::geostat::SgsParams params;
  paleo::geostat::Control control;
  QString meanPath;
  QString stdPath;
  bool persistRealizations = true;
  QString memberPathPrefix; // 如 tempDir/member_r → 写出 member_r000.tif …
  std::array<double, 6> geoTransform{ { 0, 1, 0, 0, 0, -1 } };
  QgsCoordinateReferenceSystem crs;
  QString canonicalCrsWkt;
};

} // namespace paleo
