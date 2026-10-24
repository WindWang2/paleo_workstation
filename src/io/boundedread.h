#pragma once
// 层：数据
// PW-076：导入管线统一的有界整读。超过 limit（默认 LasParser 口径 500 MB）直接失败，
// 不截断、不静默——调用方据 error 报告。header-only，避免改动库源清单。
#include <QByteArray>
#include <QFile>
#include <QString>

namespace paleo::io
{
inline constexpr qint64 kDefaultBoundedReadLimit = 500LL * 1024 * 1024;

// 成功返回 true 并写 *out；失败（打不开/超限/读短）返回 false 并写 *error。
inline bool readBoundedFile(const QString &path, qint64 limit, QByteArray *out,
                            QString *error = nullptr)
{
  QFile f(path);
  if (!f.open(QIODevice::ReadOnly))
  {
    if (error)
      *error = QStringLiteral("cannot read %1: %2").arg(path, f.errorString());
    return false;
  }
  const qint64 size = f.size();
  if (limit >= 0 && size > limit)
  {
    if (error)
      *error = QStringLiteral("file too large (%1 MB limit): %2")
                   .arg(limit / (1024 * 1024))
                   .arg(path);
    return false;
  }
  // 非常规文件（管道/设备）size 可能为 0：最多读 limit+1 字节以检出超限。
  QByteArray bytes = f.read(limit >= 0 ? limit + 1 : size);
  if (limit >= 0 && bytes.size() > limit)
  {
    if (error)
      *error = QStringLiteral("file too large (%1 MB limit): %2")
                   .arg(limit / (1024 * 1024))
                   .arg(path);
    return false;
  }
  if (out)
    *out = std::move(bytes);
  return true;
}
} // namespace paleo::io
