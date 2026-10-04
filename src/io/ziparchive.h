// 层：数据
#pragma once

#include <QByteArray>
#include <QString>
#include <QVector>

#include <cstdint>

// 方向41：最小内存 ZIP 读面（.sfpkg 与 .xlsx 都是 ZIP 容器）。
//
// 为什么不用 QgsZipUtils：它只按磁盘文件名工作（内层 surface.npz 必须落临时文件），
// 且 isZipFile 走 QMimeDatabase —— 非 .zip 扩展名在缺 mime 数据的机器上会被判成
// 非 ZIP。这里按 ZIP 记录结构直接读，全程在内存里做（不落临时文件、不依赖 mime 库），
// 内层 surface.npz 是 ZIP-of-NPY，同样直接用同一读面解。
//
// 支持：stored（method 0）与 deflate（method 8，zlib）。ZIP64（字段是 0xFFFFFFFF）
// 与加密条目明确报因，不读截断数据。单包上限 512 MiB（超限报因，不静默截断）。
// 层：数据
namespace paleo::io
{

struct ZipEntry
{
  QString name;
  std::uint16_t method = 0;
  std::uint64_t compressedSize = 0;
  std::uint64_t uncompressedSize = 0;
  std::uint32_t localHeaderOffset = 0;
  bool encrypted = false;
};

struct ZipListResult
{
  bool ok = false;
  QString error;
  QVector<ZipEntry> entries;

  const ZipEntry *entry( const QString &name ) const;
  bool contains( const QString &name ) const { return entry( name ) != nullptr; }
};

// 整包读入内存（普通文件；> 512 MiB 报因）。失败返回空 QByteArray 并写 error。
QByteArray zipReadArchive( const QString &path, QString *error );

// 列条目（中央目录）。
ZipListResult zipListBytes( const QByteArray &archive );

// 内存解出单个条目。失败时 error 给原因（缺条目/不支持的方法/ZIP64/解压失败）。
bool zipExtractBytes( const QByteArray &archive, const QString &entryName, QByteArray *out,
                      QString *error );

} // namespace paleo::io
