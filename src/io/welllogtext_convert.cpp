// 层：数据
#include "welllogtext_internal.h"

#include <QCoreApplication>
#include <QSet>
#include <QRegularExpression>
#include <algorithm>
#include <cmath>
#include <limits>
#include <utility>

namespace WellLogText::Detail
{
namespace
{
struct Samples
{
  QVector<double> index;
  QList<LasCurve> curves;
  Column axis;
};

QString sheetWellName(const QString &label)
{
  // Only the existing composite document's explicit “*-A1井” convention
  // declares an identity. Bare suffixes such as “-补充” remain unknown.
  static const QRegularExpression pattern(QStringLiteral("^(?:测井曲线|离散曲线)-(.+)井$"));
  const auto match = pattern.match(label);
  return match.hasMatch() ? match.captured(1).trimmed() : QString();
}

QString basis(const QString &name)
{
  const QString upper = name.toUpper();
  if (upper == QLatin1String("MD") || upper == QLatin1String("TVD") ||
      upper == QLatin1String("TVDSS") || upper == QLatin1String("TIME")) return upper;
  return {}; // DEPTH / 深度 do not declare the depth reference.
}

bool missing(const QString &text)
{
  return text.isEmpty() || text.compare(QStringLiteral("NA"), Qt::CaseInsensitive) == 0 ||
      text.compare(QStringLiteral("NAN"), Qt::CaseInsensitive) == 0 ||
      text.compare(QStringLiteral("NULL"), Qt::CaseInsensitive) == 0;
}

bool appendRow(const QString &path, const Table &table, int rowNumber, int indexColumn,
               int wellColumn, const QVector<int> &valueColumns, LasHeaderInfo &header,
               Samples &samples, QString *error, QList<LasIssue> *issues)
{
  const QStringList &row = table.rows.at(rowNumber);
  const int line = table.lines.value(rowNumber, rowNumber + 2);
  if (row.size() > table.headers.size())
    return fail(path, line, QCoreApplication::translate("WellLogText", "数据列超过表头列数"), error, issues);
  if (wellColumn >= 0)
  {
    const QString name = row.value(wellColumn).trimmed();
    if (!name.isEmpty() && !header.wellName.isEmpty() && header.wellName != name)
      return fail(path, line, QCoreApplication::translate("WellLogText", "文件含有不同井名，不能合并到同一口井"), error, issues);
    if (!name.isEmpty()) header.wellName = name;
  }
  bool ok = false;
  const double index = row.value(indexColumn).trimmed().toDouble(&ok);
  if (!ok || !std::isfinite(index))
    return fail(path, line, QCoreApplication::translate("WellLogText", "索引值无法识别：%1").arg(row.value(indexColumn)), error, issues);
  if (!samples.index.isEmpty() && index <= samples.index.last())
    return fail(path, line, QCoreApplication::translate("WellLogText", "索引重复或逆序，不自动排序或合并"), error, issues);
  QVector<double> values;
  for (int col : valueColumns)
  {
    const QString text = row.value(col).trimmed();
    double value = std::numeric_limits<double>::quiet_NaN();
    if (!missing(text))
    {
      value = text.toDouble(&ok);
      if (!ok || !std::isfinite(value))
        return fail(path, line, QCoreApplication::translate("WellLogText", "曲线 %1 的数值无法识别：%2").arg(table.headers.at(col), text), error, issues);
      if (value == -999.25 || value == -9999.0) value = std::numeric_limits<double>::quiet_NaN();
    }
    values.append(value);
  }
  samples.index.append(index);
  for (int i = 0; i < values.size(); ++i) samples.curves[i].values.append(values.at(i));
  return true;
}

bool readTable(const QString &path, const Table &table, LasHeaderInfo &header,
               Samples &samples, QSet<QString> &names, QString *error, QList<LasIssue> *issues)
{
  if (table.headers.size() < 2 || table.headers.size() > kMaxColumns)
    return fail(path, 1, QCoreApplication::translate("WellLogText", "需要索引列和曲线列，最多支持 256 列"), error, issues);
  int indexColumn = -1;
  int wellColumn = -1;
  QVector<Column> columns;
  for (int i = 0; i < table.headers.size(); ++i)
  {
    columns.append(column(table.headers.at(i)));
    const QString name = columns.last().name.toUpper();
    if (name.isEmpty()) return fail(path, 1, QCoreApplication::translate("WellLogText", "表头为空或单位括号无效"), error, issues);
    if (name == QLatin1String("WELL") || name == QStringLiteral("井号") || name == QStringLiteral("井名"))
    {
      if (wellColumn >= 0) return fail(path, 1, QCoreApplication::translate("WellLogText", "井名列重复"), error, issues);
      wellColumn = i;
    }
    if (isIndex(name) && indexColumn < 0) indexColumn = i;
  }
  if (indexColumn < 0)
    return fail(path, 1, QCoreApplication::translate("WellLogText", "缺少明确索引列 DEPTH/DEPT/MD/TVD/TVDSS/TIME/深度/井深"), error, issues);
  samples.axis = columns.at(indexColumn);
  QVector<int> valueColumns;
  for (int i = 0; i < columns.size(); ++i)
  {
    if (i == indexColumn || i == wellColumn) continue;
    const auto &col = columns.at(i);
    const QString key = col.name.toUpper();
    if (names.contains(key))
      return fail(path, 1, QCoreApplication::translate("WellLogText", "曲线名称重复：%1；请区分来源后重试").arg(col.name), error, issues);
    names.insert(key);
    valueColumns.append(i);
    samples.curves.append({col.name, col.unit, table.label, {}});
  }
  if (valueColumns.isEmpty() || table.rows.isEmpty())
    return fail(path, 1, QCoreApplication::translate("WellLogText", "没有可读取的曲线采样"), error, issues);
  for (int r = 0; r < table.rows.size(); ++r)
    if (!appendRow(path, table, r, indexColumn, wellColumn, valueColumns, header, samples, error, issues)) return false;
  return true;
}
}

bool convert(const QString &path, const QVector<Table> &tables, LasHeaderInfo &header,
             QList<LasCurve> &curves, QString *error, QList<LasIssue> *issues)
{
  if (tables.isEmpty())
    return fail(path, 0, QCoreApplication::translate("WellLogText", "未找到受支持的测井曲线工作表"), error, issues);
  QVector<Samples> sets;
  QVector<double> index;
  QSet<QString> names;
  int curveCount = 1;
  for (const auto &table : tables)
  {
    Samples samples;
    const QString location = table.label.isEmpty() ? path : path + QStringLiteral(" [%1]").arg(table.label);
    const QString sheetWell = sheetWellName(table.label);
    if (!sheetWell.isEmpty())
    {
      if (!header.wellName.isEmpty() && header.wellName != sheetWell)
        return fail(location, 1, QCoreApplication::translate("WellLogText", "工作表井名与文件内其他井名冲突，不能合并"), error, issues);
      header.wellName = sheetWell;
    }
    if (!readTable(location, table, header, samples, names, error, issues)) return false;
    if (!sets.isEmpty() && (samples.axis.name.compare(sets.first().axis.name, Qt::CaseInsensitive) != 0 ||
                           samples.axis.unit != sets.first().axis.unit))
      return fail(path, 1, QCoreApplication::translate("WellLogText", "工作表索引类型或单位不同，不自动换算；请分别导入"), error, issues);
    curveCount += samples.curves.size();
    if (curveCount > kMaxColumns || qint64(index.size()) + samples.index.size() > kMaxCells)
      return fail(path, 0, QCoreApplication::translate("WellLogText", "曲线数或采样数超过预算，请拆分文件"), error, issues, LasIssue::Category::Oversize);
    index += samples.index;
    sets.append(std::move(samples));
  }
  std::sort(index.begin(), index.end());
  index.erase(std::unique(index.begin(), index.end()), index.end());
  if (qint64(curveCount) * index.size() > kMaxCells)
    return fail(path, 0, QCoreApplication::translate("WellLogText", "不同采样的索引并集超过 400 万单元格预算，请分别导入"), error, issues, LasIssue::Category::Oversize);
  const Column axis = sets.first().axis;
  if (names.contains(axis.name.toUpper()))
    return fail(path, 1, QCoreApplication::translate("WellLogText", "曲线名称与索引名称冲突"), error, issues);
  curves.append({axis.name, axis.unit, {}, index});
  for (const auto &samples : sets)
    for (const auto &source : samples.curves)
    {
      LasCurve curve{source.name, source.unit, source.descr,
          QVector<double>(index.size(), std::numeric_limits<double>::quiet_NaN())};
      int target = 0;
      for (int row = 0; row < samples.index.size(); ++row)
      {
        while (index.at(target) < samples.index.at(row)) ++target;
        curve.values[target] = source.values.at(row);
      }
      curves.append(std::move(curve));
    }
  for (const auto &curve : curves)
  {
    header.curveNames.append(curve.name);
    if (curve.unit.isEmpty() && issues)
      issues->append({LasIssue::Severity::Warning, LasIssue::Category::MissingUnit, 1,
          QCoreApplication::translate("WellLogText", "%1：曲线 %2 未声明单位，保留为空").arg(path, curve.name)});
  }
  header.indexBasis = basis(axis.name);
  header.sawAscii = true;
  return true;
}
}
