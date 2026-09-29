// 层：数据
#include "benchreport.h"

#include <QDateTime>
#include <QDir>
#include <QFile>
#include <QJsonArray>
#include <QJsonDocument>
#include <QJsonObject>
#include <QSaveFile>

namespace
{
  QString escapeHtml(const QString &s)
  {
    QString out = s.toHtmlEscaped();
    return out;
  }
} // namespace

QByteArray BenchReport::toJson(const QVector<BenchResult> &results)
{
  QJsonObject root;
  root.insert(QStringLiteral("generated_utc"),
              QDateTime::currentDateTimeUtc().toString(Qt::ISODateWithMs));
  QJsonArray arr;
  for (const BenchResult &r : results)
  {
    QJsonObject o;
    o.insert(QStringLiteral("name"), r.name);
    o.insert(QStringLiteral("group"), r.group);
    o.insert(QStringLiteral("value"), r.value);
    o.insert(QStringLiteral("unit"), r.unit);
    o.insert(QStringLiteral("budget"), r.budget);
    o.insert(QStringLiteral("pass"), r.pass);
    o.insert(QStringLiteral("note"), r.note);
    arr.append(o);
  }
  root.insert(QStringLiteral("benchmarks"), arr);
  return QJsonDocument(root).toJson(QJsonDocument::Indented);
}

bool BenchReport::writeJsonFile(const QString &path, const QVector<BenchResult> &results)
{
  const QFileInfo info(path);
  if (!info.dir().exists() && !QDir().mkpath(info.absolutePath()))
    return false;
  QSaveFile f(path);
  f.setDirectWriteFallback(false);
  if (!f.open(QIODevice::WriteOnly | QIODevice::Truncate))
    return false;
  const QByteArray bytes = toJson(results);
  if (f.write(bytes) != bytes.size() || !f.commit())
    return false;
  return true;
}

QByteArray BenchReport::toHtml(const QVector<BenchResult> &results, const QString &title)
{
  QStringList groups;
  for (const BenchResult &r : results)
    if (!groups.contains(r.group))
      groups.append(r.group);

  QByteArray html;
  html += "<!DOCTYPE html>\n<html lang=\"zh\">\n<head>\n<meta charset=\"utf-8\">\n";
  html += "<title>" + escapeHtml(title).toUtf8() + "</title>\n";
  html += "<style>\n";
  html += "body{font-family:system-ui,'Noto Sans SC',sans-serif;margin:24px;background:#f7f8fa;color:#1c1f23;}\n";
  html += "h1{font-size:20px;} h2{font-size:16px;margin-top:28px;border-bottom:2px solid #d8dce2;padding-bottom:4px;}\n";
  html += "table{border-collapse:collapse;width:100%;background:#fff;box-shadow:0 1px 2px rgba(0,0,0,.06);}\n";
  html += "th,td{padding:6px 12px;text-align:left;border-bottom:1px solid #e6e9ed;font-size:13px;font-variant-numeric:tabular-nums;}\n";
  html += "th{background:#eef1f5;font-weight:600;}\n";
  html += ".pass{color:#1a7f37;font-weight:600;} .fail{color:#c62828;font-weight:600;}\n";
  html += ".bar{height:8px;border-radius:4px;background:#1a7f37;min-width:2px;}\n";
  html += ".bar.over{background:#c62828;}\n";
  html += "footer{margin-top:24px;font-size:12px;color:#6b7280;}\n";
  html += "</style>\n</head>\n<body>\n";
  html += "<h1>" + escapeHtml(title).toUtf8() + "</h1>\n";
  const int failed = std::count_if(results.begin(), results.end(),
                                   [](const BenchResult &r) { return !r.pass; });
  html += "<p>" + QByteArray::number(results.size()) + " benchmarks, <span class=\"" +
          (failed ? "fail" : "pass") + "\">" + QByteArray::number(failed) +
          " over budget</span></p>\n";

  const double maxRatio = 1.0; // 条宽 = value/budget（有预算时）
  for (const QString &g : groups)
  {
    html += "<h2>" + escapeHtml(g).toUtf8() + "</h2>\n<table>\n";
    html += "<tr><th>name</th><th>value</th><th>unit</th><th>budget</th><th>bar</th><th>note</th></tr>\n";
    for (const BenchResult &r : results)
    {
      if (r.group != g)
        continue;
      html += "<tr><td>" + escapeHtml(r.name).toUtf8() + "</td>";
      html += "<td>" + QByteArray::number(r.value, 'f', 3) + "</td>";
      html += "<td>" + escapeHtml(r.unit).toUtf8() + "</td>";
      html += "<td>" + (r.budget > 0 ? QByteArray::number(r.budget, 'f', 3) : QByteArray("--")) + "</td>";
      const bool over = r.budget > 0 && r.value > r.budget;
      const int width = r.budget > 0
                            ? qBound(2, int(100.0 * r.value / r.budget / maxRatio), 100)
                            : 100;
      html += "<td><div class=\"bar" + QByteArray(over ? " over" : "") + "\" style=\"width:" +
              QByteArray::number(width) + "%\"></div></td>";
      html += "<td>" + escapeHtml(r.note).toUtf8() + "</td></tr>\n";
    }
    html += "</table>\n";
  }
  html += "<footer>generated " +
          QDateTime::currentDateTimeUtc().toString(Qt::ISODate).toUtf8() +
          " — paleo_workstation docs/perf</footer>\n</body>\n</html>\n";
  return html;
}

bool BenchReport::writeHtmlFile(const QString &path, const QVector<BenchResult> &results,
                                const QString &title)
{
  const QFileInfo info(path);
  if (!info.dir().exists() && !QDir().mkpath(info.absolutePath()))
    return false;
  QSaveFile f(path);
  f.setDirectWriteFallback(false);
  if (!f.open(QIODevice::WriteOnly | QIODevice::Truncate))
    return false;
  const QByteArray bytes = toHtml(results, title);
  if (f.write(bytes) != bytes.size() || !f.commit())
    return false;
  return true;
}
