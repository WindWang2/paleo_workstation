// 层：数据
#include "previewdoc.h"
#include "areawellchart.h"
#include "../io/areamodel.h"
#include "../domain/wellcompositemodel.h"
#include <paleo_data/service.h>
#include <QFileInfo>
#include <QHash>
#include <QSet>
#include <cmath>
#include <limits>
#include <algorithm>

// ui 层不得直接 include io/areamodel.h（层白名单），目录口径经本服务转发。
QString areaModelDirectory(const QString &projectDir)
{
  return paleo::area_model::directory(projectDir);
}

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
QString kindLabel(const std::string &kind) {
  if (kind == "curves") return QStringLiteral("曲线");
  if (kind == "intervals") return QStringLiteral("分层");
  if (kind == "time_depth") return QStringLiteral("时深");
  if (kind == "trajectories") return QStringLiteral("井斜");
  if (kind == "samples") return QStringLiteral("样品");
  if (kind == "markers") return QStringLiteral("标志层");
  if (kind == "core_photos") return QStringLiteral("岩心照片");
  if (kind == "attachments") return QStringLiteral("附件");
  if (kind == "volumes") return QStringLiteral("地震体");
  if (kind == "horizons") return QStringLiteral("层位");
  return text(kind);
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
    std::vector<paleo::data::WellInfoRecord> wells;
    QHash<qlonglong, QString> wellNames;
    if (service.listWellInfos(wells, detail)) {
      for (const auto &well : wells) wellNames.insert(well.id, text(well.value.wellName));
    }
    PreviewDocService::WorkbookSheetPreview wellSheet;
    wellSheet.name = QStringLiteral("井");
    wellSheet.headers = {QStringLiteral("井"), QStringLiteral("类别"), QStringLiteral("名称"),
                         QStringLiteral("数量"), QStringLiteral("摘要")};
    PreviewDocService::WorkbookSheetPreview seismicSheet;
    seismicSheet.name = QStringLiteral("地震");
    seismicSheet.headers = {QStringLiteral("类别"), QStringLiteral("名称"), QStringLiteral("数量"),
                            QStringLiteral("摘要")};
    QSet<qlonglong> seismicSources;
    if (detail.empty()) {
      for (const std::string kind : {"curves", "intervals", "time_depth", "trajectories", "samples",
                                     "markers", "core_photos", "attachments"}) {
        std::vector<paleo::data::DataEntry> entries;
        if (!service.listData(kind, 0, entries, detail)) break;
        for (const auto &entry : entries) {
          const QString well = entry.wellId ? wellNames.value(entry.wellId, QString::number(entry.wellId))
                                            : QStringLiteral("未归井");
          wellSheet.rows.append({well, kindLabel(kind), text(entry.name),
                                 QString::number(entry.count), text(entry.detail)});
        }
      }
    }
    if (detail.empty()) {
      for (const std::string kind : {"volumes", "horizons"}) {
        std::vector<paleo::data::DataEntry> entries;
        if (!service.listData(kind, 0, entries, detail)) break;
        for (const auto &entry : entries) {
          seismicSources.insert(entry.sourceId);
          seismicSheet.rows.append({kindLabel(kind), text(entry.name),
                                    QString::number(entry.count), text(entry.detail)});
        }
      }
    }
    if (detail.empty()) {
      std::vector<paleo::data::SourceEntry> sources;
      if (service.listSources(sources, detail)) {
        for (const auto &entry : sources) {
          if ((entry.kind != "volume" && entry.kind != "horizon") || entry.status != "failed"
              || seismicSources.contains(entry.id)) continue;
          seismicSheet.rows.append({kindLabel(entry.kind), QFileInfo(text(entry.path)).fileName(),
                                    QStringLiteral("0"), text(entry.message)});
        }
      }
    }
    std::stable_sort(wellSheet.rows.begin(), wellSheet.rows.end(),
                     [](const QStringList &a, const QStringList &b) { return a.value(0) < b.value(0); });
    table->sheets = {wellSheet, seismicSheet};
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
bool PreviewDocService::areaPreviewWell(const QString &project, qlonglong wellId, WorkbookPreview *table,
    WellComposite::ComprehensiveWellData *chart, bool *chartOk, QString *error,
    const std::function<bool()> &cancelled) {
  if (table) *table = {};
  if (chart) *chart = {};
  if (chartOk) *chartOk = false;
  paleo::data::DataService service;
  std::string detail;
  if (wellId <= 0 || !service.openProjectReadOnly(paleo::area_model::directory(project).toUtf8().toStdString(), detail)) {
    if (error) *error = detail.empty() ? QStringLiteral("Invalid model well") : text(detail);
    if (table && error) table->error = *error;
    return false;
  }
  paleo::data::WellInfo info;
  QString wellName = QString::number(wellId);
  std::string infoError;
  if (service.readWellInfo(wellId, info, infoError)) wellName = text(info.wellName);
  if (table) {
    table->format = QStringLiteral("data_service");
    PreviewDocService::WorkbookSheetPreview wellSheet;
    wellSheet.name = QStringLiteral("井");
    wellSheet.headers = {QStringLiteral("井"), QStringLiteral("类别"), QStringLiteral("名称"),
                         QStringLiteral("数量"), QStringLiteral("摘要")};
    std::string tableError;
    for (const std::string kind : {"curves", "intervals", "time_depth", "trajectories", "samples",
                                   "markers", "core_photos", "attachments"}) {
      if (cancelled && cancelled()) break;
      std::vector<paleo::data::DataEntry> entries;
      if (!service.listData(kind, wellId, entries, tableError)) break;
      for (const auto &entry : entries)
        wellSheet.rows.append({wellName, kindLabel(kind), text(entry.name),
                               QString::number(entry.count), text(entry.detail)});
    }
    table->sheets = {wellSheet};
    table->ok = tableError.empty();
    table->error = text(tableError);
    if (!tableError.empty() && error) *error = table->error;
  }
  if (chart) {
    QString chartMessage;
    const bool drawn = readAreaWellChart(service, wellId, *chart, &chartMessage, cancelled);
    if (chartOk) *chartOk = drawn;
    if (!drawn && error && error->isEmpty()) *error = chartMessage;
  }
  return true;
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
