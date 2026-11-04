// 层：数据
#include "modelhorizonpreview.h"
#include "areamodel.h"
#include "horizonbinner.h"
#include <paleo_data/service.h>
#include <algorithm>
#include <cmath>
#include <limits>

paleo::area_model::Read readModelHorizonPreview(const QString &source, BinnedHorizon &grid,
    QString *error, const std::function<bool()> &cancelled) {
  using paleo::area_model::Read;
  grid={}; paleo::data::DataService service;long long sourceId=0;
  const auto opened=paleo::area_model::openSource(source,service,sourceId,error);
  if(opened!=Read::Ready)return opened;
  const auto fail=[error](const QString &why){if(error)*error=why;return Read::Failed;};
  std::string detail;std::vector<paleo::data::DataEntry> entries;
  if(!service.listSourceData("horizons",sourceId,entries,detail))return fail(QString::fromUtf8(detail));
  std::vector<paleo::data::DataEntry> matches;
  for(const auto &entry:entries)if(entry.sourceId==sourceId)matches.push_back(entry);
  if(matches.size()!=1)return fail(QStringLiteral("层位来源必须对应唯一模型层位"));
  const auto &entry=matches.front();
  if(entry.count>2000000)return fail(QStringLiteral("层位预览超过 200 万点预算"));
  std::vector<paleo::data::SeismicHorizonPoint> points;
  double timeScale=1.;
  for(long long offset=0;offset<entry.count;offset+=100000){
    if(cancelled&&cancelled())return fail(QStringLiteral("已取消"));
    paleo::data::SeismicHorizon horizon;
    if(!service.readHorizon(entry.id,offset,100000,horizon,detail))return fail(QString::fromUtf8(detail));
    if(offset==0){
      grid.coordinateSystem=QString::fromUtf8(horizon.coordinateSystem);
      if(grid.coordinateSystem.isEmpty()){
        paleo::data::ProjectCoordinates coordinates;
        if(!service.readProjectCoordinates(coordinates,detail))return fail(QString::fromUtf8(detail));
        grid.coordinateSystem=QString::fromUtf8(coordinates.coordinateSystem);
      }
      if(grid.coordinateSystem.isEmpty())return fail(QStringLiteral("层位缺少平面坐标系"));
      if((horizon.timeUnit!="ms"&&horizon.timeUnit!="s")||
         (horizon.timeType!="TWT"&&horizon.timeType!="OWT"))
        return fail(QStringLiteral("层位时间单位或单/双程类型不受支持"));
      timeScale=(horizon.timeUnit=="s"?1000.:1.)*(horizon.timeType=="OWT"?2.:1.);
    }
    points.insert(points.end(),horizon.points.begin(),horizon.points.end());
  }
  double xmin=std::numeric_limits<double>::max(),xmax=-xmin,ymin=xmin,ymax=-xmin;
  for(const auto &p:points)if(std::isfinite(p.x)&&std::isfinite(p.y)&&std::isfinite(p.zTime)){
    xmin=std::min(xmin,p.x);xmax=std::max(xmax,p.x);ymin=std::min(ymin,p.y);ymax=std::max(ymax,p.y);
  }
  if(xmin>xmax||ymin>ymax)return fail(QStringLiteral("层位没有有效平面位置和时间值"));
  const double span=std::max({xmax-xmin,ymax-ymin,1.});
  grid.dx=grid.dy=span/511.;grid.originX=xmin-grid.dx/2.;grid.originY=ymax+grid.dy/2.;
  grid.cols=std::max(1,int(std::ceil((xmax-xmin)/grid.dx))+1);
  grid.rows=std::max(1,int(std::ceil((ymax-ymin)/grid.dy))+1);
  const int n=grid.rows*grid.cols;QVector<double> sums(n,0.);QVector<int> counts(n,0);
  grid.z.fill(-9999.f,n);grid.zMin=std::numeric_limits<double>::max();grid.zMax=-grid.zMin;
  for(const auto &p:points){
    if(!std::isfinite(p.x)||!std::isfinite(p.y)||!std::isfinite(p.zTime)){++grid.rejected;continue;}
    const int col=std::clamp(int(std::lround((p.x-xmin)/grid.dx)),0,grid.cols-1);
    const int row=std::clamp(int(std::lround((ymax-p.y)/grid.dy)),0,grid.rows-1),i=row*grid.cols+col;
    sums[i]+=p.zTime*timeScale;if(counts[i]++)++grid.collisions;
  }
  for(int i=0;i<n;++i)if(counts[i]){
    grid.z[i]=float(sums[i]/counts[i]);++grid.filledCells;
    grid.zMin=std::min(grid.zMin,double(grid.z[i]));grid.zMax=std::max(grid.zMax,double(grid.z[i]));
  }
  return Read::Ready;
}
