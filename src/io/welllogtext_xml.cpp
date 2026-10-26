// 层：数据
#include "welllogtext_internal.h"
#include <QCoreApplication>

namespace WellLogText::Detail
{
bool parseXml(const QString &path, LasHeaderInfo &header, QList<LasCurve> &curves,
              QString *error, QList<LasIssue> *issues)
{
  QVector<Samples> sets;
  QSet<QString> names;
  QString detail;
  const auto workbook = paleo::io::streamSpreadsheetMlWorkbook(path,
      [&](const QString &label, const QStringList &headers, const QStringList &row, int line, bool isHeader) {
        if (!label.startsWith(QStringLiteral("测井曲线")) && !label.startsWith(QStringLiteral("离散曲线"))) return true;
        const auto location = path + QStringLiteral(" [%1]").arg(label);
        if (isHeader) {
          Samples samples;
          if (!beginTable(location, {label, headers, {}, {}}, header, samples, names, &detail, issues)) return false;
          sets.append(std::move(samples));
          return true;
        }
        if (sets.isEmpty()) return false;
        return appendSample(location, headers, row, line, header, sets.last(), &detail, issues);
      });
  if (!workbook.ok || !workbook.issues.isEmpty()) {
    if (detail.isEmpty()) detail = workbook.error + workbook.issues.join(QLatin1Char('\n'));
    return fail(path, 0, detail, error, issues);
  }
  // Header-only discrete sheets are normal in exported comprehensive documents.
  // They contribute no samples; a workbook without any populated curve still fails.
  sets.removeIf([](const Samples &samples) { return samples.index.isEmpty(); });
  // Numeric storage is bounded separately from the XML byte and row budgets.
  // HXN's largest well has 373301 samples and 43 output columns (~16M doubles).
  return mergeSamples(path, sets, 24'000'000, header, curves, error, issues);
}
}
