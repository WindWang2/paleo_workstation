// 层：视图
// ui/pages/dataops/dataopsexport — D1.7 批量导出清单 + D7.7 导出当前视图。
// CSV/JSON 双格式，字段含路径/类型/版本/归属（+大小/时间/标签）。
// 「所见即所得」：调用方传当前视图里可见（过滤后）的行快照。
#pragma once

#include <QDateTime>
#include <QFile>
#include <QJsonArray>
#include <QJsonDocument>
#include <QJsonObject>
#include <QString>
#include <QTextStream>
#include <QVector>

#include "dataopsmodel.h"

namespace paleo::dataops
{

struct ExportFields
{
  bool withPath = true;
  bool withType = true;
  bool withVersion = true;
  bool withEntities = true;
  bool withTags = true;
  bool withSize = true;
  bool withTime = true;
};

inline QString exportCsv(const QVector<AssetRowInfo> &rows, const ExportFields &f)
{
  QStringList header;
  if (f.withPath) header << QStringLiteral("path");
  if (f.withType) header << QStringLiteral("type");
  if (f.withVersion) header << QStringLiteral("version");
  if (f.withEntities) header << QStringLiteral("entities");
  if (f.withTags) header << QStringLiteral("tags");
  if (f.withSize) header << QStringLiteral("sizeBytes");
  if (f.withTime) header << QStringLiteral("lastModified");

  QStringList lines;
  lines << header.join(QLatin1Char(','));
  for (const AssetRowInfo &r : rows)
  {
    QStringList cells;
    // CSV 转义：引号包裹 + 内部引号翻倍；分隔符在值内同此规则。
    const auto esc = [](const QString &s) {
      QString out = s;
      out.replace(QLatin1Char('"'), QStringLiteral("\"\""));
      return QStringLiteral("\"") + out + QStringLiteral("\"");
    };
    if (f.withPath) cells << esc(r.fileName);
    if (f.withType) cells << esc(r.effectiveType);
    if (f.withVersion) cells << QString::number(r.currentVersionNo);
    if (f.withEntities) cells << esc(r.entityNames.join(QStringLiteral(";")));
    if (f.withTags) cells << esc(r.tags.join(QStringLiteral(";")));
    if (f.withSize) cells << QString::number(r.sizeBytes);
    if (f.withTime) cells << r.lastModified.toString(Qt::ISODate);
    lines << cells.join(QLatin1Char(','));
  }
  return lines.join(QLatin1Char('\n')) + QLatin1Char('\n');
}

inline QByteArray exportJson(const QVector<AssetRowInfo> &rows, const ExportFields &f)
{
  QJsonArray arr;
  for (const AssetRowInfo &r : rows)
  {
    QJsonObject o;
    o.insert(QStringLiteral("assetId"), r.assetId);
    o.insert(QStringLiteral("name"), r.displayName);
    if (f.withPath) o.insert(QStringLiteral("path"), r.fileName);
    if (f.withType) o.insert(QStringLiteral("type"), r.effectiveType);
    if (f.withVersion)
    {
      o.insert(QStringLiteral("version"), r.currentVersionNo);
      o.insert(QStringLiteral("versionCount"), r.versionCount);
      o.insert(QStringLiteral("status"), r.status);
    }
    if (f.withEntities)
    {
      QJsonArray ents;
      for (const QString &e : r.entityNames)
        ents.append(e);
      o.insert(QStringLiteral("entities"), ents);
    }
    if (f.withTags)
    {
      QJsonArray tags;
      for (const QString &t : r.tags)
        tags.append(t);
      o.insert(QStringLiteral("tags"), tags);
    }
    if (f.withSize) o.insert(QStringLiteral("sizeBytes"), double(r.sizeBytes));
    if (f.withTime)
      o.insert(QStringLiteral("lastModified"), r.lastModified.toString(Qt::ISODate));
    arr.append(o);
  }
  QJsonObject root;
  root.insert(QStringLiteral("assets"), arr);
  root.insert(QStringLiteral("count"), rows.size());
  root.insert(QStringLiteral("exportedAt"),
              QDateTime::currentDateTime().toString(Qt::ISODate));
  return QJsonDocument(root).toJson(QJsonDocument::Indented);
}

inline bool writeExportFile(const QString &path, const QByteArray &content)
{
  QFile f(path);
  if (!f.open(QIODevice::WriteOnly))
    return false;
  f.write(content);
  return true;
}

} // namespace paleo::dataops
