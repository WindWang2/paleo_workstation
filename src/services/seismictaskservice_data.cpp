// 层：数据
#include "seismictaskservice.h"
#include "seismicdataprojection.h"
#include "paleotaskservice.h"

namespace seismic {
PaleoTask *SeismicTaskService::startDataInfo(const QString& path,std::function<void(bool,const SeismicNativeInfo&,const QString&)> done) {
 if(!taskService_){if(done)done(false,{},tr("后台地震服务未连接"));return nullptr;}
 auto out=std::make_shared<SeismicNativeInfo>();
 auto *task=startBounded(tr("查询地震元数据"),[path,out](PaleoTask *task)->QString {
  paleo::SeismicDataAccess source;QString error;std::string detail;if(!source.open(path,&error))return error;
  paleo::data::SeismicFileInfo info;if(!source.inspect(info,detail,[task]{return task->cancelRequested();}))return QString::fromUtf8(detail);
  *out={info.sampleCount,info.inlineMin,info.inlineMax,info.crosslineMin,info.crosslineMax,info.firstTime,info.sampleInterval};return {};
 },{},true);
 connect(task,&PaleoTask::finished,this,[task,out,done]{if(done)done(task->state()==PaleoTask::State::Succeeded,*out,task->errorText());});
 return task;
}
PaleoTask *SeismicTaskService::startDataSlice(const QString& path,SgySliceType type,int index,
    std::function<void(bool,std::shared_ptr<const SgySliceImage>,const QString&)> done) {
 if(!taskService_){if(done)done(false,nullptr,tr("后台地震服务未连接"));return nullptr;}
 auto out=std::make_shared<SgySliceImage>();
 auto *task=startBounded(tr("读取地震剖面"),[path,type,index,out](PaleoTask *task)->QString {
  QString error;if(!paleo::readSeismicDataSlice(path,type,index,*out,error,[task]{return task->cancelRequested();}))return error;return {};
 },{},true);
 connect(task,&PaleoTask::finished,this,[task,out,done]{const bool ok=task->state()==PaleoTask::State::Succeeded;if(done)done(ok,ok?out:nullptr,task->errorText());});
 return task;
}
}
