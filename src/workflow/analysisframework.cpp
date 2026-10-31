// 层：功能
#include "analysisresultsync.h"
#include "catalog/datacatalog.h"
#include "services/projectdata.h"
#include "services/stratigraphicsubintervals.h"
#include "services/subintervalfactors.h"
#include "paleo_data/service.h"
#include <QFileInfo>
#include <cmath>

QString ensureProjectAnalysisScheme(DataCatalog *catalog, const QString &directory, const QStringList &boundaries) {
  if (!catalog || catalog->refusesWrites() || boundaries.size() < 2 ||
      !QFileInfo::exists(directory + "/artifacts/metadata/area-model/data.sqlite")) return {};
  paleo::data::DataService ds;
  QString error;
  if (!paleo::strat::openProjectDataService(directory, ds, &error)) return error;
  const auto schemes = paleo::strat::listProjectSchemes(ds, &error);
  if (!error.isEmpty()) return error;
  const std::string marker = "Paleo: project model analysis framework";
  paleo::data::StratigraphicScheme previous;
  if (!schemes.isEmpty()) {
    bool found = false;
    for (const auto &choice : schemes) if (choice.id == "project-default") {
      if (!paleo::strat::loadProjectScheme(ds, choice.id, choice.headRevision, previous, &error)) return error;
      found = true;
    }
    if (!found || previous.note != marker) return {};
  }
  ProjectDataFacade data;
  data.setCatalog(catalog, directory);
  paleo::strat::WellTopTable tops;
  for (const auto &well : data.wells()) {
    const auto values = data.topsForMapping(well.id, &error);
    if (!error.isEmpty()) return error;
    auto &row = tops[paleo::strat::stableWellKey(well.id)];
    for (const auto &top : values)
      if (boundaries.contains(top.horizon) && std::isfinite(top.md)) row.insert(top.horizon, top.md);
  }
  bool haveRange = false;
  for (const auto &row : tops) if (row.size() >= 2) haveRange = true;
  if (previous.id.empty() && !haveRange) return {};
  auto candidate = paleo::strat::draftScheme("project-default", QStringLiteral("工程默认"), boundaries, tops);
  candidate.note = marker;
  for (auto &division : candidate.divisions)
    for (const auto &old : previous.divisions) if (old.interval == division.interval) division = old;
  for (auto &well : candidate.wellOccurrences)
    for (const auto &old : previous.wellOccurrences)
      if (old.wellId == well.wellId && old.interval == well.interval) {
        well.internal = old.internal; well.internalStatus = old.internalStatus; well.locked = old.locked;
      }
  if (!previous.id.empty() && paleo::data::framework::impactedSubIntervals(previous, candidate).empty()) return {};
  paleo::data::StratigraphicScheme stored;
  std::vector<paleo::data::FrameworkIssue> issues;
  std::string detail;
  if (!ds.commitStratigraphicScheme(candidate, previous.revision.value, stored, issues, detail))
    return QString::fromStdString(detail);
  return {};
}
