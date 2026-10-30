// 层：数据
#pragma once
#include <QString>
#include <paleo_data/service.h>

namespace paleo {
// 每次调用在工作线程创建独立的 Data_service 连接。已绑定的工区错误不回落文件读口。
class SeismicDataAccess {
public:
 bool open(const QString& path, QString *error);
 QString sourcePath() const { return QString::fromUtf8(m_file); }
 std::string coordinateUnit() const { return m_unit; }
 QString coordinateSystem() const { return QString::fromUtf8(m_crs); }
 bool inspect(paleo::data::SeismicFileInfo& result,std::string& error,const paleo::data::CancelCheck& cancel={});
 bool line(bool isInline,int number,const paleo::data::SectionReadOptions& options,paleo::data::SeismicSectionResult& result,std::string& error,const paleo::data::CancelCheck& cancel={});
 bool time(double ms,const paleo::data::TimeSliceReadOptions& options,paleo::data::SeismicTimeSliceResult& result,std::string& error,const paleo::data::CancelCheck& cancel={});
 bool section(const std::vector<std::pair<int,int>>& path,int maxColumns,bool interpolate,bool outside,paleo::data::SeismicSectionResult& result,std::string& error,const paleo::data::CancelCheck& cancel={});
 bool xySection(const std::vector<std::pair<double,double>>& path,const QString& timeReference,
                paleo::data::SeismicSectionResult& result,std::string& error,const paleo::data::CancelCheck& cancel={});
private:
 paleo::data::DataService m_service;
 long long m_volume=0;
 std::string m_file;
 std::string m_unit;
 std::string m_crs;
};
QString seismicProjectSource(const QString& project,const QString& preferred,QString *error);
}
