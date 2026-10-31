// Optional acceptance against an imported project. Read-only by default;
// Explicit flags can retry XLS, create a provisional formation scheme, or apply
// user-confirmed seismic conventions, each under a project write lock.
#include <QtTest>
#include <QFileInfo>
#include <QJsonDocument>
#include <QSaveFile>
#include <paleo_data/service.h>
#include "catalog/datacatalog.h"
#include "catalog/areawellprojection.h"
#include "catalog/areasourceprojection.h"
#include "metadata/projectlock.h"
#include "domain/welllogfacies.h"
#include "domain/wellsection.h"
#include "io/areamodel.h"
#include "services/previewdoc.h"
#include "services/projectdata.h"
#include "services/subintervalfactors.h"
#include "services/stratigraphicsubintervals.h"

class TestRealProjectAcceptance : public QObject {
  Q_OBJECT
  QString project;
  QJsonObject report;
  static std::string u(const QString &s) { return s.toUtf8().toStdString(); }
private slots:
  void initTestCase() {
    project = qEnvironmentVariable("PALEO_ACCEPTANCE_PROJECT");
    if (project.isEmpty()) QSKIP("Set PALEO_ACCEPTANCE_PROJECT to an imported project directory.");
    QVERIFY(QFileInfo::exists(paleo::area_model::directory(project) + "/data.sqlite"));
  }
  void retryFailedWorkbooksWhenExplicitlyRequested() {
    if (qEnvironmentVariable("PALEO_ACCEPTANCE_RETRY_XLS") != "1") return;
    ProjectDirLock lock(project); QString detail;
    QVERIFY2(lock.tryLock(&detail), qPrintable(detail));
    paleo::data::DataService ds; std::string error;
    QVERIFY2(ds.openProject(u(paleo::area_model::directory(project)), error), error.c_str());
    std::vector<paleo::data::SourceEntry> sources; QVERIFY(ds.listSources(sources, error));
    int repaired = 0;
    for (const auto &source : sources) {
      if (!source.active || source.status != "failed" || source.kind != "workbook" ||
          QFileInfo(QString::fromUtf8(source.path)).suffix().compare("xls", Qt::CaseInsensitive)) continue;
      paleo::data::ParseOptions mapping; mapping.comprehensiveWellMetreDepth = true;
      paleo::data::ImportReport result;
      QVERIFY2(ds.reimportSource(source.id, mapping, result, error), error.c_str());
      QCOMPARE(result.failed, 0); ++repaired;
    }
    ds.closeProject();
    DataCatalog catalog; QVERIFY2(catalog.open(project, &detail), qPrintable(detail));
    QVERIFY2(registerAreaWells(catalog, &detail), qPrintable(detail));
    QVERIFY2(registerAreaSources(catalog, &detail), qPrintable(detail));
    report["retriedFailedWorkbooks"] = repaired;
    qInfo() << "Retried failed XLS sources through DataService:" << repaired;
  }
  void structuredInventory() {
    paleo::data::DataService ds; std::string error;
    QVERIFY2(ds.openProjectReadOnly(u(paleo::area_model::directory(project)), error), error.c_str());
    std::vector<paleo::data::SourceEntry> sources; QVERIFY(ds.listSources(sources, error));
    QJsonObject counts; QJsonArray failures;
    for (const auto &s : sources)
      if (s.active && s.status == "failed") failures.append(QJsonObject{{"file", QString::fromUtf8(s.path)}, {"reason", QString::fromUtf8(s.message)}});
    for (const auto *kind : {"curves", "intervals", "horizons", "volumes", "maps", "time_depth", "core_photos"}) {
      std::vector<paleo::data::DataEntry> entries;
      QVERIFY2(ds.listData(kind, 0, entries, error), error.c_str());
      qint64 rows = 0; for (const auto &e : entries) rows += e.count;
      counts[kind] = QJsonObject{{"objects", int(entries.size())}, {"rows", double(rows)}};
    }
    report["counts"] = counts; report["sourceCount"] = int(sources.size()); report["sourceFailures"] = failures;
    qInfo().noquote() << QJsonDocument(counts).toJson(QJsonDocument::Compact);
    QVERIFY(counts["curves"].toObject()["objects"].toInt() > 0);
    QVERIFY(counts["horizons"].toObject()["objects"].toInt() > 0);
    std::vector<paleo::data::DataEntry> horizons; QVERIFY(ds.listData("horizons", 0, horizons, error));
    for (const auto &entry : horizons) {
      paleo::data::SeismicHorizon h;
      QVERIFY2(ds.readHorizon(entry.id, 0, 2, h, error), error.c_str());
      QVERIFY(!h.points.empty()); QVERIFY(std::isfinite(h.points.front().zTime));
    }
  }
  void createSourceFormationFrameworkWhenExplicitlyRequested() {
    const QString formation = qEnvironmentVariable("PALEO_ACCEPTANCE_FORMATION");
    if (qEnvironmentVariable("PALEO_ACCEPTANCE_CREATE_FRAMEWORK") != "1") return;
    QVERIFY(!formation.isEmpty());
    ProjectDirLock lock(project); QString detail;
    QVERIFY2(lock.tryLock(&detail), qPrintable(detail));
    DataCatalog catalog; catalog.setLockedReadOnly(true); QVERIFY(catalog.open(project, &detail));
    paleo::data::DataService ds; std::string error;
    QVERIFY(ds.openProject(u(paleo::area_model::directory(project)), error));
    const auto existing = paleo::strat::listProjectSchemes(ds, &detail); QVERIFY2(detail.isEmpty(), qPrintable(detail));
    for (const auto &s : existing) if (s.id == "qa-formation-md") return;
    paleo::strat::WellTopTable tops;
    const QString top = formation + QStringLiteral("顶"), base = formation + QStringLiteral("底");
    for (const auto &well : catalog.entities("well")) {
      const auto id = well.extra.value("areaModelWellId").toLongLong(); if (id <= 0) continue;
      WellComposite::ComprehensiveWellData chart; PreviewDocService::WorkbookPreview table; bool ready = false;
      QVERIFY(PreviewDocService::areaPreviewWell(project, id, &table, &chart, &ready, &detail));
      if (!ready) continue;
      for (const auto &f : chart.formationIntervals)
        if (f.unitType == QStringLiteral("组") && f.name == formation && f.bottomDepth > f.topDepth) {
          auto &row = tops[paleo::strat::stableWellKey(well.id)];
          row.insert(top, f.topDepth); row.insert(base, f.bottomDepth);
        }
    }
    QVERIFY(!tops.isEmpty());
    auto draft = paleo::strat::draftScheme("qa-formation-md", QStringLiteral("验收井段·") + formation, {top, base}, tops);
    draft.note = "Acceptance: source formation MD bounds; four equal provisional subdivisions, not interpreted horizons.";
    paleo::data::StratigraphicScheme stored; std::vector<paleo::data::FrameworkIssue> issues;
    QVERIFY2(ds.commitStratigraphicScheme(draft, {}, stored, issues, error), error.c_str());
    report["createdSourceFormationFramework"] = QJsonObject{{"formation", formation}, {"wells", tops.size()}, {"revision", QString::fromStdString(stored.revision.value)}};
  }
  void confirmSeismicConventionsWhenExplicitlyRequested() {
    if (qEnvironmentVariable("PALEO_ACCEPTANCE_CONFIRM_SEISMIC") != "1") return;
    const auto crs = u(qEnvironmentVariable("PALEO_ACCEPTANCE_CRS"));
    const auto timeType = u(qEnvironmentVariable("PALEO_ACCEPTANCE_TIME_TYPE"));
    const auto timeUnit = u(qEnvironmentVariable("PALEO_ACCEPTANCE_TIME_UNIT"));
    QVERIFY(!crs.empty()); QVERIFY(timeType == "TWT" || timeType == "OWT"); QVERIFY(timeUnit == "ms");
    ProjectDirLock lock(project); QString detail; QVERIFY2(lock.tryLock(&detail), qPrintable(detail));
    paleo::data::DataService ds; std::string error;
    QVERIFY(ds.openProject(u(paleo::area_model::directory(project)), error));
    std::vector<paleo::data::DataEntry> volumes, horizons;
    QVERIFY(ds.listData("volumes", 0, volumes, error)); QVERIFY(ds.listData("horizons", 0, horizons, error));
    for (const auto &entry : volumes) {
      paleo::data::SeismicVolume v; QVERIFY(ds.readVolume(entry.id, v, error));
      QVERIFY(v.geometry.coordinateSystem.empty() || v.geometry.coordinateSystem == crs);
      QVERIFY(v.sampling.timeType.empty() || v.sampling.timeType == timeType);
      QVERIFY(v.sampling.timeUnit.empty() || v.sampling.timeUnit == timeUnit);
      v.geometry.coordinateSystem = crs; v.geometry.coordinateUnit = "m";
      v.sampling.timeType = timeType; v.sampling.timeUnit = timeUnit;
      QVERIFY2(ds.saveVolume(entry.id, v, error), error.c_str());
    }
    for (const auto &entry : horizons) {
      paleo::data::SeismicHorizon h; QVERIFY(ds.readHorizon(entry.id, 0, 1, h, error));
      QVERIFY(!h.points.empty()); const auto first = h.points.front();
      QVERIFY(h.coordinateSystem.empty() || h.coordinateSystem == crs);
      QVERIFY(h.timeType.empty() || h.timeType == timeType);
      QVERIFY(h.timeUnit.empty() || h.timeUnit == timeUnit);
      h.coordinateSystem = crs; h.coordinateUnit = "m"; h.timeType = timeType; h.timeUnit = timeUnit;
      QVERIFY2(ds.saveHorizonInfo(entry.id, h, error), error.c_str());
      QVERIFY(ds.readHorizon(entry.id, 0, 1, h, error));
      QCOMPARE(h.points.front().zTime, first.zTime); QCOMPARE(h.points.front().x, first.x);
    }
    std::vector<paleo::data::DataEntry> after; QVERIFY(ds.listData("horizons", 0, after, error));
    QCOMPARE(after.size(), horizons.size());
    for (size_t i = 0; i < after.size(); ++i) QCOMPARE(after[i].count, horizons[i].count);
    report["confirmedSeismicConventions"] = QJsonObject{{"crs", QString::fromStdString(crs)},
      {"timeType", QString::fromStdString(timeType)}, {"timeUnit", QString::fromStdString(timeUnit)},
      {"volumes", int(volumes.size())}, {"horizons", int(horizons.size())}};
  }
  void wellChartsPredictionInputsAndSandRatios() {
    paleo::data::DataService ds; std::string error;
    QVERIFY(ds.openProjectReadOnly(u(paleo::area_model::directory(project)), error));
    std::vector<paleo::data::WellSummary> wells; QVERIFY(ds.listWells(wells, error));
    QJsonArray details; int charts = 0, eligible = 0, factors = 0;
    for (const auto &well : wells) {
      if (!well.curves) continue;
      WellComposite::ComprehensiveWellData chart; PreviewDocService::WorkbookPreview table; bool ready = false; QString detail;
      QVERIFY2(PreviewDocService::areaPreviewWell(project, well.id, &table, &chart, &ready, &detail), qPrintable(detail));
      if (!ready) {
        details.append(QJsonObject{{"well", QString::fromUtf8(well.name)}, {"chartBlocker", detail}});
        continue; // Unconfirmed source conventions are a reported data blocker.
      }
      ++charts;
      WellFaciesModel model; model.id = "local-acceptance-input-only"; model.formationGroup = QStringLiteral("恩平组");
      model.curves = {"GR"}; model.window = 128;
      const auto input = prepareWellFaciesInput(chart, model);
      if (input.ready()) ++eligible;
      QVector<wellsection::LithoSegment> segments;
      for (const auto &l : chart.lithologyIntervals) segments.append({l.topDepth, l.bottomDepth, l.lithoName});
      QJsonArray windows;
      for (const auto &f : chart.formationIntervals) {
        if (f.unitType != QStringLiteral("组") || f.bottomDepth <= f.topDepth) continue;
        const auto sand = paleo::strat::sandThicknessInRange(segments, f.topDepth, f.bottomDepth, nullptr);
        if (!sand) continue;
        const double ratio = *sand / (f.bottomDepth - f.topDepth);
        const auto metrics = paleo::strat::metricsFromKnownRatio(f.topDepth, f.bottomDepth, ratio);
        QVERIFY(metrics.ok); ++factors;
        windows.append(QJsonObject{{"formation", f.name}, {"topMd", f.topDepth}, {"baseMd", f.bottomDepth}, {"sandThickness", *sand}, {"sandRatio", ratio}});
      }
      details.append(QJsonObject{{"well", chart.wellName}, {"curves", chart.continuousCurves.size()},
          {"predictionRows", input.rows.size()}, {"inputBlocker", input.reason}, {"factors", windows}});
    }
    report["wellChecks"] = details;
    report["wellSummary"] = QJsonObject{{"wells", int(wells.size())}, {"charts", charts}, {"predictionInputsReady", eligible}, {"formationFactors", factors}};
    qInfo() << "well charts" << charts << "eligible inputs" << eligible << "formation factors" << factors;
    QVERIFY(charts > 0); QVERIFY(eligible > 0); QVERIFY(factors > 0);
  }
  void boundedSeismicViewsAgree() {
    paleo::data::DataService ds; std::string error;
    QVERIFY(ds.openProjectReadOnly(u(paleo::area_model::directory(project)), error));
    std::vector<paleo::data::DataEntry> volumes; QVERIFY(ds.listData("volumes", 0, volumes, error)); QVERIFY(!volumes.empty());
    const auto id = volumes.front().id; paleo::data::SeismicFileInfo info;
    QVERIFY2(ds.inspectSeismic(id, info, error), error.c_str());
    const int il = info.inlineMin + info.inlineCount / 2 * info.inlineStep;
    const int xl = info.crosslineMin + info.crosslineCount / 2 * info.crosslineStep;
    const double time = info.firstTime + info.sampleCount / 2 * info.sampleInterval;
    paleo::data::SectionReadOptions lineOptions; lineOptions.maxColumns = 24;
    lineOptions.firstTime = time; lineOptions.lastTime = time + 8 * info.sampleInterval;
    paleo::data::SeismicSectionResult line;
    QVERIFY2(ds.readSeismicLine(id, "inline", il, lineOptions, line, error), error.c_str());
    QVERIFY(line.columns > 0); QVERIFY(line.samples > 0); QVERIFY(line.amplitudes.size() <= 24 * 9);
    int column = 0; while (column < line.columns && line.traceIndexes[column] < 0) ++column;
    QVERIFY(column < line.columns);
    paleo::data::SeismicTraceReadOptions traceOptions; traceOptions.firstSample = int(std::lround((line.firstTime - info.firstTime) / info.sampleInterval)); traceOptions.sampleCount = line.samples;
    paleo::data::SeismicTraceResult trace;
    QVERIFY2(ds.readSeismicTrace(id, line.traceIndexes[column], traceOptions, trace, error), error.c_str());
    for (int sample = 0; sample < line.samples; ++sample) QCOMPARE(line.amplitudes[column * line.samples + sample], trace.amplitudes[sample]);
    paleo::data::TimeSliceReadOptions options; options.fullResolution = true; options.maxValues = 256;
    options.inlineMin = il; options.inlineMax = il + 7 * info.inlineStep;
    options.crosslineMin = xl; options.crosslineMax = xl + 7 * info.crosslineStep; options.collectTimings = true;
    paleo::data::SeismicTimeSliceResult slice;
    QVERIFY2(ds.readSeismicTimeSlice(id, time, options, slice, error), error.c_str());
    QVERIFY(!slice.amplitudes.empty()); QVERIFY(slice.amplitudes.size() <= 64);
    report["seismic"] = QJsonObject{{"bytes", double(info.fileBytes)}, {"traces", double(info.traceCount)},
      {"samples", info.sampleCount}, {"geometryStatus", QString::fromUtf8(info.geometryStatus)},
      {"lineColumns", line.columns}, {"lineSamples", line.samples}, {"sliceValues", int(slice.amplitudes.size())}};
    qInfo().noquote() << QJsonDocument(report["seismic"].toObject()).toJson(QJsonDocument::Compact);
  }
  void cleanupTestCase() {
    const auto path = qEnvironmentVariable("PALEO_ACCEPTANCE_REPORT"); if (path.isEmpty()) return;
    QSaveFile file(path); QVERIFY(file.open(QIODevice::WriteOnly));
    const auto bytes = QJsonDocument(report).toJson(); QCOMPARE(file.write(bytes), bytes.size()); QVERIFY(file.commit());
  }
};
QTEST_GUILESS_MAIN(TestRealProjectAcceptance)
#include "tst_realprojectacceptance.moc"
