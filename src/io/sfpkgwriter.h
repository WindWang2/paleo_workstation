// 层：数据
#pragma once

#include "ziparchive.h"

#include <QByteArray>
#include <QMap>
#include <QString>
#include <QStringList>
#include <QVariantMap>
#include <QVector>

#include <vector>

// 方向67：.sfpkg 写出面——镜像读面契约（sfpkgreader，上游 Drawing sfpkg.py 格式）。
//
// 包结构（安全 ZIP，禁止 pickle）：
//   manifest.json  —— 调用方给全量 QVariantMap（snake_case 键与读面一致）；
//                     format 缺省补 "sfpkg"，version 缺省补 "1.0"，
//                     grid_shape 缺省按 grid_z 尺寸补写（给出但与 grid_z 不符 → 拒写）
//   surface.npz    —— arrays 逐个 NPY（v1.0 头，小端，行主序）打包成内层 ZIP
//   checksum.json  —— {algorithm:"sha256", manifest_sha256, surface_npz_sha256}
//                     （对写出的 manifest/npz 精确字节取哈希；读面默认校验）
//   sidecars       —— 原样字节条目（上游 shp 边车）
//
// 诚实面：致命问题（缺 grid_z / manifest 关键字段缺失或类型不对 / grid_shape 与
// grid_z 不符 / 整型数组含非整数值或越界 / dtype 不支持 / has_pickle）→ ok=false
// 不落盘；非致命（grid_x/grid_y/valid_mask 缺失——读面会逐条 issue）→ 进 issues
// 照写。压缩 stored 起步（deflate 可选），>4 GiB / >65535 条 / 偏移溢出自动 ZIP64。
// 层：数据
namespace paleo::io
{

struct SfPackageArrayInput
{
  QString name;               // npz 条目名（不带 .npy）
  QString dtype;              // NPY descr；空 = "<f8"
  int rows = 0;
  int cols = 1;               // 1 = 一维（shape 写 (rows,)）
  std::vector<double> values; // 行主序；NaN 照写（上游 nodata 口径）
};

struct SfPackageWriteResult
{
  bool ok = false;
  QString error;
  QStringList issues;      // 非致命项，包照写
  qint64 bytesWritten = 0; // 落盘字节数
  bool zip64Used = false;
};

SfPackageWriteResult writeSfPackage( const QString &path,
                                     const QVariantMap &manifest,
                                     const QVector<SfPackageArrayInput> &arrays,
                                     const QMap<QString, QByteArray> &sidecars = {},
                                     ZipCompression compression = ZipCompression::Stored );

} // namespace paleo::io
