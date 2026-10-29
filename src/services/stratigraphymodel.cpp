// 层：数据
#include "stratigraphymodel.h"
#include "../io/areamodel.h"
#include <paleo_data/service.h>
#include <QFileInfo>
#include <cmath>
QVector<WellTop> stratigraphyModelTops(const QString &project,qlonglong wellId) {
 if(project.isEmpty()||wellId<=0)return {};
 const QString directory=paleo::area_model::directory(project);
 if(!QFileInfo::exists(directory+"/data.sqlite"))return {};
 paleo::data::DataService service;std::string error;
 std::vector<paleo::data::InterpretationSummary> summaries;
 if(!service.openProjectReadOnly(directory.toStdString(),error)||!service.listInterpretations(summaries,error))return {};
 for(auto it=summaries.rbegin();it!=summaries.rend();++it) {
  paleo::data::InterpretationDocument doc;
  if(!service.readInterpretationRevision(it->id,it->revision,doc,error))continue;
  for(const auto &well:doc.wells)if(well.wellId==wellId) {
   QVector<WellTop> tops;
   for(const auto &boundary:well.boundaries) {
    if(!std::isfinite(boundary.md)||boundary.role.starts_with("candidate:")||boundary.status=="stale")continue;
    WellTop top;top.horizon=QString::fromUtf8(boundary.name);top.md=boundary.md;tops.append(top);
   }
   if(!tops.isEmpty())return tops;
  }
 }
 return {};
}
