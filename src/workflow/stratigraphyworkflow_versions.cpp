// 层：功能
#include "stratigraphyworkflow_state.h"
#include <paleo_data/service.h>
#include "../services/stratigraphyexport.h"
using namespace paleo::data;
bool StratigraphyWorkflow::isRunning() const{return bool(d->task);}
void StratigraphyWorkflow::exportCurve(const InterpretationSeries& series,const QString& path) {
 QString error;if(!exportStratigraphySeries(series,path,error))emit statusChanged(tr("导出失败：%1").arg(error));else emit statusChanged(tr("完整计算曲线已导出"));
}
QList<StratigraphyVersion> StratigraphyWorkflow::versions(const QString& id) const {
 QList<StratigraphyVersion> result;DataService service;std::string error;
 if(d->path.isEmpty()||!service.openProjectReadOnly(d->path.toStdString(),error))return result;
 if(id.isEmpty()) {
  std::vector<InterpretationSummary> list;if(service.listInterpretations(list,error))
   for(const auto& v:list)result.append({QString::fromStdString(v.id),QString::fromStdString(v.name),QString::fromStdString(v.revision)});
 }else {
  std::vector<InterpretationRevisionSummary> list;if(service.listInterpretationRevisions(id.toStdString(),list,error))
   for(const auto& v:list)result.append({QString::fromStdString(v.revision),QString::fromStdString(v.created),{}});
 }return result;
}
bool StratigraphyWorkflow::loadVersion(const QString& id,const QString& revision) {
 if(d->task||id.isEmpty())return false;DataService service;std::string error;InterpretationDocument loaded;
 if(!service.openProjectReadOnly(d->path.toStdString(),error)||!service.readInterpretationRevision(id.toStdString(),revision.toStdString(),loaded,error)) {
  emit statusChanged(QString::fromStdString(error));return false;
 }
 d->document=std::move(loaded);d->history.clear();d->future.clear();d->dirty=false;d->preferred={};
 emit documentReplaced();emit changed();emit statusChanged(tr("已读取所选解释版本"));return true;
}
