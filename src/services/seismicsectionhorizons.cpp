// 层：数据
#include "seismicdataaccess.h"

namespace paleo {
seismic::SectionHorizons SeismicDataAccess::sectionHorizons(const data::SeismicSectionResult& section,
                                                           const data::CancelCheck& cancel){
  seismic::SectionHorizons result;
  if(!m_volume)return result;
  data::SectionHorizonQuery query;query.volumeId=m_volume;
  // The vertical section display is native TWT/ms; the current imported survey
  // uses that same user-confirmed convention for undeclared horizon headers.
  query.undeclaredTimeUnit="ms";query.undeclaredTimeType="TWT";
  for(int col=0;col<section.columns;++col){
    if(size_t(col)>=section.inlineNumbers.size()||size_t(col)>=section.crosslineNumbers.size())return result;
    const bool present=size_t(col)<section.traceIndexes.size()&&section.traceIndexes[col]>=0;
    query.columns.push_back({section.inlineNumbers[col],section.crosslineNumbers[col],present});
  }
  data::SectionHorizonResult read;std::string error;
  if(!m_service.readSectionHorizons(query,read,error,cancel)){
    result.warnings.append(QString::fromUtf8(error));return result;
  }
  result.schemeId=QString::fromStdString(read.schemeId);result.revision=QString::fromStdString(read.revision);
  for(auto& curve:read.curves)result.curves.push_back({QString::fromStdString(curve.id),QString::fromStdString(curve.name),
                                                    curve.internalEdge,curve.provisional,std::move(curve.timesMs)});
  for(const auto& warning:read.warnings)result.warnings.append(QString::fromStdString(warning));
  return result;
}
}
