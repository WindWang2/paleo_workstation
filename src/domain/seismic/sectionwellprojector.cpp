// 层：数据
#include "domain/seismic/sectionwellprojector.h"
#include "domain/wellnumeric.h"

#include <algorithm>
#include <cmath>
#include <limits>

namespace seismic {
namespace {
WellTrajSample interpolate(const WellTrajSample& a, const WellTrajSample& b, double f) {
    if (f == 0) return a;
    if (f == 1) return b;
    WellTrajSample p;
    const auto mix=[f](double x,double y){return x+(y-x)*f;};
    p.md=mix(a.md,b.md);p.tvd=mix(a.tvd,b.tvd);p.x=mix(a.x,b.x);p.y=mix(a.y,b.y);
    p.twtMs=mix(a.twtMs,b.twtMs);p.tracePosition=mix(a.tracePosition,b.tracePosition);
    p.offsetDistanceM=mix(a.offsetDistanceM,b.offsetDistanceM);p.sourceZ=mix(a.sourceZ,b.sourceZ);return p;
}
WellTrajSample pathAt(const SectionWellInfo& well,double value,bool time) {
    WellTrajSample missing; missing.md=missing.tvd=missing.x=missing.y=missing.twtMs=qQNaN();
    const auto& points=well.trajectory;
    if (points.empty() || !std::isfinite(value) || value<0) return missing;
    if (time) {
        // Missing TWT stations break the time axis into runs. A binary search
        // over the full path could skip a valid run after a NaN.
        for (size_t i=0; i<points.size(); ++i) {
            const auto& point=points[i];
            if (point.twtMs==value) return point;
            if (i==0 || !(point.twtMs>value)) continue;
            const auto& previous=points[i-1];
            if (previous.twtMs>=0 && previous.twtMs<value && point.md>previous.md && std::isfinite(point.twtMs))
                return interpolate(previous,point,(value-previous.twtMs)/(point.twtMs-previous.twtMs));
        }
        return missing;
    }
    auto hi=std::lower_bound(points.begin(),points.end(),value,[](const auto& p,double v){return p.md<v;});
    if(hi==points.end() || !std::isfinite(hi->twtMs) || hi->twtMs<0) return missing;
    if(hi->md==value)return *hi;
    if(hi==points.begin())return missing;
    const auto& a=*(hi-1);const double span=hi->md-a.md;
    if(!(span>0)||!std::isfinite(a.twtMs)||a.twtMs<0)return missing;
    return interpolate(a,*hi,(value-a.md)/span);
}
double horizonTime(const SectionHorizonCurve& curve,double trace) {
    if(!std::isfinite(trace)||trace<0||trace>double(curve.timesMs.size())-1)return qQNaN();
    const auto lo=size_t(std::floor(trace)),hi=size_t(std::ceil(trace));
    const double a=curve.timesMs[lo],b=curve.timesMs[hi];return a+(b-a)*(trace-double(lo));
}
std::vector<WellTopItem> pathHorizonIntersections(const SectionWellInfo& well,const SectionHorizonCurve& curve) {
    std::vector<WellTopItem> hits;
    for(size_t i=1;i<well.trajectory.size();++i){
        const auto clipped = clipWellPathSegment(well.trajectory[i-1], well.trajectory[i],
            double(curve.timesMs.size())-1, std::numeric_limits<double>::infinity());
        if (!clipped) continue;
        const auto& [a,b] = *clipped;
        std::vector<double> splits{0,1};const double dx=b.tracePosition-a.tracePosition;
        if (std::abs(dx)>1e-12) {
            const double first=std::max(0.,std::ceil(std::min(a.tracePosition,b.tracePosition)));
            const double last=std::min(double(curve.timesMs.size())-1,std::floor(std::max(a.tracePosition,b.tracePosition)));
            for (double x=first;x<=last;++x) {
                const double f=(x-a.tracePosition)/dx;
                if (f>0 && f<1) splits.push_back(f);
            }
        }
        std::sort(splits.begin(),splits.end());
        for(size_t j=1;j<splits.size();++j){
            const auto left=interpolate(a,b,splits[j-1]),right=interpolate(a,b,splits[j]);
            const double u=left.twtMs-horizonTime(curve,left.tracePosition),v=right.twtMs-horizonTime(curve,right.tracePosition);
            if(!std::isfinite(u)||!std::isfinite(v)||u*v>0)continue;
            const auto hit=interpolate(left,right,std::abs(v-u)>1e-12?-u/(v-u):0);
            if (!hits.empty() && std::abs(hits.back().md-hit.md)<1e-8) continue;
            WellTopItem top;
            top.topName=curve.name;top.md=hit.md;top.tvd=hit.tvd;top.twtMs=hit.twtMs;
            top.tracePosition=hit.tracePosition;top.offsetDistanceM=hit.offsetDistanceM;
            hits.push_back(std::move(top));
        }
    }
    return hits;
}
}
WellTrajSample wellPathAtMd(const SectionWellInfo& well,double md){return pathAt(well,md,false);}
WellTrajSample wellPathAtTwt(const SectionWellInfo& well,double twt){
    if(well.mdTimeDepth)return pathAt(well,well.mdTimeDepth->TwtMsToDepth(twt-well.timeShiftMs),false);
    return pathAt(well,twt,true);
}

std::vector<glm::dvec2> extendWellSectionRoute(const std::vector<glm::dvec2>& route,
    const std::vector<SectionWellInfo>& wells, const QStringList& selectedWellIds,
    bool extendFirst, bool extendLast) {
    if (route.size() < 2 || selectedWellIds.isEmpty())
        return route;
    const auto endpoint = [&](const glm::dvec2& head, const glm::dvec2& neighbour) {
        const double segmentLength = glm::length(head - neighbour);
        if (!(segmentLength > 1e-6))
            return head;
        const auto outward = (head - neighbour) / segmentLength;
        double extent = 0;
        for (const auto& well : wells) {
            if (!selectedWellIds.contains(well.wellId) ||
                glm::length(glm::dvec2(well.surfaceX, well.surfaceY) - head) > 1e-6)
                continue;
            for (const auto& station : well.trajectory) {
                if (!std::isfinite(station.twtMs) || station.twtMs < 0 ||
                    !std::isfinite(station.x) || !std::isfinite(station.y))
                    continue;
                const auto displacement = glm::dvec2(station.x, station.y) - head;
                extent = std::max(extent, glm::dot(displacement, outward));
            }
        }
        return extent > 1e-6 ? head + extent * outward : head;
    };
    auto extended = route;
    const auto first = extendFirst ? endpoint(route.front(), route[1]) : route.front();
    const auto last = extendLast ? endpoint(route.back(), route[route.size()-2]) : route.back();
    if (first != route.front())
        extended.insert(extended.begin(), first);
    if (last != route.back())
        extended.push_back(last);
    return extended;
}

namespace {
struct RouteSegment {
    glm::dvec2 start, direction;
    double length, station, stationSpan;
    double offsetSign;
};
std::vector<RouteSegment> routeSegments(const std::vector<glm::dvec2>& line,
                                      const std::vector<double>& stations) {
    std::vector<RouteSegment> segments;
    // Evaluate a bent line in the same physical order in either direction.
    // A station on a bend's bisector can be equally near two different feet.
    const auto xyLess = [](const glm::dvec2& a, const glm::dvec2& b) {
        return a.x < b.x || (a.x == b.x && a.y < b.y);
    };
    const bool reverse = line.size() > 2 &&
        std::lexicographical_compare(line.rbegin(), line.rend(), line.begin(), line.end(), xyLess);
    double station = 0;
    for (size_t i = 1; i < line.size(); ++i) {
        const auto delta = line[i] - line[i-1];
        const double length = glm::length(delta);
        if (!(length > 1e-9))
            continue;
        const bool supplied = stations.size() == line.size();
        const double start = supplied ? stations[i-1] : station;
        const double span = supplied ? stations[i] - start : length;
        if (reverse)
            segments.push_back({line[i], -delta / length, length, supplied ? stations[i] : station + length, -span, -1.});
        else
            segments.push_back({line[i-1], delta / length, length, start, span, 1.});
        station += length;
    }
    if (reverse) std::reverse(segments.begin(), segments.end());
    return segments;
}

double traceAtDistance(const std::vector<double>& axis, double distance, bool extendEnds) {
    if (axis.empty()) return qQNaN();
    if (axis.size() == 1) return 0;
    if (!extendEnds) distance = std::clamp(distance, axis.front(), axis.back());
    const auto upper = std::lower_bound(axis.begin(), axis.end(), distance);
    const auto right = std::clamp<size_t>(upper - axis.begin(), 1, axis.size()-1);
    const double span = axis[right] - axis[right-1];
    return double(right-1) + (span > 0 ? (distance-axis[right-1])/span : 0.);
}

SectionPointProjection projectPoint(const std::vector<RouteSegment>& segments,
    const std::vector<double>& axis, const glm::dvec2& point, bool extendEnds) {
    SectionPointProjection result;
    if (axis.empty() || !std::isfinite(point.x) || !std::isfinite(point.y)) return result;
    double bestSquared = std::numeric_limits<double>::infinity();
    for (const auto& segment : segments) {
        const auto displacement = point - segment.start;
        const double along = glm::dot(displacement, segment.direction);
        const double bounded = std::clamp(along, 0., segment.length);
        const auto difference = displacement - bounded * segment.direction;
        const double squared = glm::dot(difference, difference);
        if (!(squared < bestSquared))
            continue;
        bestSquared = squared;
        // End wells can leave the finite survey. Keep the trajectory moving
        // past the edge so the intersecting segment clips at its true time.
        const bool beyond = extendEnds && ((&segment == &segments.front() && along < 0) ||
                            (&segment == &segments.back() && along > segment.length));
        const double cross = (segment.direction.x * displacement.y - segment.direction.y * displacement.x) * segment.offsetSign;
        result.cumulativeDistanceM = segment.station + (beyond ? along : bounded) / segment.length * segment.stationSpan;
        result.offsetDistanceM = beyond ? cross : std::copysign(std::sqrt(squared), cross);
    }
    if (std::isfinite(result.cumulativeDistanceM))
        result.tracePosition = traceAtDistance(axis, result.cumulativeDistanceM, extendEnds);
    return result;
}
}

SectionPointProjection projectPointOnSection(const std::vector<glm::dvec2>& line,
    const std::vector<double>& distances,const glm::dvec2& point) {
    return projectPoint(routeSegments(line, {}), distances, point, false);
}

std::optional<std::pair<WellTrajSample, WellTrajSample>> clipWellPathSegment(
    const WellTrajSample& a, const WellTrajSample& b, double lastTrace, double bufferDistanceM) {
    if (!std::isfinite(a.md) || !std::isfinite(b.md) || b.md <= a.md ||
        !std::isfinite(a.twtMs) || !std::isfinite(b.twtMs) || a.twtMs < 0 || b.twtMs < 0 ||
        !(lastTrace >= 0) || !(bufferDistanceM >= 0))
        return std::nullopt;
    double enter = 0, leave = 1;
    const auto clip = [&](double from, double to, double low, double high) {
        if (!std::isfinite(from) || !std::isfinite(to)) return false;
        if (from >= low && from <= high && to >= low && to <= high) return true;
        if (from == to) return false;
        const double f = (low - from) / (to - from);
        const double g = (high - from) / (to - from);
        enter = std::max(enter, std::min(f, g));
        leave = std::min(leave, std::max(f, g));
        return enter <= leave;
    };
    if (!clip(a.tracePosition, b.tracePosition, 0, lastTrace) ||
        !clip(a.offsetDistanceM, b.offsetDistanceM, -bufferDistanceM, bufferDistanceM))
        return std::nullopt;
    const auto endpoint = [&](double fraction) {
        auto point = interpolate(a,b,fraction);
        // Guard the exact boundary against sub-ULP error in the interpolant.
        point.tracePosition = std::clamp(point.tracePosition, 0., lastTrace);
        point.offsetDistanceM = std::clamp(point.offsetDistanceM, -bufferDistanceM, bufferDistanceM);
        return point;
    };
    return std::make_pair(endpoint(enter), endpoint(leave));
}

void selectSectionHorizonTies(SectionWellInfo& well, double bufferDistanceM) {
    if (!well.horizonTies) return;
    well.tops.clear();
    for (const auto& crossings : well.horizonTies->crossings) {
        const auto first = std::find_if(crossings.begin(), crossings.end(), [&](const auto& top) {
            return std::abs(top.offsetDistanceM) <= bufferDistanceM;
        });
        if (first != crossings.end()) well.tops.push_back(*first);
    }
}

void projectSectionHorizonTies(std::vector<SectionWellInfo>& wells, const SectionHorizons& horizons) {
    const auto sampled = [](const auto& curve) { return !curve.internalEdge && !curve.timesMs.empty(); };
    const bool hasHorizons = std::any_of(horizons.curves.begin(), horizons.curves.end(), sampled);
    for (auto& well : wells) {
        if (!hasHorizons || !well.mdTimeDepth) {
            if (well.horizonTies) {
                well.tops = std::move(well.horizonTies->formationTops);
                well.horizonTies.reset();
            }
            continue;
        }
        if (!well.horizonTies)
            well.horizonTies.emplace().formationTops = std::move(well.tops);
        auto& crossings = well.horizonTies->crossings;
        crossings.clear();
        for (const auto& curve : horizons.curves) {
            if (!sampled(curve)) continue;
            if (!well.trajectory.empty()) {
                crossings.push_back(pathHorizonIntersections(well, curve));
                continue;
            }
            if (well.requiresSpatialPath) continue;
            const double time = horizonTime(curve, well.tracePosition);
            const double md = well.mdTimeDepth->TwtMsToDepth(time-well.timeShiftMs);
            if (!std::isfinite(md)) continue;
            WellTopItem top;
            top.topName=curve.name;top.md=md;top.tvd=qQNaN();top.twtMs=time;
            top.offsetDistanceM=well.offsetDistanceM;
            crossings.push_back({top});
        }
        selectSectionHorizonTies(well, std::numeric_limits<double>::infinity());
    }
}

std::vector<SectionWellInfo> SectionWellProjector::ProjectWells(
    const std::vector<glm::dvec2> &polylinePoints,
    const std::vector<double> &vertexDistancesM,
    const std::vector<double> &traceDistancesM,
    const std::vector<SectionWellInfo> &wells,
    double bufferDistanceM,
    const TimeDepthModel &tdModel,const QStringList &selectedWellIds)
{
    std::vector<SectionWellInfo> results;
    if (polylinePoints.size() < 2 || wells.empty())
        return results;

    const auto segments = routeSegments(polylinePoints, vertexDistancesM);

    results.reserve(wells.size());
    for (const auto &w : wells) {
        if(!selectedWellIds.isEmpty()&&!selectedWellIds.contains(w.wellId))continue;
        SectionWellInfo proj = w;
        proj.selectedForSection=selectedWellIds.contains(w.wellId);
        const glm::dvec2 wellPos(w.surfaceX, w.surfaceY);

        const auto head = projectPoint(segments, traceDistancesM, wellPos, false);
        proj.cumulativeDistanceM = head.cumulativeDistanceM;
        proj.offsetDistanceM = head.offsetDistanceM;
        proj.tracePosition = head.tracePosition;
        proj.isWithinBuffer = std::abs(head.offsetDistanceM) <= bufferDistanceM;

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

        if (!proj.trajectory.empty()) {
            double closest = std::numeric_limits<double>::infinity();
            const double lastTrace = double(traceDistancesM.size()) - 1;
            const WellTrajSample* previous = nullptr;
            for (auto& sample : proj.trajectory) {
                const auto position = projectPoint(segments,traceDistancesM,{sample.x,sample.y},true);
                sample.tracePosition = position.tracePosition;
                sample.offsetDistanceM = position.offsetDistanceM;
                if (std::isfinite(sample.twtMs) && sample.twtMs >= 0 &&
                    sample.tracePosition >= 0 && sample.tracePosition <= lastTrace &&
                    std::abs(sample.offsetDistanceM) < std::abs(closest))
                    closest = sample.offsetDistanceM;
                if (previous) {
                    const auto visible = clipWellPathSegment(*previous, sample, lastTrace,
                                                             std::numeric_limits<double>::infinity());
                    if (visible) {
                        const double a = visible->first.offsetDistanceM, b = visible->second.offsetDistanceM;
                        const double offset = std::clamp(0., std::min(a,b), std::max(a,b));
                        if (std::abs(offset) < std::abs(closest)) closest = offset;
                    }
                }
                previous = &sample;
            }
            proj.isWithinBuffer = std::isfinite(closest) && std::abs(closest) <= bufferDistanceM;
            proj.offsetDistanceM = closest;
        } else if(proj.requiresSpatialPath)proj.isWithinBuffer=false;
        results.push_back(std::move(proj));
    }

    return results;
}

} // namespace seismic
