// 层：测试壳
#include <QtTest>
#include <QTemporaryDir>
#include <QCryptographicHash>
#include <QJsonObject>
#include "services/analysisunitcontext.h"
#include "services/stratigraphicsubintervals.h"
#include "services/subintervalfactors.h"
#include "services/seismicunitinput.h"
#include "domain/wellcompositemodel.h"
#include "domain/welllogfacies.h"
#include "io/areamodel.h"
#include "seismichorizonfixture.h"
#include <paleo_data/service.h>
#include <cmath>
using namespace paleo::strat;
namespace fw=paleo::data::framework;
class AnalysisUnitContextTest:public QObject {
 Q_OBJECT
 struct Fixture {
  QTemporaryDir dir;paleo::data::DataService ds;paleo::data::StratigraphicScheme scheme;
  QString project,source,error;
  bool init(bool seismic=false,bool negative=false){
   project=dir.filePath("project");std::string detail;
   if(!ds.createProject(paleo::area_model::directory(project).toStdString(),"test",detail))return false;
   if(seismic){
    source=dir.filePath("volume.sgy");if(!SeismicHorizonFixture::writeFile(source,SeismicHorizonFixture::buildVolume()))return false;
    paleo::data::ImportOptions opts;opts.copySources=false;paleo::data::ImportReport report;
    if(!ds.importFile(source.toStdString(),opts,report,detail))return false;
    const auto input=dir.filePath("picks.txt");if(!SeismicHorizonFixture::writeFile(input,"synthetic"))return false;
    paleo::data::ParsedFile parsed;parsed.kind="test";parsed.status="parsed";
    for(int k=0;k<2;++k){paleo::data::SeismicHorizon h;h.horizonName=k?"B":"A";h.timeUnit="ms";h.timeType="TWT";
     for(int il=100;il<104;++il)for(int xl=2000;xl<2004;++xl)
      h.points.push_back({il,xl,double((xl-2000)*10),double((il-100)*20),il==101&&xl==2001?NAN:(negative?-160.+k*80:80.+k*80)});
     parsed.horizons.push_back(h);
    }
    long long id=0;if(!ds.importPreparedFile(input.toStdString(),input.toStdString(),input.toStdString(),
      QCryptographicHash::hash("synthetic",QCryptographicHash::Sha256).toHex().toStdString(),parsed,id,detail))return false;
   }
   WellTopTable tops;tops[17]={{"A",1000},{"B",1080}};tops[stableWellKey("well-native")]={{"A",2000},{"B",2080}};
   auto draft=draftScheme("scope","test",{"A","B"},tops);
   if(seismic){std::vector<paleo::data::DataEntry> h;if(!ds.listData("horizons",0,h,detail))return false;
    for(auto &b:draft.boundaries)for(const auto &entry:h)if(entry.name==b.name)b.seismicHorizonResourceId=entry.id;
   }
   std::vector<paleo::data::FrameworkIssue> issues;
   return ds.commitStratigraphicScheme(draft,{},scheme,issues,detail);
  }
  UnitSelection unit(int index=1)const{return makeSelection(scheme,selectorOptions(scheme).first().intervalId,index);}
 };
private slots:
 void scopeDataClipsBeforeValidationAndDoesNotInventSamples(){
  Fixture f;QVERIFY(f.init());WellComposite::ComprehensiveWellData data;
  WellComposite::CurveData curve;curve.name="GR";curve.depths={990,1000,1010,1020,1030,NAN};curve.values={NAN,1,2,3,NAN,5};
  data.continuousCurves.append(curve);
  WellComposite::FormationInterval formation;formation.name="A";formation.topDepth=990;formation.bottomDepth=1040;
  data.formationIntervals.append(formation);
  const auto scoped=scopeWellData(data,analysisWellRange(f.scheme,f.unit(),"well-native",17));
  QCOMPARE(scoped.continuousCurves.first().depths,(QVector<float>{1000,1010}));
  QCOMPARE(scoped.continuousCurves.first().values,(QVector<float>{1,2}));
  QCOMPARE(scoped.formationIntervals.first().topDepth,1000.f);QCOMPARE(scoped.formationIntervals.first().bottomDepth,1020.f);
  auto invalid=analysisWellRange(f.scheme,f.unit(),"well-native",17);invalid.resolved=false;
  const auto empty=scopeWellData(data,invalid);QVERIFY(empty.continuousCurves.first().depths.isEmpty());QVERIFY(empty.formationIntervals.isEmpty());
  data.continuousCurves.first().depths={1060,1070,1080,1090};data.continuousCurves.first().values={1,2,3,4};
  data.formationIntervals.first().bottomDepth=1090;
  WellComposite::LithologyInterval lithology;lithology.topDepth=990;lithology.bottomDepth=1090;lithology.lithoName=QStringLiteral("砂岩");
  data.lithologyIntervals.append(lithology);
  const auto last=scopeWellData(data,analysisWellRange(f.scheme,f.unit(4),"well-native",17));
  WellFaciesModel model{"local","local","1",{},{"GR"},{QStringLiteral("河口坝")},2};
  const auto input=prepareWellFaciesInput(last,model);QVERIFY2(input.ready(),qPrintable(input.reason));QCOMPARE(input.rows.size(),3);
 }
 void nativeIdentityWinsAndMissingNativeDoesNotFallback(){
  Fixture f;QVERIFY(f.init());auto unit=f.unit();
  auto range=analysisWellRange(f.scheme,unit,"well-native",17);QVERIFY(range.resolved);QCOMPARE(range.top,1000.);QCOMPARE(range.wellId,17LL);
  range=analysisWellRange(f.scheme,unit,"well-native",0);QVERIFY(range.resolved);QCOMPARE(range.top,2000.);
  auto changed=f.scheme;for(auto &o:changed.wellOccurrences)if(o.wellId==17){o.status="top_missing";o.top=paleo::data::Missing;}
  QVERIFY(!analysisWellRange(changed,unit,"well-native",17).resolved);
 }
 void windowsPartitionSamplesAndRespectNonuniformDivision(){
  Fixture f;QVERIFY(f.init());QJsonArray rows;for(int i=0;i<=80;++i)rows.append(QJsonObject{{"深度",1000+i},{"GR",i}});
  int total=0;QSet<int> used;
  for(int index=1;index<=4;++index){const auto range=analysisWellRange(f.scheme,f.unit(index),"well-native",17);
   const auto scoped=scopeWellRows(rows,range);QCOMPARE(scoped.size(),qsizetype(index==4?21:20));total+=scoped.size();
   for(const auto &row:scoped){const int depth=row.toObject().value("深度").toInt();QVERIFY(!used.contains(depth));used.insert(depth);}
  }
  QCOMPARE(total,81);auto changed=f.scheme;changed.divisions.front().fractions={.1,.5,.9};
  const auto range=analysisWellRange(changed,f.unit(2),"well-native",17);QCOMPARE(range.top,1008.);QCOMPARE(range.bottom,1040.);
 }
 void freezesIdentityAndRejectsCrossIntervalOrChangedGeometry(){
  Fixture f;QVERIFY(f.init());UnitSelection frozen;QString error;auto selection=f.unit();selection.revision.clear();
  QVERIFY2(freezeAnalysisUnit(f.project,selection,frozen,&error),qPrintable(error));QCOMPARE(frozen.revision,QString::fromStdString(f.scheme.revision.value));
  auto bad=selection;bad.intervalId="unrelated";QVERIFY(!freezeAnalysisUnit(f.project,bad,selection,&error));
  auto changed=f.scheme;changed.divisions.front().fractions={.2,.5,.75};changed.divisions.front().mode="interpreted";
  paleo::data::StratigraphicScheme next;std::vector<paleo::data::FrameworkIssue> issues;std::string detail;
  QVERIFY(f.ds.commitStratigraphicScheme(changed,f.scheme.revision.value,next,issues,detail));
  QVERIFY(!analysisUnitGeometryCurrent(f.project,frozen,&error));QVERIFY(analysisUnitGeometryCurrent(f.project,f.unit(4),&error));
 }
 void signedTimeIsPreservedWithoutAbsOrExtrapolation(){
  Fixture f;QVERIFY(f.init(true,true));paleo::data::SeismicHorizonWindowResult window;QString error;
  QVERIFY2(readSeismicAnalysisWindow(f.project,f.unit(),f.source,window,&error),qPrintable(error));
  QCOMPARE(window.validTraceCount,size_t(0));
  for(const auto &trace:window.traces)if(std::isfinite(trace.parentTopTimeMs)){QVERIFY(trace.parentTopTimeMs<0);QVERIFY(trace.parentBottomTimeMs<0);}
  for(const auto value:window.amplitudes)QVERIFY(std::isnan(value));
 }
 void seismicWindowUsesUnitMaskAndSelectedVolume(){
  Fixture f;QVERIFY(f.init(true));paleo::data::SeismicHorizonWindowResult window;QString error;
  QVERIFY2(readSeismicAnalysisWindow(f.project,f.unit(),f.source,window,&error),qPrintable(error));QCOMPARE(window.validTraceCount,size_t(15));
  QCOMPARE(window.schemeRevision.value,f.scheme.revision.value);
  for(const auto &t:window.traces){
   if(t.inlineNumber==101&&t.crosslineNumber==2001){QCOMPARE(t.status,2);continue;}
   QCOMPARE(t.windowTopTimeMs,80.);QCOMPARE(t.windowBottomTimeMs,100.);QVERIFY(t.firstSampleTimeMs>=80.);
   QVERIFY(t.firstSampleTimeMs+(t.nativeSampleCount-1)*window.sampleIntervalMs<100.);
   for(int sample=0;sample<window.fixedSampleCount;++sample)QVERIFY(std::isfinite(window.amplitudes[size_t(&t-window.traces.data())*window.fixedSampleCount+sample]));
  }
  const auto input=prepareSeismicUnitInput(f.project,f.unit(),f.source);QVERIFY2(input.error.isEmpty(),qPrintable(input.error));
  QCOMPARE(input.validCells,15);QVERIFY(input.crs.contains("UTM"));QCOMPARE(input.samples.size(),4096);
  const auto other=f.dir.filePath("other.sgy");auto bytes=SeismicHorizonFixture::buildVolume();
  for(int trace=0;trace<16;++trace)for(int sample=0;sample<50;++sample){const int offset=3600+trace*(240+50*4)+240+sample*4;
   quint32 raw=qFromBigEndian<quint32>(reinterpret_cast<const uchar *>(bytes.constData()+offset));float v;std::memcpy(&v,&raw,4);v*=5;
   std::memcpy(&raw,&v,4);qToBigEndian<quint32>(raw,reinterpret_cast<uchar *>(bytes.data()+offset));
  }
  QVERIFY(SeismicHorizonFixture::writeFile(other,bytes));paleo::data::ImportOptions opts;opts.copySources=false;paleo::data::ImportReport report;std::string detail;
  QVERIFY(f.ds.importFile(other.toStdString(),opts,report,detail));
  const auto second=prepareSeismicUnitInput(f.project,f.unit(),other);QVERIFY2(second.error.isEmpty(),qPrintable(second.error));
  for(qsizetype i=0;i<input.samples.size();++i)if(std::isfinite(input.samples[i]))QVERIFY(std::abs(second.samples[i]-5*input.samples[i])<1e-4);
  QVERIFY(!prepareSeismicUnitInput(f.project,f.unit(),{}).error.isEmpty()); // ambiguous volume never chooses the first
  window.amplitudes={1};QVERIFY(!readSeismicAnalysisWindow(f.project,f.unit(),f.source,window,&error,[]{return true;}));QVERIFY(window.amplitudes.empty());
 }
};
QTEST_GUILESS_MAIN(AnalysisUnitContextTest)
#include "tst_analysisunitcontext.moc"
