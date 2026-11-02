// 层：测试壳
#include <QtTest>
#include <QSettings>
#include <QTemporaryDir>
#include <QSignalSpy>
#include <QToolButton>
#include <QCryptographicHash>
#include <QGraphicsView>
#include <QStyleOptionGraphicsItem>
#include "ui/seismicsection/seismicsectioncanvas.h"
#include "ui/seismicsection/seismicsectiondockwidget.h"
#include "ui/seismicsection/sectionhorizonrender.h"
#include "ui/datapreview/previewsectionpanel.h"
#include "ui/wellsection/wellsectionpanel.h"
#include "services/paleotaskservice.h"
#include "services/seismicdataaccess.h"
#include "services/seismicdataprojection.h"
#include "services/sectiondoc.h"
#include "io/areamodel.h"
#include "io/dataimportservice.h"
#include "domain/wellsection.h"
#include "services/wellsectioncorrelation.h"
#include "ui/wellsection/wellsectionscene.h"
#include "seismichorizonfixture.h"
#include <cmath>

class SectionHorizonsTest:public QObject {
  Q_OBJECT
  QTemporaryDir settings;
  static seismic::SectionHorizons overlay(int columns=5){
    seismic::SectionHorizons result;
    result.curves.push_back({"T70","T70",0,false,std::vector<double>(columns,150.)});
    result.curves.push_back({"edge","T70–T72/S1/S2",1,true,std::vector<double>(columns,170.)});
    return result;
  }
private slots:
  void initTestCase(){
    QSettings::setDefaultFormat(QSettings::IniFormat);
    QSettings::setPath(QSettings::IniFormat,QSettings::UserScope,settings.path());
    QCoreApplication::setOrganizationName("paleo-section-horizons-test");
    PaleoTheme::applyTheme(PaleoTheme::Theme::Light);
  }
  void drawingKeepsMissingSegments(){
    QImage image(440,240,QImage::Format_ARGB32);image.fill(Qt::white);
    auto lines=overlay();lines.curves.resize(1);lines.curves[0].timesMs[2]=qQNaN();
    QPainter painter(&image);
    seismic::paintSectionHorizons(painter,image.rect(),lines,[](int col,double t){return QPointF(50+80*col,t);});
    painter.end();
    QVERIFY(image.pixelColor(90,150)!=QColor(Qt::white));
    QCOMPARE(image.pixelColor(210,150),QColor(Qt::white));
    QVERIFY(image.pixelColor(330,150)!=QColor(Qt::white));
  }
  void correlationsUseMajorHorizonsAndCheckshots(){
    wellsection::Well a,b;a.id="A";b.id="B";
    a.tops={{"old formation",100},{"S1",200}};b.tops=a.tops;
    a.correlationTops=QVector<wellsection::Top>{{"T70",300},{"T72",700}};
    b.correlationTops=QVector<wellsection::Top>{{"T70",400},{"T72",800}};
    QCOMPARE(wellsection::links(a,b).size(),2);
    b.correlationTops=QVector<wellsection::Top>{};QVERIFY(wellsection::links(a,b).isEmpty());
    b.correlationTops.reset();QVERIFY(wellsection::links(a,b).isEmpty());
    wellsection::TimeDepth td;QVERIFY(td.model.setCheckshots({{0,0},{1000,1000}}));
    a.timeDepth=td;b.timeDepth=td;b.timeDepth->shiftMs=100;
    wellsection::SeismicGap gap;gap.columns=5;gap.samples=201;gap.stepMs=5;gap.values.assign(1005,0);
    gap.horizons=overlay();gap.horizons.curves[0].timesMs={200,300,qQNaN(),500,600};
    auto links=wellsection::correlationLinks(a,b,&gap);QCOMPARE(links.size(),1);
    QCOMPARE(links[0].name,QString("T70"));QCOMPARE(links[0].leftMd,200.);QCOMPARE(links[0].rightMd,500.);
    gap.horizons.curves[0].timesMs.back()=1200;QVERIFY(wellsection::correlationLinks(a,b,&gap).isEmpty());
    gap.horizons.curves[0].timesMs.back()=600;
    wellsectionui::RenderState state;state.wells={a,b};state.window={0,1000};state.offsets={0,0};state.strip.gaps={gap};
    state.tpl=wellsection::SectionTemplate::defaults();state.theme=wellsection::SectionTheme::byId("classic");
    state.theme.zoneFill=false;state.theme.curvedLinks=false;state.gapPx=200;state.pxPerMeter=.5;
    wellsectionui::GapItem item(&state,0);QImage image(int(state.sceneWidth()),500,QImage::Format_ARGB32);image.fill(Qt::white);
    QPainter painter(&image);QStyleOptionGraphicsItem option;option.exposedRect=item.boundingRect();item.paint(&painter,&option,nullptr);painter.end();
    const int x=int(state.columnRight(0));
    QVERIFY(image.pixelColor(x+25,119)!=QColor(Qt::white)); // Actual seismic bend, not endpoint Bezier.
    QCOMPARE(image.pixelColor(x+100,175),QColor(Qt::white)); // Missing interior is not bridged.
    QVERIFY(image.pixelColor(x+175,231)!=QColor(Qt::white));
  }
  void schemeRevisionInvalidatesCorrelationStrip(){
    WellSectionPanel panel(nullptr);wellsection::Well a,b;a.id="A";b.id="B";a.totalDepth=b.totalDepth=1000;
    a.correlationRevision=b.correlationRevision="scheme:r1";panel.setSection({a,b});
    panel.setSeismicAvailable(true,{});panel.setSeismicEnabled(true);
    wellsection::SeismicStrip strip;strip.gaps.resize(1);strip.gaps[0].reason="old sampled horizons";panel.setSeismicStrip(strip);
    QSignalSpy requested(&panel,&WellSectionPanel::seismicRequested);panel.setSection({a,b});QCOMPARE(requested.size(),0);
    QCOMPARE(panel.gapReason(0),QString("old sampled horizons"));
    a.correlationRevision=b.correlationRevision="scheme:r2";panel.setSection({a,b});QCOMPARE(requested.size(),1);
    QVERIFY(panel.gapReason(0)!=QString("old sampled horizons"));
  }
  void bandsRespectTopologyGapsAndTexture(){
    seismic::SectionHorizons data;
    data.curves.push_back({"top","T70",0,false,{40,40,40,qQNaN(),40,110,40}});
    data.curves.push_back({"base","T72",0,false,{100,100,100,100,100,100,100}});
    data.bands.push_back({"interval","T70–T72","top","base",0});
    const auto render=[&](const auto& overlay){
      QImage img(360,180,QImage::Format_ARGB32);img.fill(Qt::white);
      QPainter p(&img);seismic::paintSectionHorizons(p,img.rect(),overlay,[](int c,double t){return QPointF(30+50*c,t);});
      p.end();return img;
    };
    const auto image=render(data);
    QVERIFY(image.pixelColor(60,55)!=QColor(Qt::white));
    // 20% fill preserves the base texture: every channel remains close to white.
    const auto fill=image.pixelColor(60,55);QVERIFY(fill.red()>190&&fill.green()>190&&fill.blue()>190);
    QCOMPARE(image.pixelColor(155,55),QColor(Qt::white)); // absent column, no bridging
    QCOMPARE(image.pixelColor(305,55),QColor(Qt::white)); // inverted top/base, no bridging
    QCOMPARE(image.pixelColor(60,125),QColor(Qt::white)); // outside the actual interval
    auto noTopology=data;noTopology.bands.clear();QCOMPARE(render(noTopology).pixelColor(60,55),QColor(Qt::white));
    auto reordered=data;std::reverse(reordered.curves.begin(),reordered.curves.end());const auto reorderedImage=render(reordered);
    QCOMPARE(reorderedImage.pixelColor(60,55),fill);QCOMPARE(reorderedImage.pixelColor(155,55),QColor(Qt::white));
    QVERIFY(PaleoTheme::sectionHorizonColor("T70")!=PaleoTheme::sectionHorizonColor("T72"));
    const auto color=PaleoTheme::sectionHorizonColor("T70");PaleoTheme::applyTheme(PaleoTheme::Theme::Dark);
    QCOMPARE(PaleoTheme::sectionHorizonColor("T70"),color);PaleoTheme::applyTheme(PaleoTheme::Theme::Light);
  }
  void controlsAndExportsFollowToggle(){
    seismic::SgySliceImage image;image.width=5;image.height=201;image.values.assign(1005,0.f);
    seismic::SeismicSectionCanvas canvas;canvas.resize(700,420);canvas.setSectionData(image,1,100);
    canvas.setHorizonOverlays(overlay());
    QVERIFY(!canvas.horizonOverlaysVisible());
    const auto hidden=canvas.grabCanvasImage();canvas.setHorizonOverlaysVisible(true);
    QVERIFY(canvas.grabCanvasImage()!=hidden);
    canvas.setHorizonOverlaysVisible(false);QCOMPARE(canvas.grabCanvasImage(),hidden);
    canvas.setHorizonOverlaysVisible(true);canvas.setSectionData(image,1,100);
    QVERIFY(canvas.horizonOverlays().curves.empty());QVERIFY(canvas.horizonOverlaysVisible());
    canvas.setHorizonOverlays(overlay());canvas.setTimeSliceData(image,150,100,104,200,204);
    QVERIFY(canvas.horizonOverlays().curves.empty());
    seismic::SeismicSectionDockWidget dock;
    auto *button=dock.findChild<QToolButton *>("sectionHorizonToggle");QVERIFY(button);
    QVERIFY(!button->isChecked());button->setChecked(true);QVERIFY(dock.canvas()->horizonOverlaysVisible());
    button->setChecked(false);QVERIFY(!dock.canvas()->horizonOverlaysVisible());
    dock.showMajorHorizonConnections();QVERIFY(button->isChecked());QVERIFY(dock.canvas()->majorHorizonsOnly());
    paleo::datapreview_detail::SectionPanel preview;preview.resize(700,420);
    QVector<SegyTrace> traces(5);for(auto& t:traces)t.samples.fill(0.f,201);
    preview.setTraces(traces,1000,100);preview.setHorizons(overlay());
    auto *previewButton=qobject_cast<QToolButton *>(preview.createHorizonControl(&preview));QVERIFY(previewButton);
    const auto previewHidden=preview.grab().toImage();previewButton->setChecked(true);
    QVERIFY(preview.grab().toImage()!=previewHidden);
    preview.clearImage();QVERIFY(previewButton->toolTip().contains(QStringLiteral("没有可叠加")));
    WellSectionPanel wells(nullptr);auto *wellButton=wells.findChild<QToolButton *>("sectionHorizonToggle");QVERIFY(wellButton);
    wellButton->setChecked(true);QVERIFY(wells.seismicHorizonsEnabled());
    wellButton->setChecked(false);QVERIFY(!wells.seismicHorizonsEnabled());
  }
  void realProjectPreviewWhenRequested(){
    const auto project=qEnvironmentVariable("PALEO_ACCEPTANCE_PROJECT"),output=qEnvironmentVariable("PALEO_ACCEPTANCE_SECTION_PREVIEW");
    if(project.isEmpty()||output.isEmpty())QSKIP("Optional local project preview requires explicit paths");
    paleo::data::DataService ds;std::string error;
    QVERIFY2(ds.openProjectReadOnly(paleo::area_model::directory(project).toStdString(),error),error.c_str());
    std::vector<paleo::data::DataEntry> volumes;QVERIFY(ds.listData("volumes",0,volumes,error));QVERIFY(!volumes.empty());
    paleo::data::SeismicFileInfo info;QVERIFY(ds.inspectSeismic(volumes[0].id,info,error));
    const int line=info.inlineMin+(info.inlineCount/2)*info.inlineStep;
    paleo::data::SectionReadOptions options;options.maxColumns=512;
    paleo::data::SeismicSectionResult section;QVERIFY2(ds.readSeismicLine(volumes[0].id,"inline",line,options,section,error),error.c_str());
    paleo::data::SectionHorizonQuery query;query.volumeId=volumes[0].id;query.undeclaredTimeUnit="ms";query.undeclaredTimeType="TWT";
    for(int c=0;c<section.columns;++c)query.columns.push_back({section.inlineNumbers[c],section.crosslineNumbers[c],section.traceIndexes[c]>=0});
    paleo::data::SectionHorizonResult result;QVERIFY(ds.readSectionHorizons(query,result,error));QVERIFY(!result.bands.empty());
    double first=std::numeric_limits<double>::max(),last=std::numeric_limits<double>::lowest();
    for(const auto& curve:result.curves)for(const auto time:curve.timesMs)if(std::isfinite(time)){first=std::min(first,time);last=std::max(last,time);}
    options.firstTime=std::max(info.firstTime,first-200);options.lastTime=last+200;
    QVERIFY(ds.readSeismicLine(volumes[0].id,"inline",line,options,section,error));
    const auto image=paleo::seismicSectionImage(section);
    seismic::SectionHorizons overlays;
    for(auto& c:result.curves)overlays.curves.push_back({QString::fromStdString(c.id),QString::fromStdString(c.name),c.internalEdge,c.provisional,std::move(c.timesMs)});
    for(const auto& b:result.bands)overlays.bands.push_back({QString::fromStdString(b.id),QString::fromStdString(b.name),QString::fromStdString(b.topCurveId),QString::fromStdString(b.bottomCurveId),b.subdivision});
    seismic::SeismicSectionCanvas canvas;canvas.resize(1600,900);canvas.setSectionData(image,float(section.sampleInterval),section.firstTime);
    canvas.setColorMap(seismic::SectionColorMapType::Grayscale);
    canvas.setHorizonOverlays(overlays);canvas.setHorizonOverlaysVisible(true);
    QVERIFY(canvas.grabCanvasImage().save(output));
    qInfo()<<"Real section preview"<<line<<section.columns<<"columns"<<result.bands.size()<<"bands";
  }
  void wellGapOverlayDrawsOverSeismic(){
    WellSectionPanel panel(nullptr);panel.resize(1000,600);
    wellsection::Well a,b;a.id="A";a.name="A";a.totalDepth=1200;a.tops={{"T70",100},{"T72",1100}};
    a.x=0;a.y=0;wellsection::TimeDepth td;QVERIFY(td.model.setCheckshots({{0,0},{1500,1800}}));a.timeDepth=td;
    b=a;b.id="B";b.name="B";b.x=100;b.timeDepth->shiftMs=20;
    panel.setSection({a,b});panel.setSeismicAvailable(true,{});panel.setSeismicEnabled(true);
    wellsection::SeismicStrip strip;wellsection::SeismicGap gap;gap.columns=5;gap.samples=401;gap.startMs=0;gap.stepMs=5;
    gap.values.assign(2005,0.f);gap.columnDistancesM={0,12,50,75,100};gap.horizons=overlay();
    for(auto& curve:gap.horizons.curves)for(auto& time:curve.timesMs)time+=600;
    strip.gaps.append(gap);panel.setSeismicStrip(strip);panel.show();QTest::qWait(50);
    auto *view=panel.findChild<QGraphicsView *>();QVERIFY(view);
    const auto hidden=view->viewport()->grab().toImage();panel.setSeismicHorizonsEnabled(true);
    QVERIFY(view->viewport()->grab().toImage()!=hidden);
    panel.setSeismicHorizonsEnabled(false);QCOMPARE(view->viewport()->grab().toImage(),hidden);
  }
  void gapInverseMatchesImageSampling(){
    wellsection::Well a,b;wellsection::TimeDepth ta,tb;
    QVERIFY(ta.model.setCheckshots({{0,0},{500,400},{1500,1600}}));
    QVERIFY(tb.model.setCheckshots({{0,0},{400,600},{1800,1800}}));
    ta.shiftMs=17;tb.shiftMs=-8;a.timeDepth=ta;b.timeDepth=tb;
    for(auto domain:{wellsection::DepthDomain::MD,wellsection::DepthDomain::TVD})
      for(double f:{0.,.25,.5,1.}){
        const double time=wellsection::gapTwtMs(a,50,b,100,f,700,domain);
        const auto depth=wellsection::gapDisplayDepthAtTwt(a,50,b,100,f,time,-100,1900,domain);
        QVERIFY(std::abs(depth-700)<1e-5);
      }
    QVERIFY(std::isnan(wellsection::gapDisplayDepthAtTwt(a,50,b,100,.5,9000,-100,1900,wellsection::DepthDomain::MD)));
    b.timeDepth.reset();QVERIFY(std::isnan(wellsection::gapDisplayDepthAtTwt(a,0,b,0,.5,500,0,1000,wellsection::DepthDomain::MD)));
  }
  void feedsInlineCrosslineArbitraryWellAndPreview(){
    QTemporaryDir temp;const auto project=temp.filePath("project"),path=temp.filePath("volume.sgy");QString error;
    QVERIFY(SeismicHorizonFixture::writeFile(path,SeismicHorizonFixture::buildVolume()));
    paleo::data::DataService ds;std::string detail;
    QVERIFY2(ds.createProject(paleo::area_model::directory(project).toStdString(),"synthetic",detail),detail.c_str());
    paleo::data::ImportOptions options;options.copySources=false;paleo::data::ImportReport report;
    QVERIFY2(ds.importFile(path.toStdString(),options,report,detail),detail.c_str());
    std::vector<paleo::data::DataEntry> volumes;QVERIFY(ds.listData("volumes",0,volumes,detail));QCOMPARE(volumes.size(),size_t(1));
    paleo::data::SeismicVolume volume;QVERIFY(ds.readVolume(volumes[0].id,volume,detail));
    volume.sampling.timeUnit="ms";volume.sampling.timeType="TWT";volume.sampling.timeReference="synthetic datum";
    QVERIFY(ds.saveVolume(volumes[0].id,volume,detail));
    const auto input=temp.filePath("horizons.txt");QVERIFY(SeismicHorizonFixture::writeFile(input,"synthetic"));
    paleo::data::ParsedFile parsed;parsed.kind="test";parsed.status="parsed";
    for(int h=0;h<2;++h){
      paleo::data::SeismicHorizon horizon;horizon.horizonName=h?"T72":"T70";horizon.timeUnit="ms";horizon.timeType="TWT";
      for(int il=100;il<104;++il)for(int xl=2000;xl<2004;++xl)horizon.points.push_back({il,xl,double((xl-2000)*10),double((il-100)*20),80.+h*80+(il-100)*2+(xl-2000)});
      parsed.horizons.push_back(horizon);
    }
    long long source=0;const auto hash=QCryptographicHash::hash("synthetic",QCryptographicHash::Sha256).toHex().toStdString();
    QVERIFY2(ds.importPreparedFile(input.toStdString(),input.toStdString(),input.toStdString(),hash,parsed,source,detail),detail.c_str());
    std::vector<paleo::data::DataEntry> horizons;QVERIFY(ds.listData("horizons",0,horizons,detail));
    paleo::data::StratigraphicScheme scheme;scheme.id="default";scheme.name="synthetic";
    for(int h=0;h<2;++h){paleo::data::StratigraphicBoundary b;b.name=h?"T72":"T70";b.order=h;b.seismicHorizonResourceId=horizons[h].id;scheme.boundaries.push_back(b);}
    paleo::data::framework::deriveUnits(scheme);std::vector<paleo::data::FrameworkIssue> issues;paleo::data::StratigraphicScheme stored;
    paleo::data::WellIntervalOccurrence occurrence;occurrence.wellId=42;occurrence.interval=scheme.intervals.front().id;
    occurrence.top=100;occurrence.bottom=500;occurrence.internal={200,300,400};scheme.wellOccurrences.push_back(occurrence);
    QVERIFY2(ds.commitStratigraphicScheme(scheme,{},stored,issues,detail),detail.c_str());ds.closeProject();
    QVector<wellsection::Well> correlationWells(2);correlationWells[0].tops={{"old formation",99}};
    QStringList warnings;paleo::loadSeismicCorrelationTops(project,{{0,42},{1,43}},correlationWells,warnings);
    QVERIFY(warnings.isEmpty());QVERIFY(correlationWells[0].correlationTops);
    QVERIFY(!correlationWells[0].correlationRevision.isEmpty());
    QCOMPARE(correlationWells[0].correlationTops->size(),2);QCOMPARE(correlationWells[0].correlationTops->front().name,QString("T70"));
    QCOMPARE(correlationWells[0].tops.front().name,QString("old formation"));QVERIFY(correlationWells[1].correlationTops->isEmpty());
    paleo::area_model::registerProject(project,this);
    struct Unregister{const void *owner;~Unregister(){paleo::area_model::unregisterProject(owner);}} unregister{this};
    PaleoTaskService tasks;seismic::SeismicTaskService service(&tasks);bool done=false,ok=false;
    seismic::SeismicNativeInfo axis;
    service.startDataPlane(path,seismic::SgySliceType::Inline,101,[&](bool success,auto,const auto& info,const auto&){ok=success;axis=info;done=true;});
    QTRY_VERIFY_WITH_TIMEOUT(done,10000);QVERIFY(ok);QCOMPARE(axis.horizons.curves.size(),size_t(5));
    QCOMPARE(axis.horizons.bands.size(),size_t(5));
    QCOMPARE(axis.horizons.curves[0].timesMs[0],82.);
    done=false;service.startDataPlane(path,seismic::SgySliceType::Xline,2001,[&](bool success,auto,const auto& info,const auto&){ok=success;axis=info;done=true;});
    QTRY_VERIFY_WITH_TIMEOUT(done,10000);QVERIFY(ok);QCOMPARE(axis.horizons.curves[0].timesMs[0],81.);
    std::shared_ptr<seismic::SgyVolume> indexed;done=false;
    service.startVolumeLoad(path,[&](bool success,auto v,const auto&){ok=success;indexed=v;done=true;});
    QTRY_VERIFY_WITH_TIMEOUT(done,10000);QVERIFY(ok);done=false;
    seismic::SeismicSectionExtractionStats stats;
    service.startSectionExtraction(indexed,{{100,2000},{103,2003}},{},[&](bool success,auto,const auto& result,const auto&){ok=success;stats=result;done=true;});
    QTRY_VERIFY_WITH_TIMEOUT(done,10000);QVERIFY(ok);QCOMPARE(stats.horizons.curves.size(),size_t(5));
    QCOMPARE(stats.horizons.curves[0].timesMs.front(),80.);QCOMPARE(stats.horizons.curves[0].timesMs.back(),89.);
    done=false;std::shared_ptr<const seismic::SeismicSectionData> gap;
    service.startDataSection(path,{{0,0},{30,60}},"synthetic datum",[&](bool success,auto section,const auto&){ok=success;gap=section;done=true;});
    QTRY_VERIFY_WITH_TIMEOUT(done,10000);QVERIFY(ok);QVERIFY(gap);QCOMPARE(gap->horizons.curves.size(),size_t(5));
    DataImportService imports(nullptr);imports.setProjectDir(project);PreviewDocService preview(&imports);
    QSignalSpy ready(&preview,&PreviewDocService::seismicSectionReady);
    preview.requestSection("synthetic",{},path,false,{},true,101);QCOMPARE(ready.size(),1);
    const auto doc=qvariant_cast<PreviewDocService::SectionDoc>(ready.front()[1]);QCOMPARE(doc.horizons.curves.size(),size_t(5));
  }
};
QTEST_MAIN(SectionHorizonsTest)
#include "tst_sectionhorizons.moc"
