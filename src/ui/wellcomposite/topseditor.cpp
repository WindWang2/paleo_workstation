// 层：视图
#include "topseditor.h"

#include <QIODevice>
#include <QRegularExpression>
#include <QTextStream>

namespace WellComposite
{
namespace TopsEditor
{

TopsImportReport parseImportText(const QString &text)
{
  TopsImportReport rep;
  QTextStream ts(const_cast<QString *>(&text), QIODevice::ReadOnly);

  int lineNo = 0;
  bool firstDataLine = true;
  while (!ts.atEnd())
  {
    const QString line = ts.readLine().trimmed();
    ++lineNo;
    if (line.isEmpty())
      continue;

    // 逗号或制表符切列
    const QStringList cols = line.split(QRegularExpression(QStringLiteral("[,\t]")),
                                        Qt::SkipEmptyParts);
    if (cols.isEmpty())
      continue;

    // 表头识别：首行含非数值的 top 列即表头，跳过
    if (firstDataLine)
    {
      firstDataLine = false;
      const bool looksLikeHeader = !cols.isEmpty() && cols.size() >= 2 &&
                                   (cols.at(1).compare(QStringLiteral("top"), Qt::CaseInsensitive) == 0 ||
                                    cols.at(1) == QStringLiteral("顶深") ||
                                    cols.at(1) == QStringLiteral("顶") ||
                                    cols.at(0).compare(QStringLiteral("name"), Qt::CaseInsensitive) == 0 ||
                                    cols.at(0) == QStringLiteral("名称") || cols.at(0) == QStringLiteral("层名"));
      if (looksLikeHeader)
        continue;
    }

    if (cols.size() < 2)
    {
      rep.errors << QStringLiteral("第 %1 行：列数不足（需 name,top[,base]）: %2").arg(lineNo).arg(line);
      continue;
    }

    bool okTop = false;
    const double top = cols.at(1).trimmed().toDouble(&okTop);
    if (!okTop || top <= 0.0)
    {
      rep.errors << QStringLiteral("第 %1 行：顶深无效「%2」").arg(lineNo).arg(cols.at(1));
      continue;
    }

    TopsImportRow row;
    row.name = cols.at(0).trimmed();
    row.top = top;
    row.sourceLine = lineNo;
    if (cols.size() >= 3)
    {
      bool okBase = false;
      const double base = cols.at(2).trimmed().toDouble(&okBase);
      if (okBase && base >= top)
        row.base = base;
      else if (okBase && base < top)
        rep.errors << QStringLiteral("第 %1 行：底深 %2 < 顶深 %3（忽略底深）")
                      .arg(lineNo)
                      .arg(QString::number(base), QString::number(top));
    }
    if (row.name.isEmpty())
    {
      rep.errors << QStringLiteral("第 %1 行：层名为空").arg(lineNo);
      continue;
    }
    rep.parsedRows << row;
  }
  return rep;
}

TopsImportReport validateAgainst(const TopsImportReport &parsed,
                                 const QVector<QPair<QString, double>> &existing)
{
  TopsImportReport rep = parsed;
  rep.conflicts.clear();

  for (const auto &row : rep.parsedRows)
  {
    for (const auto &ex : existing)
    {
      if (ex.first == row.name && std::abs(ex.second - row.top) > 0.05)
      {
        rep.conflicts << QStringLiteral("第 %1 行：%2 与既有同名标志层深度冲突（既有 %3 m，导入 %4 m）")
                          .arg(row.sourceLine)
                          .arg(row.name, QString::number(ex.second, 'f', 1),
                               QString::number(row.top, 'f', 1));
      }
    }
  }

  // 行内重复同名不同深
  for (int i = 0; i < rep.parsedRows.size(); ++i)
  {
    for (int j = i + 1; j < rep.parsedRows.size(); ++j)
    {
      if (rep.parsedRows.at(i).name == rep.parsedRows.at(j).name &&
          std::abs(rep.parsedRows.at(i).top - rep.parsedRows.at(j).top) > 0.05)
      {
        rep.errors << QStringLiteral("导入内同名不同深：%1（第 %2 行 %3 m vs 第 %4 行 %5 m）")
                      .arg(rep.parsedRows.at(i).name)
                      .arg(rep.parsedRows.at(i).sourceLine)
                      .arg(QString::number(rep.parsedRows.at(i).top, 'f', 1))
                      .arg(rep.parsedRows.at(j).sourceLine)
                      .arg(QString::number(rep.parsedRows.at(j).top, 'f', 1));
      }
    }
  }
  return rep;
}

QString conflictSummary(const TopsImportReport &report)
{
  QStringList lines;
  lines << QStringLiteral("解析 %1 行；错误 %2 项；冲突 %3 项")
               .arg(report.parsedRows.size())
               .arg(report.errors.size())
               .arg(report.conflicts.size());
  if (!report.errors.isEmpty())
    lines << QStringLiteral("— 错误 —") << report.errors;
  if (!report.conflicts.isEmpty())
    lines << QStringLiteral("— 冲突（应用将覆盖既有深度）—") << report.conflicts;
  return lines.join(QLatin1Char('\n'));
}

} // namespace TopsEditor
} // namespace WellComposite
