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
}
