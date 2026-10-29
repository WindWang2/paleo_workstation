// 层：数据
#include "seismicdataaccess.h"
#include "../io/areamodel.h"

namespace paleo {
bool SeismicDataAccess::open(const QString& path,QString *error) {
 m_volume=0;m_unit.clear();m_file=path.toUtf8().toStdString();long long source=0;
 const auto state=area_model::openSource(path,m_service,source,error);
 if(state==area_model::Read::Failed)return false;
 if(state==area_model::Read::Unbound)return true;
 std::vector<data::DataEntry> entries;std::string detail;
 if(!m_service.listSourceData("volumes",source,entries,detail)){if(error)*error=QString::fromUtf8(detail);return false;}
 for(const auto& entry:entries)if(entry.sourceId==source) {
  if(m_volume){if(error)*error=QStringLiteral("地震来源关联多个地震体，请先明确关联");return false;}
  m_volume=entry.id;
 }
 if(!m_volume){if(error)*error=QStringLiteral("工区数据服务中未找到该来源的地震体");return false;}
 data::SeismicVolume volume;
 if(!m_service.readVolume(m_volume,volume,detail)){if(error)*error=QString::fromUtf8(detail);return false;}
 m_file=volume.dataFile;m_unit=volume.geometry.coordinateUnit;
 return true;
}
bool SeismicDataAccess::inspect(data::SeismicFileInfo& result,std::string& error,const data::CancelCheck& cancel) {
 return m_volume?m_service.inspectSeismic(m_volume,result,error,cancel):data::DataService::inspectSeismicFile(m_file,result,error,cancel);
}
bool SeismicDataAccess::line(bool il,int number,const data::SectionReadOptions& options,data::SeismicSectionResult& result,std::string& error,const data::CancelCheck& cancel) {
 const std::string direction=il?"inline":"crossline";
 return m_volume?m_service.readSeismicLine(m_volume,direction,number,options,result,error,cancel):data::DataService::readSeismicFileLine(m_file,direction,number,options,result,error,cancel);
}
bool SeismicDataAccess::time(double ms,const data::TimeSliceReadOptions& options,data::SeismicTimeSliceResult& result,std::string& error,const data::CancelCheck& cancel) {
 return m_volume?m_service.readSeismicTimeSlice(m_volume,ms,options,result,error,cancel):data::DataService::readSeismicFileTimeSlice(m_file,ms,options,result,error,cancel);
}
bool SeismicDataAccess::section(const std::vector<std::pair<int,int>>& path,int columns,bool interpolate,bool outside,data::SeismicSectionResult& result,std::string& error,const data::CancelCheck& cancel) {
 if(!m_volume){data::SeismicFileInfo info;if(!inspect(info,error,cancel))return false;m_unit=info.coordinateLengthUnit;}
 return m_volume?m_service.readSeismicGridSection(m_volume,path,columns,interpolate,outside,result,error,cancel):data::DataService::readSeismicFileGridSection(m_file,path,columns,interpolate,outside,result,error,cancel);
}
bool SeismicDataAccess::xySection(const std::vector<std::pair<double,double>>& path,const QString& timeReference,
    data::SeismicSectionResult& result,std::string& error,const data::CancelCheck& cancel) {
 result={};error.clear();
 if(!m_volume){error="连井剖面需要工区数据服务中已登记的地震体";return false;}
 data::SeismicVolume volume;if(!m_service.readVolume(m_volume,volume,error))return false;
 if(volume.sampling.timeType!="TWT"){error="连井剖面需要确认双程时间 TWT，不能混用单程或未知时间";return false;}
 if(timeReference.isEmpty()||volume.sampling.timeReference!=timeReference.toUtf8().toStdString()) {
  error="井时深表与地震体的时间基准未确认一致，请先校正时间基准";return false;
 }
 data::SeismicSection section;section.parentVolumeId=m_volume;
 section.coordinateSystem=m_service.projectCrs();section.coordinateUnit=m_service.projectCrsUnit();
 for(const auto& p:path){data::SeismicSectionPoint point;point.x=p.first;point.y=p.second;section.pathPoints.push_back(point);}
 data::SectionReadOptions options;options.fullResolution=true;options.maxValues=64000000;
 if(!m_service.extractSection(section,options,result,error,cancel))return false;
 const double factor=result.timeUnit=="s"?1000.:1.;
 result.firstTime*=factor;result.sampleInterval*=factor;result.timeUnit="ms";
 if(volume.geometry.coordinateUnit=="ft")for(auto& distance:result.distances)distance*=0.3048;
 return true;
}
QString seismicProjectSource(const QString& project,const QString& preferred,QString *error) {
 if(error)error->clear();
 if(!preferred.isEmpty())return preferred;
 data::DataService service;std::string detail;std::vector<data::DataEntry> entries;
 if(!service.openProjectReadOnly(area_model::directory(project).toUtf8().toStdString(),detail)||!service.listData("volumes",0,entries,detail)) {
  if(error)*error=QString::fromUtf8(detail);return {};
 }
 if(entries.size()!=1){if(error)*error=entries.empty()?QStringLiteral("请先在数据服务登记地震体"):QStringLiteral("工区有多个地震体，请先选择用于连井的地震体");return {};}
 data::SeismicVolume volume;if(!service.readVolume(entries.front().id,volume,detail)){if(error)*error=QString::fromUtf8(detail);return {};}
 return QString::fromUtf8(volume.dataFile);
}
}
