// 层：数据
#include "seismictaskservice.h"
#include "seismicdataprojection.h"
#include "paleotaskservice.h"
#include "io/areamodel.h"
#include <QDateTime>
#include <QFileInfo>
#include <cmath>

namespace seismic {
namespace {
std::string nativePlaneKey(const QString& path,SgySliceType type,int index) {
 const QFileInfo file(path);
 return (QStringLiteral("native-plane:%1:%2:%3:%4:%5:%6:%7").arg(file.absoluteFilePath()).arg(int(type)).arg(index)
    .arg(file.size()).arg(file.lastModified().toMSecsSinceEpoch()).arg(file.metadataChangeTime().toMSecsSinceEpoch())
    .arg(paleo::area_model::cacheToken(path))).toStdString();
}
}
PaleoTask *SeismicTaskService::startDataPlane(const QString& path,SgySliceType type,int index,
    std::function<void(bool,std::shared_ptr<const SgySliceImage>,const SeismicNativeInfo&,const QString&)> done) {
 if(!taskService_){if(done)done(false,nullptr,{},tr("后台地震服务未连接"));return nullptr;}
 const auto key=nativePlaneKey(path,type,index);
 auto image=std::make_shared<std::shared_ptr<const SgySliceImage>>(dataCache_.Find(key));auto axis=std::make_shared<SeismicNativeInfo>();
 auto *task=startBounded(tr("提取地震切片 (%1: %2)").arg(int(type)).arg(index),[path,type,index,image,axis](PaleoTask *task)->QString {
  paleo::SeismicDataAccess source;QString error;std::string detail;if(!source.open(path,&error))return error;
  const auto cancel=[task]{return task->cancelRequested();};paleo::data::SeismicFileInfo info;
  if(!source.inspect(info,detail,cancel))return QString::fromUtf8(detail);
  *axis={info.sampleCount,info.inlineMin,info.inlineMax,info.crosslineMin,info.crosslineMax,info.firstTime,info.sampleInterval};
  axis->coordinateSystem=source.coordinateSystem();
  if(type!=SgySliceType::Time) {
   // A single native sample per trace supplies geometry even when texture values hit the LRU.
   paleo::data::SectionReadOptions options;options.fullResolution=true;options.maxValues=64000000;
   options.firstTime=options.lastTime=info.firstTime;paleo::data::SeismicSectionResult section;
   if(!source.line(type==SgySliceType::Inline,index,options,section,detail,cancel)) {
    if(cancel())return {};
    // Entire first sample can be null while later samples and trace XY are valid.
    options.firstTime=options.lastTime=paleo::data::Missing;
    if(!source.line(type==SgySliceType::Inline,index,options,section,detail,cancel))return QString::fromUtf8(detail);
   }
   const auto unit=source.coordinateUnit().empty()?info.coordinateLengthUnit:source.coordinateUnit();
   const double scale=unit=="ft"?.3048:1.;double distance=0;bool metric=unit=="m"||unit=="ft";
   for(int col=0;col<section.columns;++col) {
    const double x=section.x[size_t(col)],y=section.y[size_t(col)];
    metric=metric&&std::isfinite(x)&&std::isfinite(y);
    if(col)distance+=std::hypot(x-section.x[size_t(col-1)],y-section.y[size_t(col-1)])*scale;
    axis->columnCoordinates.push_back({x,y});axis->columnDistancesM.push_back(float(distance));
   }
   if(!metric)axis->columnDistancesM.clear();
  }
  if(!*image){auto values=std::make_shared<SgySliceImage>();if(!paleo::readSeismicDataSlice(path,type,index,*values,error,cancel))return error;*image=values;}return {};
 },{},true);
 connect(task,&PaleoTask::finished,this,[this,task,path,type,index,key,image,axis,done]{
  const bool changed=key!=nativePlaneKey(path,type,index);
  const bool ok=task->state()==PaleoTask::State::Succeeded&&!changed;
  if(ok)dataCache_.Store(key,*image);
  if(done)done(ok,ok?*image:nullptr,*axis,changed?tr("地震来源已变化，请重新加载"):task->errorText());
 });
 return task;
}
PaleoTask *SeismicTaskService::startDataSection(const QString& path,const std::vector<std::pair<double,double>>& points,
    const QString& reference,std::function<void(bool,std::shared_ptr<const SeismicSectionData>,const QString&)> done) {
 if(!taskService_){if(done)done(false,nullptr,tr("后台地震服务未连接"));return nullptr;}
 auto out=std::make_shared<SeismicSectionData>();
 auto *task=startBounded(tr("读取连井地震剖面"),[path,points,reference,out](PaleoTask *task)->QString {
  paleo::SeismicDataAccess source;QString error;std::string detail;if(!source.open(path,&error))return error;
  paleo::data::SeismicSectionResult section;
  if(!source.xySection(points,reference,section,detail,[task]{return task->cancelRequested();}))return QString::fromUtf8(detail);
  out->columns=section.columns;out->samples=section.samples;out->firstTimeMs=section.firstTime;
  out->sampleIntervalMs=section.sampleInterval;out->timeReference=reference;out->distancesM=std::move(section.distances);
  out->values.resize(section.amplitudes.size());
  for(int sample=0;sample<section.samples;++sample)for(int col=0;col<section.columns;++col)
   out->values[size_t(sample)*section.columns+col]=section.amplitudes[size_t(col)*section.samples+sample];
  task->reportBytes(section.columns,section.columns);return {};
 },{},true);
 connect(task,&PaleoTask::finished,this,[task,out,done]{const bool ok=task->state()==PaleoTask::State::Succeeded;if(done)done(ok,ok?out:nullptr,task->errorText());});
 return task;
}
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
