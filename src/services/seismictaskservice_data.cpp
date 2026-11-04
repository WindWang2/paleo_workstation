// 层：数据
#include "seismictaskservice.h"
#include "seismicdataprojection.h"
#include "paleotaskservice.h"
#include "io/areamodel.h"
#include <QDateTime>
#include <QFileInfo>
#include <QTimer>
#include <QDataStream>
#include <QCryptographicHash>
#include <cmath>

namespace seismic {
namespace {
std::string nativePlaneKey(const QString& path,SgySliceType type,int index) {
 const QFileInfo file(path);
 // SQLite WAL checkpoints and unrelated result publication are not a change of
 // seismic source. Key amplitudes by the registered volume's actual conventions;
 // startDataPlane refreshes horizon overlays separately on every request.
 paleo::SeismicDataAccess source;QString error;QByteArray bytes;QDataStream identity(&bytes,QIODevice::WriteOnly);
 if(!source.open(path,&error))identity<<error;
 else if(source.registered()) {
  paleo::data::SeismicVolume volume;std::string detail;
  if(!source.volume(volume,detail))identity<<QString::fromStdString(detail);
  else {const auto &g=volume.geometry;const auto &s=volume.sampling;
   identity<<source.projectDirectory()<<volume.volumeId<<QString::fromStdString(volume.dataFile)
     <<g.traceCount<<g.inlineMin<<g.inlineMax<<g.crosslineMin<<g.crosslineMax
     <<QString::fromStdString(g.coordinateSystem)<<QString::fromStdString(g.coordinateUnit)
     <<s.sampleCount<<s.firstTime<<s.sampleInterval<<QString::fromStdString(s.timeUnit)
     <<QString::fromStdString(s.timeType)<<QString::fromStdString(s.timeReference);
  }
 }
 return (QStringLiteral("native-plane:%1:%2:%3:%4:%5:%6:%7").arg(file.absoluteFilePath()).arg(int(type)).arg(index)
    .arg(file.size()).arg(file.lastModified().toMSecsSinceEpoch()).arg(file.metadataChangeTime().toMSecsSinceEpoch())
    .arg(QString::fromLatin1(QCryptographicHash::hash(bytes,QCryptographicHash::Sha256).toHex()))).toStdString();
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
  axis->coordinateSystem=source.coordinateSystem();axis->inlineCount=info.inlineCount;axis->xlineCount=info.crosslineCount;
  if(type!=SgySliceType::Time) {
   // One native read supplies values, XY and overlay columns together.
   paleo::data::SectionReadOptions options;options.fullResolution=true;options.maxValues=64000000;
   paleo::data::SeismicSectionResult section;
   task->reportStage(QObject::tr("读取测线振幅"),10);
   if(!source.line(type==SgySliceType::Inline,index,options,section,detail,cancel))return QString::fromUtf8(detail);
   task->reportStage(QObject::tr("加载地震层位"),70);
   axis->horizons=source.sectionHorizons(section,cancel);
   const auto unit=source.coordinateUnit().empty()?info.coordinateLengthUnit:source.coordinateUnit();
   const double scale=unit=="ft"?.3048:1.;double distance=0;bool metric=unit=="m"||unit=="ft";
   for(int col=0;col<section.columns;++col) {
    const double x=section.x[size_t(col)],y=section.y[size_t(col)];
    metric=metric&&std::isfinite(x)&&std::isfinite(y);
    if(col)distance+=std::hypot(x-section.x[size_t(col-1)],y-section.y[size_t(col-1)])*scale;
    axis->columnCoordinates.push_back({x,y});axis->columnDistancesM.push_back(float(distance));
   }
   if(!metric)axis->columnDistancesM.clear();
   if(!*image)*image=std::make_shared<SgySliceImage>(paleo::seismicSectionImage(section));
  }
  if(!*image){auto values=std::make_shared<SgySliceImage>();if(!paleo::readSeismicDataSlice(path,type,index,*values,error,cancel,{},type!=SgySliceType::Time))return error;*image=values;}return {};
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
  out->horizons=source.sectionHorizons(section,[task]{return task->cancelRequested();});
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
  *out={info.sampleCount,info.inlineMin,info.inlineMax,info.crosslineMin,info.crosslineMax,info.firstTime,info.sampleInterval};
  out->inlineCount=info.inlineCount;out->xlineCount=info.crosslineCount;return {};
 },{},true);
 connect(task,&PaleoTask::finished,this,[task,out,done]{if(done)done(task->state()==PaleoTask::State::Succeeded,*out,task->errorText());});
 return task;
}
PaleoTask *SeismicTaskService::startDataSlice(const QString& path,SgySliceType type,int index,
    std::function<void(bool,std::shared_ptr<const SgySliceImage>,const QString&)> done,quint64 tileGeneration) {
 if(!taskService_){if(done)done(false,nullptr,tr("后台地震服务未连接"));return nullptr;}
 const auto key=nativePlaneKey(path,type,index);
 if(const auto hit=dataCache_.Find(key)){
  QTimer::singleShot(0,this,[this,path,type,index,key,hit,done]{
   const bool valid=key==nativePlaneKey(path,type,index);
   if(done)done(valid,valid?hit:nullptr,valid?QString():tr("地震来源已变化，请重新加载"));
  });return nullptr;
 }
 auto out=std::make_shared<SgySliceImage>();const QPointer<SeismicTaskService> self(this);
 auto *task=startBounded(type==SgySliceType::Time?tr("读取水平时间切片"):tr("读取地震剖面"),[path,type,index,out,self,tileGeneration](PaleoTask *task)->QString {
  paleo::NativeTimeTileCallback publish;
  if(type==SgySliceType::Time)publish=[self,path,index,tileGeneration,task](std::shared_ptr<const SgySliceImage> tile,int x,int y,int completed,int total){
   if(task->cancelRequested()||!self)return;
   SeismicTimeTile payload;payload.sampleIndex=index;payload.datasetPath=path;payload.generation=tileGeneration;
   payload.x=x;payload.y=y;payload.completed=completed;payload.total=total;payload.image=std::move(tile);
   QMetaObject::invokeMethod(self.data(),[self,payload]{if(self)emit self->timeSliceTileReady(payload);},Qt::QueuedConnection);
  };
  QString error;if(!paleo::readSeismicDataSlice(path,type,index,*out,error,[task]{return task->cancelRequested();},
      [task](int done,int total){task->reportBytes(done,total);},type!=SgySliceType::Time,std::move(publish)))return error;return {};
 },{},true);
 connect(task,&PaleoTask::finished,this,[this,task,out,done,path,type,index,key]{
  const bool changed=key!=nativePlaneKey(path,type,index);
  const bool ok=task->state()==PaleoTask::State::Succeeded&&!changed;
  if(ok)dataCache_.Store(key,out);
  if(done)done(ok,ok?out:nullptr,changed?tr("地震来源已变化，请重新加载"):task->errorText());
 });
 return task;
}
}
