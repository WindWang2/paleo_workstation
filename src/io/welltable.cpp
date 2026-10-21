// 层：数据
#include "welltable.h"
#include "outsourceworkbook.h"
#include "wellfileparsers.h"
#include "welltabletext.h"
#include "../domain/wellnumeric.h"
#include <QFileInfo>
#include <QRegularExpression>
#include <QTextStream>
#include <cmath>
#include <gdal_priv.h>
#include <ogrsf_frmts.h>

namespace paleo::io {
namespace {
bool wellNumber(const QString &text, double *value)
{
  return paleo::wellnumeric::parse(text, value) == paleo::wellnumeric::NumberKind::Value;
}
int column(const QStringList &headers, const QStringList &aliases, const QString &explicitName = {})
{
  if (!explicitName.isEmpty()) return headers.indexOf(explicitName);
  for (const auto &alias : aliases)
    for (int i = 0; i < headers.size(); ++i)
      if (headers[i].trimmed().compare(alias, Qt::CaseInsensitive) == 0) return i;
  for (int i = 0; i < headers.size(); ++i)
    for (const auto &alias : aliases)
      if (alias.size() > 1 && headers[i].contains(alias, Qt::CaseInsensitive)) return i;
  return -1;
}
WorkbookReadResult vectorOrText(const QString &path)
{
  WorkbookReadResult out;
  GDALAllRegister();
  auto *ds = static_cast<GDALDataset *>(GDALOpenEx(path.toUtf8().constData(), GDAL_OF_VECTOR | GDAL_OF_READONLY, nullptr, nullptr, nullptr));
  if (!ds) { out.error = QStringLiteral("无法读取矢量或文本表：%1").arg(path); return out; }
  for (int i = 0; i < ds->GetLayerCount(); ++i) {
    auto *layer = ds->GetLayer(i);
    if (!layer) continue;
    WorkbookSheet sheet;
    sheet.name = QString::fromUtf8(layer->GetName());
    auto *def = layer->GetLayerDefn();
    for (int c = 0; c < def->GetFieldCount(); ++c)
      sheet.headers << QString::fromUtf8(def->GetFieldDefn(c)->GetNameRef());
    const bool points = wkbFlatten(layer->GetGeomType()) == wkbPoint;
    if (points) sheet.headers << QStringLiteral("__geometry_x") << QStringLiteral("__geometry_y");
    while (auto *f = layer->GetNextFeature()) {
      QStringList row;
      for (int c = 0; c < def->GetFieldCount(); ++c) row << QString::fromUtf8(f->GetFieldAsString(c));
      if (points) {
        auto *g = f->GetGeometryRef();
        auto *p = g && wkbFlatten(g->getGeometryType()) == wkbPoint ? g->toPoint() : nullptr;
        row << (p ? QString::number(p->getX(), 'g', 17) : QString())
            << (p ? QString::number(p->getY(), 'g', 17) : QString());
      }
      sheet.rows << row;
      OGRFeature::DestroyFeature(f);
      if (sheet.rows.size() > 1000000) { out.error = QStringLiteral("井表超过一百万行，请拆分后导入"); break; }
    }
    out.sheets << sheet;
    if (out.format.isEmpty() && layer->GetSpatialRef()) {
      char *wkt = nullptr;
      layer->GetSpatialRef()->exportToWkt(&wkt);
      out.format = QString::fromUtf8(wkt);
      CPLFree(wkt);
    }
  }
  GDALClose(ds);
  out.ok = out.error.isEmpty() && !out.sheets.isEmpty();
  return out;
}
QString inferredCrs(const QString &text)
{
  QRegularExpression epsg(QStringLiteral("EPSG\\s*[:=]?\\s*(\\d{4,6})"), QRegularExpression::CaseInsensitiveOption);
  auto match = epsg.match(text);
  if (match.hasMatch()) return QStringLiteral("EPSG:") + match.captured(1);
  QRegularExpression utm(QStringLiteral("UTM\\s*(?:ZONE\\s*)?(\\d{1,2})\\s*([NS])"), QRegularExpression::CaseInsensitiveOption);
  match = utm.match(text);
  if (match.hasMatch()) {
    const int zone = match.captured(1).toInt();
    if (zone >= 1 && zone <= 60)
      return QStringLiteral("EPSG:%1").arg((match.captured(2).compare("N", Qt::CaseInsensitive) == 0 ? 32600 : 32700) + zone);
  }
  return {};
}
void parseSheet(WellTable &out, const WorkbookSheet &sheet, const QVariantMap &m)
{
  const auto &h = sheet.headers;
  const int name = column(h, {QStringLiteral("井号"), QStringLiteral("井名"), "WELL", "WELLNAME", "NAME"}, m.value("well").toString());
  const int x = column(h, {"__geometry_x", "X", "EASTING", QStringLiteral("井口横坐标"), QStringLiteral("横坐标"), QStringLiteral("经度"), "LONGITUDE"}, m.value("x").toString());
  const int y = column(h, {"__geometry_y", "Y", "NORTHING", QStringLiteral("井口纵坐标"), QStringLiteral("纵坐标"), QStringLiteral("纬度"), "LATITUDE"}, m.value("y").toString());
  const int kb = column(h, {"KB", QStringLiteral("补心海拔")}, m.value("kb").toString());
  const int td = column(h, {"TD", QStringLiteral("完钻井深"), QStringLiteral("总井深")}, m.value("td").toString());
  const int top = column(h, {QStringLiteral("层位"), QStringLiteral("层名"), "HORIZON", "TOP"});
  const int md = column(h, {"MD", QStringLiteral("顶界测深"), QStringLiteral("顶深")});
  const int tvd = column(h, {"TVD", QStringLiteral("顶界垂深"), QStringLiteral("顶垂深")});
  const bool heads = name >= 0 && x >= 0 && y >= 0;
  const bool tops = top >= 0 && (md >= 0 || tvd >= 0);
  if (!heads && !tops) return;
  if (tops && name < 0 && QRegularExpression(QStringLiteral("^(Sheet\\d*|说明|总表|分层)$"), QRegularExpression::CaseInsensitiveOption).match(sheet.name).hasMatch()) return;
  const QString type = heads ? QStringLiteral("well_head") : QStringLiteral("well_stratification");
  if (!out.type.isEmpty() && out.type != type) return;
  out.type = type;
  if (heads) {
    out.columns = h;
    out.mapping = {{"sheet", sheet.name}, {"well", h.value(name)}, {"x", h.value(x)}, {"y", h.value(y)}, {"kb", h.value(kb)}, {"td", h.value(td)}};
    if (out.crs.isEmpty()) out.crs = inferredCrs(sheet.name + " " + h.value(x) + " " + h.value(y));
    if (out.crs.isEmpty() && (h.value(x).contains(QStringLiteral("经度")) || h.value(x).compare("LONGITUDE", Qt::CaseInsensitive) == 0)) out.crs = "EPSG:4326";
  }
  int line = sheet.headerRowNumber;
  for (const auto &r : sheet.rows) {
    ++line;
    if (r.join(QString()).trimmed().isEmpty()) continue;
    QString well = name >= 0 ? r.value(name).trimmed() : sheet.name.trimmed();
    double a = 0, b = 0, measuredDepth = 0;
    const bool hasMd = md >= 0 && wellNumber(r.value(md), &measuredDepth);
    const bool hasTvd = tvd >= 0 && wellNumber(r.value(tvd), &a);
    const bool valid = !well.isEmpty() && (heads
        ? wellNumber(r.value(x), &a) && wellNumber(r.value(y), &b)
        : !r.value(top).trimmed().isEmpty() && (hasMd || hasTvd));
    if (!valid) {
      ++out.report.rejectedRows;
      out.report.issues << QStringLiteral("%1 第 %2 行：井名、坐标或层位深度无效，已跳过").arg(sheet.name).arg(line);
      continue;
    }
    if (heads) {
      WellHeadRecord record;
      record.name = well; record.x = a; record.y = b;
      // Optional values remain unknown, never invent elevation/depth zero.
      record.kb = record.td = qQNaN();
      double optionalValue = 0;
      if (kb >= 0 && wellNumber(r.value(kb), &optionalValue)) record.kb = optionalValue;
      if (td >= 0 && wellNumber(r.value(td), &optionalValue)) record.td = optionalValue;
      out.heads << record;
      if (out.preview.size() < 8) out.preview << QStringList{well, QString::number(a, 'g', 12), QString::number(b, 'g', 12)};
    } else {
      WellTopRecord record;
      record.wellName = well; record.topName = r.value(top).trimmed();
      record.hasMd = hasMd; record.md = measuredDepth;
      record.hasTvd = hasTvd; record.tvd = a;
      out.tops << record;
    }
    ++out.report.acceptedRows;
  }
}
}

QString wellSourceCrsHint(const QString &text) { return inferredCrs(text); }
bool isWellTableContainer(const QString &path)
{
  return QStringList{"xlsx", "gpkg", "shp", "geojson", "csv", "tsv", "txt"}.contains(QFileInfo(path).suffix().toLower());
}
WellTable readWellTable(const QString &path, const QVariantMap &mapping)
{
  WellTable out;
  const bool xlsx = QFileInfo(path).suffix().compare("xlsx", Qt::CaseInsensitive) == 0;
  const bool text = QStringList{"csv", "tsv", "txt", "dat"}.contains(QFileInfo(path).suffix().toLower());
  const auto wb = xlsx ? readWorkbook(path) : text ? readWellDelimitedTable(path) : vectorOrText(path);
  if (!wb.ok) { out.error = wb.error; return out; }
  out.crs = mapping.value("crs").toString();
  if (!xlsx && !text && out.crs.isEmpty()) out.crs = wb.format;
  for (const auto &sheet : wb.sheets) {
    out.sheets << sheet.name;
    if (!mapping.value("sheet").toString().isEmpty() && sheet.name != mapping.value("sheet").toString()) continue;
    // A point preview and its import must describe the same selected sheet.
    // Stratification workbooks still aggregate all compatible well sheets.
    if (out.type == QLatin1String("well_head")) continue;
    if (out.columns.isEmpty()) out.columns = sheet.headers;
    parseSheet(out, sheet, mapping);
  }
  out.mapping.insert("crs", out.crs);
  if (wb.format == QLatin1String("headerless")) out.mapping.insert(QStringLiteral("headerless"), true);
  if (!projectWellCoordinates(&out.heads, out.crs, mapping.value("projectCrs").toString(), &out.error)) return out;
  if (out.type.isEmpty()) out.error = QStringLiteral("未识别到井名与坐标，或层位与深度列，请调整字段对应关系");
  if (out.type == QLatin1String("well_stratification")) out.normalized = writeWellTopsText(out.tops);
  else {
    QTextStream stream(&out.normalized);
    stream.setRealNumberPrecision(17);
    for (const auto &h : out.heads) stream << h.name << '\t' << h.x << '\t' << h.y << '\t' << (std::isfinite(h.kb) ? QString::number(h.kb, 'g', 17) : "-99999") << '\t' << (std::isfinite(h.td) ? QString::number(h.td, 'g', 17) : "-99999") << '\n';
  }
  return out;
}
}
