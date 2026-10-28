// 层：数据
#include "previewdoc.h"
#include "../io/areamodel.h"
#include "../domain/wellcompositemodel.h"
#include <paleo_data/service.h>
#include <QSet>
#include <cmath>
#include <limits>

namespace {
QString text(const std::string &s) { return QString::fromUtf8(s); }
bool chartFrom(paleo::data::DataService &service, long long source,
               WellComposite::ComprehensiveWellData &out, std::string &error) {
  std::vector<paleo::data::DataEntry> entries;
  QSet<qlonglong> sets, wells;
  double lo = std::numeric_limits<double>::infinity(), hi = -lo;
  const auto depth = [&](double d) { if (std::isfinite(d)) { lo = qMin(lo, d); hi = qMax(hi, d); } };
  if (!service.listData("curves", 0, entries, error)) return false;
  for (const auto &entry : entries) {
    if (entry.sourceId != source || sets.contains(entry.parentId)) continue;
    sets.insert(entry.parentId); wells.insert(entry.wellId);
    paleo::data::LogSet logs;
    if (!service.readLogSet(entry.parentId, true, logs, error)) return false;
    for (const auto &curve : logs.logCurves) {
      WellComposite::CurveData value;
      value.name = text(curve.curveName); value.unit = text(curve.curveUnit);
      for (size_t i = 0; i < logs.samplingAxis.depthValues.size(); ++i) {
        value.depths.append(float(logs.samplingAxis.depthValues[i]));
        value.values.append(float(curve.sampleValues.at(i))); depth(logs.samplingAxis.depthValues[i]);
      }
      if (text(logs.logSetName).contains(QStringLiteral("离散"))) {
        value.mode = WellComposite::CurveDisplayMode::Discrete; out.discreteCurves.append(value);
      } else out.continuousCurves.append(value);
    }
  }
  if (!service.listData("intervals", 0, entries, error)) return false;
  for (const auto &entry : entries) {
    if (entry.sourceId != source) continue;
    wells.insert(entry.wellId);
    paleo::data::IntervalTrack track;
    std::vector<paleo::data::IntervalRecord> rows;
    if (!service.readIntervalTrackInfo(entry.id, track, error) ||
        !service.readIntervals(entry.id, paleo::data::Missing, paleo::data::Missing, rows, error)) return false;
    const QString kind = text(track.trackType), name = text(track.trackName);
    for (const auto &row : rows) {
      const auto &v = row.value;
      if (!std::isfinite(v.topDepth) || !std::isfinite(v.bottomDepth)) continue;
      depth(v.topDepth); depth(v.bottomDepth);
      if (kind.contains(QStringLiteral("岩性"))) {
        WellComposite::LithologyInterval item;
        item.topDepth = float(v.topDepth); item.bottomDepth = float(v.bottomDepth); item.lithoName = text(v.intervalName);
        out.lithologyIntervals.append(item);
      } else if (kind.contains(QStringLiteral("沉积相"))) {
        WellComposite::FaciesInterval item;
        item.topDepth = float(v.topDepth); item.bottomDepth = float(v.bottomDepth);
        item.majorFacies = text(v.intervalName); item.microFacies = text(v.faciesType);
        out.faciesIntervals.append(item);
      } else if (kind.contains(QStringLiteral("取心"))) {
        WellComposite::CoreBarrel item;
        item.topDepth = float(v.topDepth); item.bottomDepth = float(v.bottomDepth); item.barrelNo = text(v.intervalName);
        for (const auto &p : v.properties) {
          const QString label = text(p.measurementName);
          if (label.contains(QStringLiteral("进尺"))) item.cutLength = float(p.numericValue);
          else if (label.contains(QStringLiteral("心长"))) item.recoveredLength = float(p.numericValue);
          else if (label.contains(QStringLiteral("收获率"))) item.recoveryRate = float(p.numericValue);
          else item.description += text(p.originalText) + " ";
        }
        out.coreBarrels.append(item);
      } else if (kind.contains(QStringLiteral("地层")) || name.contains(QStringLiteral("砂层")) || name.contains(QStringLiteral("层序"))) {
        WellComposite::FormationInterval item;
        item.topDepth = float(v.topDepth); item.bottomDepth = float(v.bottomDepth);
        item.name = text(v.intervalName); item.unitType = name;
        if (name.contains(QStringLiteral("砂层"))) out.sandIntervals.append(item);
        else if (name.contains(QStringLiteral("层序"))) out.sequenceIntervals.append(item);
        else out.formationIntervals.append(item);
      } else {
        WellComposite::TextInterval item;
        item.topDepth = float(v.topDepth); item.bottomDepth = float(v.bottomDepth);
        item.category = name; item.text = text(v.intervalName);
        for (const auto &property : v.properties) item.text += " " + text(property.originalText);
        out.textIntervals.append(item);
      }
    }
  }
  if (wells.size() > 1) { error = "source contains multiple wells; select one well before drawing"; return false; }
  if (!wells.isEmpty() && *wells.begin() > 0) {
    paleo::data::WellInfo info;
    if (!service.readWellInfo(*wells.begin(), info, error)) return false;
    out.wellName = text(info.wellName); out.x = info.wellheadX; out.y = info.wellheadY;
  }
  if (std::isfinite(lo)) { out.minDepth = lo; out.maxDepth = hi; }
  return true;
}
}

bool PreviewDocService::areaPreviewAt(const QString &path, WorkbookPreview *table,
    WellComposite::ComprehensiveWellData *chart, bool *chartOk, QString *error) {
  paleo::data::DataService service;
  long long source = 0;
  const auto state = paleo::area_model::openSource(path, service, source, error);
  if (state == paleo::area_model::Read::Unbound) return false;
  if (table) *table = {};
  if (chart) *chart = {};
  if (chartOk) *chartOk = false;
  if (state == paleo::area_model::Read::Failed) { if (table && error) table->error = *error; return true; }
  std::string detail;
  if (table) {
    table->format = QStringLiteral("data_service");
    std::vector<paleo::data::DataEntry> entries;
    for (const std::string kind : {"intervals", "curves", "time_depth", "trajectories", "samples", "tables"}) {
      if (!service.listData(kind, 0, entries, detail)) break;
      for (const auto &entry : entries) {
        if (entry.sourceId != source || table->sheets.size() >= 64) continue;
        paleo::data::DataTable data;
        if (!service.readTable(kind, entry.id, 0, 1000, data, detail)) break;
        WorkbookSheetPreview sheet; sheet.name = text(entry.name);
        for (const auto &column : data.columns) sheet.headers << text(column).left(4096);
        for (const auto &row : data.rows) {
          QStringList cells; for (const auto &cell : row) cells << text(cell).left(4096);
          sheet.rows.append(cells);
        }
        table->sheets.append(sheet);
      }
      if (!detail.empty()) break;
    }
    table->ok = detail.empty(); table->error = text(detail);
  }
  if (chart) {
    const bool ok = chartFrom(service, source, *chart, detail);
    if (chartOk) *chartOk = ok && !chart->isEmpty();
  }
  if (error) *error = text(detail);
  return true;
}
