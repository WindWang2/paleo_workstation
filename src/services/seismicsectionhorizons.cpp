// 层：数据
#include "seismicdataaccess.h"
#include "domain/documentrelation.h"
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
QByteArray horizonKey(const QString& path,const data::SeismicSectionResult& section,
                      const QString &project,long long volume,const QString &context){
  const QFileInfo file(path);QByteArray bytes;QDataStream stream(&bytes,QIODevice::WriteOnly);
  const QFileInfo db(project+QStringLiteral("/data.sqlite")),wal(db.filePath()+QStringLiteral("-wal"));
  stream<<file.absoluteFilePath()<<file.size()<<file.lastModified()<<file.metadataChangeTime()
        <<db.filePath()<<db.size()<<db.lastModified()<<db.metadataChangeTime()
        <<wal.size()<<wal.lastModified()<<wal.metadataChangeTime()
        <<volume<<context;
  for(int col=0;col<section.columns;++col)
    stream<<section.inlineNumbers[col]<<section.crosslineNumbers[col]<<bool(section.traceIndexes[col]>=0);
  return QCryptographicHash::hash(bytes,QCryptographicHash::Sha256);
}
}
seismic::SectionHorizons SeismicDataAccess::sectionHorizons(const data::SeismicSectionResult& section,
                                                           const data::CancelCheck& cancel,
                                                           const document::InterpretationContext *context){
  seismic::SectionHorizons result;
  if(!m_volume)return result;
  if(cancel&&cancel())return result;
  if(context&&(!context->valid()||context->modelStoreId!="area-model"||
               (context->targetVolume&&context->targetVolume->id!=m_volume))) {
    result.warnings.append(QStringLiteral("地震层位叠加的文档方案或地震体无效"));return result;
  }
  if(context&&context->schemeId.isEmpty())return result;
  if(section.columns<0||section.inlineNumbers.size()!=size_t(section.columns)||section.crosslineNumbers.size()!=size_t(section.columns)||section.traceIndexes.size()!=size_t(section.columns))return result;
  const auto path=QString::fromUtf8(m_file),project=QString::fromUtf8(m_service.projectPath());
  const auto contextKey=context?context->key():QString();
  const auto key=horizonKey(path,section,project,m_volume,contextKey);
  {QMutexLocker lock(&horizonCacheMutex);if(const auto* hit=horizonCache.object(key))return *hit;}
  data::SectionHorizonQuery query;query.volumeId=m_volume;
  if(context) {
    query.schemeId=context->schemeId.toStdString();query.revision=context->schemeRevision.toStdString();
  }
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
  if(context&&(read.schemeId!=query.schemeId||read.revision!=query.revision)) {
    result.warnings.append(QStringLiteral("无法读取固定的地震分层方案修订"));return result;
  }
  result.schemeId=QString::fromStdString(read.schemeId);result.revision=QString::fromStdString(read.revision);
  for(auto& curve:read.curves)result.curves.push_back({QString::fromStdString(curve.id),QString::fromStdString(curve.name),
                                                    curve.internalEdge,curve.provisional,std::move(curve.timesMs)});
  for(const auto& warning:read.warnings)result.warnings.append(QString::fromStdString(warning));
  for(const auto& band:read.bands)result.bands.push_back({QString::fromStdString(band.id),QString::fromStdString(band.name),
      QString::fromStdString(band.topCurveId),QString::fromStdString(band.bottomCurveId),band.subdivision});
  if((!cancel||!cancel())&&key==horizonKey(path,section,project,m_volume,contextKey)){
    qsizetype cost=0;for(const auto& curve:result.curves)cost+=curve.timesMs.size()*sizeof(double)+curve.name.size()*2+curve.id.size()*2;
    QMutexLocker lock(&horizonCacheMutex);horizonCache.insert(key,new seismic::SectionHorizons(result),qMax<qsizetype>(cost,1));
  }
  return result;
}
}
