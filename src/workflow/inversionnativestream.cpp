// 层：功能
#include "algorithms/inversion/volume.h"
#include "algorithms/inversion/bandlimit.h"
#include "algorithms/inversion/sparse.h"
#include "algorithms/inversion/lowfreq.h"
#include "inversionnative.h"
#include "services/seismicdataaccess.h"
#include "services/workerexceptionsink.h"
#include "nativecppadapter.h"
#include "nativeinputs_inversion.h"
#include <QDir>
#include <QFileInfo>
#include <QDateTime>
#include <QScopeGuard>
#include <QUuid>
#include <atomic>
#include <thread>
#include <algorithm>
#include <cmath>

namespace paleo::inv {
namespace {
QJsonObject headerFor(const InversionWorkflow::InversionJob& job){
 auto h=QJsonObject::fromVariantMap(job.spatialEvidence);
 h.insert(QStringLiteral("nIl"),job.nIl);h.insert(QStringLiteral("nXl"),job.nXl);h.insert(QStringLiteral("nS"),job.nS);
 h.insert(QStringLiteral("t0Ms"),job.t0Ms);h.insert(QStringLiteral("dtMs"),job.dtMs);
 h.insert(QStringLiteral("inlineMin"),job.inlineMin);h.insert(QStringLiteral("inlineStep"),job.inlineStep);
 h.insert(QStringLiteral("xlineMin"),job.xlineMin);h.insert(QStringLiteral("xlineStep"),job.xlineStep);
 h.insert(QStringLiteral("method"),job.req.method);h.insert(QStringLiteral("lowCutHz"),job.req.lowCutHz);
 h.insert(QStringLiteral("hasLowFreq"),job.hasLowFreq);h.insert(QStringLiteral("lowFreqLayersUsed"),job.lowFreq.layersUsed);
 h.insert(QStringLiteral("sourceSeismic"),job.req.seismicPath);h.insert(QStringLiteral("waveletDominantHz"),job.wavelet.dominantFreqHz());
 h.insert(QStringLiteral("amplitudeCalibrated"),job.wavelet.amplitudeScale>0);
 h.insert(QStringLiteral("amplitudeScale"),job.wavelet.amplitudeScale>0?job.wavelet.amplitudeScale:1.);
 h.insert(QStringLiteral("processedTraces"),job.processedTraces);h.insert(QStringLiteral("failedTraces"),job.failedTraces);
 h.insert(QStringLiteral("meanResidualEnergyRatio"),job.meanResidualEnergyRatio);
 h.insert(QStringLiteral("meanLowFreqVarianceFraction"),job.meanLowFreqVarianceFraction);
 h.insert(QStringLiteral("frequencyBand"),QStringLiteral("低频模型 0–%1Hz + 地震带限（不含带外高频）").arg(job.req.lowCutHz));
 if(job.req.method==QStringLiteral("sparse")){
  h.insert(QStringLiteral("lambdaUsed"),job.req.lambda);h.insert(QStringLiteral("maxIterations"),job.req.maxIterations);
 }return h;
}
bool invertOne(const InversionWorkflow::InversionJob& job,int il,int xl,const float* samples,float* output,double& statistic,QString& reason){
 std::vector<float> low(size_t(job.nS),NAN);const float* lowPtr=nullptr;
 if(job.hasLowFreq){inversion::lowFreqTraceAt(job.lowFreq,il,xl,low.data());
  if(std::any_of(low.begin(),low.end(),[](float v){return std::isfinite(v);}))lowPtr=low.data();}
 NativeCppAdapter adapter;
 if(job.req.method==QStringLiteral("sparse")){
  SparseSpikeInputs input;input.trace=samples;input.n=job.nS;input.sampleIntervalMs=job.dtMs;
  input.wavelet=&job.wavelet;input.lowFreq=lowPtr;input.options.lambda=job.req.lambda;input.options.maxIterations=job.req.maxIterations;
  inversion::SparseSpikeResult result;const auto run=adapter.runSparseSpike(input,&result,nullptr);
  if(!run.ok){reason=QString::fromStdString(result.reason);return false;}
  if(result.impedance.size()!=size_t(job.nS))return false;
  std::copy(result.impedance.begin(),result.impedance.end(),output);statistic=result.residualEnergyRatio;
 }else{
  BandlimitedInputs input;input.trace=samples;input.n=job.nS;input.sampleIntervalMs=job.dtMs;
  input.wavelet=&job.wavelet;input.lowFreq=lowPtr;input.options.lowCutHz=job.req.lowCutHz;
  inversion::BandlimitedResult result;const auto run=adapter.runBandlimited(input,&result,nullptr);
  if(!run.ok){reason=QString::fromStdString(result.reason);return false;}
  if(result.impedance.size()!=size_t(job.nS))return false;
  std::copy(result.impedance.begin(),result.impedance.end(),output);statistic=result.lowFreqVarianceFraction;
 }return std::any_of(output,output+job.nS,[](float v){return std::isfinite(v);});
}
}
bool computeRegisteredInversion(InversionWorkflow::InversionJob* job,
 const std::function<bool()>& cancelled,const std::function<void(double)>& progress){
 job->ok=false;job->processedTraces=job->failedTraces=0;
 job->meanResidualEnergyRatio=job->meanLowFreqVarianceFraction=0;
 const auto fail=[&](const QString& reason){job->error=reason;return false;};
 SeismicDataAccess source;QString reason;std::string error;data::SeismicVolume volume;
 if(!source.openProjectSource(job->req.projectDirectory,job->req.seismicPath,&reason))return fail(reason);
 if(!source.volume(volume,error))return fail(QString::fromStdString(error));
 if(!source.coordinatesMatchProject()||source.coordinateSystem()!=job->spatialEvidence.value(QStringLiteral("coordinateSystem")).toString()||
    volume.sampling.timeType!="TWT"||volume.sampling.timeReference!=job->req.timeReference.toStdString())
  return fail(QStringLiteral("地震体坐标或时间声明在准备后发生变化，未发布"));
 const double unit=volume.sampling.timeUnit=="s"?1000.:1.;
 const QString directory=QDir::temp().filePath(QStringLiteral("paleo-inv-%1").arg(QUuid::createUuid().toString(QUuid::Id128)));
 if(!QDir().mkpath(directory))return fail(QStringLiteral("反演临时目录创建失败"));
 job->tempVolumePath=QDir(directory).filePath(QStringLiteral("impedance.iimp"));
 // A failed/cancelled run owns only this freshly created directory.
 auto cleanup=qScopeGuard([&]{if(!job->ok)InversionWorkflow::reclaimTempVolume(job->tempVolumePath);});
 inversion::ImpedanceVolumeStream stream(job->tempVolumePath);
 if(!stream.open(headerFor(*job),&reason))return fail(reason);
 const int threads=std::clamp(job->req.maxThreads>0?job->req.maxThreads:4,1,4);
 double sum=0;QString firstFailure;
 for(int il=0;il<job->nIl;++il){
  if(cancelled&&cancelled())return fail(QStringLiteral("已取消"));
  const QFileInfo file(job->req.seismicPath);
  if(file.size()!=job->spatialEvidence.value(QStringLiteral("sourceBytes")).toLongLong()||
     file.lastModified().toMSecsSinceEpoch()!=job->spatialEvidence.value(QStringLiteral("sourceModifiedMs")).toLongLong())
   return fail(QStringLiteral("地震来源在反演期间变化，未发布"));
  data::SectionReadOptions options;options.fullResolution=true;options.maxValues=64000000;
  options.firstTime=job->t0Ms/unit;options.lastTime=(job->t0Ms+(job->nS-1)*job->dtMs)/unit;
  data::SeismicSectionResult line;
  if(!source.line(true,job->inlineMin+il*job->inlineStep,options,line,error,cancelled))return fail(QString::fromStdString(error));
  const double returnedUnit=line.timeUnit=="s"?1000.:line.timeUnit=="ms"?1.:NAN;
  if(line.decimated||line.samples!=job->nS||std::abs(line.firstTime*returnedUnit-job->t0Ms)>1e-6||
     !std::isfinite(returnedUnit)||std::abs(line.sampleInterval*returnedUnit-job->dtMs)>1e-6||
     line.crosslineNumbers.size()!=size_t(line.columns)||line.inlineNumbers.size()!=size_t(line.columns)||
     line.traceIndexes.size()!=size_t(line.columns)||line.amplitudes.size()!=size_t(line.columns)*job->nS)
   return fail(QStringLiteral("DataService 返回的测线网格/时间轴与反演请求不符"));
  std::vector<int> columns(size_t(job->nXl),-1);
  for(int c=0;c<line.columns;++c){
   if(line.inlineNumbers[size_t(c)]!=job->inlineMin+il*job->inlineStep)return fail(QStringLiteral("DataService 测线号不一致"));
   const int delta=line.crosslineNumbers[size_t(c)]-job->xlineMin;
   if(delta<0||delta%job->xlineStep)continue;const int xl=delta/job->xlineStep;
   if(xl>=job->nXl)continue;
   if(columns[size_t(xl)]>=0)return fail(QStringLiteral("DataService 返回重复网格道"));
   if(line.traceIndexes[size_t(c)]>=0)columns[size_t(xl)]=c;
  }
  std::vector<float> output(size_t(job->nXl)*job->nS,NAN);
  std::vector<double> stats(size_t(job->nXl),0);
  std::vector<unsigned char> success(size_t(job->nXl),0);
  std::vector<QString> failures(size_t(job->nXl));
  std::atomic<int> next{0};std::atomic<bool> aborted{false};WorkerExceptionSink exceptions;
  auto run=exceptions.wrap([&]{
   for(int xl=next.fetch_add(1);xl<job->nXl&&!aborted.load();xl=next.fetch_add(1)){
    if(cancelled&&cancelled()){aborted=true;break;}
    const int column=columns[size_t(xl)];if(column<0)continue;
    success[size_t(xl)]=invertOne(*job,il,xl,line.amplitudes.data()+size_t(column)*job->nS,
      output.data()+size_t(xl)*job->nS,stats[size_t(xl)],failures[size_t(xl)]);
   }
  });
  std::vector<std::thread> pool;
  auto join=qScopeGuard([&]{for(auto& thread:pool)if(thread.joinable())thread.join();});
  try{for(int t=1;t<threads;++t)pool.emplace_back(run);}catch(...){exceptions.capture(std::current_exception());aborted=true;}
  run();for(auto& thread:pool)if(thread.joinable())thread.join();
  if(exceptions.failed())return fail(QStringLiteral("反演工作线程异常，未发布"));
  if(aborted||(cancelled&&cancelled()))return fail(QStringLiteral("已取消"));
  const QFileInfo after(job->req.seismicPath);
  if(after.size()!=file.size()||after.lastModified()!=file.lastModified())return fail(QStringLiteral("地震来源在反演期间变化，未发布"));
  for(int xl=0;xl<job->nXl;++xl){++job->processedTraces;if(success[size_t(xl)])sum+=stats[size_t(xl)];else {
    ++job->failedTraces;if(firstFailure.isEmpty())firstFailure=failures[size_t(xl)];
  }}
  if(!stream.append(output.data(),qint64(output.size()),&reason))return fail(reason);
  if(progress)progress(double(il+1)/job->nIl*.98);
 }
 const int succeeded=job->processedTraces-job->failedTraces;
 if(succeeded==0)return fail(QStringLiteral("没有成功反演的有效地震道，未发布：%1").arg(firstFailure));
 if(!firstFailure.isEmpty())job->spatialEvidence.insert(QStringLiteral("firstTraceFailure"),firstFailure);
 if(job->req.method==QStringLiteral("sparse"))job->meanResidualEnergyRatio=sum/succeeded;
 else job->meanLowFreqVarianceFraction=sum/succeeded;
 if(cancelled&&cancelled())return fail(QStringLiteral("已取消"));
 if(!stream.finish(headerFor(*job),&reason))return fail(reason);
 job->ok=true;if(progress)progress(1);return true;
}
}
