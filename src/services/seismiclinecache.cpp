// 层：数据
#include "seismiclinecache.h"
#include <QCache>
#include <QCryptographicHash>
#include <QDateTime>
#include <QFile>
#include <QFileInfo>
#include <QHash>
#include <QMutex>
#include <QScopeGuard>
#include <QWaitCondition>
#include <memory>

namespace paleo {
namespace {
constexpr qsizetype Budget=128*1024*1024;
struct Pending {QWaitCondition ready;};
struct Cache {
 QMutex mutex;QCache<QString,data::SeismicSectionResult> lines{Budget};
 QHash<QString,std::shared_ptr<Pending>> pending;quint64 generation=0,reads=0,hits=0,waits=0;
};
Cache& cache(){static Cache value;return value;}
QString identity(const QString& path){
 QFileInfo info(path);QFile file(path);if(!info.isFile()||!file.open(QIODevice::ReadOnly))return {};
 QCryptographicHash hash(QCryptographicHash::Sha256);const qint64 block=qMin<qint64>(65536,info.size());
 const auto head=file.read(block);if(head.size()!=block)return {};hash.addData(head);
 if(info.size()>block){if(!file.seek(info.size()-block))return {};const auto tail=file.read(block);if(tail.size()!=block)return {};hash.addData(tail);}
 const QFileInfo after(path);
 if(info.size()!=after.size()||info.lastModified()!=after.lastModified()||info.metadataChangeTime()!=after.metadataChangeTime())return {};
 return info.canonicalFilePath()+QStringLiteral("|%1:%2:%3:").arg(info.size()).arg(info.lastModified().toMSecsSinceEpoch())
     .arg(info.metadataChangeTime().toMSecsSinceEpoch())+QString::fromLatin1(hash.result().toHex());
}
qsizetype bytes(const data::SeismicSectionResult& s){
 return qsizetype(s.amplitudes.size()*sizeof(float)+(s.x.size()+s.y.size()+s.pathX.size()+s.pathY.size()+s.distances.size())*sizeof(double)+
   s.traceIndexes.size()*sizeof(long long)+(s.inlineNumbers.size()+s.crosslineNumbers.size()+s.vertexColumns.size())*sizeof(int));
}
}
bool cachedSeismicLine(const QString& path,const QString& context,bool isInline,int number,
 const data::SectionReadOptions& o,data::SeismicSectionResult& result,std::string& error,
 const data::CancelCheck& cancel,const SeismicLineRead& read){
 result={};error.clear();
 const auto stopped=[&]{if(cancel&&cancel()){error="地震读取已取消";return true;}return false;};
 if(stopped())return false;
 const auto stamp=identity(path);if(stamp.isEmpty())return read(result,error);
 const auto key=stamp+'|'+context+QStringLiteral("|%1:%2:%3:%4:%5:%6:%7").arg(isInline).arg(number)
     .arg(o.firstTime,0,'g',17).arg(o.lastTime,0,'g',17).arg(o.maxColumns).arg(o.maxValues).arg(o.fullResolution);
 auto& c=cache();std::shared_ptr<Pending> pending;quint64 generation=0;
 {
  QMutexLocker lock(&c.mutex);
  for(;;){
   if(stopped())return false;
   if(const auto* hit=c.lines.object(key)){result=*hit;++c.hits;if(stopped()){result={};return false;}return true;}
   pending=c.pending.value(key);
   if(!pending){pending=std::make_shared<Pending>();c.pending.insert(key,pending);generation=c.generation;++c.reads;break;}
   ++c.waits;pending->ready.wait(&c.mutex,25);
  }
 }
 // A scope guard wakes independent waiters even if the source reader throws.
 const auto release=qScopeGuard([&]{QMutexLocker lock(&c.mutex);c.pending.remove(key);pending->ready.wakeAll();});
 data::SeismicSectionResult loaded;
 if(!read(loaded,error)||stopped())return false;
 if(identity(path)!=stamp){error="地震来源在读取期间变化，请重新加载";return false;}
 {
  QMutexLocker lock(&c.mutex);const auto size=bytes(loaded);
  if(c.generation==generation&&size<=Budget)c.lines.insert(key,new data::SeismicSectionResult(loaded),size);
 }
 result=std::move(loaded);return true;
}
void clearSeismicLineCache(){auto& c=cache();QMutexLocker lock(&c.mutex);c.lines.clear();++c.generation;c.reads=c.hits=c.waits=0;}
SeismicLineCacheStats seismicLineCacheStats(){auto& c=cache();QMutexLocker lock(&c.mutex);return {c.reads,c.hits,c.waits,size_t(c.lines.totalCost()),size_t(c.lines.size())};}
}
