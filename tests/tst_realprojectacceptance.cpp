// Optional acceptance against an imported project. Read-only by default;
// Explicit flags can retry XLS, create a provisional formation scheme, or apply
// user-confirmed seismic conventions, each under a project write lock.
#include <QtTest>
#include <QFileInfo>
#include <QJsonDocument>
#include <QSaveFile>
#include <QElapsedTimer>
#include <paleo_data/service.h>
#include "catalog/datacatalog.h"
#include "catalog/areawellprojection.h"
#include "catalog/areasourceprojection.h"
#include "catalog/wellavailability.h"
#include "metadata/projectlock.h"
#include "domain/welllogfacies.h"
#include "domain/wellsection.h"
#include "io/areamodel.h"
#include "services/previewdoc.h"
#include "services/projectdata.h"
#include "services/subintervalfactors.h"
#include "services/stratigraphicsubintervals.h"
#include "services/analysisunitcontext.h"
#include "services/seismicunitinput.h"
#include "services/areawellchart.h"
#include "services/wellpredictioninput.h"
#include "workflow/wellfaciesworkflow.h"
#include "workflow/wellpredictionservice.h"
#include "domain/faciescatalog.h"

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
    QHash<qlonglong,WellDataAvailability> availability;QString availabilityError;
    QVERIFY2(readWellDataAvailability(ds,availability,&availabilityError),qPrintable(availabilityError));
    int withTracks=0,withCurves=0,timeDepthOnly=0;
    for(const auto &value:availability){withTracks+=value.tracks;withCurves+=value.curves;timeDepthOnly+=!value.tracks&&value.timeDepth;}
    report["wellAvailability"]=QJsonObject{{"withTrackData",withTracks},{"withCurves",withCurves},{"timeDepthWithoutTracks",timeDepthOnly}};
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
  void projectPlanarRangeUsesUnifiedCoordinates() {
    paleo::data::DataService ds;std::string error;
    QVERIFY2(ds.openProjectReadOnly(u(paleo::area_model::directory(project)),error),error.c_str());
    paleo::data::ProjectPlanarRange range;
    QVERIFY2(ds.readProjectPlanarRange(range,error),error.c_str());
    QVERIFY2(range.merged.valid,range.status.c_str());
    QCOMPARE(range.merged.coordinateSystem,ds.projectCrs());
    QCOMPARE(range.merged.coordinateUnit,ds.projectCrsUnit());
    if(range.wells.valid) {
      QVERIFY(range.merged.minX<=range.wells.minX&&range.merged.maxX>=range.wells.maxX);
      QVERIFY(range.merged.minY<=range.wells.minY&&range.merged.maxY>=range.wells.maxY);
    }
    if(range.seismic.valid) {
      QVERIFY(range.merged.minX<=range.seismic.minX&&range.merged.maxX>=range.seismic.maxX);
      QVERIFY(range.merged.minY<=range.seismic.minY&&range.merged.maxY>=range.seismic.maxY);
    }
    qInfo()<<"Project planar range"<<QString::fromStdString(range.status)
           <<QString::fromStdString(range.merged.coordinateSystem)<<range.merged.minX<<range.merged.minY
           <<range.merged.maxX<<range.merged.maxY<<"wells"<<range.wellsWithCoordinates<<"volumes"<<range.volumesWithRange;
  }
  void seismicSectionHorizonsFollowActualColumns() {
    paleo::data::DataService ds;std::string error;
    QVERIFY2(ds.openProjectReadOnly(u(paleo::area_model::directory(project)),error),error.c_str());
    std::vector<paleo::data::DataEntry> volumes;QVERIFY(ds.listData("volumes",0,volumes,error));QVERIFY(!volumes.empty());
    paleo::data::SeismicFileInfo info;QVERIFY2(ds.inspectSeismic(volumes[0].id,info,error),error.c_str());
    const int line=info.inlineMin+(info.inlineCount/2)*info.inlineStep;
    paleo::data::SectionReadOptions options;options.maxColumns=256;options.firstTime=options.lastTime=info.firstTime;
    paleo::data::SeismicSectionResult section;QVERIFY2(ds.readSeismicLine(volumes[0].id,"inline",line,options,section,error),error.c_str());
    paleo::data::SectionHorizonQuery query;query.volumeId=volumes[0].id;query.undeclaredTimeUnit="ms";query.undeclaredTimeType="TWT";
    for(int col=0;col<section.columns;++col)query.columns.push_back({section.inlineNumbers[col],section.crosslineNumbers[col],section.traceIndexes[col]>=0});
    QElapsedTimer clock;clock.start();paleo::data::SectionHorizonResult result;
    QVERIFY2(ds.readSectionHorizons(query,result,error),error.c_str());
    int major=0,minor=0;
    for(const auto& curve:result.curves){
      QCOMPARE(curve.timesMs.size(),size_t(section.columns));
      if(curve.internalEdge)++minor;else ++major;
      for(int col=0;col<section.columns;++col)if(section.traceIndexes[col]<0)QVERIFY(std::isnan(curve.timesMs[col]));
    }
    QVERIFY(major>0);if(!result.schemeId.empty())QVERIFY(minor>0);
    qInfo()<<"Section horizon overlay"<<"columns"<<section.columns<<"major"<<major<<"internal"<<minor<<"bands"<<result.bands.size()<<"elapsed_ms"<<clock.elapsed();
    for(const auto& band:result.bands){
      const auto exists=[&](const auto& id){return std::any_of(result.curves.begin(),result.curves.end(),[&](const auto& c){return c.id==id;});};
      QVERIFY(exists(band.topCurveId));QVERIFY(exists(band.bottomCurveId));
    }

  }
  void nativeBatchWellInputs() {
    paleo::data::DataService ds;std::string error;QString detail;
    QVERIFY2(ds.openProjectReadOnly(u(paleo::area_model::directory(project)),error),error.c_str());
    std::vector<paleo::data::WellSummary> wells;QVERIFY(ds.listWells(wells,error));
    QVector<WellFaciesModel> models;
    if(qEnvironmentVariable("PALEO_ACCEPTANCE_MODEL_CHECK")=="1"){
      WellFaciesService service;service.configure(WellFaciesConfig::load());
      QSignalSpy fetched(&service,&WellFaciesService::modelsReady),failed(&service,&WellFaciesService::failed);
      service.fetchModels();QTRY_VERIFY_WITH_TIMEOUT(!fetched.isEmpty()||!failed.isEmpty(),30000);
      QVERIFY2(failed.isEmpty(),failed.isEmpty()?"":qPrintable(failed.first()[0].toString()));
      models=qvariant_cast<QVector<WellFaciesModel>>(fetched.first()[0]);QVERIFY(!models.isEmpty());
    }
    paleo::data::StratigraphicScheme scheme;QVERIFY(ds.readStratigraphicScheme("default",{},scheme,error));
    QJsonArray results;
    for(const auto *name:{"HZ19-1-1A","HZ21-1-1","HZ21-8-1","HZ25-4-1","HZ27-1-3","HZ27-5-1","HZ27-5-2","XJ24-1-1X"}){
      const auto found=std::find_if(wells.begin(),wells.end(),[&](const auto &well){return well.name==name;});QVERIFY(found!=wells.end());
      WellComposite::ComprehensiveWellData data;QVERIFY2(readAreaWellChart(ds,found->id,data,&detail),qPrintable(detail));
      data.continuousCurves.clear();QVERIFY2(paleo::readModelWellPredictionInput(project,data,&detail),qPrintable(detail));
      QSet<qlonglong> groups;int samples=0;
      for(const auto &curve:data.continuousCurves){QVERIFY(curve.modelLogSetId>0);groups.insert(curve.modelLogSetId);samples+=curve.depths.size();}
      QVERIFY(samples>0);QJsonArray units;
      for(const auto &sub:scheme.subIntervals){
        const auto unit=paleo::strat::makeSelection(scheme,QString::fromStdString(sub.interval.value),sub.index);
        const auto range=paleo::strat::analysisWellRange(scheme,unit,{},found->id);
        if(!range.resolved){units.append(QJsonObject{{"unit",paleo::strat::layerToken(unit)},{"ready",false},{"reason",QString::fromStdString(range.reason)}});continue;}
        if(models.isEmpty())continue;
        const auto plan=planWellFaciesRuns(paleo::strat::scopeWellData(data,range),models,{});int count=0;
        for(const auto &run:plan.runs)count+=run.input.rows.size();
        units.append(QJsonObject{{"unit",paleo::strat::layerToken(unit)},{"ready",plan.ready()},{"reason",plan.reason},{"rows",count}});
      }
      if(!models.isEmpty())QVERIFY(std::any_of(units.begin(),units.end(),[](const auto &unit){return unit.toObject().value("ready").toBool();}));
      results.append(QJsonObject{{"well",name},{"groups",groups.size()},{"curves",data.continuousCurves.size()},{"samples",samples},{"units",units}});
      qInfo()<<"Native batch input"<<name<<"groups"<<groups.size()<<"curves"<<data.continuousCurves.size()<<"samples"<<samples;
    }
    report.insert("nativeBatchWellInputs",results);
    if(!models.isEmpty()){
      DataCatalog catalog;catalog.setLockedReadOnly(true);QVERIFY2(catalog.open(project,&detail),qPrintable(detail));
      RemotePredictionRequest request;request.id="acceptance-native-batch";request.kind="wells";request.facies=FaciesCatalog::defaults();
      for(const auto &row:results){const auto name=row.toObject().value("well").toString();const auto ids=catalog.wellsMatchingName(name);QCOMPARE(ids.size(),1);
        request.wells.append(QVariantMap{{"id",ids.first()},{"name",name}});}
      for(const auto &sub:scheme.subIntervals){const auto unit=paleo::strat::makeSelection(scheme,QString::fromStdString(sub.interval.value),sub.index);
        if(paleo::strat::layerToken(unit)=="T70-T72-S1"){request.unitSelection=paleo::strat::toMap(unit);request.horizon=paleo::strat::layerToken(unit);break;}}
      QVERIFY(!request.unitSelection.isEmpty());WellPredictionService service;service.bindCatalog(&catalog,project);
      QSignalSpy checked(&service,&WellPredictionService::readinessReady);service.check(request);QTRY_COMPARE_WITH_TIMEOUT(checked.size(),1,60000);
      QVERIFY2(checked.first()[2].toString().isEmpty(),qPrintable(checked.first()[2].toString()));
      const auto rows=checked.first()[1].toList();QCOMPARE(rows.size(),8);int ready=0;
      for(const auto &row:rows){const auto well=row.toMap();ready+=well.value("ready").toBool();
        QVERIFY(!well.value("reason").toString().contains("explicit curve group"));QVERIFY(!well.value("reason").toString().contains(QStringLiteral("当前基准为 未声明")));}
      QVERIFY(ready>0);report.insert("nativeBatchReadiness",QJsonObject{{"unit",request.horizon},{"ready",ready},{"wells",QJsonArray::fromVariantList(rows)}});
      qInfo()<<"Actual batch readiness"<<request.horizon<<ready<<"of"<<rows.size();
      if(qEnvironmentVariable("PALEO_ACCEPTANCE_LIVE_BATCH")=="1"){
        const auto config=WellFaciesConfig::load();
        QCOMPARE(config.baseUrl.adjusted(QUrl::StripTrailingSlash).toString(),qEnvironmentVariable("PALEO_ACCEPTANCE_AUTHORIZED_API"));
        const auto liveWell=qEnvironmentVariable("PALEO_ACCEPTANCE_BATCH_WELL");
        QVariant chosen=request.wells.first();
        if(!liveWell.isEmpty()){
          const auto found=std::find_if(request.wells.cbegin(),request.wells.cend(),[&](const auto &well){return well.toMap().value("name").toString()==liveWell;});
          QVERIFY(found!=request.wells.cend());chosen=*found;
        }
        request.id="acceptance-native-live";request.wells={chosen};
        QSignalSpy completed(&service,&WellPredictionService::completed),failed(&service,&WellPredictionService::failed);
        service.start(request);QTRY_VERIFY_WITH_TIMEOUT(!completed.isEmpty()||!failed.isEmpty(),180000);
        QVERIFY2(failed.isEmpty(),failed.isEmpty()?"":qPrintable(failed.first()[1].toString()));
        const auto result=qvariant_cast<RemotePredictionResult>(completed.first()[0]);QCOMPARE(result.points.size(),1);
        const auto point=result.points.first().toMap();QVERIFY2(point.value("facies_code").toInt()>0,qPrintable(point.value("prediction_reason").toString()));
        const auto intervals=QJsonDocument::fromJson(point.value("facies_intervals").toString().toUtf8()).array();QVERIFY(!intervals.isEmpty());
        for(const auto &value:intervals){const auto interval=value.toObject();QVERIFY(interval.value("top").toDouble()>=point.value("prediction_depth_top").toDouble());QVERIFY(interval.value("bottom").toDouble()<=point.value("prediction_depth_bottom").toDouble());}
        report.insert("nativeBatchLive",QJsonObject{{"well",point.value("name").toString()},{"unit",request.horizon},{"intervals",intervals.size()},{"model",point.value("prediction_model").toString()}});
      }
    }
  }
  void wholeWellPreviewInputs() {
    const auto name=qEnvironmentVariable("PALEO_ACCEPTANCE_PREDICT_WELL");if(name.isEmpty())return;
    paleo::data::DataService ds;std::string error;
    QVERIFY2(ds.openProjectReadOnly(u(paleo::area_model::directory(project)),error),error.c_str());
    std::vector<paleo::data::WellSummary> wells;QVERIFY(ds.listWells(wells,error));qlonglong id=0;
    for(const auto &well:wells)if(QString::fromStdString(well.name)==name)id=well.id;
    QVERIFY(id>0);WellComposite::ComprehensiveWellData data;QString detail;
    QVERIFY2(readAreaWellChart(ds,id,data,&detail),qPrintable(detail));
    WellFaciesModel model{"acceptance","local input check","1",{}, {"GR"},{QStringLiteral("河口坝")},128};
    const auto strict=planWellFaciesRuns(data,{model},{});const auto plan=planWellFaciesRuns(data,{model},{},true);
    QVERIFY2(plan.ready(),qPrintable(plan.reason));int count=0;
    for(const auto &run:plan.runs){QVERIFY(run.input.rows.size()>=model.window);count+=run.input.rows.size();}
    qInfo()<<"Whole-well input"<<name<<"runs"<<plan.runs.size()<<"valid rows"<<count<<"skips"<<plan.skipped<<"strict reason"<<strict.reason;
    report.insert("wholeWellPreview",QJsonObject{{"runs",plan.runs.size()},{"rows",count},{"strictReason",strict.reason}});
    if(qEnvironmentVariable("PALEO_ACCEPTANCE_LIVE_WELL")=="1"){
      const auto config=WellFaciesConfig::load();
      QCOMPARE(config.baseUrl.adjusted(QUrl::StripTrailingSlash).toString(),qEnvironmentVariable("PALEO_ACCEPTANCE_AUTHORIZED_API"));
      WellFaciesWorkflow workflow;workflow.setAllowPartialPrediction(true);
      QSignalSpy available(&workflow,&WellFaciesWorkflow::availabilityChanged),results(&workflow,&WellFaciesWorkflow::resultReady),busy(&workflow,&WellFaciesWorkflow::busyChanged),status(&workflow,&WellFaciesWorkflow::statusChanged);
      workflow.configure(config,false);workflow.setData(data);
      QTRY_VERIFY_WITH_TIMEOUT(!available.isEmpty()&&available.last()[0].toBool(),30000);
      results.clear();workflow.run();QTRY_VERIFY_WITH_TIMEOUT(!busy.isEmpty()&&!busy.last()[0].toBool(),180000);
      QVERIFY2(!results.isEmpty(),status.isEmpty()?"No prediction result":qPrintable(status.last()[0].toString()));
      const auto result=qvariant_cast<WellFaciesResult>(results.last()[0]);QVERIFY(!result.confidence.depths.isEmpty());
      qInfo()<<"Live whole-well prediction returned"<<result.confidence.depths.size()<<"samples and"<<result.intervals.size()<<"facies intervals";
    }
  }
  void analysisUnitWindowsAndPredictionInputs() {
    paleo::data::DataService ds;std::string error;
    QVERIFY2(ds.openProjectReadOnly(u(paleo::area_model::directory(project)),error),error.c_str());
    paleo::data::StratigraphicScheme scheme;QVERIFY(ds.readStratigraphicScheme("default",{},scheme,error));
    std::vector<paleo::data::WellSummary> wells;QVERIFY(ds.listWells(wells,error));
    QSet<qlonglong> logged;
    for(const auto &well:wells){std::vector<paleo::data::DataEntry> curves;QVERIFY(ds.listData("curves",well.id,curves,error));if(!curves.empty())logged.insert(well.id);}
    int best=-1;auto selection=paleo::strat::UnitSelection{};
    for(const auto &sub:scheme.subIntervals){
      const auto unit=paleo::strat::makeSelection(scheme,QString::fromStdString(sub.interval.value),sub.index);int ready=0;
      for(const auto &well:wells)if(logged.contains(well.id)&&paleo::strat::analysisWellRange(scheme,unit,"catalog-"+QString::number(well.id),well.id).resolved)++ready;
      if(ready>best){best=ready;selection=unit;}
    }
    QVERIFY(best>0);int rows=0,nativeWindows=0;
    for(const auto &well:wells){
      const auto range=paleo::strat::analysisWellRange(scheme,selection,"catalog-"+QString::number(well.id),well.id);
      if(!range.resolved)continue;++nativeWindows;QVERIFY(range.bottom>range.top);
      if(rows||!logged.contains(well.id))continue;
      paleo::data::WellIntervalLogQuery query;query.wellIds={well.id};query.stratigraphy.source=paleo::data::StratigraphicTrackSource::SeismicFramework;
      query.stratigraphy.schemeId=selection.schemeId.toStdString();query.stratigraphy.revision=selection.revision.toStdString();
      query.stratigraphy.subIntervalId.value=selection.subIntervalId.toStdString();
      std::vector<paleo::data::WellStratigraphicLogSlice> slices;QVERIFY(ds.queryWellLogsByStratigraphy(query,slices,error));
      if(slices.empty()||!slices.front().resolved)continue;
      for(const auto depth:slices.front().featureTable.depths){QVERIFY(depth>=range.top);QVERIFY(depth<range.bottom||(range.index==4&&depth==range.bottom));}
      rows=int(slices.front().featureTable.depths.size());
    }
    QVERIFY(rows>0);
    std::vector<paleo::data::DataEntry> volumes;QVERIFY(ds.listData("volumes",0,volumes,error));QVERIFY(!volumes.empty());
    paleo::data::SeismicVolume volume;QVERIFY(ds.readVolume(volumes.front().id,volume,error));
    const auto input=paleo::strat::prepareSeismicUnitInput(project,selection,QString::fromStdString(volume.dataFile));
    QVERIFY2(input.error.isEmpty(),qPrintable(input.error));QVERIFY(input.validCells>0);QCOMPARE(input.unit.revision,selection.revision);
    report["unitAnalysis"]=QJsonObject{{"unit",selection.displayName},{"nativeWindows",nativeWindows},{"loggedWells",best},{"featureRows",rows},{"seismicCells",input.validCells}};
    qInfo()<<"Unit analysis"<<selection.displayName<<"native windows"<<nativeWindows<<"logged wells"<<best<<"sample rows"<<rows<<"seismic cells"<<input.validCells;
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
