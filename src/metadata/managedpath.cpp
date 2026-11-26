// 层：数据
#include "managedpath.h"
#include "storeerrors_internal.h"

#include <QCryptographicHash>
#include <QDir>
#include <QStandardPaths>
#include <QFile>
#include <QFileInfo>
#include <QStringList>

namespace paleo::managedpath
{
using paleo::store_detail::setError;
namespace {
QString applicationScratchRelative; // Configured by the Windows composition root before workers start.
}

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
    if (info.isSymbolicLink() || info.isJunction()) return QString();
    if (info.exists())
    {
      // canonicalFilePath() 恒以 '/' 作分隔符（含 Windows）；QDir::separator()
      // 在 Windows 是 '\'，拼进前缀会让 startsWith 永假——曾致全部受管导入
      // 在 Windows 报 "unsafe managed path/destination"。
      const QString canonical = info.canonicalFilePath();
      const QString relative = QDir(root).relativeFilePath(canonical);
      if (QDir::isAbsolutePath(relative) || relative == QStringLiteral("..") || relative.startsWith(QStringLiteral("../"))) return QString();
    }
  }
  return current;
}

bool isWithinRoot(const QString &root, const QString &path)
{
  if (root.isEmpty() || path.isEmpty()) return false;
  const QString base = QFileInfo(root).canonicalFilePath();
  if (base.isEmpty()) return false;
  QFileInfo existing(path);
  while (!existing.exists()) {
    if (existing.isSymbolicLink() || existing.isJunction() || existing.absoluteFilePath() == existing.absolutePath()) return false;
    existing.setFile(existing.absolutePath());
  }
  const QString actual = existing.canonicalFilePath();
  if (actual.isEmpty()) return false;
  const QString relative = QDir(base).relativeFilePath(actual);
  return !QDir::isAbsolutePath(relative) && relative != QStringLiteral("..") && !relative.startsWith(QStringLiteral("../"));
}

QString defaultProjectVolume()
{
#ifdef Q_OS_WIN
  return QStringLiteral("D:/");
#else
  return {};
#endif
}

QString defaultProjectDirectory()
{
  const QString volume = defaultProjectVolume();
  return volume.isEmpty() ? QDir(QStandardPaths::writableLocation(QStandardPaths::DocumentsLocation)).filePath(QStringLiteral("Paleo"))
                          : QDir(volume).filePath(QStringLiteral("PaleoProjects"));
}

void configureApplicationDataLocations()
{
#ifdef Q_OS_WIN
  // The shared seismic engine reads this setting in worker threads. Bind it
  // once before the first worker, independently of project switches.
  const QByteArray profile = QDir::homePath().toCaseFolded().toUtf8();
  const QByteArray identity = QCryptographicHash::hash(profile, QCryptographicHash::Sha256).toHex();
  applicationScratchRelative = QDir(defaultProjectVolume()).relativeFilePath(defaultProjectDirectory()) +
                               QStringLiteral("/.cache/") + QString::fromLatin1(identity);
  if (qEnvironmentVariableIsEmpty("SEISMIC_INDEX_CACHE_DIR")) {
    const QString root = QDir(defaultProjectVolume()).filePath(applicationScratchRelative + QStringLiteral("/seismic-index"));
    qputenv("SEISMIC_INDEX_CACHE_DIR", root.toUtf8());
  }
#endif
}

QString workLocationRoot(const QString &projectDir, QString &relative)
{
  if (applicationScratchRelative.isEmpty()) return projectDir;
  const QString volume = defaultProjectVolume();
  const QString canonical = QFileInfo(projectDir).canonicalFilePath();
  const QString location = QDir(volume).relativeFilePath(canonical);
  if (!projectDir.isEmpty() && !QDir::isAbsolutePath(location) && location != QStringLiteral("..") && !location.startsWith(QStringLiteral("../")))
    return projectDir;
  const QString key = projectDir.isEmpty() ? QStringLiteral("preview") : QString::fromLatin1(
      QCryptographicHash::hash(canonical.toCaseFolded().toUtf8(), QCryptographicHash::Sha256).toHex());
  relative = applicationScratchRelative + '/' + key + '/' + relative;
  return volume;
}

std::unique_ptr<QTemporaryDir> temporaryDirectory(const QString &projectDir, const QString &prefix)
{
  if (!isSafePathSegment(prefix)) return {};
  if (!projectDir.isEmpty() && !QFileInfo(projectDir).isDir()) return {};
  QString relative = QStringLiteral("artifacts/staging");
  const QString root = workLocationRoot(projectDir, relative);
  QString parent = QDir::tempPath();
  if (!root.isEmpty()) {
    parent = resolveManagedPath(root, relative);
    if (parent.isEmpty() || !QDir().mkpath(parent) || resolveManagedPath(root, relative) != parent)
      return {};
  }
  auto dir = std::make_unique<QTemporaryDir>(QDir(parent).filePath(prefix + QStringLiteral("-XXXXXX")));
  if (!dir->isValid()) return {};
  return dir;
}

std::unique_ptr<QTemporaryDir> temporaryPreviewDirectory(const QString &projectDir, const QString &prefix)
{
  auto result = temporaryDirectory(projectDir, prefix);
  if (result || projectDir.isEmpty() || !QFileInfo(projectDir).isDir()) return result;
  // A read-only old project can render from a private disposable workspace.
  // Production Windows config pins the projectless location to the data drive.
  return temporaryDirectory({}, prefix);
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
