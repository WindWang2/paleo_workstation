// 层：数据
#include "wellsection.h"
#include <algorithm>
#include <cmath>

namespace wellsection {
QVector<Link> correlationLinks(const Well &a,const Well &b,const SeismicGap *gap){
  if(!gap||(gap->horizons.curves.empty()&&gap->horizons.schemeId.isEmpty()))return links(a,b);
  QVector<Link> result;
  if(!gap->valid()||!a.timeDepth||!b.timeDepth)return result;
  for(const auto &curve:gap->horizons.curves){
    if(curve.internalEdge||curve.timesMs.size()!=size_t(gap->columns)||curve.timesMs.size()<2)continue;
    const auto left=a.timeDepth->model.TwtMsToDepth(curve.timesMs.front()-a.timeDepth->shiftMs);
    const auto right=b.timeDepth->model.TwtMsToDepth(curve.timesMs.back()-b.timeDepth->shiftMs);
    if(std::isfinite(left)&&std::isfinite(right))result.append({curve.name,left,right});
  }
  std::sort(result.begin(),result.end(),[](const auto &a,const auto &b){return a.leftMd<b.leftMd;});
  return result;
}
double gapDisplayDepthAtTwt(const Well& a,double offA,const Well& b,double offB,
                            double fraction,double twtMs,double top,double base,DepthDomain domain){
  double lo=top,hi=base;
  if(!a.timeDepth||!b.timeDepth||!std::isfinite(twtMs)||!std::isfinite(fraction)||fraction<0||fraction>1||!(hi>lo))return qQNaN();
  const auto clip=[&](const Well& well,double offset){
    const auto& points=well.timeDepth->model.points();
    if(points.empty())return;
    const auto display=[&](double md){return (domain==DepthDomain::TVD?well.tvdOf(md):md)-offset;};
    lo=std::max(lo,display(points.front().depthM));hi=std::min(hi,display(points.back().depthM));
  };
  clip(a,offA);clip(b,offB);
  if(!(hi>=lo))return qQNaN();
  const double topTime=gapTwtMs(a,offA,b,offB,fraction,lo,domain),baseTime=gapTwtMs(a,offA,b,offB,fraction,hi,domain);
  if(!std::isfinite(topTime)||!std::isfinite(baseTime)||twtMs<topTime||twtMs>baseTime||baseTime<topTime)return qQNaN();
  for(int step=0;step<32;++step){
    const double mid=(lo+hi)/2,t=gapTwtMs(a,offA,b,offB,fraction,mid,domain);
    if(!std::isfinite(t))return qQNaN();
    if(t<twtMs)lo=mid;else hi=mid;
  }
  return (lo+hi)/2;
}
}
