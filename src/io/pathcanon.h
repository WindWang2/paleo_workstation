// 层：数据
#pragma once
#include <QDir>
#include <QFileInfo>
#include <QString>

// io/ — 路径规范化统一口径（wave/io-perf-cache D7.3）。
// 相对/绝对/符号链接 → 唯一形态：存在的路径用 canonicalFilePath（解符号
// 链接），不存在的退回「绝对化 + 清理 ../ 与重复分隔符」。缓存键、索引
// 身份、SHA 摘要表全部先过这里——两条写法不同的路径必须命中同一条缓存。
namespace PathCanon
{
  inline QString canonicalize(const QString &path, const QString &baseDir = QString())
  {
    if (path.isEmpty())
      return QString();
    QString p = QDir::isAbsolutePath(path) ? path
                 : !baseDir.isEmpty()      ? QDir(baseDir).absoluteFilePath(path)
                                           : QDir::current().absoluteFilePath(path);
    QFileInfo info(p);
    const QString canon = info.canonicalFilePath();
    if (!canon.isEmpty())
      return canon;
    // 不存在（或权限不可达）：绝对化 + 清洗，保持形态稳定。
    return QDir::cleanPath(info.absoluteFilePath());
  }

  // 规范化路径 + mtime + size 的缓存指纹（LAS/SHA 缓存键的公共成分）。
  inline QString fingerprint(const QString &path, qint64 mtimeMs, qint64 sizeBytes)
  {
    return QStringLiteral("%1|%2|%3").arg(canonicalize(path)).arg(mtimeMs).arg(sizeBytes);
  }
} // namespace PathCanon
