// 层：数据
#include "previewdoc.h"
#include "areawellchart.h"
#include "../io/areamodel.h"
#include "../domain/wellcompositemodel.h"
#include <paleo_data/service.h>
#include <QSet>
#include <cmath>
#include <limits>
#include <algorithm>

namespace {
QString text(const std::string &s) { return QString::fromUtf8(s); }
bool chartFrom(paleo::data::DataService &service, long long source,
               WellComposite::ComprehensiveWellData &out, std::string &error, qlonglong selected,
               const std::function<bool()> &cancelled) {
  std::vector<long long> wells;
  if (!service.listSourceWells(source, wells, error)) return false;
  if (selected && std::find(wells.begin(), wells.end(), selected) == wells.end()) {
    error = "Selected well does not belong to this model source"; return false;
  }
  if (!selected && wells.size() != 1) {
    error = "Select a single model well before drawing this source"; return false;
  }
  QString detail;
  const bool ok = readAreaWellChart(service, selected ? selected : wells.front(), out, &detail, cancelled);
  error = detail.toUtf8().toStdString(); return ok;
}
bool fillPreview(paleo::data::DataService &service, qlonglong source,
    PreviewDocService::WorkbookPreview *table, WellComposite::ComprehensiveWellData *chart,
    bool *chartOk, QString *error, qlonglong selected = 0, const std::function<bool()> &cancelled = {}) {
  if (table) *table = {};
  if (chart) *chart = {};
  if (chartOk) *chartOk = false;
  std::string detail;
  if (table) {
    table->format = QStringLiteral("data_service");
    std::vector<paleo::data::DataEntry> entries;
    for (const std::string kind : {"intervals", "curves", "time_depth", "trajectories", "samples", "tables"}) {
      if (!service.listSourceData(kind, source, entries, detail)) break;
      for (const auto &entry : entries) {
        if (entry.sourceId != source || table->sheets.size() >= 64) continue;
        paleo::data::DataTable data;
        if (!service.readTable(kind, entry.id, 0, 1000, data, detail)) break;
        PreviewDocService::WorkbookSheetPreview sheet; sheet.name = text(entry.name);
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
    std::string chartError;
    const bool ok = chartFrom(service, source, *chart, chartError, selected, cancelled);
    if (!ok) detail = chartError;
    if (chartOk) *chartOk = ok && !chart->isEmpty();
  }
  if (error) *error = text(detail);
  return true;
}
}
bool PreviewDocService::areaPreviewAt(const QString &path, WorkbookPreview *table,
    WellComposite::ComprehensiveWellData *chart, bool *chartOk, QString *error) {
  paleo::data::DataService service; long long source = 0;
  const auto state = paleo::area_model::openSource(path, service, source, error);
  if (state == paleo::area_model::Read::Unbound) return false;
  if (state == paleo::area_model::Read::Ready) return fillPreview(service,source,table,chart,chartOk,error);
  if(table){*table={};if(error)table->error=*error;}
  if(chart)*chart={};if(chartOk)*chartOk=false;
  return true;
}
bool PreviewDocService::areaPreviewSource(const QString &project, qlonglong source, WorkbookPreview *table,
    WellComposite::ComprehensiveWellData *chart, bool *chartOk, QString *error, qlonglong selected,
    const std::function<bool()> &cancelled) {
  if(table)*table={};if(chart)*chart={};if(chartOk)*chartOk=false;
  paleo::data::DataService service;std::string detail;
  if(source<=0 || !service.openProjectReadOnly(paleo::area_model::directory(project).toUtf8().toStdString(),detail)) {
    if(error)*error=detail.empty()?QStringLiteral("Invalid model source"):text(detail);
    if(table&&error)table->error=*error;return false;
  }
  return fillPreview(service,source,table,chart,chartOk,error,selected,cancelled);
}
