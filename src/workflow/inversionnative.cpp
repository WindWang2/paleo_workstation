// 层：功能
#include "algorithms/inversion/lowfreq.h"
#include "inversionnative.h"
#include "services/seismicdataaccess.h"
#include "services/seismicwelltrace.h"
#include <QDir>
#include <QStorageInfo>
#include <QFileInfo>
#include <QDateTime>
#include <cmath>
#include <algorithm>

namespace paleo::inv {
namespace {
bool fail(QString* error,const QString& reason){if(error)*error=reason;return false;}
bool cropAxis(int min,int step,int count,int requestedMin,int requestedMax,int& begin,int& size){
 if(step<=0||count<=0||requestedMin>requestedMax)return false;
 const double first=std::ceil((double(requestedMin)-min)/step),last=std::floor((double(requestedMax)-min)/step);
 begin=int(std::clamp(first,0.,double(count)));const int end=int(std::clamp(last,-1.,double(count-1)));
 size=end-begin+1;return size>0;
}
std::vector<float> resample(const std::vector<double>& time,const std::vector<float>& values,double first,double dt,int n){
 std::vector<float> out(size_t(n),NAN);if(time.size()!=values.size()||time.size()<2)return out;
 size_t cursor=0;
 for(int i=0;i<n;++i){const double t=first+i*dt;
  while(cursor+1<time.size()&&time[cursor+1]<t)++cursor;
  if(cursor+1>=time.size()||t<time[cursor]||!std::isfinite(values[cursor])||!std::isfinite(values[cursor+1]))continue;
  const double f=(t-time[cursor])/(time[cursor+1]-time[cursor]);out[size_t(i)]=float(values[cursor]+f*(values[cursor+1]-values[cursor]));
 }return out;
}
bool buildLowFreq(InversionWorkflow::InversionJob* job,const data::SeismicCoordinateRange& geometry,
                  int ilOffset,int xlOffset,QString* error){
 if(job->req.lowFreqWells.empty()){job->hasLowFreq=false;return true;}
 inversion::LowFreqModelInput input;input.nIl=job->nIl;input.nXl=job->nXl;input.nS=job->nS;
 input.t0Ms=job->t0Ms;input.dtMs=job->dtMs;input.lowCutHz=job->req.lowCutHz;
 const auto& origin=geometry.corners[0];
 input.ilStepX=(geometry.corners[3].x-origin.x)/std::max(1,geometry.inlineCount-1);
 input.ilStepY=(geometry.corners[3].y-origin.y)/std::max(1,geometry.inlineCount-1);
 input.xlStepX=(geometry.corners[1].x-origin.x)/std::max(1,geometry.crosslineCount-1);
 input.xlStepY=(geometry.corners[1].y-origin.y)/std::max(1,geometry.crosslineCount-1);
 input.originX=origin.x+ilOffset*input.ilStepX+xlOffset*input.xlStepX;
 input.originY=origin.y+ilOffset*input.ilStepY+xlOffset*input.xlStepY;
 QVariantList evidence;
 for(const auto& well:job->req.lowFreqWells){
  if(well.timeReference!=job->req.timeReference||well.trajectory.size()<2)
   return fail(error,QStringLiteral("井 %1 缺同一时间零点的实际空间轨迹").arg(well.wellId));
  if(well.twtMs.size()!=well.impedance.size()||well.twtMs.size()<2)return fail(error,QStringLiteral("井阻抗时间轴无效"));
  for(size_t i=0;i<well.twtMs.size();++i)if(!std::isfinite(well.twtMs[i])||(i&&well.twtMs[i]<=well.twtMs[i-1]))
   return fail(error,QStringLiteral("井阻抗 TWT 必须严格递增"));
  inversion::LowFreqWellTrace trace;
  if(!wellPathGridPositions(well.trajectory,job->t0Ms,job->dtMs,job->nS,trace.xSamples,trace.ySamples,error))return false;
  trace.impedance=resample(well.twtMs,well.impedance,job->t0Ms,job->dtMs,job->nS);
  bool any=false;for(int s=0;s<job->nS;++s){const auto k=size_t(s);
   if(!std::isfinite(trace.xSamples[k])||!std::isfinite(trace.ySamples[k]))trace.impedance[k]=NAN;
   any=any||std::isfinite(trace.impedance[k]);}
  if(!any)return fail(error,QStringLiteral("井 %1 与反演时间窗口无共同有效覆盖").arg(well.wellId));
  input.wells.push_back(std::move(trace));
  auto item=trajectoryEvidence(well.trajectory,well.trajectorySource,well.timeReference);
  item.insert(QStringLiteral("wellId"),well.wellId);
  item.insert(QStringLiteral("impedanceSha256"),curveEvidenceHash(well.twtMs,well.impedance));evidence.append(item);
 }
 const double modelBytes=double(job->nIl)*job->nXl*(job->req.horizons.size()+1)*sizeof(float)*4;
 if(!job->req.horizons.empty()&&modelBytes>double(1ull<<30))return fail(error,QStringLiteral("层位低频模型超过 1 GiB 预算，请缩小反演范围"));
 for(const auto& horizon:job->req.horizons){
  if(horizon.twtMs.size()!=size_t(geometry.inlineCount)*geometry.crosslineCount)return fail(error,QStringLiteral("层位网格与原始地震测网不匹配"));
  inversion::HorizonTwtGrid cropped;cropped.name=horizon.name;cropped.twtMs.reserve(size_t(job->nIl)*job->nXl);
  for(int i=0;i<job->nIl;++i)for(int j=0;j<job->nXl;++j)
   cropped.twtMs.push_back(horizon.twtMs[size_t(i+ilOffset)*geometry.crosslineCount+j+xlOffset]);
  input.horizons.push_back(std::move(cropped));
 }
 job->lowFreq=inversion::lowFreqImpedance(input);
 if(!job->lowFreq.ok)return fail(error,QString::fromStdString(job->lowFreq.reason));
 job->hasLowFreq=true;job->spatialEvidence.insert(QStringLiteral("wellConstraints"),evidence);
 job->spatialEvidence.insert(QStringLiteral("lowFrequencyPositions"),input.horizons.empty()?QStringLiteral("xy_at_each_twt"):QStringLiteral("trajectory_layer_centroid"));
 return true;
}
}
bool prepareRegisteredInversion(const InversionWorkflow::InversionJobRequest& req,
 const inversion::Wavelet& wavelet,InversionWorkflow::InversionJob* job,QString* error){
 SeismicDataAccess source;std::string detail;data::SeismicVolume volume;data::SeismicCoordinateRange grid;
 if(!source.openProjectSource(req.projectDirectory,req.seismicPath,error))return false;
 if(!source.volume(volume,detail)||!source.range(grid,detail))return fail(error,QString::fromStdString(detail));
 if(!source.coordinatesMatchProject())return fail(error,QStringLiteral("反演井与地震体需要同一工程米制坐标系"));
 if(volume.sampling.timeType!="TWT"||req.timeReference.isEmpty()||volume.sampling.timeReference!=req.timeReference.toStdString())
  return fail(error,QStringLiteral("地震体与井约束必须采用同一 TWT 时间零点"));
 if(grid.status!="complete"||!grid.affineValid||grid.corners.size()!=4||grid.coordinateUnit!="m")
  return fail(error,QStringLiteral("反演需要可验证的完整米制地震测网"));
 const double scale=volume.sampling.timeUnit=="ms"?1.:volume.sampling.timeUnit=="s"?1000.:NAN;
 const double first=volume.sampling.firstTime*scale,dt=volume.sampling.sampleInterval*scale;
 const int count=volume.sampling.sampleCount;
 if(!std::isfinite(first)||!std::isfinite(dt)||dt<=0||count<16)return fail(error,QStringLiteral("地震体共同时间采样轴无效"));
 if(std::abs(wavelet.sampleIntervalMs-dt)>1e-9)return fail(error,QStringLiteral("子波与地震体采样间隔不一致"));
 int ilOffset=0,xlOffset=0;
 if(!cropAxis(grid.inlineMin,grid.inlineStep,grid.inlineCount,req.firstInline,req.lastInline,ilOffset,job->nIl)||
    !cropAxis(grid.crosslineMin,grid.crosslineStep,grid.crosslineCount,req.firstCrossline,req.lastCrossline,xlOffset,job->nXl))
  return fail(error,QStringLiteral("请求范围与地震测网不相交"));
 const int start=std::isfinite(req.firstTimeMs)?int(std::clamp(std::ceil((req.firstTimeMs-first)/dt),0.,double(count))):0;
 const int end=std::isfinite(req.lastTimeMs)?int(std::clamp(std::floor((req.lastTimeMs-first)/dt),-1.,double(count-1))):count-1;
 job->nS=end-start+1;job->t0Ms=first+start*dt;job->dtMs=dt;
 if(job->nS<16)return fail(error,QStringLiteral("反演时间窗口少于 16 个真实样点"));
 // DDS reads one complete inline; budget includes its full crossline extent.
 if(qint64(grid.crosslineCount)*job->nS>64000000)return fail(error,QStringLiteral("单测线超过 DataService 读取预算，请缩短时间窗口"));
 const double outputBytes=double(job->nIl)*job->nXl*job->nS*4+65546;
 for(const auto& path:{QDir::tempPath(),req.projectDirectory}){
  const QStorageInfo disk(path);
  if(disk.isValid()&&disk.bytesAvailable()>=0&&double(disk.bytesAvailable())<outputBytes*2+64*1024*1024)
   return fail(error,QStringLiteral("反演落盘与发布需要约 %1 GiB 可用空间，当前不足；请缩小范围")
      .arg(outputBytes*2/double(1ull<<30),0,'f',2));
 }
 job->inlineMin=grid.inlineMin+ilOffset*grid.inlineStep;job->inlineStep=grid.inlineStep;
 job->xlineMin=grid.crosslineMin+xlOffset*grid.crosslineStep;job->xlineStep=grid.crosslineStep;
 job->wavelet=wavelet;job->dataServiceSource=true;
 job->spatialEvidence={{QStringLiteral("reader"),QStringLiteral("DataService_inline_stream")},
  {QStringLiteral("coordinateSystem"),QString::fromStdString(grid.coordinateSystem)},
  {QStringLiteral("timeReference"),req.timeReference},{QStringLiteral("outputBytes"),outputBytes},
  {QStringLiteral("timeUnit"),QStringLiteral("ms")},{QStringLiteral("spatialDecimation"),false}};
 const QFileInfo file(req.seismicPath);
 job->spatialEvidence.insert(QStringLiteral("sourceBytes"),file.size());
 job->spatialEvidence.insert(QStringLiteral("sourceModifiedMs"),file.lastModified().toMSecsSinceEpoch());
 const auto& origin=grid.corners[0];
 const double ix=(grid.corners[3].x-origin.x)/std::max(1,grid.inlineCount-1),iy=(grid.corners[3].y-origin.y)/std::max(1,grid.inlineCount-1);
 const double xx=(grid.corners[1].x-origin.x)/std::max(1,grid.crosslineCount-1),xy=(grid.corners[1].y-origin.y)/std::max(1,grid.crosslineCount-1);
 job->spatialEvidence.insert(QStringLiteral("originX"),origin.x+ilOffset*ix+xlOffset*xx);
 job->spatialEvidence.insert(QStringLiteral("originY"),origin.y+ilOffset*iy+xlOffset*xy);
 job->spatialEvidence.insert(QStringLiteral("ilStepX"),ix);job->spatialEvidence.insert(QStringLiteral("ilStepY"),iy);
 job->spatialEvidence.insert(QStringLiteral("xlStepX"),xx);job->spatialEvidence.insert(QStringLiteral("xlStepY"),xy);
 if(!buildLowFreq(job,grid,ilOffset,xlOffset,error))return false;
 job->prepared=true;return true;
}
}
