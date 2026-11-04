// 层：数据
#include "seismicwelltrace.h"
#include "seismicdataaccess.h"
#include <algorithm>
#include <cmath>
#include <limits>

namespace paleo {
bool wellPathGridPositions(const std::vector<seismic::WellTrajSample>& path,double first,double step,int count,
                          std::vector<double>& x,std::vector<double>& y,QString *error){
 x.clear();y.clear();if(error)error->clear();
 if(path.size()<2||count<1||!std::isfinite(first)||!std::isfinite(step)||step<=0){if(error)*error=QStringLiteral("沿井时间网格或轨迹无效");return false;}
 double previous=-INFINITY;
 for(const auto& p:path)if(std::isfinite(p.twtMs)){
  if(p.twtMs<=previous){if(error)*error=QStringLiteral("沿井 TWT 非严格递增，不能作为单值子波/反演轨迹");return false;}previous=p.twtMs;
 }
 x.assign(size_t(count),NAN);y.assign(size_t(count),NAN);
 for(size_t i=1;i<path.size();++i){const auto& a=path[i-1];const auto& b=path[i];
  if(!std::isfinite(a.x)||!std::isfinite(a.y)||!std::isfinite(b.x)||!std::isfinite(b.y)||
     !std::isfinite(a.twtMs)||!std::isfinite(b.twtMs)||b.twtMs<=a.twtMs)continue;
  const int lo=int(std::clamp(std::ceil((a.twtMs-first)/step),0.,double(count)));
  const int hi=int(std::clamp(std::floor((b.twtMs-first)/step),-1.,double(count-1)));
  for(int j=lo;j<=hi;++j){const double f=(first+j*step-a.twtMs)/(b.twtMs-a.twtMs);
   x[size_t(j)]=a.x+(b.x-a.x)*f;y[size_t(j)]=a.y+(b.y-a.y)*f;}
 }
 return true;
}
namespace {
bool pointGrid(const data::SeismicCoordinateRange& g,double x,double y,int& il,int& xl){
 if(!g.affineValid||g.corners.size()!=4)return false;
 const auto& p=g.corners[0];const double ax=(g.corners[3].x-p.x)/std::max(1,g.inlineCount-1);
 const double ay=(g.corners[3].y-p.y)/std::max(1,g.inlineCount-1);
 const double bx=(g.corners[1].x-p.x)/std::max(1,g.crosslineCount-1),by=(g.corners[1].y-p.y)/std::max(1,g.crosslineCount-1);
 const double det=ax*by-ay*bx;if(std::abs(det)<1e-12)return false;
 const double a=((x-p.x)*by-(y-p.y)*bx)/det,b=(ax*(y-p.y)-ay*(x-p.x))/det;
 if(a<-.5||a>g.inlineCount-.5||b<-.5||b>g.crosslineCount-.5)return false;
 il=g.inlineMin+std::clamp(int(std::llround(a)),0,g.inlineCount-1)*g.inlineStep;
 xl=g.crosslineMin+std::clamp(int(std::llround(b)),0,g.crosslineCount-1)*g.crosslineStep;return true;
}
float temporal(const data::SeismicSectionResult& section,int column,double time){
 if(column<0||column>=section.columns||section.samples<1||section.sampleInterval<=0)return NAN;
 if(size_t(column)<section.traceIndexes.size()&&section.traceIndexes[size_t(column)]<0)return NAN;
 const double t=(time-section.firstTime)/section.sampleInterval;
 if(t<0||t>section.samples-1)return NAN;
 const auto lo=int(std::floor(t)),hi=int(std::ceil(t));
 const float a=section.amplitudes[size_t(column)*section.samples+lo],b=section.amplitudes[size_t(column)*section.samples+hi];
 return std::isfinite(a)&&std::isfinite(b)?float(a+(b-a)*(t-lo)):NAN;
}
}
bool SeismicDataAccess::wellTrace(const std::vector<seismic::WellTrajSample>& path,const QString& reference,
                                SeismicWellTrace& result,std::string& error,const data::CancelCheck& cancel){
 result={};error.clear();data::SeismicVolume native;
 if(!volume(native,error))return false;
 if(native.sampling.timeType!="TWT"||reference.isEmpty()||native.sampling.timeReference!=reference.toStdString()){
  error="井轨迹与地震体需要同一已声明 TWT 时间零点";return false;}
 if(m_unit!="m"||m_service.projectCrsUnit()!="m"||m_crs!=m_service.projectCrs()){
  error="沿井取道需要地震体与工程采用同一米制坐标系";return false;}
 const double scale=native.sampling.timeUnit=="s"?1000.:native.sampling.timeUnit=="ms"?1.:NAN;
 const double first=native.sampling.firstTime*scale,step=native.sampling.sampleInterval*scale;
 const int count=native.sampling.sampleCount;
 if(count<2||count>1000000||!std::isfinite(first)||!std::isfinite(step)||step<=0){error="地震时间网格无效或超出沿井取样预算";return false;}
 std::vector<double> x,y;QString reason;if(!wellPathGridPositions(path,first,step,count,x,y,&reason)){error=reason.toStdString();return false;}
 SeismicWellTrace out;out.firstTimeMs=first;out.sampleIntervalMs=step;out.amplitudes.assign(size_t(count),NAN);
 out.coordinateSystem=QString::fromStdString(m_crs);out.timeReference=reference;
 size_t begin=0;
 while(begin<path.size()){
  const auto valid=[](const auto& p){return std::isfinite(p.x)&&std::isfinite(p.y)&&std::isfinite(p.twtMs);};
  while(begin<path.size()&&!valid(path[begin]))++begin;
  size_t end=begin;while(end<path.size()&&valid(path[end]))++end;
  if(end-begin<2){begin=end+size_t(end<path.size());continue;}
  if(cancel&&cancel()){error="沿井取道已取消";return false;}
  std::vector<std::pair<double,double>> xy;std::vector<double> lengths(end-begin,0);
  for(size_t i=begin;i<end;++i){
   // Checkshots may repeat XY over a vertical interval. Keep every MD/TWT
   // checkpoint, but a geometric curtain must not contain zero-length edges.
   if(xy.empty()||std::hypot(path[i].x-xy.back().first,path[i].y-xy.back().second)>1e-6)xy.push_back({path[i].x,path[i].y});
   if(i>begin)lengths[i-begin]=lengths[i-begin-1]+std::hypot(path[i].x-path[i-1].x,path[i].y-path[i-1].y);
  }
  data::SeismicSectionResult curtain;
  const bool stationary=lengths.back()<1e-6;
  if(stationary){data::SeismicCoordinateRange geometry;int il=0,xl=0;
   if(!range(geometry,error,cancel)||!pointGrid(geometry,xy[0].first,xy[0].second,il,xl)){error="井位置不在可验证地震测网内";return false;}
   const int next=il+std::max(1,geometry.inlineStep);
   if(!section({{il,xl},{next,xl}},2,false,true,curtain,error,cancel))return false;
  }else if(!xySection(xy,reference,curtain,error,cancel))return false;
  if(curtain.timeUnit=="s"){curtain.firstTime*=1000;curtain.sampleInterval*=1000;curtain.timeUnit="ms";}
  if(curtain.timeUnit!="ms"||!std::isfinite(curtain.firstTime)||!std::isfinite(curtain.sampleInterval)||
     std::abs(curtain.firstTime-first)>1e-6||std::abs(curtain.sampleInterval-step)>1e-6){error="沿井取道返回的时间轴与已登记地震体不一致";return false;}
  out.sourceColumns+=curtain.columns;
  if(curtain.samples<1||curtain.columns<1||curtain.amplitudes.size()!=size_t(curtain.samples)*curtain.columns){error="沿井地震剖面返回尺寸无效";return false;}
  for(size_t segment=begin+1;segment<end;++segment){const auto& a=path[segment-1];const auto& b=path[segment];
   const int lo=int(std::clamp(std::ceil((a.twtMs-first)/step),0.,double(count)));
   const int hi=int(std::clamp(std::floor((b.twtMs-first)/step),-1.,double(count-1)));
   for(int i=lo;i<=hi;++i){if((i%64)==0&&cancel&&cancel()){error="沿井取道已取消";return false;}
    const double time=first+i*step,f=(time-a.twtMs)/(b.twtMs-a.twtMs);
    const double d=lengths[segment-begin-1]+f*(lengths[segment-begin]-lengths[segment-begin-1]);
    int col=0;if(!stationary){
     if(curtain.distances.size()!=size_t(curtain.columns)){error="沿井剖面缺少真实路径距离轴";return false;}
     const auto at=std::lower_bound(curtain.distances.begin(),curtain.distances.end(),d);
     if(at==curtain.distances.end())col=curtain.columns-1;
     else if(at!=curtain.distances.begin())col=(d-*(at-1)<=*at-d)?int(at-curtain.distances.begin()-1):int(at-curtain.distances.begin());
    }
    out.amplitudes[size_t(i)]=temporal(curtain,col,time);
   }
  }
  begin=end;
 }
 for(float value:out.amplitudes)std::isfinite(value)?++out.finiteSamples:++out.missingSamples;
 if(out.finiteSamples<2){error="真实井轨迹与地震体无足够共同覆盖";return false;}
 result=std::move(out);return true;
}
}
