// 层：数据
#include "nativemodel.h"
#include "nativemodel_internal.h"
#include "dataimport_internal.h"
#include "dataimport_log.h"
#include <paleo_data/service.h>
#include <QDir>
#include <QFile>
#include <QSet>
#include <cmath>
#include <map>

namespace paleo::native_model {
namespace {
std::string u(const QString &v) { return v.toUtf8().toStdString(); }
QString q(const std::string &v) { return QString::fromUtf8(v.data(), qsizetype(v.size())); }
using NativeFile = paleo::data::ParsedFile;

QString linkedWell(const dataimport_detail::FamilyContext &ctx) {
  for (const auto &link : ctx.cat->linksForAsset(ctx.assetId))
    if (link.entityType == QLatin1String("well") && !link.unresolved) {
      const auto entity = ctx.cat->entityById(link.entityId);
      if (entity.entityType == QLatin1String("well")) return entity.name;
    }
  return {};
}

NativeFile logFile(const dataimport_detail::FamilyContext &ctx,
                   const dataimport_detail::LogInspection &input) {
  NativeFile model; model.kind = "well_log"; model.status = "registered";
  if (input.curves.size() < 2) return model;
  QString name = linkedWell(ctx);
  if (name.isEmpty()) name = input.header.wellName;
  if (name.isEmpty()) return model; // Do not invent a well for an unassigned source.
  paleo::data::WellDataset well;
  well.wellInfo.wellName = u(name);
  paleo::data::LogSet logs;
  logs.logSetName = u(ctx.fi.fileName());
  logs.depthInfo.depthType = u(input.header.indexBasis);
  logs.depthInfo.depthUnit = u(input.curves.first().unit);
  const auto &axis = input.curves.first().values;
  logs.samplingAxis.depthValues.assign(axis.begin(), axis.end());
  for (int index = 1; index < input.curves.size(); ++index) {
    const auto &curve = input.curves.at(index);
    paleo::data::LogCurve value;
    value.curveName = u(curve.name); value.curveUnit = u(curve.unit);
    value.description = u(curve.descr);
    value.sampleValues.assign(curve.values.begin(), curve.values.end());
    logs.logCurves.push_back(std::move(value));
  }
  well.defaultDepthInfo = logs.depthInfo;
  well.wellbore.logSets.push_back(std::move(logs));
  model.wells.push_back(std::move(well)); model.status = "parsed";
  return model;
}

NativeFile wellFile(const dataimport_detail::FamilyContext &ctx) {
  NativeFile model; model.kind = u(ctx.cls.type); model.status = "registered";
  std::map<std::string, paleo::data::WellDataset> wells;
  if (ctx.cls.type == QLatin1String("well_head")) {
    QSet<QString> names;
    for (const auto &row : ctx.parsedHeads) {
      // A duplicate head remains an unresolved registration, never a merged well.
      if (row.name.isEmpty() || names.contains(row.name)) return model;
      names.insert(row.name);
    }
    for (const auto &row : ctx.parsedHeads) {
      auto &well = wells[u(row.name)]; auto &info = well.wellInfo;
      info.wellName = u(row.name); info.wellType = u(row.wellType);
      info.wellheadX = row.x; info.wellheadY = row.y;
      info.totalDrilledDepth = row.td;
      info.coordinateSystem = u(ctx.version.extra.value("wellCoordinatesCrs").toString());
    }
  } else if (ctx.cls.type == QLatin1String("well_stratification")) {
    for (const auto &row : ctx.parsedTops) {
      if (row.wellName.isEmpty()) continue;
      auto &well = wells[u(row.wellName)]; well.wellInfo.wellName = u(row.wellName);
      if (well.wellbore.markerTracks.empty()) {
        paleo::data::MarkerTrack track; track.trackName = "source tops";
        // MD is the explicit primary field; unknown values stay NaN.
        track.depthInfo.depthType = "MD";
        well.wellbore.markerTracks.push_back(std::move(track));
      }
      paleo::data::Marker marker; marker.markerName = u(row.topName);
      marker.depthValue = row.hasMd ? row.md : paleo::data::Missing;
      marker.tvd = row.hasTvd ? row.tvd : paleo::data::Missing;
      well.wellbore.markerTracks.front().markers.push_back(std::move(marker));
    }
  } else if (ctx.cls.type == QLatin1String("time_depth")) {
    const auto &source = ctx.parsedTd;
    QString name = linkedWell(ctx); if (name.isEmpty()) name = source.wellName;
    for (const QString &basis : {QStringLiteral("MD"), QStringLiteral("TVD"), QStringLiteral("TVDSS")}) {
      paleo::data::TimeDepthTable table;
      table.relationName = u(basis); table.depthInfo.depthType = u(basis);
      table.timeUnit = "ms"; // Legacy TdRow explicitly uses timeMs, not inferred TWT.
      for (const auto &row : source.rows) {
        const double depth = basis == QLatin1String("MD") ? (row.hasMd ? row.md : paleo::data::Missing)
            : basis == QLatin1String("TVD") ? (row.hasTvd ? row.tvd : paleo::data::Missing) : row.tvdss;
        table.depthTimePairs.push_back({depth, row.timeMs});
      }
      if (name.isEmpty()) model.unassignedTimeDepth.push_back(std::move(table));
      else { auto &well = wells[u(name)]; well.wellInfo.wellName = u(name);
        well.timeDepthRelations.push_back(std::move(table)); }
    }
  } else if (ctx.cls.type == QLatin1String("well_deviation")) {
    QString name = linkedWell(ctx); if (name.isEmpty()) name = ctx.parsedDev.wellName;
    if (!name.isEmpty()) {
      auto &well = wells[u(name)]; well.wellInfo.wellName = u(name);
      paleo::data::TrajectorySurvey survey; survey.surveyName = u(ctx.fi.fileName());
      for (const auto &row : ctx.parsedDev.stations)
        survey.surveyPoints.push_back({row.md, row.inclinationDeg, row.azimuthDeg});
      well.wellbore.trajectories.push_back(std::move(survey));
    }
  }
  for (auto &[name, well] : wells) model.wells.push_back(std::move(well));
  if (!model.wells.empty() || !model.unassignedTimeDepth.empty()) model.status = "parsed";
  return model;
}

QVariantMap logMetadata(const dataimport_detail::LogInspection &input) {
  const auto &axis = input.curves.first();
  return {{"well", input.header.wellName}, {"names", input.header.curveNames},
          {"basis", input.header.indexBasis}, {"null", input.header.nullValue},
          {"axisName", axis.name}, {"axisUnit", axis.unit}, {"axisDescription", axis.descr},
          {"wrap", input.header.wrap}, {"delimiter", int(input.header.delimiter)},
          {"sawAscii", input.header.sawAscii}};
}
} // namespace

bool prepare(ImportSession &session, const CatalogVersion &version,
             const dataimport_detail::FamilyContext &ctx,
             const dataimport_detail::LogInspection *log, QString *error) {
  if (!QStringList{"well_log", "well_head", "well_stratification", "time_depth", "well_deviation"}.contains(ctx.cls.type))
    return true; // CatalogStorage already registers all other resources; no empty DB per photo.
  NativeFile model = log ? logFile(ctx, *log) : wellFile(ctx);
  if (model.status != "parsed") return true; // Unresolved identities keep the legacy read contract.
  preserveWellColumns(model, ctx);
  if (!session.ensureStagingRoot(error)) return false;
  const QString relative = QStringLiteral("artifacts/raw/%1/%2/native-model/data.sqlite")
      .arg(version.assetId, version.id);
  CatalogVersion location; location.managed = true; location.path = relative;
  const QString database = DataCatalog::resolvedVersionPath(session.stagingRoot, location);
  if (database.isEmpty()) { if (error) *error = QStringLiteral("unsafe native model path"); return false; }
  const QString root = QFileInfo(database).absolutePath();
  const QString input = version.managed ? session.cat->versionFilePath(version) : version.path;
  const QString published = version.managed ? QDir(root).relativeFilePath(input) : input;
  const auto fail = [&](const QString &detail) {
    // This directory is generated from validated IDs under this session only.
    if (QFileInfo::exists(database)) QFile::setPermissions(database, QFile::permissions(database) | QFileDevice::WriteOwner);
    QDir(root).removeRecursively();
    if (error) *error = detail;
    return false;
  };
  model.path = u(version.sourceUri);
  model.message = "prepared by the workstation format adapter";
  long long sourceId = 0, logSetId = 0;
  std::string detail;
  {
    paleo::data::DataService service;
    if (!service.createProject(u(root), u(ctx.fi.fileName()), detail)) {
      service.closeProject(); return fail(q(detail));
    }
    if (!service.importPreparedFile(u(input), u(version.sourceUri), u(published),
            u(version.sha256), model, sourceId, detail, [&session] { return session.cancelled; })) {
      service.closeProject(); return fail(q(detail));
    }
    if (log && model.status == "parsed") {
      std::vector<paleo::data::DataEntry> curves;
      if (!service.listData("curves", 0, curves, detail) || curves.empty()) {
        service.closeProject(); return fail(q(detail));
      }
      logSetId = curves.front().parentId;
    }
  } // Closing the last SQLite writer checkpoints its WAL before publication.
  const QString digest = dataimport_detail::logFileDigest(database);
  if (digest.isEmpty()) return fail(QStringLiteral("cannot hash native model"));
  QVariantMap binding{{"schema", 1}, {"path", relative}, {"sha256", digest},
                      {"sourceSha256", version.sha256}, {"sourceId", qlonglong(sourceId)},
                      {"kind", ctx.cls.type}, {"typed", model.status == "parsed"}};
  binding.insert("parseReport", version.extra.value("wellParseReport"));
  if (ctx.cls.type == QLatin1String("time_depth")) binding.insert("wellName", ctx.parsedTd.wellName);
  if (ctx.cls.type == QLatin1String("well_deviation")) binding.insert("wellName", ctx.parsedDev.wellName);
  if (logSetId) {
    binding.insert("logSetId", qlonglong(logSetId));
    binding.insert("logHeader", logMetadata(*log));
  }
  QFile::setPermissions(database, QFileDevice::ReadOwner | QFileDevice::ReadUser |
                                   QFileDevice::ReadGroup | QFileDevice::ReadOther);
  if (!session.cat->updateVersionExtra(version.id, {{"nativeModel", binding}}, error))
    return fail(error ? *error : QStringLiteral("cannot bind native model"));
  return true;
}
}
