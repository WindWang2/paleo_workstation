// 层：数据
#include "domain/seismic/sectionwellprojector.h"
#include "domain/wellnumeric.h"

#include <algorithm>
#include <cmath>
#include <limits>

namespace seismic {
namespace {
WellTrajSample interpolate(const WellTrajSample& a, const WellTrajSample& b, double f) {
    WellTrajSample p;
    const auto mix=[f](double x,double y){return x+(y-x)*f;};
    p.md=mix(a.md,b.md);p.tvd=mix(a.tvd,b.tvd);p.x=mix(a.x,b.x);p.y=mix(a.y,b.y);
    p.twtMs=mix(a.twtMs,b.twtMs);p.tracePosition=mix(a.tracePosition,b.tracePosition);
    p.offsetDistanceM=mix(a.offsetDistanceM,b.offsetDistanceM);p.sourceZ=mix(a.sourceZ,b.sourceZ);return p;
}
WellTrajSample pathAt(const SectionWellInfo& well,double value,bool time) {
    WellTrajSample missing; missing.md=missing.tvd=missing.x=missing.y=missing.twtMs=qQNaN();
    const auto& points=well.trajectory;
    const auto key=[time](const WellTrajSample& p){return time?p.twtMs:p.md;};
    if(points.empty()||!std::isfinite(value)||value<key(points.front())||value>key(points.back()))return missing;
    auto hi=std::lower_bound(points.begin(),points.end(),value,[&](const auto& p,double v){return key(p)<v;});
    if(hi==points.end())return missing;
    if(key(*hi)==value)return *hi;
    if(hi==points.begin())return missing;
    const auto& a=*(hi-1);const auto& b=*hi;const double span=key(b)-key(a);
    if(!(span>0))return missing;
    return interpolate(a,b,(value-key(a))/span);
}
double horizonTime(const SectionHorizonCurve& curve,double trace) {
    if(!std::isfinite(trace)||trace<0||trace>double(curve.timesMs.size())-1)return qQNaN();
    const auto lo=size_t(std::floor(trace)),hi=size_t(std::ceil(trace));
    const double a=curve.timesMs[lo],b=curve.timesMs[hi];return a+(b-a)*(trace-double(lo));
}
std::optional<WellTrajSample> pathHorizonIntersection(const SectionWellInfo& well,const SectionHorizonCurve& curve) {
    for(size_t i=1;i<well.trajectory.size();++i){
        const auto& a=well.trajectory[i-1];const auto& b=well.trajectory[i];
        if(!std::isfinite(a.tracePosition)||!std::isfinite(b.tracePosition)||!std::isfinite(a.twtMs)||!std::isfinite(b.twtMs)||b.md<=a.md)continue;
        std::vector<double> splits{0,1};const double dx=b.tracePosition-a.tracePosition;
        if(std::abs(dx)>1e-12)for(int x=int(std::floor(std::min(a.tracePosition,b.tracePosition)))+1;x<std::max(a.tracePosition,b.tracePosition);++x)
            splits.push_back((x-a.tracePosition)/dx);
        std::sort(splits.begin(),splits.end());
        for(size_t j=1;j<splits.size();++j){
            const auto left=interpolate(a,b,splits[j-1]),right=interpolate(a,b,splits[j]);
            const double u=left.twtMs-horizonTime(curve,left.tracePosition),v=right.twtMs-horizonTime(curve,right.tracePosition);
            if(!std::isfinite(u)||!std::isfinite(v)||u*v>0)continue;
            return interpolate(left,right,std::abs(v-u)>1e-12?-u/(v-u):0);
        }
    }
    return std::nullopt;
}
}
WellTrajSample wellPathAtMd(const SectionWellInfo& well,double md){return pathAt(well,md,false);}
WellTrajSample wellPathAtTwt(const SectionWellInfo& well,double twt){
    if(well.mdTimeDepth)return pathAt(well,well.mdTimeDepth->TwtMsToDepth(twt-well.timeShiftMs),false);
    return pathAt(well,twt,true);
}

SectionPointProjection projectPointOnSection(const std::vector<glm::dvec2>& line,
    const std::vector<double>& distances,const glm::dvec2& point) {
    SectionPointProjection out;if(line.size()<2||distances.empty()||!std::isfinite(point.x)||!std::isfinite(point.y))return out;
    double best=std::numeric_limits<double>::infinity(),cum=0;
    for(size_t i=1;i<line.size();++i){
        const auto a=line[i-1],v=line[i]-a;const double length=glm::length(v);if(length<1e-9)continue;
        const double f=std::clamp(glm::dot(point-a,v)/(length*length),0.,1.);
        const double distance=glm::length(point-a-f*v);
        if(distance<best){best=distance;out.cumulativeDistanceM=cum+f*length;
            out.offsetDistanceM=(v.x*(point.y-a.y)-v.y*(point.x-a.x)>=0?1:-1)*distance;}
        cum+=length;
    }
    if(!std::isfinite(best))return out;
    const auto hi=std::lower_bound(distances.begin(),distances.end(),out.cumulativeDistanceM);
    if(hi==distances.begin())out.tracePosition=0;
    else if(hi==distances.end())out.tracePosition=double(distances.size()-1);
    else {const auto index=size_t(hi-distances.begin());const double span=distances[index]-distances[index-1];
        if(span>0)out.tracePosition=double(index-1)+(out.cumulativeDistanceM-distances[index-1])/span;}
    return out;
}
void projectSectionHorizonTies(std::vector<SectionWellInfo>& wells,const SectionHorizons& horizons){
    for(auto &well:wells){
        if(!well.mdTimeDepth)continue;
        well.tops.clear();
        for(const auto &curve:horizons.curves){
            if(curve.internalEdge||curve.timesMs.empty())continue;
            if(!well.trajectory.empty()){
                const auto hit=pathHorizonIntersection(well,curve);if(!hit)continue;
                WellTopItem top;top.topName=curve.name;top.md=hit->md;top.tvd=hit->tvd;top.twtMs=hit->twtMs;
                top.tracePosition=hit->tracePosition;well.tops.push_back(top);continue;
            }
            if(well.requiresSpatialPath)continue;
            if(curve.internalEdge||curve.timesMs.empty()||!std::isfinite(well.tracePosition)||well.tracePosition<0||well.tracePosition>double(curve.timesMs.size()-1))continue;
            const auto lo=size_t(std::floor(well.tracePosition)),hi=size_t(std::ceil(well.tracePosition));
            const double a=curve.timesMs[lo],b=curve.timesMs[hi];if(!std::isfinite(a)||!std::isfinite(b))continue;
            const double time=a+(b-a)*(well.tracePosition-double(lo));
            const double md=well.mdTimeDepth->TwtMsToDepth(time-well.timeShiftMs);
            if(!std::isfinite(md))continue;
            WellTopItem top;top.topName=curve.name;top.md=md;top.tvd=qQNaN();top.twtMs=time;well.tops.push_back(top);
        }
    }
}

std::vector<SectionWellInfo> SectionWellProjector::ProjectWells(
    const std::vector<glm::dvec2> &polylinePoints,
    const std::vector<double> &vertexDistancesM,
    const std::vector<double> &traceDistancesM,
    const std::vector<SectionWellInfo> &wells,
    double bufferDistanceM,
    const TimeDepthModel &tdModel)
{
    std::vector<SectionWellInfo> results;
    if (polylinePoints.size() < 2 || wells.empty())
        return results;

    // Calculate vertex distances if not provided
    std::vector<double> vDist = vertexDistancesM;
    if (vDist.size() != polylinePoints.size()) {
        // 尺寸不匹配 = 调用方给的累计距离不可信，整体重算。用 assign 而非
        // resize：resize 只初始化新增元素，原样保留的前若干个值是调用方残留
        // （vDist[0] 尤其——重算循环从 i=1 起，永不清它），距离基准被污染后
        // 井位沿剖面距离与 tracePosition 静默错值（finding-37）。
        vDist.assign(polylinePoints.size(), 0.0);
        double cum = 0.0;
        for (std::size_t i = 1; i < polylinePoints.size(); ++i) {
            cum += glm::length(polylinePoints[i] - polylinePoints[i - 1]);
            vDist[i] = cum;
        }
    }

    const double totalLengthM = vDist.back();
    const int totalTraces = static_cast<int>(traceDistancesM.size());

    results.reserve(wells.size());
    for (const auto &w : wells) {
        SectionWellInfo proj = w;
        const glm::dvec2 wellPos(w.surfaceX, w.surfaceY);

        double minDistance = std::numeric_limits<double>::infinity();
        double bestCumDist = 0.0;
        double bestOffsetSigned = 0.0;

        for (std::size_t i = 0; i + 1 < polylinePoints.size(); ++i) {
            const glm::dvec2 p0 = polylinePoints[i];
            const glm::dvec2 p1 = polylinePoints[i + 1];
            const glm::dvec2 v = p1 - p0;
            const double len = glm::length(v);
            if (len < 1e-6)
                continue;

            const glm::dvec2 toWell = wellPos - p0;
            const double t = std::clamp(glm::dot(toWell, v) / (len * len), 0.0, 1.0);
            const glm::dvec2 q = p0 + t * v;
            const double dist = glm::length(wellPos - q);

            if (dist < minDistance) {
                minDistance = dist;
                bestCumDist = vDist[i] + t * (vDist[i + 1] - vDist[i]);
                // Sign: cross product (v.x * toWell.y - v.y * toWell.x)
                const double cross = v.x * toWell.y - v.y * toWell.x;
                bestOffsetSigned = (cross >= 0.0 ? 1.0 : -1.0) * dist;
            }
        }

        proj.cumulativeDistanceM = bestCumDist;
        proj.offsetDistanceM = bestOffsetSigned;
        proj.isWithinBuffer = (std::abs(minDistance) <= bufferDistanceM);

        // Map cumulative distance to trace index
        if (totalTraces > 1 && !traceDistancesM.empty()) {
            auto it = std::lower_bound(traceDistancesM.begin(), traceDistancesM.end(), bestCumDist);
            if (it == traceDistancesM.begin()) {
                proj.tracePosition = 0.0;
            } else if (it == traceDistancesM.end()) {
                proj.tracePosition = static_cast<double>(totalTraces - 1);
            } else {
                const std::size_t idx = std::distance(traceDistancesM.begin(), it);
                const double d0 = traceDistancesM[idx - 1];
                const double d1 = traceDistancesM[idx];
                const double span = std::max(1e-4, d1 - d0);
                const double frac = std::clamp((bestCumDist - d0) / span, 0.0, 1.0);
                proj.tracePosition = static_cast<double>(idx - 1) + frac;
            }
        } else if (totalLengthM > 1e-4 && totalTraces > 0) {
            proj.tracePosition = (bestCumDist / totalLengthM) * static_cast<double>(totalTraces - 1);
        } else {
            proj.tracePosition = 0.0;
        }

        // Calibrate Tops
        for (auto &top : proj.tops) {
          if (!proj.calibrated && top.twtMs <= 0.0) {
            const double depth = top.tvd > 0.0 ? top.tvd : top.md;
            top.twtMs = tdModel.DepthToTwtMs(depth);
          }
        }

        // Calibrate Curves
        for (auto &curve : proj.curves) {
          if (!proj.calibrated && curve.twtMs.size() != curve.depthsM.size()) {
            curve.twtMs.clear();
            curve.twtMs.reserve(curve.depthsM.size());
            for (double d : curve.depthsM) {
              curve.twtMs.push_back(tdModel.DepthToTwtMs(d));
            }
          }
            if (!curve.values.empty()) {
                float vMin = std::numeric_limits<float>::infinity();
                float vMax = -std::numeric_limits<float>::infinity();
                for (float v : curve.values) {
                    if (!paleo::wellnumeric::isNullCurveValue(v)) {
                        vMin = std::min(vMin, v);
                        vMax = std::max(vMax, v);
                    }
                }
                if (vMin < vMax) {
                    curve.minVal = vMin;
                    curve.maxVal = vMax;
                }
            }
        }

        if(!proj.trajectory.empty()){
            proj.isWithinBuffer=false;double closest=std::numeric_limits<double>::infinity();
            for(auto& sample:proj.trajectory){
                const auto position=projectPointOnSection(polylinePoints,traceDistancesM,{sample.x,sample.y});
                sample.tracePosition=position.tracePosition;sample.offsetDistanceM=position.offsetDistanceM;
                if(!std::isfinite(sample.twtMs)||sample.twtMs<0)continue;
                if(std::abs(position.offsetDistanceM)<closest){closest=std::abs(position.offsetDistanceM);proj.offsetDistanceM=position.offsetDistanceM;}
                if(std::isfinite(sample.tracePosition)&&std::abs(position.offsetDistanceM)<=bufferDistanceM)proj.isWithinBuffer=true;
            }
        }else if(proj.requiresSpatialPath)proj.isWithinBuffer=false;
        results.push_back(proj);
    }

    return results;
}

} // namespace seismic
