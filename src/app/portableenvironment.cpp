// 层：组装根
#include "portableenvironment.h"
#include <QCoreApplication>
#include <QCryptographicHash>
#include <QDir>
#include <QFileInfo>
#include <QProcess>
#include <algorithm>
#include <cpl_conv.h>
#include <ogr_srs_api.h>
#ifdef Q_OS_WIN
#define NOMINMAX
#include <windows.h>
#endif

namespace
{
  bool isPureAscii(const QString &text)
  {
    return std::all_of(text.cbegin(), text.cend(),
                       [](QChar ch) { return ch.unicode() < 128; });
  }

  // 便携包绑定的环境变量及其相对安装根的落点。初始化时只回写这些键，
  // 不碰继承环境的其余变量。
  const QList<QPair<QString, QString>> &portableEnvBindings()
  {
    static const QList<QPair<QString, QString>> bindings{
      {"QGIS_PREFIX_PATH", "qgis"}, {"PROJ_DATA", "share/proj"},
      {"PROJ_LIB", "share/proj"}, {"GDAL_DATA", "share/gdal"},
      {"QT_PLUGIN_PATH", "plugins"}, {"QML2_IMPORT_PATH", "qml"},
      {"QTWEBENGINEPROCESS_PATH", "QtWebEngineProcess.exe"}};
    return bindings;
  }

#ifdef Q_OS_WIN
  // 环境变量的消费方分属三个互不兼容的编码域：CRT getenv 按本地代码页
  //（中文系统为 GBK）解码、PROJ 把字节当 UTF-8、Qt 再按 local8Bit 读回。
  // 非 ASCII 安装路径无论以哪种字节序写入都会被另外的域写坏（实测中文
  // 路径写入后按 GBK 读出乱码，8.3 短名在中文系统上也可能保留 CJK 字
  // 符）。唯一在三域都无损的是纯 ASCII 路径，因此为非 ASCII 安装根建立
  // 纯 ASCII 的目录联接锚点，环境变量一律指锚点。
  QString windowsShortPath(const QString &path)
  {
    const std::wstring wide = path.toStdWString();
    wchar_t buffer[32768];
    const auto length = GetShortPathNameW(wide.c_str(), buffer, 32768);
    if (length > 0 && length < 32768)
      return QString::fromWCharArray(buffer, int(length));
    return QString();
  }

  bool createDirectoryJunction(const QString &link, const QString &target)
  {
    // mklink 是 cmd 内建命令；junction 不需要管理员权限。QProcess 参数走
    // Unicode 通道，中文目标路径可正确传递；mklink 的两个路径实参都必须用
    // 反斜杠——正斜杠会被 cmd 当作开关解析（实测报「无效开关」）。
    if (QFileInfo::exists(link) && !QDir().rmdir(link))
      return false;
    const int code = QProcess::execute(
        QStringLiteral("cmd"),
        {QStringLiteral("/d"), QStringLiteral("/c"), QStringLiteral("mklink"),
         QStringLiteral("/J"), QDir::toNativeSeparators(link),
         QDir::toNativeSeparators(target)});
    return code == 0;
  }

  // 返回可用于环境变量的纯 ASCII 安装根：优先 8.3 短路径（本身就纯
  // ASCII 时），否则在 ProgramData（路径按约定为 ASCII）下建目录联接。
  // 都失败时返回空串，调用方退回长路径并在报告中说明。
  QString resolveAsciiAnchor(const QString &root)
  {
    if (isPureAscii(root))
      return root;
    const auto shortPath = windowsShortPath(root);
    if (!shortPath.isEmpty() && isPureAscii(shortPath))
      return shortPath;
    const QString base = QStringLiteral("C:/ProgramData/PaleoPortable");
    if (!QDir().mkpath(base))
      return QString();
    const auto hash = QString::fromLatin1(
        QCryptographicHash::hash(root.toLower().toUtf8(),
                                 QCryptographicHash::Sha1).toHex().left(16));
    const QString link = base + QLatin1Char('/') + hash;
    if (QFileInfo::exists(link)) {
      // symLinkTarget() 对 junction 不可靠（Qt 6.5 前不解析）；canonicalPath
      // 穿透联接得到真实目标的规范路径，版本间行为一致。
      const auto current = QDir(link).canonicalPath();
      const auto wanted = QDir(root).canonicalPath();
      if (!wanted.isEmpty() && current.compare(wanted, Qt::CaseInsensitive) == 0)
        return link;
      if (!QDir().rmdir(link))
        return QString();
    }
    if (createDirectoryJunction(link, QDir::toNativeSeparators(root)))
      return link;
    return QString();
  }
#endif
}

PortableEnvironment inspectPortableEnvironment(const QString &directory,
                                              const QProcessEnvironment &inherited,
                                              const QString &anchor)
{
  PortableEnvironment result;
  result.values = inherited;
  const QDir root(directory);
  if (!QFileInfo::exists(root.filePath(QStringLiteral("plugins/platforms/qwindows.dll"))))
    return result;
  result.detected = true;
  // A portable package must use one matching dependency/data set, regardless
  // of another GIS installation's inherited variables. PROJ_DATA takes
  // precedence over the legacy PROJ_LIB in modern PROJ.
  const QDir bindingRoot(anchor.isEmpty() ? directory : anchor);
  for (const auto &entry : portableEnvBindings())
    result.values.insert(entry.first, bindingRoot.absoluteFilePath(entry.second));
  for (const auto &file : {"share/proj/proj.db", "qgis/resources/srs.db"}) {
    const auto path = root.absoluteFilePath(QLatin1String(file));
    if (!QFileInfo(path).isReadable()) result.missing << path;
  }
  return result;
}

PortableEnvironment initializePortableEnvironment()
{
  QString directory;
#ifdef Q_OS_WIN
  // argv[0] is ANSI on some Windows entry points and may be relative to an
  // unrelated working directory. Read the actual executable's Unicode path.
  wchar_t path[32768];
  const auto size = GetModuleFileNameW(nullptr, path, 32768);
  if (size && size < 32768) directory = QFileInfo(QString::fromWCharArray(path, int(size))).absolutePath();
#else
  if (QCoreApplication::instance()) directory = QCoreApplication::applicationDirPath();
#endif
  auto result = inspectPortableEnvironment(directory, QProcessEnvironment::systemEnvironment());
  if (!result.detected) return result;
#ifdef Q_OS_WIN
  if (!isPureAscii(directory)) {
    const auto anchor = resolveAsciiAnchor(directory);
    if (!anchor.isEmpty())
      result = inspectPortableEnvironment(directory, result.values, anchor);
    else
      result.anchorNote = QObject::tr(
          "安装路径含非 ASCII 字符且无法建立 ASCII 锚点（8.3 短路径与 "
          "ProgramData 联接均不可用），环境变量保留长路径");
  }
#endif
  for (const auto &entry : portableEnvBindings())
    if (result.values.value(entry.first) != qEnvironmentVariable(qPrintable(entry.first)))
      qputenv(entry.first.toUtf8().constData(),
              result.values.value(entry.first).toUtf8());
  const auto proj = result.values.value(QStringLiteral("PROJ_DATA")).toUtf8();
  const char *paths[] = {proj.constData(), nullptr};
  OSRSetPROJSearchPaths(paths); // GDAL has its own per-thread PROJ contexts.
  CPLSetConfigOption("GDAL_DATA", result.values.value(QStringLiteral("GDAL_DATA")).toUtf8().constData());
  return result;
}
