// 层：测试壳
#include "algorithms/inversion/lowfreq.h"
#include "algorithms/inversion/wavelet.h"
#include "algorithms/inversion/volume.h"
#include <QtTest>
#include <QTemporaryDir>
#include <QElapsedTimer>
#include <QFile>
#include <QtEndian>
#include <cmath>
#include <cstring>
#include "io/areamodel.h"
#include "services/seismicdataaccess.h"
#include "services/seismicwelltrace.h"
#include "workflow/inversionworkflow.h"
#include "workflow/sectionworkbench.h"
#include "workflow/wellimpedancetwt.h"

namespace {
struct Fixture {
 QTemporaryDir temp;QString project=temp.filePath("project"),path=temp.filePath("body.sgy"),error;
 paleo::data::DataService ds;std::string detail;
 bool init(bool synthetic=false){
  QFile file(path);if(!file.open(QIODevice::WriteOnly))return false;file.write(QByteArray(3200,' '));
  const auto put16=[](QByteArray& b,int at,quint16 v){qToBigEndian(v,reinterpret_cast<uchar*>(b.data()+at));};
  const auto put32=[](QByteArray& b,int at,quint32 v){qToBigEndian(v,reinterpret_cast<uchar*>(b.data()+at));};
  QByteArray binary(400,0);put16(binary,16,2000);put16(binary,20,256);put16(binary,24,5);file.write(binary);
  for(int i=0;i<3;++i)for(int j=0;j<3;++j){
   QByteArray header(240,0);put32(header,0,i*3+j+1);put32(header,188,100+i);put32(header,192,200+j);
   put32(header,72,300000+100*j);put32(header,76,2300000+100*i);file.write(header);
   const auto wavelet=paleo::inversion::makeRicker(25,2,128);
   QByteArray values(256*4,0);for(int k=0;k<256;++k){
    float v=float(1000*i+100*j+k)*.00001f;
    if(synthetic){v=0;for(const auto& spike:std::vector<std::pair<int,float>>{{40,.1f},{160,-.07f}}){
      if((spike.first==40&&j==2)||(spike.first==160&&j==0))continue;
      const int at=k-spike.first-int(wavelet.t0Ms/2);
      if(at>=0&&at<wavelet.sampleCount())v+=spike.second*wavelet.samples[size_t(at)];
    }}
    quint32 bits;std::memcpy(&bits,&v,4);put32(values,k*4,bits);
   }file.write(values);
  }file.close();
  if(!ds.createProject(paleo::area_model::directory(project).toStdString(),"native inversion",detail)||!ds.setProjectCrs("EPSG:32650","m",detail))return false;
  paleo::data::ImportReport report;paleo::data::ImportOptions options;options.copySources=false;
  if(!ds.importFile(path.toStdString(),options,report,detail))return false;
  std::vector<paleo::data::DataEntry> entries;if(!ds.listData("volumes",0,entries,detail)||entries.size()!=1)return false;
  paleo::data::SeismicVolume body;if(!ds.readVolume(entries[0].id,body,detail))return false;
  body.geometry.coordinateSystem="EPSG:32650";body.geometry.coordinateUnit="m";
  body.sampling={256,0,2,"ms","TWT","shared zero"};return ds.saveVolume(entries[0].id,body,detail);
 }
 std::vector<seismic::WellTrajSample> pathPoints()const{
  std::vector<seismic::WellTrajSample> points;
  for(int i=0;i<3;++i){seismic::WellTrajSample p;p.md=i*200;p.twtMs=i*200;p.x=300000+i*100;p.y=2300100;points.push_back(p);}return points;
 }
 paleo::inv::InversionWorkflow::InversionJobRequest request()const{
  paleo::inv::InversionWorkflow::InversionJobRequest r;r.seismicPath=path;r.projectDirectory=project;r.timeReference="shared zero";
  r.method="bandlimited";r.hasWaveletValue=true;r.wavelet=paleo::inversion::makeRicker(25,2,64);r.maxThreads=2;
  paleo::inv::InversionWorkflow::LowFreqWellInput w;w.wellId="D1";w.trajectory=pathPoints();w.timeReference=r.timeReference;w.trajectorySource="fixture";
  for(int k=0;k<201;++k){w.twtMs.push_back(k*2);w.impedance.push_back(k<100?5000.f:6000.f);}r.lowFreqWells.push_back(w);return r;
 }
};
}
class NativeInversionTest:public QObject {
 Q_OBJECT
private slots:
 void strictImpedanceKeepsCurveMissingAndCoverage(){
  std::vector<double> times;std::vector<float> impedance;
  QVERIFY(paleo::inv::wellImpedanceTwt({0,100,200,300,400},{400,400,400,400,400},
    {100,200,300},{2.4f,NAN,2.4f},{0,400},{0,400},&times,&impedance,true));
  QCOMPARE(times.size(),size_t(5));QVERIFY(std::isnan(impedance[0]));QVERIFY(std::isnan(impedance[2]));
  QVERIFY(std::isnan(impedance[4]));QCOMPARE(impedance[1],6000.f);QCOMPARE(impedance[3],6000.f);
 }
 void alongPathUsesMovingTraceAndPreservesGaps(){
  Fixture f;QVERIFY2(f.init(),f.detail.c_str());paleo::SeismicDataAccess access;
  QVERIFY(access.openProjectSource(f.project,f.path,&f.error));paleo::SeismicWellTrace result;
  auto path=f.pathPoints();QVERIFY2(access.wellTrace(path,"shared zero",result,f.detail),f.detail.c_str());
  QCOMPARE(result.amplitudes[0],1000.f*.00001f);QCOMPARE(result.amplitudes[100],1200.f*.00001f);QCOMPARE(result.amplitudes[200],1400.f*.00001f);
  QVERIFY(std::isnan(result.amplitudes[220]));
  path[1].x=NAN;QVERIFY(!access.wellTrace(path,"shared zero",result,f.detail));
  path=f.pathPoints();path[1].twtMs=-1;QVERIFY(!access.wellTrace(path,"shared zero",result,f.detail));
  QVERIFY(!access.wellTrace(f.pathPoints(),"different zero",result,f.detail));
  auto fixed=f.pathPoints();for(auto& p:fixed)p.x=300100;
  QVERIFY2(access.wellTrace(fixed,"shared zero",result,f.detail),f.detail.c_str());QCOMPARE(result.amplitudes[0],1100.f*.00001f);
  path=f.pathPoints();auto repeated=path.front();repeated.md=100;repeated.twtMs=100;
  path.insert(path.begin()+1,repeated);QVERIFY2(access.wellTrace(path,"shared zero",result,f.detail),f.detail.c_str());
  QCOMPARE(result.amplitudes[25],1025.f*.00001f);
 }
 void knownWaveletRecoveredAlongDeviatedPath(){
  Fixture f;QVERIFY2(f.init(true),f.detail.c_str());paleo::inv::InversionWorkflow::WaveletJob job;
  job.prepared=true;job.req.projectDirectory=f.project;job.req.seismicPath=f.path;
  job.req.timeReference="shared zero";job.req.trajectory=f.pathPoints();job.req.waveletLengthMs=128;
  job.spikes={{80,.1f},{320,-.07f}};paleo::inv::InversionWorkflow workflow;
  QVERIFY2(workflow.computeWaveletJob(&job),qPrintable(job.error));QVERIFY(job.fitCorrelation>.999);
  const auto truth=paleo::inversion::makeRicker(25,2,128);QCOMPARE(job.wavelet.sampleCount(),truth.sampleCount());
  for(int i=0;i<truth.sampleCount();++i)QVERIFY(std::abs(job.wavelet.samples[size_t(i)]-truth.samples[size_t(i)])<.002);
 }
 void registeredWaveletAndBothInversions(){
  Fixture f;QVERIFY2(f.init(),f.detail.c_str());paleo::inv::InversionWorkflow workflow;
  paleo::inv::WaveletJobRequest request;request.projectDirectory=f.project;request.seismicPath=f.path;
  request.trajectory=f.pathPoints();request.timeReference="shared zero";request.requireTrajectory=true;
  request.tdDepthM={0,400};request.tdTimeMs={0,400};request.acDepthsM={0,100,200,300,400};
  request.denDepthsM=request.acDepthsM;request.denValues={2.4,2.4,2.4,2.4,2.4};request.acUsPerM={400,400,350,350,400};
  paleo::inv::InversionWorkflow::WaveletJob wavelet;
  QVERIFY2(workflow.prepareWaveletJob(request,&wavelet,&f.error),qPrintable(f.error));
  QVERIFY2(workflow.computeWaveletJob(&wavelet),qPrintable(wavelet.error));QVERIFY(wavelet.spatialEvidence.contains("trajectorySha256"));
  for(const auto& method:{QString("bandlimited"),QString("sparse")}){
   auto req=f.request();req.method=method;req.maxIterations=8;
   paleo::inv::InversionWorkflow::InversionJob job;
   QVERIFY2(workflow.prepareInversionJob(req,&job,&f.error),qPrintable(f.error));
   QVERIFY(job.dataServiceSource);QCOMPARE(job.lowFreq.smoothedWells[0].xSamples[100],300100.);
   QVERIFY2(workflow.computeInversionJob(&job),qPrintable(job.error));
   QJsonObject header;QVERIFY(paleo::inversion::readImpedanceVolumeHeader(job.tempVolumePath,&header,&f.error));
   QCOMPARE(header.value("reader").toString(),QString("DataService_inline_stream"));QCOMPARE(job.processedTraces,9);QCOMPARE(job.failedTraces,0);
   QFile file(job.tempVolumePath);QVERIFY(file.open(QIODevice::ReadOnly));std::vector<float> values;
   QVERIFY(paleo::inversion::readImpedanceVolumeBlob(file.readAll(),&values,nullptr,&f.error));QCOMPARE(values.size(),size_t(9*256));
   QVERIFY(std::any_of(values.begin(),values.end(),[](float v){return std::isfinite(v)&&v>0;}));
   DataCatalog catalog;QVERIFY(catalog.open(f.project,&f.error));DerivedAssetRegistrar registrar(&catalog,f.project);
   const auto published=workflow.publishInversionJob(job,registrar,&f.error);QVERIFY2(published.ok,qPrintable(f.error));
   QCOMPARE(published.volumeHeader.value("reader").toString(),QString("DataService_inline_stream"));
   QVERIFY(QFile::exists(QDir(f.project).filePath(published.relativePath)));
   workflow.reclaimTempVolume(job.tempVolumePath);
  }
 }
 void cropCancelAndFailures(){
  Fixture f;QVERIFY2(f.init(),f.detail.c_str());auto req=f.request();paleo::inv::InversionWorkflow workflow;
  req.firstInline=req.lastInline=101;req.firstCrossline=201;req.lastCrossline=202;req.firstTimeMs=100;req.lastTimeMs=300;
  paleo::inv::InversionWorkflow::InversionJob job;
  QVERIFY2(workflow.prepareInversionJob(req,&job,&f.error),qPrintable(f.error));QCOMPARE(job.nIl,1);QCOMPARE(job.nXl,2);QCOMPARE(job.nS,101);
  QVERIFY2(workflow.computeInversionJob(&job),qPrintable(job.error));QCOMPARE(job.processedTraces,2);workflow.reclaimTempVolume(job.tempVolumePath);
  QVERIFY(workflow.prepareInversionJob(req,&job,&f.error));
  QVERIFY(!workflow.computeInversionJob(&job,[]{return true;}));QVERIFY(!job.ok);
  req=f.request();QVERIFY(workflow.prepareInversionJob(req,&job,&f.error));bool cancel=false;
  QVERIFY(!workflow.computeInversionJob(&job,[&]{return cancel;},[&](double progress){if(progress>0)cancel=true;}));
  QVERIFY(!QFile::exists(job.tempVolumePath));
  req.lowFreqWells[0].timeReference="other";QVERIFY(!workflow.prepareInversionJob(req,&job,&f.error));
  req=f.request();req.lowFreqWells[0].trajectory.clear();QVERIFY(!workflow.prepareInversionJob(req,&job,&f.error));
  DataCatalog catalog;QVERIFY(catalog.open(f.project,&f.error));
  req=f.request();req.projectDirectory.clear();req.timeReference.clear();req.lowFreqWells.clear();
  paleo::area_model::registerProject(f.project,&catalog);
  const bool prepared=workflow.prepareInversionJob(req,&job,&f.error);
  paleo::area_model::unregisterProject(&catalog);
  QVERIFY2(prepared,qPrintable(f.error));QVERIFY(job.dataServiceSource);QVERIFY(!job.hasLowFreq);
 }
 void streamRejectsIncompleteAndHeaderReadsWithoutBodyAllocation(){
  QTemporaryDir temp;const auto path=temp.filePath("test.iimp");QString error;
  const QJsonObject header{{"nIl",1},{"nXl",2},{"nS",2}};float values[]={1,2,3,4};
  {paleo::inversion::ImpedanceVolumeStream stream(path);QVERIFY(stream.open(header,&error));
   QVERIFY(stream.append(values,2,&error));QVERIFY(!stream.finish(header,&error));}
  QVERIFY(!QFile::exists(path));
  {paleo::inversion::ImpedanceVolumeStream stream(path);QVERIFY(stream.open(header,&error));
   QVERIFY(!stream.append(values,5,&error));QVERIFY(stream.append(values,4,&error));QVERIFY(stream.finish(header,&error));}
  QJsonObject read;QVERIFY(paleo::inversion::readImpedanceVolumeHeader(path,&read,&error));QCOMPARE(read,header);
  QFile truncated(path);QVERIFY(truncated.open(QIODevice::ReadWrite));QVERIFY(truncated.resize(12));truncated.close();
  QVERIFY(!paleo::inversion::readImpedanceVolumeHeader(path,&read,&error));
 }
 void realAreaAlongWellAndSmallVolume(){
  const auto project=qEnvironmentVariable("PALEO_REAL_TRAJECTORY_PROJECT");
  if(project.isEmpty())QSKIP("Set PALEO_REAL_TRAJECTORY_PROJECT for local data; source stays read-only");
  DataCatalog catalog;QString error;QVERIFY2(catalog.open(project,&error),qPrintable(error));
  SectionWorkbench work(&catalog);auto candidates=work.sectionWells();
  QString source=paleo::seismicProjectSource(project,{},&error);QVERIFY2(!source.isEmpty(),qPrintable(error));
  paleo::SeismicDataAccess access;QVERIFY(access.openProjectSource(project,source,&error));
  int count=0;for(const auto& well:candidates){
   if(well.trajectory.size()<2)continue;paleo::SeismicWellTrace trace;std::string detail;QElapsedTimer timer;timer.start();
   QVERIFY2(access.wellTrace(well.trajectory,well.timeReference,trace,detail),detail.c_str());
   qInfo()<<well.wellName<<"along-well valid"<<trace.finiteSamples<<"columns"<<trace.sourceColumns<<"ms"<<timer.elapsed();
   QVERIFY(trace.finiteSamples>100);
   if(count==0){
    const seismic::WellCurveItem *ac=nullptr,*den=nullptr;
    for(const auto& curve:well.curves){const auto name=seismic::baseCurveMnemonic(curve.curveName);
     if(name=="AC")ac=&curve;if(name=="DEN"||name=="RHOB")den=&curve;}
    QVERIFY(ac&&den);
    paleo::inv::WaveletJobRequest request;request.projectDirectory=project;request.seismicPath=source;
    request.wellId=well.wellId;request.trajectory=well.trajectory;request.trajectorySource=well.trajectorySource;
    request.timeReference=well.timeReference;request.requireTrajectory=true;
    request.acDepthsM=ac->depthsM;request.acUsPerM=ac->values;request.denDepthsM=den->depthsM;request.denValues=den->values;
    std::vector<std::pair<double,double>> pairs;
    paleo::inv::appendTimeDepthPairs(ac->depthsM,ac->twtMs,&pairs);paleo::inv::appendTimeDepthPairs(den->depthsM,den->twtMs,&pairs);
    QVERIFY(paleo::inv::buildTimeDepthTable(std::move(pairs),&request.tdDepthM,&request.tdTimeMs));
    paleo::inv::InversionWorkflow workflow;paleo::inv::InversionWorkflow::WaveletJob wavelet;
    QVERIFY2(workflow.prepareWaveletJob(request,&wavelet,&error),qPrintable(error));
    QVERIFY2(workflow.computeWaveletJob(&wavelet),qPrintable(wavelet.error));
    qInfo()<<"real along-well wavelet"<<well.wellName<<wavelet.fitCorrelation<<wavelet.wavelet.dominantFreqHz()<<"Hz";
    paleo::data::SeismicCoordinateRange geometry;QVERIFY(access.range(geometry,detail));
    const auto& p=well.trajectory[well.trajectory.size()/2];const auto& o=geometry.corners[0];
    const double ax=(geometry.corners[3].x-o.x)/(geometry.inlineCount-1),ay=(geometry.corners[3].y-o.y)/(geometry.inlineCount-1);
    const double bx=(geometry.corners[1].x-o.x)/(geometry.crosslineCount-1),by=(geometry.corners[1].y-o.y)/(geometry.crosslineCount-1),det=ax*by-ay*bx;
    const int il=int(std::llround(((p.x-o.x)*by-(p.y-o.y)*bx)/det));
    const int xl=int(std::llround((ax*(p.y-o.y)-ay*(p.x-o.x))/det));
    paleo::inv::InversionWorkflow::InversionJobRequest req;req.projectDirectory=project;req.seismicPath=source;
    req.timeReference=well.timeReference;req.wavelet=wavelet.wavelet;req.hasWaveletValue=true;req.method="bandlimited";
    req.firstInline=req.lastInline=geometry.inlineMin+il*geometry.inlineStep;
    req.firstCrossline=geometry.crosslineMin+xl*geometry.crosslineStep;req.lastCrossline=req.firstCrossline+geometry.crosslineStep;
    req.firstTimeMs=p.twtMs-250;req.lastTimeMs=p.twtMs+250;
    paleo::inv::InversionWorkflow::LowFreqWellInput input;input.wellId=well.wellId;input.trajectory=well.trajectory;
    input.timeReference=well.timeReference;input.trajectorySource=well.trajectorySource;input.requireTrajectory=true;
    QVERIFY(paleo::inv::wellImpedanceTwt(ac->depthsM,ac->values,den->depthsM,den->values,request.tdDepthM,request.tdTimeMs,&input.twtMs,&input.impedance,true));
    req.lowFreqWells.push_back(input);
    for(const auto& method:{QString("bandlimited"),QString("sparse")}){
     req.method=method;req.maxIterations=20;paleo::inv::InversionWorkflow::InversionJob job;timer.restart();
     QVERIFY2(workflow.prepareInversionJob(req,&job,&error),qPrintable(error));
     QVERIFY2(workflow.computeInversionJob(&job),qPrintable(job.error));
     qInfo()<<"real native inversion"<<method<<job.nIl<<job.nXl<<job.nS<<"failed"<<job.failedTraces<<"ms"<<timer.elapsed();
     QCOMPARE(job.failedTraces,0);workflow.reclaimTempVolume(job.tempVolumePath);
    }
   }
   if(++count==2)break;
  }QCOMPARE(count,2);
 }
};
QTEST_MAIN(NativeInversionTest)
#include "tst_inversion_native.moc"
