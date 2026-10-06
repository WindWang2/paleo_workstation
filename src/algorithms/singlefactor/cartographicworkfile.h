// 层：数据
#pragma once

#include "types.h"

#include <qgscoordinatereferencesystem.h>

#include <QString>
#include <functional>
#include <string>
#include <vector>

// 制图工作场的文件写口。不包含 Processing 算法类。
// 解析约束会打开矢量图层，只在调用线程使用。写文件只走 GDAL 和数值核。
// 层：数据
namespace paleo::singlefactor
{

struct CartographicConstraintParse
{
  bool ok = false;
  QString error;
  std::vector<ConstraintLine> lines;
  std::vector<std::string> ignored;
};

// 打开 vectorUri 指向的线图层，把坐标变到 targetCrs。失败时 ok 为假。
CartographicConstraintParse parseCartographicConstraints( const QString &vectorUri,
                                                         const QgsCoordinateReferenceSystem &targetCrs );

struct CartographicWorkWrite
{
  QString analysisPath;
  QString outputPath;
  std::vector<ConstraintLine> lines;
  std::vector<std::string> ignored;
  std::vector<double> levels;
  double transition = 0.0;
  // 为真时用分析栅格的 GDAL WKT 建立 CRS，不构造 QgsRasterLayer。
  // 为假时沿用 crs，无效 CRS 表示局部工程网。
  bool deriveCrsFromDataset = false;
  QgsCoordinateReferenceSystem crs;
  // 规范局部网格 WKT（ARCH-05 参数化）：crs 与之同坐标系时输出 GeoTIFF 按
  // 原串写（保 EDATUM）。空 = 无规范覆盖。由调用方注入，算法核不问 catalog。
  QString canonicalCrsWkt;
  std::function<bool()> cancelled;
};

struct CartographicWorkWritten
{
  bool ok = false;
  bool cancelled = false;
  QString error;
  QString outputPath;
  QString qcPath;
  int modifiedCells = 0;
  int unchanged = 0;
  int unresolvedCrossings = 0;
};

// 读分析栅格、按级别提线、跑制图核，并写出工作场和 QC。不跑 Processing。
CartographicWorkWritten writeCartographicWorkFile( const CartographicWorkWrite &request );

} // namespace paleo::singlefactor
