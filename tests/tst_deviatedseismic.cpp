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
#include "workflow/sectionworkbench.h"
#include "ui/seismicsection/seismicsectioncanvas.h"
#include <paleo_data/service.h>
#include <cmath>

struct DeviatedFixture {
  QTemporaryDir temp;QString project,error;std::string detail;
  paleo::data::DataService ds;DataCatalog catalog;long long wellId=0;
  bool init(bool positions=true,bool survey=false){
    project=temp.filePath("project");
    if(!ds.createProject(paleo::area_model::directory(project).toStdString(),"deviated",detail)||
       !ds.setProjectCrs("EPSG:32650","m",detail))return false;
    const auto path=temp.filePath("checkshots.txt");QFile file(path);
    if(!file.open(QIODevice::WriteOnly)||file.write("fixture")!=7)return false;file.close();
    paleo::data::ParsedFile parsed;parsed.kind="test";parsed.status="parsed";
    paleo::data::WellDataset well;well.wellInfo.wellName="D1";well.wellInfo.wellheadX=300000;well.wellInfo.wellheadY=2300000;
    well.defaultDepthInfo.depthType="MD";well.defaultDepthInfo.depthUnit="m";well.defaultDepthInfo.mdReference="KB";
    paleo::data::LogSet logs;logs.logSetName="logs";logs.depthInfo=well.defaultDepthInfo;
    logs.samplingAxis.depthValues={0,100,200,300};logs.logCurves.push_back({"GR","API",{20,40,80,120},{}});well.wellbore.logSets.push_back(logs);
    paleo::data::TimeDepthTable table;table.relationName="TDR";table.depthInfo=well.defaultDepthInfo;
    table.timeType="TWT";table.timeUnit="ms";table.timeReference="shared zero";table.timeSign="negated";table.coordinateUnit="m";
    for(int i=0;i<4;++i)table.depthTimePairs.push_back({double(i*100),double(-i*100),positions?300000.+i*i*10:qQNaN(),positions?2300000.:qQNaN(),25.-i*90});
    well.timeDepthRelations.push_back(table);
    if(survey){paleo::data::TrajectorySurvey track;track.surveyName="real survey";track.azimuthReference="grid north";
      track.surveyPoints={{0,0,90},{300,30,90}};well.wellbore.trajectories.push_back(track);}
    parsed.wells.push_back(well);long long source=0;
    if(!ds.importPreparedFile(path.toStdString(),path.toStdString(),path.toStdString(),
      QCryptographicHash::hash("fixture",QCryptographicHash::Sha256).toHex().toStdString(),parsed,source,detail))return false;
    std::vector<paleo::data::DataEntry> relations;
    if(!ds.listData("time_depth",0,relations,detail)||relations.size()!=1||
       !ds.confirmTimeDepthSemantics(relations[0].id,"ms","TWT","shared zero",true,detail))return false;
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
  void nativePositionsSurviveFacadeAndWorkbench(){
    DeviatedFixture f;QVERIFY2(f.init(),qPrintable(f.error+QString::fromStdString(f.detail)));
    ProjectDataFacade data;data.setCatalog(&f.catalog,f.project);const auto rows=data.tdTableFor("well-D1");
    QCOMPARE(rows.size(),4);QCOMPARE(rows[2].x,300040.);QCOMPARE(rows[2].y,2300000.);
    QCOMPARE(rows[2].timeMs,200.);QCOMPARE(rows[2].z,-155.);QVERIFY(std::isnan(rows[2].tvd));
    QCOMPARE(rows[2].positionCrs,QString("EPSG:32650"));QVERIFY(rows[2].timeDepthId>0);
    SectionWorkbench bench(&f.catalog);const auto wells=bench.sectionWells();QCOMPARE(wells.size(),size_t(1));
    const auto &well=wells[0];QVERIFY(well.requiresSpatialPath);QCOMPARE(well.trajectory.size(),size_t(4));
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
    SectionWorkbench bench(&catalog);const auto wells=bench.sectionWells();int deviated=0,positioned=0;bool anchor=false;
    for(const auto &w:wells){
      if(w.trajectory.size()<2)continue;++positioned;double dx=0,dy=0;const auto &first=w.trajectory.front();
      for(const auto &p:w.trajectory){dx=std::max(dx,std::abs(p.x-first.x));dy=std::max(dy,std::abs(p.y-first.y));}
      if(std::hypot(dx,dy)>.5)++deviated;
      if(w.wellName=="HZ19-1-1A"){anchor=true;QVERIFY(dy>100);QCOMPARE(w.trajectory.size(),size_t(184));
        const auto &last=w.trajectory.back();QCOMPARE(last.x,303492.91);QVERIFY(std::isfinite(last.twtMs));}
    }
    QVERIFY(anchor);QVERIFY(deviated>0);qInfo()<<"native candidate wells"<<wells.size()<<"positioned"<<positioned<<"with horizontal displacement"<<deviated;
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
