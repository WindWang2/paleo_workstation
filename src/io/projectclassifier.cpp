#include "projectclassifier.h"
#include "wellfileparsers.h"

#include <QDir>
#include <QFileInfo>
#include <QStringList>

#include <algorithm>

namespace
{
  QString lowerAscii(const QString &s)
  {
    return s.toLower();
  }

  // path parts（POSIX '/' 分段，等价参考实现的 pathlib.parts）
  QStringList pathParts(const QString &path)
  {
    QStringList parts;
    const QString norm = QDir::fromNativeSeparators(path);
    for (const QString &p : norm.split(QLatin1Char('/'), Qt::SkipEmptyParts))
      parts.append(lowerAscii(p));
    return parts;
  }

  bool containsAny(const QString &text, std::initializer_list<const char *> needles)
  {
    for (const char *n : needles)
      if (text.contains(QString::fromUtf8(n)))
        return true;
    return false;
  }
} // namespace

ProjectClassification classifyProjectPath(const QString &path)
{
  const QFileInfo fi(path);
  const QString ext = lowerAscii(fi.suffix());
  const QString name = lowerAscii(fi.fileName());
  const QStringList parts = pathParts(path);

  auto make = [](QString t, QString f, QString s) { return ProjectClassification{t, f, s}; };

  if (ext == QLatin1String("las"))
    return make(QStringLiteral("well_log"), ext, QStringLiteral("input"));
  if (ext == QLatin1String("sgy") || ext == QLatin1String("segy"))
    return make(QStringLiteral("seismic"), ext, QStringLiteral("input"));
  if (ext == QLatin1String("geojson"))
    return make(QStringLiteral("geojson"), ext, QStringLiteral("input"));
  if (ext == QLatin1String("dat"))
  {
    // 精确段 "td" 或段含 时深；段含 层位 / 井分层 / 井位；文件名含 wellhead。
    for (const QString &p : parts)
    {
      if (p == QLatin1String("td") || p.contains(QString::fromUtf8("时深")))
        return make(QStringLiteral("time_depth"), ext, QStringLiteral("input"));
    }
    for (const QString &p : parts)
      if (p.contains(QString::fromUtf8("层位")))
        return make(QStringLiteral("horizon"), ext, QStringLiteral("input"));
    for (const QString &p : parts)
      if (p.contains(QString::fromUtf8("井分层")))
        return make(QStringLiteral("well_stratification"), ext, QStringLiteral("input"));
    if (containsAny(name, {"wellhead", "well_head"}) ||
        std::any_of(parts.begin(), parts.end(),
                    [](const QString &p) { return p.contains(QString::fromUtf8("井位")); }))
      return make(QStringLiteral("well_head"), ext, QStringLiteral("input"));
    return make(QStringLiteral("tabular"), ext, QStringLiteral("input"));
  }
  if (ext == QLatin1String("pdf") || ext == QLatin1String("ppt") ||
      ext == QLatin1String("pptx") || ext == QLatin1String("doc") ||
      ext == QLatin1String("docx"))
    return make(QStringLiteral("document"), ext, QStringLiteral("reference"));
  if (ext == QLatin1String("png") || ext == QLatin1String("jpg") ||
      ext == QLatin1String("jpeg") || ext == QLatin1String("tif") ||
      ext == QLatin1String("tiff"))
    return make(QStringLiteral("image_reference"), ext, QStringLiteral("reference"));
  if (ext == QLatin1String("xml"))
    return make(QStringLiteral("unknown"), ext, QStringLiteral("reference"));
  return make(QStringLiteral("unknown"), ext.isEmpty() ? QStringLiteral("none") : ext,
              QStringLiteral("reference"));
}

ProjectClassification classifyProjectImport(const QString &path, const QByteArray &content)
{
  const QString ext = lowerAscii(QFileInfo(path).suffix());
  if (ext == QLatin1String("xml"))
  {
    const WellXmlKind kind = sniffWellXml(content);
    if (kind == WellXmlKind::WellHead)
      return {QStringLiteral("well_head"), ext, QStringLiteral("input")};
    if (kind == WellXmlKind::WellLog)
      return {QStringLiteral("well_log"), ext, QStringLiteral("input")};
    // 判不出 → 参考（§3）
  }
  return classifyProjectPath(path);
}

QStringList projectClassifierTypes()
{
  // 与 classifyProjectPath/classifyProjectImport 的输出对齐（确认表词表）；
  // "reference" 是确认表伪类型，其余全部可由分类器产出。
  return {QStringLiteral("well_head"),        QStringLiteral("well_log"),
          QStringLiteral("well_stratification"), QStringLiteral("time_depth"),
          QStringLiteral("horizon"),          QStringLiteral("seismic"),
          QStringLiteral("tabular"),          QStringLiteral("geojson"),
          QStringLiteral("document"),         QStringLiteral("image_reference"),
          QStringLiteral("reference"),        QStringLiteral("unknown")};
}

bool isClassifierType(const QString &type)
{
  return projectClassifierTypes().contains(type);
}

bool isFixedAuxiliaryPath(const QString &path)
{
  // T22：只锁 HZ28-6-1 命名文件；「参考资料」整目录锁定已拆成
  // isDefaultReferencePath（默认显示「参考」，可改）。
  return QFileInfo(path).completeBaseName().contains(QStringLiteral("HZ28-6-1"));
}

bool isDefaultReferencePath(const QString &path)
{
  const QStringList parts = QFileInfo(path).absolutePath().split(QLatin1Char('/'));
  for (const QString &p : parts)
    if (p == QString::fromUtf8("参考资料"))
      return true;
  return false;
}
