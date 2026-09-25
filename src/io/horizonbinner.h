#pragma once
#include <QByteArray>
#include <QString>
#include <QVector>

// io/ — 层位散点 → 装箱时间栅格（plan §2/§3：一张图不画 26 万个点）。
// 网格几何从层位文件头冻结：Grid_size、P1/P2/P3（inline,crossline,x,y）、Z 单位。
// X 随 crossline、Y 随 inline 增加；北向上 geotransform 原点在最大 y、行方向 -dy。
struct HorizonHeader
{
  int gridRows = 0, gridCols = 0; // 411 inline 行 × 641 crossline 列
  int p1Inline = 0, p1Xline = 0; double p1x = 0, p1y = 0;
  int p2Inline = 0, p2Xline = 0; double p2x = 0, p2y = 0;
  int p3Inline = 0, p3Xline = 0; double p3x = 0, p3y = 0;
  QString zUnits;
};

struct BinnedHorizon
{
  int rows = 0, cols = 0;
  QVector<float> z;      // row-major（行=inline 升序），nodata=-9999
  int collisions = 0;    // 同像元多点：保留最后一点，计数进元数据
  double dx = 0, dy = 0; // 像元尺寸（米）
  double originX = 0, originY = 0; // 北向上左上角
  double zMin = 0, zMax = 0;
  int filledCells = 0;
};

// 从 '# Grid_size:411x641' / '# P1: ...' / '# Z_units: ms' 行取参数。
bool parseHorizonHeader(const QByteArray &text, HorizonHeader *out, QString *error = nullptr);

// 装箱：行=inline-p1Inline（0..rows-1），列=crossline-p1Xline。
bool binHorizon(const QByteArray &text, BinnedHorizon *out, QString *error = nullptr);

// 写北向上 Float32 GeoTIFF（geotransform/nodata/局部测网 CRS）。
bool writeHorizonGeoTiff(const BinnedHorizon &b, const QString &destPath, QString *error = nullptr);
