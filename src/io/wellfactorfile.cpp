// 层：数据
#include "wellfactorfile.h"
#include <QFile>
#include <QFileInfo>
#include <QSet>
#include <QTextStream>
#include <cmath>
namespace {
QStringList csvFields(const QString &line) {
  QStringList out; QString field; bool quoted = false;
  for (qsizetype i = 0; i < line.size(); ++i) {
    const auto c = line[i];
    if (c == '"') {
      if (quoted && i + 1 < line.size() && line[i + 1] == '"') { field += c; ++i; }
      else quoted = !quoted;
    } else if (c == ',' && !quoted) { out << field; field.clear(); }
    else field += c;
  }
  if (quoted) return {};
  return out << field;
}
}
namespace paleo::io {
bool isWellFactorFile(const QString &path) {
  const auto name = QFileInfo(path).fileName();
  return name.endsWith(QStringLiteral("_单因素.csv")) || name.endsWith(QStringLiteral("_factors.csv"), Qt::CaseInsensitive);
}
QVariantList readWellFactorFile(const QString &path, QString *error) {
  if (error) error->clear();
  const auto fail = [&](const QString &why) { if (error) *error = why + ": " + path; return QVariantList{}; };
  QFile file(path);
  if (!file.open(QIODevice::ReadOnly) || file.size() > 20 * 1024 * 1024)
    return fail(QStringLiteral("无法读取井单因素文件或超过 20 MB"));
  QTextStream stream(&file);
  const auto headers = csvFields(stream.readLine());
  for (const auto *key : {"well_name", "horizon", "sand_thick", "sand_ratio", "log_layer_thickness_md"})
    if (!headers.contains(QLatin1String(key))) return fail(QStringLiteral("井单因素文件缺少必需列"));
  QVariantList result; QSet<QString> identities;
  while (!stream.atEnd()) {
    const auto line = stream.readLine(); if (line.trimmed().isEmpty()) continue;
    const auto values = csvFields(line);
    if (values.size() != headers.size()) return fail(QStringLiteral("井单因素行列数或引号无效"));
    QVariantMap row;
    for (qsizetype i = 0; i < headers.size(); ++i) {
      const auto &key = headers[i]; const auto value = values[i].trimmed();
      if (QStringList{"well_name", "horizon", "source", "quality", "crs"}.contains(key)) row.insert(key, value);
      else if (!value.isEmpty()) {
        bool ok; const double number = value.toDouble(&ok);
        if (!ok || !std::isfinite(number) || number < 0 ||
            ((key == "sand_ratio" || key == "coverage_ratio") && number > 1 + 1e-8))
          return fail(QStringLiteral("井单因素数值无效"));
        row.insert(key, number);
      }
    }
    const auto identity = row.value("well_name").toString() + '\x1f' + row.value("horizon").toString();
    if (row.value("well_name").toString().isEmpty() || row.value("horizon").toString().isEmpty() || identities.contains(identity))
      return fail(QStringLiteral("井名/层位为空或存在重复井层记录"));
    const double thick = row.value("log_layer_thickness_md").toDouble();
    if (row.contains("sand_thick") && (!row.contains("log_layer_thickness_md") || thick <= 0 ||
        row.value("sand_thick").toDouble() > thick + 1e-6 ||
        (row.contains("sand_ratio") && std::abs(row.value("sand_ratio").toDouble() - row.value("sand_thick").toDouble() / thick) > 1e-6)))
      return fail(QStringLiteral("砂厚、层厚与砂地比不一致"));
    identities.insert(identity); result << row;
  }
  if (result.isEmpty()) return fail(QStringLiteral("井单因素文件没有记录"));
  return result;
}
}
