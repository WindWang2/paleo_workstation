// 层：测试壳
#include <QtTest>
#include <QTemporaryDir>
#include <QCryptographicHash>
#include <qgsapplication.h>
#include <qgscoordinatereferencesystem.h>
#include "catalog/datacatalog.h"
#include "io/areamodel.h"
#include "services/projectdata.h"
#include "services/sectionwellgeometry.h"
#include "services/wellpathpositions.h"
#include "services/seismicdataaccess.h"
#include "services/seismicdataprojection.h"
#include "workflow/sectionworkbench.h"
#include "ui/seismicsection/seismicsectioncanvas.h"
#include "ui/paleotheme.h"
#include <paleo_data/service.h>
#include <cmath>

struct DeviatedFixture {
  QTemporaryDir temp;QString project,error;std::string detail;
  paleo::data::DataService ds;DataCatalog catalog;long long wellId=0;
  bool init(bool positions=true,bool survey=false,bool hasCurves=true,bool hasTimeDepth=true,bool moved=true,bool plateau=false){
    project=temp.filePath("project");
    if(!ds.createProject(paleo::area_model::directory(project).toStdString(),"deviated",detail)||
       !ds.setProjectCrs("EPSG:32650","m",detail))return false;
    const auto path=temp.filePath("checkshots.txt");QFile file(path);
    if(!file.open(QIODevice::WriteOnly)||file.write("fixture")!=7)return false;file.close();
    paleo::data::ParsedFile parsed;parsed.kind="test";parsed.status="parsed";
    paleo::data::WellDataset well;well.wellInfo.wellName="D1";well.wellInfo.wellheadX=300000;well.wellInfo.wellheadY=2300000;
    well.defaultDepthInfo.depthType="MD";well.defaultDepthInfo.depthUnit="m";well.defaultDepthInfo.mdReference="KB";
    paleo::data::LogSet logs;logs.logSetName="logs";logs.depthInfo=well.defaultDepthInfo;
    logs.samplingAxis.depthValues={0,100,200,300};logs.logCurves.push_back({"GR","API",{20,40,80,120},{}});if(hasCurves)well.wellbore.logSets.push_back(logs);
    paleo::data::TimeDepthTable table;table.relationName="TDR";table.depthInfo=well.defaultDepthInfo;
    table.timeType="TWT";table.timeUnit="ms";table.timeReference="shared zero";table.timeSign="negated";table.coordinateUnit="m";
    for(int i=0;i<4;++i)table.depthTimePairs.push_back({double(i*100),double(plateau&&i==1?0:-i*100),positions?300000.+(moved?i*i*10:0):qQNaN(),positions?2300000.:qQNaN(),25.-i*90});
    if(hasTimeDepth)well.timeDepthRelations.push_back(table);
    if(survey){paleo::data::TrajectorySurvey track;track.surveyName="real survey";track.azimuthReference="grid north";
      track.surveyPoints={{0,0,90},{300,30,90}};well.wellbore.trajectories.push_back(track);}
    parsed.wells.push_back(well);long long source=0;
    if(!ds.importPreparedFile(path.toStdString(),path.toStdString(),path.toStdString(),
      QCryptographicHash::hash("fixture",QCryptographicHash::Sha256).toHex().toStdString(),parsed,source,detail))return false;
    std::vector<paleo::data::DataEntry> relations;
    if(hasTimeDepth&&(!ds.listData("time_depth",0,relations,detail)||relations.size()!=1||
       !ds.confirmTimeDepthSemantics(relations[0].id,"ms","TWT","shared zero",true,detail)))return false;
    std::vector<paleo::data::WellSummary> wells;if(!ds.listWells(wells,detail)||wells.size()!=1)return false;wellId=wells[0].id;
    if(!catalog.open(project,&error))return false;
    CatalogEntity entity;entity.id="well-D1";entity.name="D1";entity.entityType="well";entity.hasSurface=true;
    entity.surfaceX=300000;entity.surfaceY=2300000;entity.coordinateStatus="ok";entity.td=300;
    entity.extra={{"areaModelWellId",wellId},{"wellCoordinatesCrs","EPSG:32650"},{"wellCoordinateUnit","m"}};
    return catalog.addEntity(entity,&error);
  }
};

class DeviatedSeismicTest:public QObject {
  Q_OBJECT
private slots:
  void initTestCase(){QVERIFY(PaleoTheme::ensureApplicationFonts());qApp->setFont(PaleoTheme::bodyFont());}
  void repeatedTimeKeepsObservedGeometryButDoesNotInventInverseCalibration(){
    DeviatedFixture f;QVERIFY(f.init(true,false,true,true,true,true));SectionWorkbench bench(&f.catalog);
    const auto wells=bench.sectionWells();QCOMPARE(wells.size(),size_t(1));const auto &well=wells[0];
    QCOMPARE(well.trajectory.size(),size_t(4));QCOMPARE(well.trajectory[0].twtMs,0.);QCOMPARE(well.trajectory[1].twtMs,0.);
    QCOMPARE(well.trajectory[1].md,100.);QCOMPARE(well.trajectory[1].x,300010.);
    QVERIFY(!well.mdTimeDepth);QVERIFY(well.curves.empty());QVERIFY(well.tops.empty());
    QVector<TdSample> reversed{{0,0,0,{}},{100,100,100,{}},{90,200,200,{}}};
    QVERIFY(!observedMdTimeIsMonotone(reversed));
  }
  void timeDepthOnlyWellStillProvidesItsRealBore(){
    DeviatedFixture f;QVERIFY(f.init(true,false,false));SectionWorkbench bench(&f.catalog);
    QVERIFY(bench.wells().empty()); // Analysis picker still requires curves.
    const auto candidates=bench.sectionWells();QCOMPARE(candidates.size(),size_t(1));
    QVERIFY(candidates[0].curves.empty());QCOMPARE(candidates[0].trajectory.size(),size_t(4));
    const auto projected=seismic::SectionWellProjector::ProjectWells({{300000,2300000},{300100,2300000}},{},{0,25,50,75,100},candidates,500,{}, {"well-D1"});
    QCOMPARE(projected.size(),size_t(1));QVERIFY(projected[0].selectedForSection);QVERIFY(projected[0].isWithinBuffer);
    QCOMPARE(projected[0].trajectory[2].tracePosition,1.6);QCOMPARE(projected[0].trajectory[2].twtMs,200.);
  }
  void explicitPinsKeepMissingTimeOrGeometryWithoutInventingBores(){
    DeviatedFixture missing;QVERIFY(missing.init(true,false,true,false));SectionWorkbench bench(&missing.catalog);
    auto candidates=bench.sectionWells();QCOMPARE(candidates.size(),size_t(1));QVERIFY(candidates[0].trajectory.empty());
    QVERIFY(candidates[0].requiresSpatialPath);QVERIFY(!candidates[0].alignmentStatus.isEmpty());
    DeviatedFixture flat;QVERIFY(flat.init(true,false,true,true,false));SectionWorkbench flatBench(&flat.catalog);
    auto second=flatBench.sectionWells();QCOMPARE(second.size(),size_t(1));QVERIFY(second[0].trajectory.empty());
    second[0].wellId="flat";second[0].surfaceX+=100;candidates.push_back(second[0]);
    seismic::SectionWellInfo unselected=candidates.front();unselected.wellId="unselected";candidates.push_back(unselected);
    std::vector<double> axis;for(int i=0;i<3000;++i)axis.push_back(100.*i/2999);
    const auto selected=seismic::SectionWellProjector::ProjectWells({{300000,2300000},{300100,2300000}},{},axis,candidates,500,{}, {"well-D1","flat"});
    QCOMPARE(selected.size(),size_t(2));for(const auto &well:selected){QVERIFY(well.selectedForSection);QVERIFY(!well.isWithinBuffer);QVERIFY(well.trajectory.empty());}
    seismic::SeismicSectionCanvas canvas;canvas.resize(900,650);
    seismic::SgySliceImage image;image.width=3000;image.height=301;image.values.assign(903000,0);image.rgba.assign(3612000,255);
    image.valueMin=-1;image.valueMax=1;canvas.setSectionData(image,1);canvas.setWells({});const auto background=canvas.grabCanvasImage();
    canvas.setWells(selected);canvas.setMaxVisibleWells(1);
    const auto markers=canvas.grabCanvasImage();QVERIFY(markers!=background);
    // Selected pins change the header, but no bore/curve is fabricated inside the seismic viewport.
    const QRect vp(QPoint(qRound(canvas.traceToPixelX(0)),qRound(canvas.timeToPixelY(0))),
                   QPoint(qRound(canvas.traceToPixelX(2999)),qRound(canvas.timeToPixelY(300))));
    QVERIFY(markers.copy(vp)==background.copy(vp));
    for(const auto &well:selected){
      const int x=qRound(canvas.traceToPixelX(well.tracePosition));const QRect pin(x-6,vp.top()-10,13,10);
      QVERIFY(markers.copy(pin)!=background.copy(pin));
    }
  }
  void manualProjectCrsSurvivesConflictingSourceDeclarations(){
    DeviatedFixture f;QVERIFY(f.init());
    const auto path=f.temp.filePath("other-crs.txt");QFile file(path);QVERIFY(file.open(QIODevice::WriteOnly));file.write("other");file.close();
    paleo::data::ParsedFile parsed;parsed.kind="test";parsed.status="parsed";
    parsed.coordinateSystems={{"EPSG:32649","m"},{"EPSG:32650","m"}};long long source=0;
    QVERIFY(f.ds.importPreparedFile(path.toStdString(),path.toStdString(),path.toStdString(),
      QCryptographicHash::hash("other",QCryptographicHash::Sha256).toHex().toStdString(),parsed,source,f.detail));
    paleo::data::ProjectCrsInfo crs;QVERIFY(f.ds.readProjectCrsInfo(crs,f.detail));QVERIFY(crs.hasConflict);QCOMPARE(crs.selectionBasis,std::string("manual"));
    QCOMPARE(paleo::area_model::confirmedProjectCrs(f.project),QString("EPSG:32650"));
    ProjectDataFacade data;data.setCatalog(&f.catalog,f.project);const auto rows=data.tdTableFor("well-D1");
    QCOMPARE(rows.size(),4);QCOMPARE(rows[2].x,300040.);QVERIFY(rows[2].positionError.isEmpty());
  }
  void nativePositionsSurviveFacadeAndWorkbench(){
    DeviatedFixture f;QVERIFY2(f.init(),qPrintable(f.error+QString::fromStdString(f.detail)));
    ProjectDataFacade data;data.setCatalog(&f.catalog,f.project);const auto rows=data.tdTableFor("well-D1");
    QCOMPARE(rows.size(),4);QCOMPARE(rows[2].x,300040.);QCOMPARE(rows[2].y,2300000.);
    QCOMPARE(rows[2].timeMs,200.);QCOMPARE(rows[2].z,-155.);QVERIFY(std::isnan(rows[2].tvd));
    QCOMPARE(rows[2].positionCrs,QString("EPSG:32650"));QVERIFY(rows[2].timeDepthId>0);
    SectionWorkbench bench(&f.catalog);const auto wells=bench.sectionWells();QCOMPARE(wells.size(),size_t(1));
    const auto &well=wells[0];QVERIFY(well.requiresSpatialPath);QCOMPARE(well.trajectory.size(),size_t(4));
    QVERIFY(requiresSpatialWellSampling(well));
    QCOMPARE(well.trajectory[2].x,300040.);QCOMPARE(well.trajectory[2].twtMs,200.);
    QCOMPARE(well.bottomX,300090.);QVERIFY(well.trajectorySource.startsWith("time_depth_xyz:"));
    QVERIFY(!well.curves.empty());QCOMPARE(well.curves[0].twtMs[2],200.);
    // Source payload remains signed; reads must not progressively negate it.
    std::vector<paleo::data::DataEntry> entries;QVERIFY(f.ds.listData("time_depth",f.wellId,entries,f.detail));
    paleo::data::TimeDepthTable again;QVERIFY(f.ds.readTimeDepth(entries[0].id,again,f.detail));
    QCOMPARE(again.depthTimePairs[2].timeValue,200.);QCOMPARE(again.depthTimePairs[2].x,300040.);
  }
  void typedSurveyFallbackAndMissingGeometry(){
    DeviatedFixture f;QVERIFY(f.init(false,true));ProjectDataFacade data;data.setCatalog(&f.catalog,f.project);
    QString error;const auto survey=data.trajectoryFor("well-D1",&error);QVERIFY2(survey,qPrintable(error));
    SectionWorkbench bench(&f.catalog);const auto wells=bench.sectionWells();QCOMPARE(wells.size(),size_t(1));
    QVERIFY(wells[0].trajectory.size()>2);QVERIFY(wells[0].bottomX>300050);
    QCOMPARE(wells[0].trajectorySource,QString("survey_minimum_curvature"));
    DeviatedFixture empty;QVERIFY(empty.init(false));SectionWorkbench without(&empty.catalog);
    const auto unknown=without.sectionWells();QCOMPARE(unknown.size(),size_t(1));QVERIFY(unknown[0].trajectory.empty());
    QVERIFY(requiresSpatialWellSampling(unknown[0]));
    const auto projected=seismic::SectionWellProjector::ProjectWells({{300000,2300000},{300100,2300000}},{},{0,100},unknown,500);
    QVERIFY(!projected[0].isWithinBuffer);QVERIFY(unknown[0].trajectoryStatus.contains(QStringLiteral("未叠加")));
    QVector<TdSample> constant(2);constant[0].md=0;constant[1].md=100;
    for(auto &p:constant){p.x=300000;p.y=2300000;p.timeMs=p.md;}
    auto noSurvey=unknown[0];attachSectionWellGeometry(noSurvey,constant,std::nullopt,nullptr,nullptr,0);
    QVERIFY(noSurvey.trajectory.empty());QVERIFY(noSurvey.trajectoryStatus.contains(QStringLiteral("恒定")));
  }
  void coordinatesUnitsAndCrsAreNormalized(){
    paleo::data::TimeDepthTable table;table.coordinateSystem="EPSG:32650";table.coordinateUnit="ft";
    table.depthTimePairs={{0,0,1000,2000,-10},{100,100,1100,2000,-110}};
    QVector<TdSample> rows(2);attachTimeDepthPositions(table,"EPSG:32650",rows);
    QVERIFY(std::abs(rows[0].x-304.8)<1e-8);QVERIFY(std::abs(rows[1].x-335.28)<1e-8);
    QCOMPARE(rows[0].z,-10.);QVERIFY(std::isnan(rows[0].tvd));
    table.coordinateUnit="m";table.coordinateSystem="EPSG:32649";table.depthTimePairs[0].x=500000;table.depthTimePairs[0].y=2300000;
    rows=QVector<TdSample>(2);attachTimeDepthPositions(table,"EPSG:32650",rows);
    QVERIFY(std::isfinite(rows[0].x));QVERIFY(std::abs(rows[0].x-500000)>100000);
    table.coordinateSystem="invalid";rows=QVector<TdSample>(2);attachTimeDepthPositions(table,"EPSG:32650",rows);
    QVERIFY(std::isnan(rows[0].x));QVERIFY(!rows[0].positionError.isEmpty());
  }
  void obliquePathIntersectionUsesActualTraceNotWellhead(){
    seismic::SectionWellInfo w;w.wellId="D";w.requiresSpatialPath=true;w.calibrated=true;
    w.surfaceX=0;w.surfaceY=0;w.trajectory={{0,0,0,0,0},{100,90,100,0,100}};
    seismic::TimeDepthModel md;QVERIFY(md.setCheckshots({{0,0},{100,100}}));w.mdTimeDepth=md;
    auto projected=seismic::SectionWellProjector::ProjectWells({{0,0},{100,0}},{},{0,25,50,75,100},{w},1);
    QCOMPARE(projected[0].trajectory[1].tracePosition,4.);
    const auto midpoint=seismic::wellPathAtMd(projected[0],50);QCOMPARE(midpoint.x,50.);QCOMPARE(midpoint.tracePosition,2.);
    QVERIFY(std::isnan(seismic::wellPathAtMd(projected[0],101).tracePosition));
    seismic::SectionHorizons horizons;seismic::SectionHorizonCurve curve;curve.name="T70";curve.timesMs={30,42.5,55,67.5,80};horizons.curves.push_back(curve);
    seismic::projectSectionHorizonTies(projected,horizons);QCOMPARE(projected[0].tops.size(),size_t(1));
    const auto &top=projected[0].tops[0];QVERIFY(std::abs(top.md-60)<1e-8);QVERIFY(std::abs(top.tracePosition-2.4)<1e-8);
    QVERIFY(std::abs(top.twtMs-60)<1e-8); // head-only sampling incorrectly gives MD=30
    w.surfaceY=500;w.trajectory[0].y=500;w.trajectory[1].y=0;
    projected=seismic::SectionWellProjector::ProjectWells({{0,0},{100,0}},{},{0,25,50,75,100},{w},1);
    QVERIFY(projected[0].isWithinBuffer); // the bore enters the section even though the head is distant
    projected[0].trajectory[1].x=qQNaN();projected[0].trajectory[1].tracePosition=qQNaN();
    seismic::projectSectionHorizonTies(projected,horizons);QVERIFY(projected[0].tops.empty());
  }
  void boreAndCurvePixelsFollowTheDeviatedPathAndBreakAtGaps(){
    seismic::SeismicSectionCanvas canvas;canvas.resize(900,650);
    seismic::SgySliceImage image;image.width=5;image.height=201;image.values.assign(1005,0);image.rgba.assign(4020,255);
    image.valueMin=-1;image.valueMax=1;canvas.setSectionData(image,1);canvas.setShowWellCurves(false);canvas.setShowFormationTops(false);
    const auto background=canvas.grabCanvasImage();
    seismic::SectionWellInfo w;w.wellId="D";w.calibrated=true;w.requiresSpatialPath=true;
    w.trajectory={{0,0,0,0,0},{100,90,50,0,100},{200,180,100,0,200}};
    seismic::TimeDepthModel md;QVERIFY(md.setCheckshots({{0,0},{200,200}}));w.mdTimeDepth=md;
    auto projected=seismic::SectionWellProjector::ProjectWells({{0,0},{100,0}},{},{0,25,50,75,100},{w},1);
    seismic::WellCurveItem gr;gr.curveName="GR";gr.depthsM={80,100,120};gr.twtMs={80,100,120};gr.values={20,150,20};
    projected[0].curves.push_back(gr);canvas.setWells(projected);const auto bore=canvas.grabCanvasImage();
    const int x=qRound(canvas.traceToPixelX(2)),y=qRound(canvas.timeToPixelY(100));
    const auto changed=[&](const QImage& a,const QImage& b,int px,int py){int n=0;
      for(int dy=-2;dy<=2;++dy)for(int dx=-2;dx<=2;++dx)if(a.rect().contains(px+dx,py+dy))n+=a.pixel(px+dx,py+dy)!=b.pixel(px+dx,py+dy);return n;};
    QVERIFY(changed(bore,background,x,y)>0);
    QVERIFY(changed(bore,background,qRound(canvas.traceToPixelX(.3)),y)==0); // no false vertical bore at the head
    canvas.setShowWellCurves(true);const auto curve=canvas.grabCanvasImage();QVERIFY(changed(curve,bore,x+40,y)>0);
    projected[0].trajectory[1].tracePosition=qQNaN();canvas.setWells(projected);canvas.setShowWellCurves(false);
    const auto gap=canvas.grabCanvasImage();QVERIFY(changed(gap,background,x,y)==0); // never bridge a missing trajectory point
  }
  void realProjectContainsDeviatedWellPaths(){
    const auto project=qEnvironmentVariable("PALEO_DEVIATED_REAL_PROJECT");if(project.isEmpty())QSKIP("Optional external project");
    DataCatalog catalog;catalog.setLockedReadOnly(true);QString error;QVERIFY2(catalog.open(project,&error),qPrintable(error));
    paleo::data::DataService source;std::string detail;QVERIFY(source.openProjectReadOnly(paleo::area_model::directory(project).toStdString(),detail));
    SectionWorkbench bench(&catalog);const auto wells=bench.sectionWells();int deviated=0,positioned=0;bool anchor=false;
    for(const auto &w:wells){
      if(w.trajectory.size()<2)continue;++positioned;double dx=0,dy=0;const auto &first=w.trajectory.front();
      for(const auto &p:w.trajectory){dx=std::max(dx,std::abs(p.x-first.x));dy=std::max(dy,std::abs(p.y-first.y));}
      if(std::hypot(dx,dy)>.5)++deviated;
      if(!anchor&&std::hypot(dx,dy)>.5&&w.trajectorySource.startsWith("time_depth_xyz:")){
        paleo::data::TimeDepthTable original;const auto id=w.trajectorySource.section(':',1).toLongLong();
        QVERIFY2(source.readTimeDepth(id,original,detail),detail.c_str());
        QCOMPARE(w.trajectory.size(),original.depthTimePairs.size());
        const double time=(original.timeUnit=="s"?1000.:1.)*(original.timeType=="OWT"?2.:1.);
        for(size_t i=0;i<w.trajectory.size();++i){
          const auto &p=w.trajectory[i];const auto &raw=original.depthTimePairs[i];
          if(std::isfinite(raw.timeValue))QVERIFY(std::abs(p.twtMs-(raw.timeValue*time+w.timeShiftMs))<1e-8);
          if(original.coordinateUnit=="m"&&(original.coordinateSystem.empty()||original.coordinateSystem==source.projectCrs())){
            if(std::isfinite(raw.x))QCOMPARE(p.x,raw.x);if(std::isfinite(raw.y))QCOMPARE(p.y,raw.y);
          }
        }
        anchor=true;
      }
    }
    QVERIFY(anchor);QVERIFY(deviated>0);qInfo()<<"native candidate wells"<<wells.size()<<"positioned"<<positioned<<"with horizontal displacement"<<deviated;
  }
  void realSelectedRouteKeepsEveryPin(){
    const auto project=qEnvironmentVariable("PALEO_DEVIATED_REAL_PROJECT");
    const auto names=qEnvironmentVariable("PALEO_SECTION_SELECTED_NAMES").split(',',Qt::SkipEmptyParts);
    const auto seismicPath=qEnvironmentVariable("PALEO_SECTION_SELECTED_SEGY");
    if(project.isEmpty()||names.size()<2||seismicPath.isEmpty())QSKIP("Optional user-supplied project, wells and seismic file");
    DataCatalog catalog;catalog.setLockedReadOnly(true);QString error;QVERIFY2(catalog.open(project,&error),qPrintable(error));
    SectionWorkbench bench(&catalog);const auto candidates=bench.sectionWells();
    QStringList ids;std::vector<glm::dvec2> route;
    for(const auto &name:names){
      const auto well=std::find_if(candidates.begin(),candidates.end(),[&](const auto &w){return w.wellName==name;});
      QVERIFY2(well!=candidates.end(),qPrintable(name));ids.append(well->wellId);route.push_back({well->surfaceX,well->surfaceY});
    }
    paleo::SeismicDataAccess source;std::string detail;
    QVERIFY2(source.openProjectSource(project,seismicPath,&error),qPrintable(error));
    paleo::data::SeismicVolume volume;QVERIFY2(source.volume(volume,detail),detail.c_str());
    std::vector<std::pair<double,double>> points;for(const auto &p:route)points.push_back({p.x,p.y});
    paleo::data::SeismicSectionResult section;QElapsedTimer clock;clock.start();
    QVERIFY2(source.xySection(points,QString::fromStdString(volume.sampling.timeReference),section,detail,[&]{return clock.elapsed()>60000;}),detail.c_str());
    auto projected=seismic::SectionWellProjector::ProjectWells(route,{},section.distances,candidates,500,{},ids);
    QCOMPARE(projected.size(),size_t(ids.size()));int bores=0,locations=0;
    for(const auto &well:projected){QVERIFY(well.selectedForSection);
      if(well.trajectory.empty())++locations;else ++bores;
      qInfo()<<well.wellName<<"path_points"<<well.trajectory.size()<<"curves"<<well.curves.size()<<"status"<<well.alignmentStatus;
    }
    seismic::SeismicSectionCanvas canvas;canvas.resize(1600,900);std::vector<float> distances;std::vector<glm::dvec2> coords;
    for(size_t i=0;i<section.distances.size();++i){distances.push_back(float(section.distances[i]));coords.push_back({section.x[i],section.y[i]});}
    const auto horizons=source.sectionHorizons(section,[&]{return clock.elapsed()>60000;});
    seismic::projectSectionHorizonTies(projected,horizons);
    canvas.setSectionData(paleo::seismicSectionImage(section),float(section.sampleInterval),section.firstTime,distances,coords);
    canvas.setHorizonOverlays(horizons);
    const auto background=canvas.grabCanvasImage();
    canvas.setWells(projected);canvas.setMaxVisibleWells(1);const auto image=canvas.grabCanvasImage();
    const int headerY=qRound(canvas.timeToPixelY(section.firstTime))-10;
    for(const auto &well:projected){
      const QRect pin(qRound(canvas.traceToPixelX(well.tracePosition))-6,headerY,13,10);
      QVERIFY2(image.copy(pin)!=background.copy(pin),qPrintable(well.wellName));
    }
    const auto output=qEnvironmentVariable("PALEO_SECTION_SELECTED_PNG");if(!output.isEmpty())QVERIFY(image.save(output));
    qInfo()<<"selected"<<ids.size()<<"real bores"<<bores<<"location-only"<<locations<<"section"<<section.columns<<"x"<<section.samples;
  }
};
int main(int argc,char **argv){
  QTemporaryDir profile;QgsApplication app(argc,argv,true,profile.path());
  app.setPrefixPath(qEnvironmentVariable("QGIS_PREFIX_PATH"),true);QgsApplication::init(profile.path());
  QString error;if(!QgsApplication::createDatabase(&error))return 1;app.initQgis();
  QgsCoordinateReferenceSystem::invalidateCache(true);DeviatedSeismicTest test;const auto result=QTest::qExec(&test,argc,argv);
  QgsApplication::exitQgis();return result;
}
#include "tst_deviatedseismic.moc"
