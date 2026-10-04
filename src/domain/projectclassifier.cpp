// 层：数据
#include "projectclassifier.h"
#include "arearules.h"
#include "wellrecords.h"

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
  // 方向41：外委格式进词表登记（不旁路分类器）。
  // .sfpkg = 上游单因素无损包（facies_workflow/sfpkg.py），.xlsx = 外委 OOXML
  // 工作簿。旧的二进制 .xls 不在这里——读取面没有实现它，不给假能力。
  if (ext == QLatin1String("sfpkg"))
    return make(QStringLiteral("single_factor_package"), ext, QStringLiteral("input"));
  if (ext == QLatin1String("xlsx"))
    return make(QStringLiteral("outsource_workbook"), ext, QStringLiteral("input"));
  if (ext == QLatin1String("dat"))
  {
    // 目录段规则经 AreaRules（默认 = 原中文目录名表：时深(td)/层位/井分层/
    // 井位 + 文件名 wellhead；第二工区经 project_area.json 换表，见
    // docs/AREA_PARAMETERS.md）。规则按表序先中先得；段/文件名大小写不敏感。
    const AreaRules::ClassifierRules rules = AreaRules::active().classifier;
    for (const AreaRules::DatPathRule &rule : rules.datPathRules)
    {
      for (const QString &p : parts)
      {
        const bool exact = std::any_of(rule.exactSegments.begin(), rule.exactSegments.end(),
                                       [&p](const QString &s) {
                                         return p.compare(s, Qt::CaseInsensitive) == 0;
                                       });
        const bool keyword = std::any_of(rule.segmentKeywords.begin(), rule.segmentKeywords.end(),
                                         [&p](const QString &k) {
                                           return p.contains(k, Qt::CaseInsensitive);
                                         });
        if (exact || keyword)
          return make(rule.type, ext, QStringLiteral("input"));
      }
      for (const QString &k : rule.filenameKeywords)
        if (name.contains(k, Qt::CaseInsensitive))
          return make(rule.type, ext, QStringLiteral("input"));
    }
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
    if (kind == WellXmlKind::WellDeviation)
      return {QStringLiteral("well_deviation"), ext, QStringLiteral("input")};
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
          QStringLiteral("well_deviation"),    QStringLiteral("horizon"),
          QStringLiteral("seismic"),           QStringLiteral("tabular"),
          QStringLiteral("geojson"),           QStringLiteral("document"),
          QStringLiteral("image_reference"),   QStringLiteral("single_factor_package"),
          QStringLiteral("outsource_workbook"), QStringLiteral("reference"),
          QStringLiteral("unknown")};
}

bool isClassifierType(const QString &type)
{
  return projectClassifierTypes().contains(type);
}

bool isFixedAuxiliaryPath(const QString &path)
{
  // T22：只锁 HZ28-6-1 命名文件；「参考资料」整目录锁定已拆成
  // isDefaultReferencePath（默认显示「参考」，可改）。钉死值经 AreaRules
  // （本工区默认 HZ28-6-1；空串 = 显式无固定辅助）。
  const QString stem = AreaRules::active().classifier.fixedAuxiliaryNameStem;
  if (stem.isEmpty())
    return false;
  return QFileInfo(path).completeBaseName().contains(stem);
}

bool isDefaultReferencePath(const QString &path)
{
  // 目录段名经 AreaRules（本工区默认「参考资料」；原文精确匹配，不小写化——
  // 中文名无大小写，英文目录名保持字面）。
  const QStringList names = AreaRules::active().classifier.referenceDirNames;
  if (names.isEmpty())
    return false;
  const QStringList parts = QFileInfo(path).absolutePath().split(QLatin1Char('/'));
  for (const QString &p : parts)
    if (std::any_of(names.begin(), names.end(),
                    [&p](const QString &n) { return p == n; }))
      return true;
  return false;
}
