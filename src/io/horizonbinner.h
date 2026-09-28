// 层：数据
#pragma once
#include <QByteArray>
#include <QString>
#include <QVector>
#include <QtGlobal> // qQNaN

// io/ — 层位散点 → 装箱时间栅格（plan §2/§3：一张图不画 26 万个点）。
// 网格几何从层位文件头冻结：Grid_size、P1/P2/P3（inline,crossline,x,y）、Z 单位。
// X 随 crossline、Y 随 inline 增加；北向上 geotransform 原点在最大 y、行方向 -dy。
struct HorizonHeader
{
  int gridRows = 0, gridCols = 0; // 411 inline 行 × 641 crossline 列
  int p1Inline = 0, p1Xline = 0; double p1x = 0, p1y = 0;
  int p2Inline = 0, p2Xline = 0; double p2x = 0, p2y = 0;
  int p3Inline = 0, p3Xline = 0; double p3x = 0, p3y = 0;
  bool hasP1 = false, hasP2 = false, hasP3 = false;
  QString zUnits;
};

struct BinnedHorizon
{
  int rows = 0, cols = 0;
  QVector<float> z;      // row-major（行=inline 升序），nodata=-9999
  int collisions = 0;    // 同像元多点：保留最后一点，计数进元数据
  int rejected = 0;      // 越界点不写入，只计入拒绝数（plan §3）
  double dx = 0, dy = 0; // 像元尺寸（米）
  double originX = 0, originY = 0; // 北向上左上角
  double zMin = 0, zMax = 0;
  int filledCells = 0;
  bool hasInlineRange = false, hasXlineRange = false;
  // 测网 inline/crossline 范围（来自层位头 P1 + Grid_size 跨度）——
  // 写进 GeoTIFF 的 PALEO_INLINE_*/PALEO_XLINE_*，验证→地震剖面导航
  // 依赖它（audit #38/T21）。装箱语义下恒有值。
  int inlineMin = 0, inlineMax = 0, xlineMin = 0, xlineMax = 0;
  // 地震采样间隔/起始时间（ms）：来自 SEG-Y，散点文本里没有——只在调用方
  // 实际拿到时才写 PALEO_DT_MS/PALEO_T0_MS，不编值（T21「as available」）。
  double dtMs = qQNaN();
  double t0Ms = qQNaN();
};

// 从 '# Grid_size:411x641' / '# P1: ...' / '# Z_units: ms' 行取参数。
bool parseHorizonHeader(const QByteArray &text, HorizonHeader *out, QString *error = nullptr);

// 装箱：行=inline-p1Inline（0..rows-1），列=crossline-p1Xline。
bool binHorizon(const QByteArray &text, BinnedHorizon *out, QString *error = nullptr);

// 写北向上 Float32 GeoTIFF（geotransform/nodata/局部测网 CRS）+ PALEO_INLINE_*/
// PALEO_XLINE_* 测网号域 + PALEO_FILLED_CELLS/COLLISIONS/REJECTED 装箱计数
// （T21：验证→地震剖面导航靠号域）；dtMs/t0Ms 有限时另写 PALEO_DT_MS/T0_MS。
bool writeHorizonGeoTiff(const BinnedHorizon &b, const QString &destPath, QString *error = nullptr);
