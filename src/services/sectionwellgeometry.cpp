// 层：数据
#include "sectionwellgeometry.h"
#include <algorithm>
#include <cmath>

bool observedMdTimeIsMonotone(const QVector<TdSample>& rows){
  int count=0;double previousMd=0,previousTime=0,firstTime=0;
  for(const auto &row:rows){
    if(!std::isfinite(row.md)||!std::isfinite(row.timeMs)||row.timeMs<0)continue;
    if(row.md<0||(count&&(row.md<=previousMd||row.timeMs<previousTime)))return false;
    if(!count)firstTime=row.timeMs;
    previousMd=row.md;previousTime=row.timeMs;++count;
  }
  return count>=2&&previousTime>firstTime;
}

bool requiresSpatialWellSampling(const seismic::SectionWellInfo& well){
  if(well.requiresSpatialPath&&well.trajectory.empty())return true;
  const seismic::WellTrajSample* first=nullptr;
  for(const auto& point:well.trajectory){
    if(!std::isfinite(point.x)||!std::isfinite(point.y))continue;
    if(!first)first=&point;
    else if(std::hypot(point.x-first->x,point.y-first->y)>1e-6)return true;
  }
  return false;
}

void attachSectionWellGeometry(seismic::SectionWellInfo &well, const QVector<TdSample> &rows,
    const std::optional<paleo::WellDeviationSurvey> &survey,
    const seismic::TimeDepthModel *md, const seismic::TimeDepthModel *tvd, double shiftMs)
{
  int positioned=0;double displacement=0;const TdSample *first=nullptr;
  for(const auto &row:rows)if(std::isfinite(row.md)&&std::isfinite(row.x)&&std::isfinite(row.y)&&std::isfinite(row.timeMs)){
    ++positioned;if(!first)first=&row;
    displacement=std::max(displacement,std::hypot(row.x-first->x,row.y-first->y));
  }
  well.trajectory.clear();
  if(positioned>=2&&(!well.requiresSpatialPath||displacement>1e-6)&&(!survey||displacement>1e-6)){
    bool gap=false;
    for(const auto &row:rows){
      if(!std::isfinite(row.md)){gap=true;continue;}
      // Invalid positions retain their MD and break the observed trajectory.
      seismic::WellTrajSample p;p.md=row.md;p.tvd=row.tvd;p.x=row.x;p.y=row.y;p.sourceZ=row.z;
      p.twtMs=md&&md->points().empty()?md->DepthToTwtMs(row.md)+shiftMs:row.timeMs+shiftMs;
      if(gap){p.x=p.y=qQNaN();gap=false;}
      well.trajectory.push_back(p);
    }
    well.trajectorySource=QStringLiteral("time_depth_xyz:%1").arg(first->timeDepthId);
    well.trajectoryStatus=QObject::tr("时深轨迹 · %1 点").arg(positioned);
  }else if(survey){
    QVector<double> depths;for(const auto &p:survey->points())depths.append(p.md);
    // Densify the minimum-curvature arc, only INSIDE observed survey coverage.
    if(well.requiresSpatialPath&&!depths.isEmpty()) {
      const double end=depths.last();
      for(double d=depths.first()+25;d<end;d+=25)depths.append(d);
    }
    std::sort(depths.begin(),depths.end());depths.erase(std::unique(depths.begin(),depths.end()),depths.end());
    for(double d:depths){
      const auto point=survey->pointAt(d);seismic::WellTrajSample p;
      p.md=d;p.tvd=point.tvd;p.x=well.surfaceX+point.east;p.y=well.surfaceY+point.north;
      p.twtMs=md?md->DepthToTwtMs(d)+shiftMs:tvd?tvd->DepthToTwtMs(p.tvd)+shiftMs:qQNaN();
      well.trajectory.push_back(p);
    }
    well.trajectorySource=QStringLiteral("survey_minimum_curvature");well.trajectoryStatus=QObject::tr("测斜轨迹");
  }else {
    well.trajectoryStatus=positioned>=2
      ?QObject::tr("时深表 X/Y 恒定，缺少可还原斜井的 Survey，未叠加井身")
      :QObject::tr("缺少轨迹坐标或真实测斜，未叠加井身");
    for(const auto &row:rows)if(!row.positionError.isEmpty()){well.trajectoryStatus=row.positionError;break;}
  }
  // Bottom refers to the last observed, time-calibrated spatial point, never an
  // invented tip at total MD or a uniform-velocity extension.
  for(auto it=well.trajectory.rbegin();it!=well.trajectory.rend();++it)
    if(std::isfinite(it->x)&&std::isfinite(it->y)&&std::isfinite(it->twtMs)&&it->twtMs>=0){
      well.bottomX=it->x;well.bottomY=it->y;well.bottomTwtMs=it->twtMs;break;
    }
  well.alignmentStatus+=QStringLiteral(" · ")+well.trajectoryStatus;
}
