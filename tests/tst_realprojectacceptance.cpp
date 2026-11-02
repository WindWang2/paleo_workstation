// Optional acceptance against an imported project. Read-only by default;
// Explicit flags can retry XLS, create a provisional formation scheme, or apply
// user-confirmed seismic conventions, each under a project write lock.
#include <QtTest>
#include <QFileInfo>
#include <QJsonDocument>
#include <QSaveFile>
#include <QElapsedTimer>
#include <QScopeGuard>
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
#include "domain/faciestaxonomy.h"
#include "services/wellreadiness.h"
#include "workflow/wellsectionworkflow.h"
#include "workflow/sectionworkbench.h"
#include "services/paleotaskservice.h"
#include "services/seismictaskservice.h"
#include "qgis/wellattributestore.h"
#include "services/seismicdataaccess.h"
#include "services/timedepthalignment.h"
#include "linkage/seismicmaplink.h"

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
  void liveLocatedBatchResponseContract(){
    if(qEnvironmentVariable("PALEO_ACCEPTANCE_LIVE_BATCH")!="1")QSKIP("Explicit authorized live batch only");
    const auto config=WellFaciesConfig::load();
    QCOMPARE(config.baseUrl.adjusted(QUrl::StripTrailingSlash).toString(),qEnvironmentVariable("PALEO_ACCEPTANCE_AUTHORIZED_API"));
    paleo::data::DataService ds;std::string error;QString detail;
    QVERIFY(ds.openProjectReadOnly(u(paleo::area_model::directory(project)),error));
    paleo::data::StratigraphicScheme scheme;QVERIFY(ds.readStratigraphicScheme("default",{},scheme,error));
    DataCatalog catalog;catalog.setLockedReadOnly(true);QVERIFY(catalog.open(project,&detail));
    const auto availability=paleo::analysisReadiness(&catalog,project);QJsonArray batches;
    for(const auto *token:{"T30-T32-S1","T70-T72-S1"}){
      RemotePredictionRequest request;request.id=QStringLiteral("diagnostic-")+token;request.horizon=token;request.kind="wells";request.facies=FaciesCatalog::defaults();
      for(const auto &sub:scheme.subIntervals){const auto unit=paleo::strat::makeSelection(scheme,QString::fromStdString(sub.interval.value),sub.index);
        if(paleo::strat::layerToken(unit)==token){request.unitSelection=paleo::strat::toMap(unit);break;}}
      QVERIFY(!request.unitSelection.isEmpty());
      for(const auto &well:catalog.entities("well"))if(availability.value(well.id).hasCurves&&well.hasSurface&&std::isfinite(well.surfaceX)&&std::isfinite(well.surfaceY))
        request.wells.append(QVariantMap{{"id",well.id},{"name",well.name},{"x",well.surfaceX},{"y",well.surfaceY},
          {"vector_formations",WellAttributeStore::rows(project,nullptr,WellAttributeStore::Table::Formations,well.id)}});
      QCOMPARE(request.wells.size(),47);WellPredictionService service;service.bindCatalog(&catalog,project);
      QSignalSpy completed(&service,&WellPredictionService::completed),failed(&service,&WellPredictionService::failed);
      service.start(request);QTRY_VERIFY_WITH_TIMEOUT(!completed.isEmpty()||!failed.isEmpty(),600000);
      QVERIFY2(failed.isEmpty(),failed.isEmpty()?"":qPrintable(failed.first()[1].toString()));
      const auto result=qvariant_cast<RemotePredictionResult>(completed.first()[0]);QCOMPARE(result.points.size(),request.wells.size());
      QVariantList normalized;QString hierarchyError;QVERIFY(FaciesTaxonomy::normalize(result.facies,&normalized,&hierarchyError));
      QCOMPARE(normalized,result.facies);
      for(int i=0;i<request.facies.size();++i)QCOMPARE(result.facies.value(i),request.facies[i]);
      QJsonArray rows;int predicted=0;QSet<int> codes;for(const auto &v:result.facies)codes.insert(v.toMap().value("code").toInt());
      for(int i=0;i<result.points.size();++i){const auto point=result.points[i].toMap();const auto wanted=request.wells[i].toMap();
        const auto intervals=QJsonDocument::fromJson(point.value("facies_intervals").toString().toUtf8()).array();
        const auto formations=QJsonDocument::fromJson(point.value("formation_intervals").toString().toUtf8()).array();
        const bool skipped=point.value("prediction_status")==QStringLiteral("未预测");predicted+=!skipped;
        if(!skipped)QCOMPARE(formations.size(),1);
        QVERIFY(point.value("id")==wanted.value("id")&&point.value("x")==wanted.value("x")&&point.value("y")==wanted.value("y"));
        QVERIFY(skipped||codes.contains(point.value("facies_code").toInt()));
        rows.append(QJsonObject{{"name",point.value("name").toString()},{"skipped",skipped},{"reason",point.value("prediction_reason").toString()},
          {"intervals",intervals},{"formations",formations},{"top",point.value("prediction_depth_top").toDouble()},{"bottom",point.value("prediction_depth_bottom").toDouble()}});
      }
      batches.append(QJsonObject{{"unit",token},{"schemaCount",result.facies.size()},{"requestSchemaCount",request.facies.size()},{"predicted",predicted},{"rows",rows},
        {"points",QJsonArray::fromVariantList(result.points)},{"facies",QJsonArray::fromVariantList(result.facies)}});
      report.insert("liveBatchResponseContract",batches);qInfo()<<"Live batch contract"<<token<<"schema"<<result.facies.size()<<"predicted"<<predicted;
    }
  }
  void fullWellScopeReadiness(){
    paleo::data::DataService ds;std::string error;QString detail;
    QVERIFY2(ds.openProjectReadOnly(u(paleo::area_model::directory(project)),error),error.c_str());
    std::vector<paleo::data::WellSummary> wells;QVERIFY(ds.listWells(wells,error));
    QHash<qlonglong,WellDataAvailability> availability;QVERIFY(readWellDataAvailability(ds,availability,&detail));
    DataCatalog catalog;catalog.setLockedReadOnly(true);QVERIFY2(catalog.open(project,&detail),qPrintable(detail));
    RemotePredictionRequest request;request.kind="wells";request.facies=FaciesCatalog::defaults();
    int withCurves=0,withTracks=0,timeDepthOnly=0,locatedCurves=0;
    for(const auto &well:wells){
      const auto present=availability.value(well.id);withCurves+=present.curves;withTracks+=present.tracks;timeDepthOnly+=present.timeDepth&&!present.tracks;
      if(!present.curves)continue;
      const auto ids=catalog.wellsMatchingName(QString::fromStdString(well.name));QCOMPARE(ids.size(),1);
      const auto entity=catalog.entityById(ids.first());locatedCurves+=entity.hasSurface&&std::isfinite(entity.surfaceX)&&std::isfinite(entity.surfaceY);
      request.wells.append(QVariantMap{{"id",ids.first()},{"name",QString::fromStdString(well.name)}});
    }
    QVERIFY(withCurves>8);QCOMPARE(request.wells.size(),withCurves);
    QJsonObject inventory{{"totalWells",int(wells.size())},{"withCurves",withCurves},{"withTracks",withTracks},{"timeDepthOnly",timeDepthOnly},{"locatedCurves",locatedCurves}};
    paleo::data::StratigraphicScheme scheme;QVERIFY(ds.readStratigraphicScheme("default",{},scheme,error));
    QHash<qlonglong,QVector<wellsection::LithoSegment>> lithologies;QJsonArray scopes;
    for(const auto *token:{"T30-T32-S1","T70-T72-S1"}){
      paleo::strat::UnitSelection unit;
      for(const auto &sub:scheme.subIntervals){const auto candidate=paleo::strat::makeSelection(scheme,QString::fromStdString(sub.interval.value),sub.index);
        if(paleo::strat::layerToken(candidate)==token){unit=candidate;break;}}
      QVERIFY(unit.isValid());QJsonArray factors;int thickness=0,sand=0,locatedThickness=0,locatedSand=0;
      for(const auto &well:wells){
        const auto range=paleo::strat::analysisWellRange(scheme,unit,{},well.id);
        QJsonObject row{{"well",QString::fromStdString(well.name)},{"rangeResolved",range.resolved}};
        if(!range.resolved){row.insert("reason",QString::fromStdString(range.reason));factors.append(row);continue;}
        ++thickness;row.insert("thickness",range.bottom-range.top);
        const auto ids=catalog.wellsMatchingName(QString::fromStdString(well.name));
        bool located=false;
        if(ids.size()==1){const auto entity=catalog.entityById(ids.first());located=entity.hasSurface&&std::isfinite(entity.surfaceX)&&std::isfinite(entity.surfaceY);locatedThickness+=located;}
        row.insert("hasLocation",located);
        if(!lithologies.contains(well.id)){
          WellComposite::ComprehensiveWellData data;
          if(readAreaWellChart(ds,well.id,data,&detail))for(const auto &interval:data.lithologyIntervals){
            wellsection::LithoSegment segment;segment.topMd=interval.topDepth;segment.baseMd=interval.bottomDepth;segment.litho=interval.lithoName;lithologies[well.id].append(segment);
          }
        }
        const auto value=paleo::strat::sandThicknessInRange(lithologies.value(well.id),range.top,range.bottom,nullptr);
        if(value){++sand;locatedSand+=located;row.insert("sandThickness",*value);row.insert("sandRatio",*value/(range.bottom-range.top));}
        factors.append(row);
      }
      QJsonObject scope{{"unit",token},{"thicknessWells",thickness},{"locatedThicknessWells",locatedThickness},{"sandWells",sand},{"locatedSandWells",locatedSand},{"factorInputs",factors}};
      if(qEnvironmentVariable("PALEO_ACCEPTANCE_MODEL_CHECK")=="1"){
        request.id=QString::fromLatin1(token);request.horizon=QString::fromLatin1(token);request.unitSelection=paleo::strat::toMap(unit);
        WellPredictionService service;service.bindCatalog(&catalog,project);QSignalSpy checked(&service,&WellPredictionService::readinessReady);
        service.check(request);QTRY_COMPARE_WITH_TIMEOUT(checked.size(),1,120000);
        QVERIFY2(checked.first()[2].toString().isEmpty(),qPrintable(checked.first()[2].toString()));
        const auto results=checked.first()[1].toList();QCOMPARE(results.size(),withCurves);int ready=0,locatedReady=0;
        for(const auto &result:results)if(result.toMap().value("ready").toBool()){
          ++ready;const auto entity=catalog.entityById(result.toMap().value("id").toString());
          locatedReady+=entity.hasSurface&&std::isfinite(entity.surfaceX)&&std::isfinite(entity.surfaceY);
        }
        scope.insert("predictionCandidates",withCurves);scope.insert("predictionReady",ready);scope.insert("predictionWells",QJsonArray::fromVariantList(results));
        scope.insert("locatedPredictionReady",locatedReady);
      }
      scopes.append(scope);qInfo()<<"Full project scope"<<token<<"thickness"<<thickness<<"sand"<<sand<<"prediction ready"<<scope.value("predictionReady").toInt();
    }
    inventory.insert("scopes",scopes);report.insert("fullWellScope",inventory);
    qInfo()<<"Full project wells"<<wells.size()<<"with curves"<<withCurves<<"located curves"<<locatedCurves;
  }
  void sectionVolumeLoadUsesNativeCrsAndGeometry(){
    QString error;const auto path=paleo::seismicProjectSource(project,{},&error);QVERIFY2(!path.isEmpty(),qPrintable(error));
    paleo::area_model::registerProject(project,this);
    const auto unregister=qScopeGuard([this]{paleo::area_model::unregisterProject(this);});
    PaleoTaskService tasks;seismic::SeismicTaskService service;service.setTaskService(&tasks);
    bool done=false,ok=false;std::shared_ptr<seismic::SgyVolume> volume;
    service.startVolumeLoad(path,[&](bool success,auto value,const QString &detail){ok=success;volume=value;error=detail;done=true;},true);
    QTRY_VERIFY_WITH_TIMEOUT(done,180000);QVERIFY2(ok&&volume,qPrintable(error));
    SeismicMapLink link(nullptr,nullptr);link.setActiveVolume(volume);
    QVERIFY(link.gridGeometry().valid);QCOMPARE(link.sourceCrs(),QStringLiteral("EPSG:32650"));
    const auto grid=link.gridGeometry();const auto il=volume->InlineValues()[volume->InlineValues().size()/2];
    const auto &lines=volume->XlineValues();QVERIFY(lines.size()>2);const auto middle=lines.size()/2;
    QVector<QgsPointXY> points;for(auto index:{middle,middle+1}){double x=0,y=0;grid.inlineXlineToXy(il,lines[index],&x,&y);points.append(QgsPointXY(x,y));}
    std::vector<glm::ivec2> route;
    connect(&link,&SeismicMapLink::sectionExtractRequested,&link,[&](auto,std::vector<glm::ivec2> path,QString,auto){route=std::move(path);});
    QSignalSpy extracted(&link,&SeismicMapLink::sectionExtractedFromMap);link.triggerSectionFromMapPolyline(points,"native-geometry");
    QCOMPARE(extracted.size(),1);QVERIFY2(extracted.first()[0].toBool(),qPrintable(extracted.first()[1].toString()));
    seismic::SgySectionOptions options;options.maxColumns=32;done=false;std::shared_ptr<const seismic::SgySliceImage> image;
    service.startSectionExtraction(volume,route,options,[&](bool success,auto value,const auto &,const QString &detail){ok=success;image=value;error=detail;done=true;});
    QTRY_VERIFY_WITH_TIMEOUT(done,60000);QVERIFY2(ok&&image,qPrintable(error));
    QVERIFY(std::any_of(image->values.begin(),image->values.end(),[](float v){return std::isfinite(v);}));
    report.insert("sectionVolumeLoad",QJsonObject{{"sourceCrs",link.sourceCrs()},{"geometryValid",true},{"fileBytes",double(QFileInfo(path).size())},{"traces",double(volume->Index()->traceCount)},{"columns",image->width},{"samples",image->height}});
  }
  void wellSeismicAlignmentInventory(){
    paleo::data::DataService ds;std::string error;QString detail;
    QVERIFY2(ds.openProjectReadOnly(u(paleo::area_model::directory(project)),error),error.c_str());
    std::vector<paleo::data::DataEntry> entries;QVERIFY(ds.listData("time_depth",0,entries,error));QJsonArray tables;
    for(const auto &entry:entries){
      paleo::data::TimeDepthTable table;QVERIFY(ds.readTimeDepth(entry.id,table,error));
      double lo=INFINITY,hi=-INFINITY;int negative=0;
      for(const auto &point:table.depthTimePairs)if(std::isfinite(point.timeValue)){lo=std::min(lo,point.timeValue);hi=std::max(hi,point.timeValue);negative+=point.timeValue<0;}
      tables.append(QJsonObject{{"id",double(entry.id)},{"wellId",double(entry.wellId)},{"name",QString::fromStdString(entry.name)},
        {"depthType",QString::fromStdString(table.depthInfo.depthType)},{"depthUnit",QString::fromStdString(table.depthInfo.depthUnit)},
        {"timeUnit",QString::fromStdString(table.timeUnit)},{"timeType",QString::fromStdString(table.timeType)},
        {"timeReference",QString::fromStdString(table.timeReference)},{"samples",int(table.depthTimePairs.size())},{"negativeTimes",negative},{"minTime",lo},{"maxTime",hi}});
    }
    DataCatalog catalog;catalog.setLockedReadOnly(true);QVERIFY(catalog.open(project,&detail));
    const auto readiness=paleo::analysisReadiness(&catalog,project);QJsonArray wells;ProjectDataFacade data;data.setCatalog(&catalog,project);
    for(const auto &well:catalog.entities("well"))if(readiness.value(well.id).hasCurves){
      const auto samples=data.tdTableFor(well.id);wells.append(QJsonObject{{"id",well.id},{"name",well.name},{"hasTimeDepth",readiness.value(well.id).hasTimeDepth},
        {"tdUsable",readiness.value(well.id).tdUsable},{"selectedSamples",samples.size()},{"reason",data.lastError()}});
    }
    std::vector<paleo::data::DataEntry> volumes;QVERIFY(ds.listData("volumes",0,volumes,error));QJsonArray seismic;
    for(const auto &entry:volumes){paleo::data::SeismicVolume volume;QVERIFY(ds.readVolume(entry.id,volume,error));
      seismic.append(QJsonObject{{"id",double(entry.id)},{"timeUnit",QString::fromStdString(volume.sampling.timeUnit)},{"timeType",QString::fromStdString(volume.sampling.timeType)},{"timeReference",QString::fromStdString(volume.sampling.timeReference)}});}
    WellSectionWorkflow section(&catalog);const auto choices=section.wellChoices();QCOMPARE(choices.size(),wells.size());
    SectionWorkbench setup(&catalog);const auto setupRows=setup.wells();QCOMPARE(setupRows.size(),wells.size());
    const auto snap=setup.wellSnapWells(true);QVERIFY(!snap.isEmpty());
    QStringList selected;for(const auto *name:{"HZ19-1-1A","HZ21-1-1"}){const auto ids=catalog.wellsMatchingName(name);QCOMPARE(ids.size(),1);selected.append(ids.first());}
    QVERIFY(section.availableMnemonics(selected).contains("GR"));
    QSignalSpy loaded(&section,&WellSectionWorkflow::sectionReady);section.request(selected,{"GR"});QCOMPARE(loaded.size(),1);
    const auto drawn=loaded.first()[1].value<QVector<wellsection::Well>>();QCOMPARE(drawn.size(),2);
    for(const auto &well:drawn){QVERIFY(!well.curves.isEmpty());QVERIFY(!well.curves.first().depths.isEmpty());}
    PaleoTaskService tasks;seismic::SeismicTaskService seismicTasks(&tasks);section.setSeismicTaskService(&seismicTasks);
    QSignalSpy strips(&section,&WellSectionWorkflow::seismicReady);section.requestSeismic(drawn,section.seismicSource());
    QTRY_COMPARE_WITH_TIMEOUT(strips.size(),1,30000);const auto strip=strips.first()[1].value<wellsection::SeismicStrip>();
    const bool hasUnaligned=std::any_of(drawn.cbegin(),drawn.cend(),[](const auto &well){return !well.timeDepth||well.timeDepth->timeReference.isEmpty();});
    if(hasUnaligned){QVERIFY(!strip.anyValid());QVERIFY(!strip.gaps.first().reason.isEmpty());}
    report.insert("wellSeismicAlignment",QJsonObject{{"tables",tables},{"curveWells",wells},{"volumes",seismic},
      {"wellChoices",choices.size()},{"setupChoices",setupRows.size()},{"routeChoices",snap.size()},
      {"mdWellsDrawn",drawn.size()},{"alignedSeismic",strip.anyValid()},{"seismicReason",strip.gaps.isEmpty()?strip.status:strip.gaps.first().reason}});
    int usable=0;for(const auto &well:wells)usable+=well.toObject().value("tdUsable").toBool();qInfo()<<"Alignment inventory: curves"<<wells.size()<<"usable TD"<<usable<<"relations"<<tables.size();
  }
  void confirmPetrelAlignmentWhenExplicitlyRequested(){
    if(qEnvironmentVariable("PALEO_ACCEPTANCE_CONFIRM_PETREL_ALIGNMENT")!="1")QSKIP("Explicit source/sign/datum authorization required");
    const auto reference=qEnvironmentVariable("PALEO_ACCEPTANCE_TIME_REFERENCE").trimmed();QVERIFY(!reference.isEmpty());
    ProjectDirLock lock(project);QString detail;QVERIFY2(lock.tryLock(&detail),qPrintable(detail));
    paleo::data::DataService ds;std::string error;
    QVERIFY2(ds.openProject(u(paleo::area_model::directory(project)),error),error.c_str());
    std::vector<paleo::data::SourceIndexEntry> sources;QVERIFY(ds.listSourceIndexEntries(sources,error));
    QSet<qlonglong> authorized;
    for(const auto &source:sources){const auto name=QFileInfo(QString::fromStdString(source.path)).fileName();
      if(name=="Merge_TDR"||name=="HZ19-6_(XJ30_2nd_PSDM-t)_DT_ALL")authorized.insert(source.id);}
    QCOMPARE(authorized.size(),2);
    std::vector<paleo::data::DataEntry> entries,volumes;QVERIFY(ds.listData("time_depth",0,entries,error));QVERIFY(ds.listData("volumes",0,volumes,error));
    QCOMPARE(volumes.size(),size_t(1));paleo::data::SeismicVolume volume;QVERIFY(ds.readVolume(volumes.front().id,volume,error));
    QCOMPARE(volume.geometry.coordinateSystem,std::string("EPSG:32650"));QCOMPARE(volume.sampling.timeUnit,std::string("ms"));
    QVERIFY(volume.sampling.timeType.empty()||volume.sampling.timeType=="TWT");
    QHash<qlonglong,std::vector<paleo::data::TimeDepthPoint>> original;std::vector<long long> ids;
    for(const auto &entry:entries)if(authorized.contains(entry.sourceId)){
      paleo::data::TimeDepthTable table;QVERIFY(ds.readTimeDepth(entry.id,table,error));
      QCOMPARE(table.depthInfo.depthUnit,std::string("m"));QCOMPARE(table.timeType,std::string("TWT"));
      QVERIFY(table.timeUnit.empty()||table.timeUnit=="ms");
      if(table.timeSign=="negated")for(auto &point:table.depthTimePairs)point.timeValue=-point.timeValue;
      original.insert(entry.id,table.depthTimePairs);ids.push_back(entry.id);
    }
    QCOMPARE(ids.size(),size_t(119));
    const auto backup=project+"/artifacts/backups/time-depth-before-"+QDateTime::currentDateTimeUtc().toString("yyyyMMdd-HHmmsszzz")+".sqlite";
    QVERIFY(QDir().mkpath(QFileInfo(backup).absolutePath()));QVERIFY2(ds.snapshotDatabase(u(backup),error),error.c_str());
    QVERIFY2(ds.confirmTimeDepthAlignment(ids,"ms","TWT",u(reference),true,volumes.front().id,error),error.c_str());
    for(const auto &entry:entries)if(original.contains(entry.id)){
      paleo::data::TimeDepthTable after;QVERIFY(ds.readTimeDepth(entry.id,after,error));const auto raw=original.value(entry.id);
      QCOMPARE(after.depthTimePairs.size(),raw.size());QCOMPARE(after.timeReference,u(reference));QCOMPARE(after.timeSign,std::string("negated"));
      for(size_t i=0;i<raw.size();++i){QCOMPARE(after.depthTimePairs[i].depthValue,raw[i].depthValue);QCOMPARE(after.depthTimePairs[i].timeValue,-raw[i].timeValue);}
    }
    paleo::data::SeismicVolume after;QVERIFY(ds.readVolume(volumes.front().id,after,error));QCOMPARE(after.sampling.timeReference,u(reference));
    QCOMPARE(after.sampling.firstTime,volume.sampling.firstTime);QCOMPARE(after.sampling.sampleInterval,volume.sampling.sampleInterval);QCOMPARE(after.sampling.sampleCount,volume.sampling.sampleCount);
    report.insert("confirmedPetrelAlignment",QJsonObject{{"relations",int(ids.size())},{"sources",authorized.size()},{"reference",reference},{"backup",backup}});
  }
  void selectedWellsUseRealCheckshotsAndSeismic(){
    if(qEnvironmentVariable("PALEO_ACCEPTANCE_VERIFY_PETREL_ALIGNMENT")!="1")QSKIP("Explicit aligned-project acceptance only");
    paleo::area_model::registerProject(project,this);
    const auto unregister=qScopeGuard([this]{paleo::area_model::unregisterProject(this);});
    DataCatalog catalog;catalog.setLockedReadOnly(true);QString error;QVERIFY2(catalog.open(project,&error),qPrintable(error));
    ProjectDataFacade data;data.setCatalog(&catalog,project);SectionWorkbench workbench(&catalog);QStringList ids;QJsonArray details;
    for(const auto *name:{"HZ21-7-1","HZ21-8-1"}){
      const auto matches=catalog.wellsMatchingName(name);QCOMPARE(matches.size(),1);ids<<matches.first();
      const auto rows=data.tdTableFor(ids.last());QVERIFY2(!rows.isEmpty(),qPrintable(data.lastError()));
      seismic::TimeDepthModel model;double shift=0;QString status,reference;
      QVERIFY2(workbench.mdTimeDepth(ids.last(),&model,&shift,&status,&reference),qPrintable(status));
      int checked=0;for(const auto &point:rows)if(point.timeMs>=0){QVERIFY(std::abs(model.DepthToTwtMs(point.md)-point.timeMs)<1e-7);++checked;}
      QVERIFY(checked>100);details.append(QJsonObject{{"name",name},{"samples",rows.size()},{"checkshotsMatched",checked},{"reference",reference}});
    }
    WellSectionWorkflow section(&catalog);section.setSectionWorkbench(&workbench);QSignalSpy ready(&section,&WellSectionWorkflow::sectionReady);
    section.request(ids,{"GR"});QCOMPARE(ready.size(),1);const auto wells=ready.first()[1].value<QVector<wellsection::Well>>();QCOMPARE(wells.size(),2);
    for(const auto &well:wells){QVERIFY(well.timeDepth);QVERIFY(!well.curves.isEmpty());}
    PaleoTaskService tasks;seismic::SeismicTaskService seismicTasks(&tasks);section.setSeismicTaskService(&seismicTasks);
    QSignalSpy extracted(&section,&WellSectionWorkflow::seismicReady);section.requestSeismic(wells,section.seismicSource());
    QTRY_COMPARE_WITH_TIMEOUT(extracted.size(),1,60000);const auto strip=extracted.first()[1].value<wellsection::SeismicStrip>();
    QVERIFY2(strip.anyValid(),qPrintable(strip.gaps.isEmpty()?strip.status:strip.gaps.first().reason));
    const auto overlays=workbench.sectionWells();int visible=0,curvePoints=0;
    for(const auto &well:overlays)if(ids.contains(well.wellId)){
      QVERIFY(well.calibrated);for(const auto &curve:well.curves)for(double time:curve.twtMs)if(std::isfinite(time))++curvePoints;++visible;
    }
    QCOMPARE(visible,2);QVERIFY(curvePoints>100);
    report.insert("selectedWellAlignment",QJsonObject{{"wells",details},{"seismicValid",true},{"overlayWells",visible},{"curvePoints",curvePoints}});
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
