// 层：数据
#include "stratigraphymodel.h"
#include "../io/areamodel.h"
#include <paleo_data/service.h>
#include <QFileInfo>
#include <cmath>
ModelTopsResult readStratigraphyModelTops(const QString &project,qlonglong wellId) {
 ModelTopsResult result;
 if(project.isEmpty()||wellId<=0)return result;
 const QString directory=paleo::area_model::directory(project);
 if(!QFileInfo::exists(directory+"/data.sqlite"))return result; // 无工区模型库
 paleo::data::DataService service;std::string error;
 std::vector<paleo::data::InterpretationSummary> summaries;
 if(!service.openProjectReadOnly(directory.toStdString(),error)) {
  result.status=ModelTopsStatus::Failed;result.error=QString::fromUtf8(error);return result;
 }
 if(!service.listInterpretations(summaries,error)) {
  result.status=ModelTopsStatus::Failed;result.error=QString::fromUtf8(error);return result;
 }
 if(summaries.empty())return result; // 工程还没有解释方案：无模型分层
 result.status=ModelTopsStatus::Ready;
 for(auto it=summaries.rbegin();it!=summaries.rend();++it) {
  paleo::data::InterpretationDocument doc;
  if(!service.readInterpretationRevision(it->id,it->revision,doc,error)) {
   // #826：不 continue 跳过损坏/不可读修订——那会把更早版本的分层当成当前解释。
   result.status=ModelTopsStatus::Failed;result.tops.clear();
   result.error=QStringLiteral("解释版本 %1（%2）读取失败：%3")
       .arg(QString::fromUtf8(it->name),QString::fromUtf8(it->revision),QString::fromUtf8(error));
   return result;
  }
  for(const auto &well:doc.wells)if(well.wellId==wellId) {
   for(const auto &boundary:well.boundaries) {
    if(!std::isfinite(boundary.md)||boundary.role.starts_with("candidate:")||boundary.status=="stale")continue;
    WellTop top;top.horizon=QString::fromUtf8(boundary.name);top.md=boundary.md;result.tops.append(top);
   }
   return result; // 最新提到该井的修订即权威；分层为空也不向更早修订借数据
  }
 }
 return result;
}
QVector<WellTop> stratigraphyModelTops(const QString &project,qlonglong wellId) {
 const ModelTopsResult result=readStratigraphyModelTops(project,wellId);
 return result.status==ModelTopsStatus::Ready?result.tops:QVector<WellTop>{};
}
