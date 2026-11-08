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
                               QVector<wellsection::Well> &wells,QStringList &warnings,
                               const document::InterpretationContext *context,
                               const std::function<bool()> &cancelled) {
  if(nativeWells.isEmpty())return;
  const auto directory=area_model::directory(project);
  if(!context&&!QFileInfo::exists(directory+"/data.sqlite"))return;
  // A native project uses seismic correlation, including when no picks exist.
  for(auto &well:wells){well.correlationTops=QVector<wellsection::Top>{};well.correlationRevision.clear();}
  if(cancelled&&cancelled())return;
  if(context&&(!context->valid()||context->modelStoreId!="area-model"||
               context->purpose!=document::RelationPurpose::Correlation)) {
    warnings.append(QStringLiteral("井间对比方案或用途无效"));return;
  }
  data::DataService ds;std::string error;
  if(!ds.openProjectReadOnly(directory.toUtf8().toStdString(),error)) {
    warnings.append(QString::fromStdString(error));return;
  }
  std::string id,revision;
  if(context) {
    if(context->schemeId.isEmpty())return;
    id=context->schemeId.toStdString();revision=context->schemeRevision.toStdString();
  } else {
    std::vector<data::StratigraphicSchemeSummary> schemes;
    if(!ds.listStratigraphicSchemes(schemes,error)){warnings.append(QString::fromUtf8(error));return;}
    if(schemes.empty())return;
    const auto found=std::find_if(schemes.begin(),schemes.end(),[](const auto &s){return s.id=="default";});
    id=(found==schemes.end()?schemes.front():*found).id;
  }
  data::StratigraphicScheme scheme;
  if(!ds.readStratigraphicScheme(id,revision,scheme,error)) {
    warnings.append(QString::fromStdString(error));return;
  }
  const data::StratigraphicInterval *selectedInterval=nullptr;
  if(context&&!context->intervalId.isEmpty()) {
    selectedInterval=data::framework::findInterval(scheme,{context->intervalId.toStdString()});
    const auto *sub=context->aggregate?nullptr:data::framework::findSubInterval(scheme,{context->subIntervalId.toStdString()});
    if(!selectedInterval||selectedInterval->status!="valid"||
       (!context->aggregate&&(!sub||sub->interval!=selectedInterval->id||sub->status!="valid"))) {
      warnings.append(QStringLiteral("井间对比所选层段不属于该方案修订"));return;
    }
  }
  QSet<qlonglong> requestedWells;
  for(const auto id:nativeWells)requestedWells.insert(id);
  QHash<qlonglong,QVector<const data::WellIntervalOccurrence *>> occurrences;
  for(const auto &occ:scheme.wellOccurrences) {
    if(cancelled&&cancelled())return;
    if(!requestedWells.contains(occ.wellId)||occ.depthType!="MD"||occ.depthUnit!="m"||occ.status=="pending")continue;
    if(selectedInterval&&occ.interval!=selectedInterval->id)continue;
    occurrences[occ.wellId].append(&occ);
  }
  for(auto &well:wells)well.correlationRevision=QString::fromStdString(scheme.id+":"+scheme.revision.value);
  for(auto it=nativeWells.cbegin();it!=nativeWells.cend();++it) {
    if(cancelled&&cancelled())return;
    if(it.key()<0||it.key()>=wells.size())continue;
    auto &tops=*wells[it.key()].correlationTops;QHash<QString,double> depths;QSet<QString> conflicts;
    const auto append=[&](const data::BoundaryId &id,double md) {
      const auto *boundary=data::framework::findBoundary(scheme,id);
      if(!boundary||boundary->seismicHorizonResourceId<=0||!std::isfinite(md))return;
      const auto name=QString::fromStdString(boundary->name);
      if(depths.contains(name)&&std::abs(depths.value(name)-md)>1e-6)conflicts.insert(name);
      else depths.insert(name,md);
    };
    for(const auto *occ:occurrences.value(it.value())) {
      const auto *interval=selectedInterval?selectedInterval:data::framework::findInterval(scheme,occ->interval);
      if(!interval||interval->status!="valid")continue;
      append(interval->top,occ->top);append(interval->bottom,occ->bottom);
    }
    for(const auto &boundary:scheme.boundaries) {
      const auto name=QString::fromStdString(boundary.name);
      if(depths.contains(name)&&!conflicts.contains(name))tops.append({name,depths.value(name)});
    }
    std::sort(tops.begin(),tops.end(),[](const auto &a,const auto &b){return a.md<b.md;});
  }
}
}
