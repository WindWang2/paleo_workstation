// 层：数据
#include "welltable.h"
#include "boundedread.h" // PW-076：.dat 分支同口径有界读
#include "outsourceworkbook.h"
#include "wellfileparsers.h"
#include "welltabletext.h"
#include "../domain/wellnumeric.h"
#include <QFileInfo>
#include <QFile>
#include <QHash>
#include <QJsonDocument>
#include <QJsonObject>
#include <QRegularExpression>
#include <QTextStream>
#include <cmath>
#include <algorithm>
#include <gdal_priv.h>
#include <ogrsf_frmts.h>

namespace paleo::io {
namespace {
// FA-001 同口径的格数预算（SpreadsheetML/xlsx 4M 格）：挡「行数 × 列填充宽」
// 的乘积放大，矢量全读与文本/工作簿路径一致。
constexpr qint64 kMaxWellTableCells = 4'000'000;
// 预览与全量路径共用的层数闸（此前只有预览路径有）。
constexpr int kMaxWellTableLayers = 16;
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
    for (const auto &alias : aliases) {
      if (alias.size() <= 1) continue;
      // ASCII：词边界，并认 <别名>_NAME。不放宽 \b，故 MD 不命中 BASE_MD，WELL_NAME 仍命中 WELL。
      if (::wellHeaderMatchesAlias(headers[i].trimmed(), alias)) return i;
    }
  return -1;
}
int coordinateColumn(const QStringList &headers, bool horizontal, const QString &explicitName)
{
  if (!explicitName.isEmpty()) return headers.indexOf(explicitName);
  // Survey X = northing, Y = easting; GIS x = easting, y = northing.
  // Explicit semantic labels take precedence over the leading X/Y letters.
  const QStringList semantic = horizontal
      ? QStringList{QStringLiteral("横坐标"), QStringLiteral("东坐标"), QStringLiteral("经度"), "EASTING", "LONGITUDE"}
      : QStringList{QStringLiteral("纵坐标"), QStringLiteral("北坐标"), QStringLiteral("纬度"), "NORTHING", "LATITUDE"};
  const int labelled = column(headers, semantic);
  return labelled >= 0 ? labelled : column(headers, horizontal ? QStringList{"__geometry_x", "X"} : QStringList{"__geometry_y", "Y"});
}
WorkbookReadResult vectorOrText(const QString &path, int rowLimit = 1000001, bool *limited = nullptr)
{
  WorkbookReadResult out;
  int sampledRows = 0;
  // issue-810：累计格数预算（与 SpreadsheetML/xlsx 的 4M 格同口径）——行数
  // 闸挡不住「行数 × 列填充宽」的乘积放大，宽表可把整表 QString 撑到 GB 级。
  qint64 cellBudget = kMaxWellTableCells;
  GDALAllRegister();
  auto *ds = static_cast<GDALDataset *>(GDALOpenEx(path.toUtf8().constData(), GDAL_OF_VECTOR | GDAL_OF_READONLY, nullptr, nullptr, nullptr));
  if (!ds) { out.error = QStringLiteral("无法读取矢量或文本表：%1").arg(path); return out; }
  // issue-810：全量路径（readWellTable 正式导入）补层数闸——预览路径早就有
  // 16 层口径，正式路径此前完全裸奔（gpkg 几百层会整库读进来）。
  if (!limited && ds->GetLayerCount() > kMaxWellTableLayers)
  {
    out.error = QStringLiteral("矢量图层超过 %1 个，请拆分后导入：%2")
                    .arg(kMaxWellTableLayers)
                    .arg(path);
    GDALClose(ds);
    return out;
  }
  for (int i = 0; i < ds->GetLayerCount(); ++i) {
    if (limited && i >= kMaxWellTableLayers) { *limited = true; break; }
    auto *layer = ds->GetLayer(i);
    if (!layer) continue;
    WorkbookSheet sheet;
    sheet.name = QString::fromUtf8(layer->GetName());
    auto *def = layer->GetLayerDefn();
    for (int c = 0; c < def->GetFieldCount(); ++c)
      sheet.headers << QString::fromUtf8(def->GetFieldDefn(c)->GetNameRef());
    cellBudget -= sheet.headers.size();
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
      cellBudget -= qint64(row.size());
      ++sampledRows;
      OGRFeature::DestroyFeature(f);
      const bool overRows = (limited && sampledRows >= rowLimit) || sheet.rows.size() >= 1000001;
      if (overRows || cellBudget < 0) {
        if (limited) *limited = true;
        else out.error = cellBudget < 0
            ? QStringLiteral("井表累计单元格超过 %1 上限，请拆分后导入：%2").arg(kMaxWellTableCells).arg(path)
            : QStringLiteral("井表超过一百万行，请拆分后导入");
        break;
      }
    }
    // finding-59：遍历中途出错（坏层/IO 错）不能当正常结束——GDAL 的错误
    // 栈非空即说明这次读取不完整，静默部分导入会把坏文件当好数据收。
    if (!limited && CPLGetLastErrorType() != CE_None)
    {
      out.error = QStringLiteral("矢量层读取中途出错（可能文件损坏）：%1 — %2")
                      .arg(path, QString::fromUtf8(CPLGetLastErrorMsg()));
      out.sheets.clear();
      GDALClose(ds);
      return out;
    }
    out.sheets << sheet;
    if (out.format.isEmpty() && layer->GetSpatialRef()) {
      char *wkt = nullptr;
      layer->GetSpatialRef()->exportToWkt(&wkt);
      out.format = QString::fromUtf8(wkt);
      CPLFree(wkt);
    }
    if (limited && *limited) break;
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
void parseSheet(WellTable &out, const WorkbookSheet &sheet, const QVariantMap &m,
                QVector<WellTopRecord> *basesOut = nullptr)
{
  const auto &h = sheet.headers;
  const int name = column(h, {QStringLiteral("井号"), QStringLiteral("井名"), "WELL", "WELLNAME", "NAME"}, m.value("well").toString());
  const int x = coordinateColumn(h, true, m.value("x").toString());
  const int y = coordinateColumn(h, false, m.value("y").toString());
  const int kb = column(h, {"KB", QStringLiteral("补心海拔")}, m.value("kb").toString());
  const int td = column(h, {"TD", QStringLiteral("完钻井深"), QStringLiteral("总井深")}, m.value("td").toString());
  const int top = column(h, {QStringLiteral("层位"), QStringLiteral("层名"), "HORIZON", "TOP"});
  const int md = column(h, {"MD", QStringLiteral("顶界测深"), QStringLiteral("顶深")});
  const int tvd = column(h, {"TVD", QStringLiteral("顶界垂深"), QStringLiteral("顶垂深")});
  // issue-796：底界列与顶界列在同一趟行解析里解出——此前分层表先整表解析
  // 一遍取 tops、再整表解析一遍取 bases（两次全表扫描 + 两次列匹配）。
  const int mdBase = basesOut ? column(h, {"BOTTOM_MD", "BASE_MD", QStringLiteral("底界测深"), QStringLiteral("底测深")}) : -1;
  const int tvdBase = basesOut ? column(h, {"BOTTOM_TVD", "BASE_TVD", QStringLiteral("底界垂深"), QStringLiteral("底垂深")}) : -1;
  const bool heads = name >= 0 && x >= 0 && y >= 0;
  const bool tops = top >= 0 && (md >= 0 || tvd >= 0);
  if (!heads && !tops) return;
  if (tops && name < 0 && QRegularExpression(QStringLiteral("^(Sheet\\d*|说明|总表|分层)$"), QRegularExpression::CaseInsensitiveOption).match(sheet.name).hasMatch()) return;
  // 层位+深度优先：分层表即使重复了井口坐标，也不能被当成井口把层丢掉。
  const QString type = tops ? QStringLiteral("well_stratification") : QStringLiteral("well_head");
  // 已判定分层后，后续纯井口页仍并入 heads——否则井口坐标被静默整表丢弃
  // （与 tableFromWorkbook 里 well_head 后遇分层仍保留层的 probe 逻辑对称）。
  // 只是不再把 type 翻回井口；映射/CRS 只由定型页登记，避免重新导入时
  // mapping 指向井口页而丢层。
  const bool definesTable = out.type.isEmpty() || out.type == type;
  if (!out.type.isEmpty() && out.type != type && !tops && out.type != QLatin1String("well_stratification"))
    return;
  if (out.type.isEmpty() || tops) out.type = type;
  if (heads) {
    if (definesTable) {
      out.columns = h;
      out.mapping = {{"sheet", sheet.name}, {"well", h.value(name)}, {"x", h.value(x)}, {"y", h.value(y)}, {"kb", h.value(kb)}, {"td", h.value(td)}};
      if (out.crs.isEmpty()) out.crs = inferredCrs(sheet.name + " " + h.value(x) + " " + h.value(y));
      if (out.crs.isEmpty() && (h.value(x).contains(QStringLiteral("经度")) || h.value(x).compare("LONGITUDE", Qt::CaseInsensitive) == 0)) out.crs = "EPSG:4326";
    }
  }
  int line = sheet.headerRowNumber;
  for (const auto &r : sheet.rows) {
    ++line;
    if (r.join(QString()).trimmed().isEmpty()) continue;
    QString well = name >= 0 ? r.value(name).trimmed() : sheet.name.trimmed();
    double xVal = 0, yVal = 0, measuredDepth = 0, tvdVal = 0;
    const bool hasMd = md >= 0 && wellNumber(r.value(md), &measuredDepth);
    const bool hasTvd = tvd >= 0 && wellNumber(r.value(tvd), &tvdVal);
    const bool validHead = heads && !well.isEmpty() && wellNumber(r.value(x), &xVal) && wellNumber(r.value(y), &yVal);
    const bool validTop = tops && !well.isEmpty() && !r.value(top).trimmed().isEmpty() && (hasMd || hasTvd);
    if (!validHead && !validTop) {
      ++out.report.rejectedRows;
      out.report.issues << QStringLiteral("%1 第 %2 行：井名、坐标或层位深度无效，已跳过").arg(sheet.name).arg(line);
      continue;
    }
    if (validHead) {
      WellHeadRecord record;
      record.name = well; record.x = xVal; record.y = yVal;
      // Optional values remain unknown, never invent elevation/depth zero.
      record.kb = record.td = qQNaN();
      double optionalValue = 0;
      if (kb >= 0 && wellNumber(r.value(kb), &optionalValue)) record.kb = optionalValue;
      if (td >= 0 && wellNumber(r.value(td), &optionalValue)) record.td = optionalValue;
      out.heads << record;
      // 预览行上限只影响向导识别预览表的行数（异常大表防卡）；井数以
      // 确认页统计与导入结果为准，不受此限。
      if (out.preview.size() < 200) out.preview << QStringList{well, QString::number(xVal, 'g', 12), QString::number(yVal, 'g', 12)};
    }
    if (validTop) {
      WellTopRecord record;
      record.wellName = well; record.topName = r.value(top).trimmed();
      record.hasMd = hasMd; record.md = measuredDepth;
      record.hasTvd = hasTvd; record.tvd = tvdVal;
      out.tops << record;
    }
    // 底界记录（同一趟行解析产出）：层名与顶界同键，深度取底界列。
    if (basesOut && top >= 0 && !well.isEmpty() && !r.value(top).trimmed().isEmpty()) {
      double baseMd = 0, baseTvd = 0;
      const bool hasBaseMd = mdBase >= 0 && wellNumber(r.value(mdBase), &baseMd);
      const bool hasBaseTvd = tvdBase >= 0 && wellNumber(r.value(tvdBase), &baseTvd);
      if (hasBaseMd || hasBaseTvd) {
        WellTopRecord base;
        base.wellName = well; base.topName = r.value(top).trimmed();
        base.hasMd = hasBaseMd; base.md = baseMd;
        base.hasTvd = hasBaseTvd; base.tvd = baseTvd;
        *basesOut << base;
      }
    }
    ++out.report.acceptedRows;
  }
}
}

QString wellSourceCrsHint(const QString &text) { return inferredCrs(text); }
QVector<WellTopRecord> readWellTableBases(const QString &path)
{
  const auto extension = QFileInfo(path).suffix().toLower();
  if (extension == "dat") {
    // finding-61：与 readWellDelimitedTable 同口径的 64 MB 预算——此前
    // readAll() 无闸，坏/巨大 .dat 会把整文件读进内存。
    QByteArray bytes;
    if (!readBoundedFile(path, 64ll * 1024 * 1024, &bytes)) return {};
    const auto tops = parseWellTopsText(bytes);
    // issue-796：井名+层名 → 顶界记录的哈希索引（首条命中，与 find_if 一致）
    // ——逐行 find_if 是 O(行×顶界)，大表直接把分类/导入卡死。
    QHash<QString, const WellTopRecord *> topIndex;
    topIndex.reserve(tops.size());
    for (const auto &t : tops) {
      const QString key = t.wellName + QLatin1Char('\x1f') + t.topName;
      if (!topIndex.contains(key)) topIndex.insert(key, &t);
    }
    QVector<WellTopRecord> out;
    for (const auto &line : bytes.split('\n')) {
      const QByteArray prefix("# PaleoLayerInterval: "); if (!line.startsWith(prefix)) continue;
      const auto row = QJsonDocument::fromJson(line.mid(prefix.size())).object();
      const auto found = topIndex.constFind(row.value("well").toString() + QLatin1Char('\x1f') + row.value("layer").toString());
      if (found == topIndex.constEnd()) continue;
      const WellTopRecord *current = found.value();
      if (current->hasMd != row.contains("topMd") || current->hasTvd != row.contains("topTvd")) continue;
      if ((current->hasMd && std::abs(current->md - row.value("topMd").toDouble()) >= .001) ||
          (current->hasTvd && std::abs(current->tvd - row.value("topTvd").toDouble()) >= .001)) continue;
      WellTopRecord base; base.wellName = current->wellName; base.topName = current->topName;
      base.hasMd = row.contains("baseMd"); base.md = row.value("baseMd").toDouble();
      base.hasTvd = row.contains("baseTvd"); base.tvd = row.value("baseTvd").toDouble();
      if (base.hasMd || base.hasTvd) out << base;
    }
    return out;
  }
  const auto workbook = extension == "xlsx" ? readWorkbook(path) : readWellDelimitedTable(path);
  // 底界收进 bases（与旧的 parseSheet(..., bases=true) 出口一致）；顶界落进
  // scratch 后丢弃——单趟行解析同时产出两者，见 parseSheet 的 basesOut。
  WellTable scratch;
  QVector<WellTopRecord> bases;
  if (workbook.ok) for (const auto &sheet : workbook.sheets) parseSheet(scratch, sheet, {}, &bases);
  return bases;
}
bool isWellTableContainer(const QString &path)
{
  return QStringList{"xlsx", "gpkg", "shp", "geojson", "csv", "tsv", "txt"}.contains(QFileInfo(path).suffix().toLower());
}
static WellTable tableFromWorkbook(const WorkbookReadResult &wb, const QVariantMap &mapping)
{
  WellTable out;
  if (!wb.ok) { out.error = wb.error; return out; }
  // 分层表的底界记录（与顶界同一趟行解析产出，见 parseSheet 的 basesOut）。
  QVector<WellTopRecord> layerBases;
  out.crs = mapping.value("crs").toString();
  if (!QStringList{"xlsx", "headerless", ""}.contains(wb.format) && out.crs.isEmpty()) out.crs = wb.format;
  for (const auto &sheet : wb.sheets) {
    out.sheets << sheet.name;
    if (!mapping.value("sheet").toString().isEmpty() && sheet.name != mapping.value("sheet").toString()) continue;
    // A point preview and its import must describe the same selected sheet.
    // Stratification workbooks still aggregate all compatible well sheets.
    // 井口表不把后续无关页并进来；但后页若是分层，仍要保留层位。
    if (out.type == QLatin1String("well_head")) {
      WellTable probe;
      parseSheet(probe, sheet, mapping);
      if (probe.type != QLatin1String("well_stratification")) continue;
    }
    if (out.columns.isEmpty()) out.columns = sheet.headers;
    // issue-796：底界与顶界在同一趟行解析里产出——此前分层表在这里解析一遍
    // 取 tops、下面再对全表解析一遍取 bases（两次全表扫描）。
    parseSheet(out, sheet, mapping, &layerBases);
  }
  out.mapping.insert("crs", out.crs);
  if (wb.format == QLatin1String("headerless")) out.mapping.insert(QStringLiteral("headerless"), true);
  // 分层表的表头坐标列是附带信息（井位以井口表为准，下游只用井名+层位+深度）；
  // 源 CRS 未知时不因投影失败整表拒绝（paleo_comp_data 分层总表被
  // 「井点坐标系未知」拦死的回归）。#664 的空 CRS 硬闸保留给井口表——
  // 那里坐标是主数据。
  const bool stratIncidentalHeads =
      out.type == QLatin1String("well_stratification") && out.crs.isEmpty();
  if (!stratIncidentalHeads &&
      !projectWellCoordinates(&out.heads, out.crs, mapping.value("projectCrs").toString(), &out.error)) return out;
  if (out.type.isEmpty()) out.error = QStringLiteral("未识别到井名与坐标，或层位与深度列，请调整字段对应关系");
  if (out.type == QLatin1String("well_stratification")) {
    out.normalized = writeWellLayerTable(out.tops, layerBases);
  }
  else {
    QTextStream stream(&out.normalized);
    stream.setRealNumberPrecision(17);
    for (const auto &h : out.heads) stream << h.name << '\t' << h.x << '\t' << h.y << '\t' << (std::isfinite(h.kb) ? QString::number(h.kb, 'g', 17) : "-99999") << '\t' << (std::isfinite(h.td) ? QString::number(h.td, 'g', 17) : "-99999") << '\n';
  }
  return out;
}
WellTable readWellTable(const QString &path, const QVariantMap &mapping)
{
  const auto ext = QFileInfo(path).suffix().toLower();
  return tableFromWorkbook(ext == "xlsx" ? readWorkbook(path) :
      QStringList{"csv", "tsv", "txt", "dat"}.contains(ext) ? readWellDelimitedTable(path) : vectorOrText(path), mapping);
}
WellTable previewWellTable(const QString &path, const QVariantMap &mapping)
{
  const auto ext = QFileInfo(path).suffix().toLower(); bool limited = false;
  const auto deferred = [&] { WellTable out; out.scanLimited = true; out.mapping = mapping; out.crs = mapping.value("crs").toString(); return out; };
  WorkbookReadResult workbook;
  if (QStringList{"csv", "tsv", "txt", "dat"}.contains(ext)) workbook = previewWellDelimitedTable(path, &limited);
  else if (ext == "xlsx") {
    workbook = previewXlsxWorkbook(path, &limited);
    if (limited && !workbook.ok && workbook.error.isEmpty()) return deferred();
  } else {
    if (!QStringList{"shp", "gpkg"}.contains(ext) && QFileInfo(path).size() > 2 * 1024 * 1024) {
      return deferred();
    }
    workbook = vectorOrText(path, 201, &limited);
  }
  for (auto &sheet : workbook.sheets) if (sheet.rows.size() > 200) {
    sheet.rows.resize(200); sheet.rowNumbers.resize(qMin(sheet.rowNumbers.size(), qsizetype(200))); limited = true;
  }
  auto out = tableFromWorkbook(workbook, mapping); out.scanLimited = limited;
  if (limited && out.crs.isEmpty()) out.crs = mapping.value("crs").toString();
  if (limited && out.mapping.isEmpty()) out.mapping = mapping;
  if (limited && out.heads.isEmpty() && out.tops.isEmpty()) out.error.clear();
  return out;
}
QByteArray writeWellLayerTable(const QVector<WellTopRecord> &tops, const QVector<WellTopRecord> &bases) {
    auto bytes = writeWellTopsText(tops);
    // issue-796：井名+层名 → 顶界记录哈希索引（首条命中，与 find_if 一致）
    // ——逐条 find_if 是 O(底界×顶界)，大表导入直接卡死。
    QHash<QString, const WellTopRecord *> topIndex;
    topIndex.reserve(tops.size());
    for (const auto &t : tops) {
      const QString key = t.wellName + QLatin1Char('\x1f') + t.topName;
      if (!topIndex.contains(key)) topIndex.insert(key, &t);
    }
    for (const auto &base : bases) {
      const auto found = topIndex.constFind(base.wellName + QLatin1Char('\x1f') + base.topName);
      if (found == topIndex.constEnd()) continue;
      const WellTopRecord *top = found.value();
      QJsonObject row{{"well", base.wellName}, {"layer", base.topName}};
      if (top->hasMd) row.insert("topMd", top->md);
      if (top->hasTvd) row.insert("topTvd", top->tvd);
      if (base.hasMd && (!top->hasMd || base.md > top->md)) row.insert("baseMd", base.md);
      if (base.hasTvd && (!top->hasTvd || base.tvd > top->tvd)) row.insert("baseTvd", base.tvd);
      bytes += "# PaleoLayerInterval: " + QJsonDocument(row).toJson(QJsonDocument::Compact) + '\n';
    }
    return bytes;
}
}
