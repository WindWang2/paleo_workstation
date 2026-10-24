// 层：数据
#include "managedpath.h"
#include "storeerrors_internal.h"

#include <QCryptographicHash>
#include <QDir>
#include <QFile>
#include <QFileInfo>
#include <QStringList>

namespace paleo::managedpath
{
using paleo::store_detail::setError;

QString resolveManagedPath(const QString &projectDir, const QString &relativePath)
{
  if (relativePath.isEmpty()) return QString();
  if (QDir::isAbsolutePath(relativePath) || relativePath.contains(QLatin1Char('\\')))
    return QString();
  const QStringList segments = relativePath.split(QLatin1Char('/'));
  for (const QString &segment : segments)
    if (!isSafePathSegment(segment)) return QString();

  const QString root = QFileInfo(projectDir).canonicalFilePath();
  if (root.isEmpty()) return QString();
  QString current = root;
  for (const QString &segment : segments)
  {
    current = QDir(current).filePath(segment);
    const QFileInfo info(current);
    if (info.isSymbolicLink()) return QString();
    if (info.exists())
    {
      // canonicalFilePath() 恒以 '/' 作分隔符（含 Windows）；QDir::separator()
      // 在 Windows 是 '\'，拼进前缀会让 startsWith 永假——曾致全部受管导入
      // 在 Windows 报 "unsafe managed path/destination"。
      const QString canonical = info.canonicalFilePath();
      if (!canonical.startsWith(root + QLatin1Char('/'))) return QString();
    }
  }
  return current;
}

QString sha256FileHex(const QString &path, QString *error, const std::function<bool()> &cancelled)
{
  QFile f(path);
  if (!f.open(QIODevice::ReadOnly))
  {
    setError(error, QStringLiteral("cannot read %1").arg(path));
    return QString();
  }
  QCryptographicHash hash(QCryptographicHash::Sha256);
  // 64KB chunks: a 1MB stack buffer overflows the default Windows thread
  // stack when this runs on the QTest main thread.
  char buf[64 << 10];
  qint64 n = 0;
  while ((n = f.read(buf, sizeof(buf))) > 0) {
    if (cancelled && cancelled()) {
      setError(error, QStringLiteral("SHA verification cancelled"));
      return QString();
    }
    hash.addData(QByteArrayView(buf, static_cast<qsizetype>(n)));
  }
  if (n < 0)
  {
    setError(error, QStringLiteral("read error on %1").arg(path));
    return QString();
  }
  return QString::fromLatin1(hash.result().toHex());
}

bool isSafePathSegment(const QString &segment)
{
  // §3：一段路径拒绝空段、"."、任何含 ".." 的段、斜杠/反斜杠、NUL 与控制字符
  //（含换行、回车、Tab、DEL）。
  if (segment.isEmpty() || segment == QLatin1Char('.') ||
      segment.contains(QLatin1String("..")))
    return false;
  for (const QChar c : segment)
  {
    if (c == QLatin1Char('/') || c == QLatin1Char('\\'))
      return false;
    const ushort u = c.unicode();
    if (u < 0x20 || u == 0x7F)
      return false;
  }
  return true;
}

} // namespace paleo::managedpath
