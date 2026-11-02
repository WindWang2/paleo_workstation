// 层：数据
#include "wellsectioncorrelation.h"
#include "io/areamodel.h"
#include <paleo_data/service.h>
#include <QFileInfo>
#include <QSet>
#include <algorithm>
#include <cmath>

namespace paleo {
void loadSeismicCorrelationTops(const QString &project,const QHash<int,qlonglong> &nativeWells,
                               QVector<wellsection::Well> &wells,QStringList &warnings) {
  if(nativeWells.isEmpty()||!QFileInfo::exists(area_model::directory(project)+"/data.sqlite"))return;
  // A native project uses seismic correlation, including when no picks exist.
  for(auto &well:wells)well.correlationTops=QVector<wellsection::Top>{};
  data::DataService ds;std::string error;std::vector<data::StratigraphicSchemeSummary> schemes;
  if(!ds.openProjectReadOnly(area_model::directory(project).toStdString(),error)||!ds.listStratigraphicSchemes(schemes,error)) {
    warnings.append(QString::fromStdString(error));return;
  }
  if(schemes.empty())return;
  const auto found=std::find_if(schemes.begin(),schemes.end(),[](const auto &s){return s.id=="default";});
  data::StratigraphicScheme scheme;
  if(!ds.readStratigraphicScheme((found==schemes.end()?schemes.front():*found).id,{},scheme,error)) {
    warnings.append(QString::fromStdString(error));return;
  }
  for(auto it=nativeWells.cbegin();it!=nativeWells.cend();++it) {
    if(it.key()<0||it.key()>=wells.size())continue;
    auto &tops=*wells[it.key()].correlationTops;QHash<QString,double> depths;QSet<QString> conflicts;
    const auto append=[&](const data::BoundaryId &id,double md) {
      const auto *boundary=data::framework::findBoundary(scheme,id);
      if(!boundary||boundary->seismicHorizonResourceId<=0||!std::isfinite(md))return;
      const auto name=QString::fromStdString(boundary->name);
      if(depths.contains(name)&&std::abs(depths.value(name)-md)>1e-6)conflicts.insert(name);
      else depths.insert(name,md);
    };
    for(const auto &occ:scheme.wellOccurrences) {
      if(occ.wellId!=it.value()||occ.depthType!="MD"||occ.depthUnit!="m"||occ.status=="pending")continue;
      const auto *interval=data::framework::findInterval(scheme,occ.interval);
      if(!interval||interval->status!="valid")continue;
      append(interval->top,occ.top);append(interval->bottom,occ.bottom);
    }
    for(const auto &boundary:scheme.boundaries) {
      const auto name=QString::fromStdString(boundary.name);
      if(depths.contains(name)&&!conflicts.contains(name))tops.append({name,depths.value(name)});
    }
    std::sort(tops.begin(),tops.end(),[](const auto &a,const auto &b){return a.md<b.md;});
  }
}
}
