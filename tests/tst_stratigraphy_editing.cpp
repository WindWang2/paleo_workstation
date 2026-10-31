#include <QtTest>
#include <QTemporaryDir>
#include <QFile>
#include <QPushButton>
#include <QDoubleSpinBox>
#include <QComboBox>
#include <QLineEdit>
#include "../src/ui/wellcomposite/multiwellview.h"
#include "../src/ui/wellcomposite/editsession.h"
#include <QMouseEvent>
#include <QJsonDocument>
#include <QImage>
#include <algorithm>
#include <cmath>
#include <paleo_data/service.h>
#include "../src/io/areamodel.h"
#include "../src/workflow/stratigraphyworkflow.h"
#include "../src/ui/pages/stratigraphypage.h"
#include "../src/services/previewdoc.h"
#include "../src/ui/paleotheme.h"
#include "../src/services/paleotaskservice.h"
#include "../src/services/stratigraphyexport.h"
using namespace paleo::data;
namespace {
struct Fixture {
 QTemporaryDir directory;
 long long first=0,second=0;
 QString interpretation,revision;
 bool create(QString& error) {
  DataService service;std::string e;const auto path=paleo::area_model::directory(directory.path()).toStdString();
  if(!service.createProject(path,"SYNTHETIC ONLY",e)){error=QString::fromStdString(e);return false;}
  for(int w=0;w<2;++w) {
   QFile file(directory.filePath(QString("SYNTH-%1.las").arg(w+1)));if(!file.open(QIODevice::WriteOnly))return false;
   QByteArray las="~Version\nVERS. 2.0\nWRAP. NO\n~Well\nWELL. SYNTH-"+QByteArray::number(w+1)+"\nNULL. -999.25\n~Curve\nMD.m\nGR.API\nSP.mV\n~Ascii\n";
   for(int i=100;i<=240;++i)las+=QByteArray::number(i)+" "+QByteArray::number(50+20*std::sin(i*.3))+" "+QByteArray::number(-20+10*std::cos(i*.2))+'\n';file.write(las);file.close();
   ImportReport report;if(!service.importFile(file.fileName().toStdString(),{},report,e)){error=QString::fromStdString(e);return false;}
  }
  std::vector<WellSummary> wells;if(!service.listWells(wells,e)||wells.size()!=2)return false;
  first=wells[0].id;second=wells[1].id;CorrelationInputRequest request;
  for(const auto& w:wells){std::vector<DataEntry> curves;if(!service.listData("curves",w.id,curves,e)||curves.empty())return false;CorrelationWellSelection s;s.wellId=w.id;s.curveId=curves[0].id;request.wells.push_back(s);}
  InterpretationDocument doc;doc.id="manual-test";doc.name="Synthetic annotation";
  if(!service.captureCorrelationInputs(request,doc.snapshot,e)){error=QString::fromStdString(e);return false;}doc.projectId=doc.snapshot.projectId;
  for(size_t n=0;n<doc.snapshot.wells.size();++n) {
   WellInterpretation w;w.wellId=doc.snapshot.wells[n].wellId;w.name=doc.snapshot.wells[n].well.wellName;w.input=doc.snapshot.wells[n];
   const double offset=n*10;const char* names[]={"T70","T72","T73","T80","internal"};const double depths[]={110,140,170,190,125};
   for(int i=0;i<5;++i){InterpretationBoundary b;b.id=names[i];b.name=names[i];b.role=i==4?"cycle":names[i];b.md=b.originalMd=depths[i]+offset;b.locked=true;w.boundaries.push_back(b);}
   InterpretationCycle parent;parent.id="parent";parent.name="E12 L3";parent.segmentId="E12";parent.topBoundaryId="T70";parent.bottomBoundaryId="T72";parent.level=3;parent.direction=1;parent.source="manual";w.cycles.push_back(parent);
   auto child=parent;child.id="upper";child.name="Upper";child.level=4;child.parentId="parent";child.bottomBoundaryId="internal";w.cycles.push_back(child);
   child.id="lower";child.name="Lower";child.topBoundaryId="internal";child.bottomBoundaryId="T72";w.cycles.push_back(child);
   InterpretationSeries curve;curve.id="pace";curve.kind="PACE";curve.name="PACE24";curve.segmentId="E12";curve.axis={110+offset,125+offset,140+offset};curve.values={0,1,0};w.series.push_back(curve);
   curve.id="cdmt";curve.kind="FISCHER_CDMT";curve.name="CDMT";curve.axisKind="CYCLE_INDEX";curve.axisUnit="1";curve.axis={0,1,2};w.series.push_back(curve);doc.wells.push_back(w);
  }
  InterpretationCommitResult result;if(!service.commitInterpretation(doc,{"","initial"},result,e)){error=QString::fromStdString(e);return false;}
  interpretation=QString::fromStdString(result.interpretationId);revision=QString::fromStdString(result.revision);return true;
 }
};
const WellInterpretation& well(const StratigraphyWorkflow& wf,long long id){for(const auto& w:wf.document().wells)if(w.wellId==id)return w;throw std::runtime_error("missing well");}
const InterpretationBoundary& boundary(const StratigraphyWorkflow& wf,long long id,const std::string& key){for(const auto& b:well(wf,id).boundaries)if(b.id==key)return b;throw std::runtime_error("missing boundary");}
void mouse(QWidget* viewport,QEvent::Type type,const QPoint& point,Qt::MouseButton button,Qt::MouseButtons buttons) {
 QMouseEvent event(type,QPointF(point),QPointF(viewport->mapToGlobal(point)),button,buttons,Qt::NoModifier);QApplication::sendEvent(viewport,&event);QCoreApplication::processEvents();
}
}
class StratigraphyEditingTest:public QObject {
 Q_OBJECT
private slots:
 void splitSharedBoundaryUndoRedoAndPersistence() {
  Fixture fixture;QString error;QVERIFY2(fixture.create(error),qPrintable(error));StratigraphyWorkflow wf(nullptr);wf.setProject(fixture.directory.path(),false);
  const auto id=fixture.first;const auto raw=well(wf,id).input.log.values;const auto numeric=well(wf,id).series[1].values;
  QSignalSpy failures(&wf,&StratigraphyWorkflow::statusChanged);
  wf.moveBoundary(id,"T72",140);QVERIFY(!wf.canUndo());QVERIFY(!wf.hasUnsavedChanges());
  wf.moveBoundary(id,"T72",105);QCOMPARE(boundary(wf,id,"T72").md,140.);QVERIFY(!wf.canUndo());QVERIFY(!failures.isEmpty());
  wf.addBoundary(id,120,"Manual pick");QCOMPARE(well(wf,id).cycles.size(),size_t(4));QCOMPARE(well(wf,id).series[0].status,std::string("current"));QCOMPARE(well(wf,id).series[1].status,std::string("stale"));
  const auto added=well(wf,id).boundaries.back().id;QVERIFY(boundary(wf,id,added).locked);QCOMPARE(boundary(wf,id,added).originalMd,120.);
  for(const auto& c:well(wf,id).cycles)if(c.level==4)QCOMPARE(c.parentId,std::string("parent"));
  wf.undo();QCOMPARE(well(wf,id).cycles.size(),size_t(3));QCOMPARE(well(wf,id).series[1].status,std::string("current"));wf.redo();QCOMPARE(boundary(wf,id,added).md,120.);
  wf.moveBoundary(id,QString::fromStdString(added),122);QCOMPARE(boundary(wf,id,added).md,122.);QCOMPARE(boundary(wf,id,added).originalMd,120.);
  wf.undo();QCOMPARE(boundary(wf,id,added).md,120.);wf.redo();
  wf.removeBoundary(id,QString::fromStdString(added));QCOMPARE(well(wf,id).cycles.size(),size_t(3));
  wf.removeBoundary(id,"internal");QCOMPARE(well(wf,id).cycles.size(),size_t(2));
  wf.removeBoundary(id,"T70");QCOMPARE(well(wf,id).boundaries.size(),size_t(4));
  wf.undo();QCOMPARE(boundary(wf,id,"internal").md,125.);
  wf.editCycle(id,"upper","Renamed",3,-1);QCOMPARE(well(wf,id).cycles[1].level,4);
  wf.editCycle(id,"upper","Renamed",4,-1);QCOMPARE(well(wf,id).cycles[1].direction,-1);
  wf.editHierarchy(id,"E12",{{"upper",1,-1},{"lower",1,-1}});QCOMPARE(well(wf,id).cycles.back().direction,-1);
  const auto count=wf.document().edits.size();wf.editHierarchy(id,"E12",{{"upper",1,1},{"lower",1,-1}});QCOMPARE(wf.document().edits.size(),count);
  wf.editBoundary(id,"T72",142,"ignored regional name",true);QCOMPARE(boundary(wf,id,"T72").name,std::string("T72"));QCOMPARE(boundary(wf,id,"T72").originalMd,140.);
  QCOMPARE(well(wf,id).input.log.values,raw);QCOMPARE(well(wf,id).series[1].values,numeric);
  wf.save();QVERIFY(!wf.hasUnsavedChanges());const auto saved=wf.document().revision;QVERIFY(saved!=fixture.revision.toStdString());
  QVERIFY(wf.loadVersion(fixture.interpretation,fixture.revision));QCOMPARE(boundary(wf,id,"T72").md,140.);
  QVERIFY(wf.loadVersion(fixture.interpretation,QString::fromStdString(saved)));QCOMPARE(boundary(wf,id,"T72").md,142.);QVERIFY(!wf.canUndo());
  wf.setProject(fixture.directory.path(),true);wf.addBoundary(id,118,"read only");QCOMPARE(wf.document().revision,saved);
 }
 // #868/#869：未保存时「加载井道图」走 PaleoNotify::ask，离屏默认拒绝（旧 QMessageBox 会阻塞）；
 // 页内 Ctrl+Z（stratigraphy.undo）撤销编辑。
 void unsavedReplaceDefaultsToCancelAndUndoShortcut() {
  Fixture fixture;QString error;QVERIFY2(fixture.create(error),qPrintable(error));StratigraphyWorkflow wf(nullptr);wf.setProject(fixture.directory.path(),false);
  StratigraphyPage page(&wf);page.show();QVERIFY(QTest::qWaitForWindowExposed(&page));
  const auto id=fixture.first;wf.addBoundary(id,120,"Manual pick");QVERIFY(wf.hasUnsavedChanges());const auto cycles=well(wf,id).cycles.size();
  auto* load=page.findChild<QPushButton*>("loadCorrelationWells");QVERIFY(load);load->click();
  QVERIFY(wf.hasUnsavedChanges());QCOMPARE(well(wf,id).cycles.size(),cycles);
  QVERIFY(wf.canUndo());page.activateWindow();page.setFocus();
  QTest::keySequence(&page,QKeySequence(QKeySequence::Undo));QCoreApplication::processEvents();
  QCOMPARE(well(wf,id).cycles.size(),cycles-1);
 }
 void annotationOnWellTracksAndFlattenedDrag() {
  PaleoTheme::applyLightTheme();
  Fixture fixture;QString error;QVERIFY2(fixture.create(error),qPrintable(error));StratigraphyWorkflow wf(nullptr);wf.setProject(fixture.directory.path(),false);
  StratigraphyPage page(&wf);page.resize(1800,1100);page.show();QTest::qWait(100);
  auto* section=page.findChild<WellComposite::MultiWellView*>("stratigraphySection");QVERIFY(section);QCOMPARE(section->panelCount(),2);
  auto* panel=section->panelAt(0);QVERIFY(panel);auto* canvas=panel->canvas();auto* view=canvas->depthViewport();QVERIFY(view);
  const auto id=panel->property("wellId").toLongLong();QCOMPARE(id,fixture.first);
  WellComposite::ComprehensiveWellData preview;QVERIFY(PreviewDocService::wellCompositeForWell(paleo::area_model::directory(fixture.directory.path()),id,&preview,&error));
  QCOMPARE(preview.continuousCurves.size(),2);QCOMPARE(panel->currentData().continuousCurves.size(),3);
  QCOMPARE(panel->currentData().continuousCurves[1].name,preview.continuousCurves[1].name);
  QCOMPARE(panel->currentData().continuousCurves[1].values,preview.continuousCurves[1].values);
  auto firstTrack=canvas->tracks().front();const auto firstWidth=firstTrack->width();
  for(const auto& track:wf.tracks())QVERIFY2(!track.mnemonic.contains("CDMT"),"Cycle-index CDMT must not be rendered as a depth track");
  const auto point=[canvas](double md){return QPoint(40,qRound(canvas->depthToY(md)));};
  auto* md=page.findChild<QDoubleSpinBox*>("boundaryMd");auto* name=page.findChild<QLineEdit*>("boundaryName");QVERIFY(md&&name);
  QTest::mouseClick(view,Qt::LeftButton,Qt::NoModifier,point(120));QCoreApplication::processEvents();
  QVERIFY2(std::abs(md->value()-120)<.5,qPrintable(QString("picked=%1, viewport=%2x%3, y=%4, range=%5..%6").arg(md->value()).arg(view->width()).arg(view->height()).arg(point(120).y()).arg(canvas->visibleTopDepth()).arg(canvas->visibleBottomDepth())));md->setValue(120);name->setText("Native graph pick");page.findChild<QPushButton*>("addBoundary")->click();
  const auto added=well(wf,fixture.first).boundaries.back().id;QCOMPARE(boundary(wf,fixture.first,added).md,120.);
  auto* flatten=page.findChild<QComboBox*>("flattenDatum");QVERIFY(flatten);flatten->setCurrentIndex(flatten->findData("T70"));QCoreApplication::processEvents();
  QVERIFY(std::abs(canvas->depthToY(110)-section->panelAt(1)->canvas()->depthToY(120))<=1);
  const auto start=point(120),end=point(123);
  mouse(view,QEvent::MouseButtonPress,start,Qt::LeftButton,Qt::LeftButton);
  mouse(view,QEvent::MouseMove,(start+end)/2,Qt::NoButton,Qt::LeftButton);
  mouse(view,QEvent::MouseMove,end,Qt::NoButton,Qt::LeftButton);
  mouse(view,QEvent::MouseButtonRelease,end,Qt::LeftButton,Qt::NoButton);
  QTRY_VERIFY(std::abs(boundary(wf,fixture.first,added).md-123)<.5);
  wf.undo();QCOMPARE(boundary(wf,fixture.first,added).md,120.);wf.redo();QVERIFY(std::abs(boundary(wf,fixture.first,added).md-123)<.5);
  page.findChild<QDoubleSpinBox*>("displayTop")->setValue(0);page.findChild<QDoubleSpinBox*>("displayBottom")->setValue(30);page.findChild<QPushButton*>("applyDisplayRange")->click();
  QVERIFY(std::abs(section->displayDepthRange().first)<.01);QVERIFY(std::abs(section->displayDepthRange().second-30)<.01);const auto imagePath=fixture.directory.filePath("all-wells.png");QVERIFY(section->grab().save(imagePath));QImage exported(imagePath);QVERIFY(!exported.isNull());QVERIFY(exported.width()>300);QVERIFY(exported.height()>300);
  page.findChild<QPushButton*>("fitFormation")->click();QVERIFY(section->displayDepthRange().first<0);QVERIFY(section->displayDepthRange().second>80);
  QCOMPARE(section->panelAt(0),panel);QCOMPARE(canvas->tracks().front(),firstTrack);QCOMPARE(firstTrack->width(),firstWidth);
  QVERIFY(!panel->editSession()->isDirty());
  const double oldPpm=canvas->pxPerMeter();canvas->setZoomFactor(canvas->zoomFactor()*1.2);canvas->setScrollDepth(115);
  QVERIFY(canvas->pxPerMeter()>oldPpm);QVERIFY(std::abs(canvas->pxPerMeter()-section->panelAt(1)->canvas()->pxPerMeter())<.01);
  QVERIFY(std::abs(canvas->depthToY(110)-section->panelAt(1)->canvas()->depthToY(120))<=1);
  page.findChild<QPushButton*>("fitFormation")->click();
  QCoreApplication::processEvents();
  auto* secondCanvas=section->panelAt(1)->canvas();
  QVERIFY(std::abs(view->mapTo(section,QPoint(0,qRound(canvas->depthToY(110)))).y()-secondCanvas->depthViewport()->mapTo(section,QPoint(0,qRound(secondCanvas->depthToY(120)))).y())<=1);
  const auto capture=qEnvironmentVariable("PALEO_VISUAL_CAPTURE");if(!capture.isEmpty()){QDir().mkpath(capture);QVERIFY(page.grab().save(capture+"/native-stratigraphy-annotated.png"));}
  wf.setViewOptions({{"flatten","T70"},{"top",0.},{"bottom",30.}});wf.save();const auto saved=QString::fromStdString(wf.document().revision);
  flatten->setCurrentIndex(0);QVERIFY(wf.loadVersion(fixture.interpretation,saved));
  QTRY_VERIFY(std::abs(section->displayDepthRange().first)<.01&&std::abs(section->displayDepthRange().second-30)<.01);
  QVERIFY(wf.loadVersion(fixture.interpretation,fixture.revision));QCOMPARE(md->value(),0.);QVERIFY(name->text().isEmpty());
  wf.setProject(fixture.directory.path(),true);QVERIFY(!section->panelAt(0)->canvas()->editMode());
  section->panelAt(0)->setEditMode(true);QVERIFY(!section->panelAt(0)->canvas()->editMode());
 }
 void candidateAdoptionProfilesGeometryAndExport() {
  Fixture fixture;QString error;QVERIFY2(fixture.create(error),qPrintable(error));StratigraphyWorkflow wf(nullptr);wf.setProject(fixture.directory.path(),false);
  DataService service;std::string e;QVERIFY(service.openProject(paleo::area_model::directory(fixture.directory.path()).toStdString(),e));
  auto doc=wf.document();auto candidate=doc.wells[0].boundaries[1];candidate.id="T72-candidate";candidate.role="candidate:T72";candidate.source="intersection";candidate.locked=false;candidate.md=candidate.originalMd=145;candidate.evidenceJson="{\"synthetic\":true}";doc.wells[0].boundaries.push_back(candidate);
  InterpretationCommitResult result;QVERIFY2(service.commitInterpretation(doc,{doc.revision,"candidate"},result,e),e.c_str());wf.setProject(fixture.directory.path(),false);
  const auto id=fixture.first;wf.confirmBoundary(id,"T72-candidate");QCOMPARE(boundary(wf,id,"T72").md,145.);QVERIFY(boundary(wf,id,"T72").locked);QCOMPARE(boundary(wf,id,"T72").status,std::string("confirmed"));QCOMPARE(boundary(wf,id,"T72").originalMd,145.);QCOMPARE(boundary(wf,id,"T72").evidenceJson,candidate.evidenceJson);
  QVERIFY(std::none_of(well(wf,id).boundaries.begin(),well(wf,id).boundaries.end(),[](const auto& b){return b.id=="T72-candidate";}));
  for(const auto& s:well(wf,id).series)QCOMPARE(s.status,std::string("stale"));wf.undo();QCOMPARE(boundary(wf,id,"T72").md,140.);wf.redo();
  auto settings=wf.geometrySettings();QCOMPARE(settings["maximumLogExtrapolationM"].toDouble(),100.);QVERIFY(settings["allowTopExtrapolation"].toBool());QVERIFY(settings["allowBottomExtrapolation"].toBool());
  settings["wellOffsetMs"]=12.5;settings["coordinateSystem"]="SYNTHETIC_TEST_CRS";wf.setGeometrySettings(settings);wf.setViewOptions({{"flatten","T70"},{"segmentScope","E12"}});
  auto profile=wf.document().profile;profile.arOrders={2,4,8};profile.invertPhase=false;wf.setProfile(profile);const auto raw=well(wf,id).input.log.values;
  wf.save();const auto revision=QString::fromStdString(wf.document().revision);QVERIFY(wf.loadVersion(fixture.interpretation,revision));QCOMPARE(wf.geometrySettings()["wellOffsetMs"].toDouble(),12.5);QCOMPARE(wf.document().profile.arOrders,profile.arOrders);QCOMPARE(well(wf,id).input.log.values,raw);
  const auto view=QJsonDocument::fromJson(QByteArray::fromStdString(wf.document().viewStateJson)).object();QCOMPARE(view["flatten"].toString(),QString("T70"));QCOMPARE(view["segmentScope"].toString(),QString("E12"));
  const auto file=fixture.directory.filePath("derived.csv");auto series=well(wf,id).series.front();series.values[1]=paleo::data::Missing;series.quality={0,1,4};QVERIFY2(exportStratigraphySeries(series,file,error),qPrintable(error));QFile csv(file);QVERIFY(csv.open(QIODevice::ReadOnly));const auto lines=csv.readAll().split('\n');QCOMPARE(lines[0],QByteArray("axis,value,quality"));QCOMPARE(lines[2],QByteArray("125,,1"));QCOMPARE(lines[3],QByteArray("140,0,4"));
 }
 void loadTracksWithoutRunningSubdivision() {
  Fixture fixture;QString error;QVERIFY2(fixture.create(error),qPrintable(error));PaleoTaskService tasks;StratigraphyWorkflow wf(&tasks);wf.setProject(fixture.directory.path(),false);
  wf.loadWells({fixture.first,fixture.second});QVERIFY(wf.isRunning());wf.addBoundary(fixture.first,120,"busy edit");QTRY_VERIFY_WITH_TIMEOUT(!wf.isRunning(),15000);
  QCOMPARE(wf.document().wells.size(),size_t(2));QVERIFY(!wf.tracks().isEmpty());QVERIFY(well(wf,fixture.first).boundaries.empty());QVERIFY(well(wf,fixture.first).cycles.empty());QVERIFY(!wf.canSubdivide({fixture.first,fixture.second}));
 }
 void perWellSegmentTemplatesPreserveReferenceAndRecommendGroup() {
  Fixture fixture;QString error;QVERIFY2(fixture.create(error),qPrintable(error));StratigraphyWorkflow wf(nullptr);wf.setProject(fixture.directory.path(),false);
  QSignalSpy status(&wf,&StratigraphyWorkflow::statusChanged);wf.makeTemplate(fixture.first,"Synthetic manual E12",0);
  const auto templates=wf.templates();QVERIFY2(!templates.isEmpty(),status.isEmpty()?"no template":qPrintable(status.last()[0].toString()));
  const auto key=templates.last().id;auto rows=wf.prepareAssignments("E12",key);QCOMPARE(rows.size(),2);
  for(const auto& row:rows)if(row.well==fixture.first){QVERIFY(row.key.isEmpty());QVERIFY(row.choices.isEmpty());}else{QCOMPARE(row.well,fixture.second);QCOMPARE(row.key,key);QCOMPARE(row.choices.size(),1);}
  auto groups=wf.previewClustering(1);QCOMPARE(groups.members.size(),size_t(2));wf.saveClustering(groups);rows=wf.prepareAssignments("E12",key);
  for(const auto& row:rows)if(row.well==fixture.second){QCOMPARE(row.key,key);QVERIFY(row.choices[0].sameGroup);}
  const auto other=wf.prepareAssignments("E3",key);QCOMPARE(other.size(),2);for(const auto& row:other){QVERIFY(row.key.isEmpty());QVERIFY(row.choices.isEmpty());}
  QCOMPARE(boundary(wf,fixture.first,"internal").md,125.);QCOMPARE(boundary(wf,fixture.second,"internal").md,135.);
 }
};
QTEST_MAIN(StratigraphyEditingTest)
#include "tst_stratigraphy_editing.moc"
