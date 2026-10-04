// 层：数据
#pragma once

#include <QString>
#include <QStringList>
#include <QVariantMap>
#include <QVector>

#include <cstdint>
#include <vector>

// 方向41：外委单因素包 .sfpkg 完整读取（上游 Drawing/drawing/facies_workflow/sfpkg.py
// @27fdb998a32d7a7f50d7e6ef0d2ebb3a5d06378f 的格式定义）。
//
// 包结构（安全 ZIP，禁止 pickle）：
//   manifest.json  —— 因素名/层位/方法/方法参数/值域/来源/变差/分区/级别/CRS 等
//   surface.npz    —— numpy NPZ（ZIP of .npy，allow_pickle=False）：
//                     grid_z / grid_x / grid_y / valid_mask / boundary_mask /
//                     coverage_status / region_ids / well_region_ids / source_trend_grid
//   checksum.json  —— {surface_npz_sha256, manifest_sha256, algorithm:"sha256"}
//   可选边车      —— 上游 shp_sidecars 原样拷贝的条目
//
// 诚实面：包级失败（缺 manifest/npz、校验和不匹配、pickle）→ ok=false + error；
// 单字段坏数据（dtype 非数值、shape 与 grid_shape 不符、缺失可选数组、manifest
// 字段类型不对）→ 进 issues 逐条列因，不静默丢弃、不假装读全。
// 层：数据
namespace paleo::io
{

// NPZ 里的一个数组。values 为行主序展开；fortran_order 已在解析期转置。
struct SfPackageArray
{
  QString name;
  QString dtype; // NPY descr，原样（如 "<f8"）
  bool fortranOrder = false;
  bool integral = false; // descr 是整数/布尔类型
  int rows = 0;
  int cols = 1; // 一维数组 cols=1
  std::vector<double> values;
};

struct SfPackageManifest
{
  QString format;
  QString version;
  QString factorName;
  QString horizon;
  QString method;
  QString valueSource;
  QString barrierValuePolicy;
  QString analysisValuePolicy;
  QString interpolationModel;
  QString crsWkt;
  QString dataSource;
  QString createdAt;
  QString partitionVersion;
  bool hasPickle = false;
  bool partitionComplete = false;
  double valueMin = 0;
  double valueMax = 0;
  bool hasValueRange = false;
  double barrierBufferDistance = 0;
  double contourStopBufferDistance = 0;
  double globalAnisotropyRatio = 1;
  double globalAnisotropyAngle = 0;
  QVariantMap methodParams;
  QVariantMap fieldModel;
  QVariantMap contourPartition;
  QVariantMap coverageInfo;
  QVariantList levels;
  QVariantList barriers;
  QVariantList directions;
  QVariantList boundaries;
  QVariantList partitionBarriers;
  QVariantList partitionExtensions;
  QVariantList gridShape;
  // 完整原始 JSON：未在上面单列的字段（含上游 extra_manifest 追加的键）原样保留，
  // 读取「完整字段」不靠枚举穷举。
  QVariantMap raw;
};

struct SfPackageReadResult
{
  bool ok = false;
  QString error; // 包级失败原因（空 = 没失败）
  QString path;
  QStringList zipEntries;   // ZIP 条目原样列出
  QStringList sidecars;     // 非 manifest/npz/checksum 的条目（上游 shp 边车）
  bool checksumPresent = false;
  SfPackageManifest manifest;
  QVector<SfPackageArray> arrays;
  QStringList issues; // 单字段坏数据逐条列因

  const SfPackageArray *array( const QString &name ) const;
};

// 读 .sfpkg。任何包级失败都返回 ok=false，不抛异常。
SfPackageReadResult readSfPackage( const QString &path );

// NPY/NPZ 单文件解析（测试与诊断用；.npz 是 ZIP of .npy）。
struct NpyArray
{
  bool ok = false;
  QString error;
  QString descr;
  bool fortranOrder = false;
  bool integral = false;
  int rows = 0;
  int cols = 1;
  std::vector<double> values;
};

NpyArray parseNpy( const QByteArray &bytes );

} // namespace paleo::io
