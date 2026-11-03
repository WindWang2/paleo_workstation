// 层：数据
#include "seismicdataaccess.h"
#include "io/areamodel.h"
#include <QCache>
#include <QCryptographicHash>
#include <QDataStream>
#include <QDateTime>
#include <QFileInfo>
#include <QMutex>

namespace paleo {
namespace {
QMutex horizonCacheMutex;
QCache<QByteArray,seismic::SectionHorizons> horizonCache{16*1024*1024};
QByteArray horizonKey(const QString& path,const data::SeismicSectionResult& section){
  const QFileInfo file(path);QByteArray bytes;QDataStream stream(&bytes,QIODevice::WriteOnly);
  stream<<file.absoluteFilePath()<<file.size()<<file.lastModified()<<file.metadataChangeTime()<<area_model::cacheToken(path);
  for(int col=0;col<section.columns;++col)
    stream<<section.inlineNumbers[col]<<section.crosslineNumbers[col]<<bool(section.traceIndexes[col]>=0);
  return QCryptographicHash::hash(bytes,QCryptographicHash::Sha256);
}
}
seismic::SectionHorizons SeismicDataAccess::sectionHorizons(const data::SeismicSectionResult& section,
                                                           const data::CancelCheck& cancel){
  seismic::SectionHorizons result;
  if(!m_volume)return result;
  if(cancel&&cancel())return result;
  if(section.columns<0||section.inlineNumbers.size()!=size_t(section.columns)||section.crosslineNumbers.size()!=size_t(section.columns)||section.traceIndexes.size()!=size_t(section.columns))return result;
  const auto key=horizonKey(QString::fromUtf8(m_file),section);
  {QMutexLocker lock(&horizonCacheMutex);if(const auto* hit=horizonCache.object(key))return *hit;}
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
  for(const auto& band:read.bands)result.bands.push_back({QString::fromStdString(band.id),QString::fromStdString(band.name),
      QString::fromStdString(band.topCurveId),QString::fromStdString(band.bottomCurveId),band.subdivision});
  if((!cancel||!cancel())&&key==horizonKey(QString::fromUtf8(m_file),section)){
    qsizetype cost=0;for(const auto& curve:result.curves)cost+=curve.timesMs.size()*sizeof(double)+curve.name.size()*2+curve.id.size()*2;
    QMutexLocker lock(&horizonCacheMutex);horizonCache.insert(key,new seismic::SectionHorizons(result),qMax<qsizetype>(cost,1));
  }
  return result;
}
}
