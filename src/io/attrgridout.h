// 层：数据
#pragma once
#include <QString>
#include <QtGlobal>

#include <vector>

// io/ — 时间切片属性网格 → 地理参考 GeoTIFF（goal/attr-volume）。
// 网格 = IL×XL（行=inline 升序、列=crossline 升序）；地理参考由
// SgyCoordinateMapper 拟合仿射（x = a·il + b·xl + c；y = d·il + e·xl + f，
// 服务层传入）推导全参 geotransform（GT2/GT4 旋转项如实保留——测网斜置
// 时不硬掰正北）。NaN 像元写 nodata(-9999)（诚实留空，不插值）。
// 测网号域/时间/参数摘要写 PALEO_* 元数据（horizonbinner 同约定，
// 验证→剖面导航/去重缓存复验依赖它）。
namespace paleo::sattr
{

struct AttrTimeSliceGrid
{
  QString attrId;
  int nIl = 0, nXl = 0;
  std::vector<int> ilValues;   // inline 轴值域（升序）
  std::vector<int> xlValues;   // crossline 轴值域（升序）
  std::vector<float> values;   // nIl*nXl，[il*nXl + xl]，NaN = 无效
  double valueMin = 0.0;
  double valueMax = 0.0;
  qint64 validCells = 0;
  int sampleIndex = 0;         // 目标采样号（0 = 首样）
  double timeMs = 0.0;         // t0 + sampleIndex·dt
  double sampleIntervalMs = 0.0;
  double startTimeMs = 0.0;    // 首样 TWT（记录延迟）
  QString sourceSgyPath;
  QString paramHash;           // 参数包摘要（缓存复验）
};

// affine[6] = {a, b, c, d, e, f}。几何/仿射无效（尺寸 0、轴值表不符、
// 步长 ≤0）→ 如实失败——绝不产无地理参考的「栅格」。
bool writeTimeSliceGeoTiff(const QString &path, const AttrTimeSliceGrid &grid,
                           const double affine[6], QString *error = nullptr);

// 摘要读回（缓存命中路径补统计用）：栅格尺寸 + PALEO_* 元数据回填
// summary（nIl/nXl/valueMin/valueMax/validCells/sampleIndex/timeMs/
// sampleIntervalMs/startTimeMs/attrId/paramHash）。文件缺元数据键 →
// 如实失败（缓存产物身份不全，视同未命中重算）。
bool readTimeSliceGeoTiffSummary(const QString &path,
                                 AttrTimeSliceGrid *summary,
                                 QString *error = nullptr);

} // namespace paleo::sattr
