// 层：功能
#include "algorithms/inversion/wavelet.h"
#include "inversionnative.h"
#include "services/seismicdataaccess.h"
#include "services/seismicwelltrace.h"
#include <QCryptographicHash>
#include <QDataStream>
#include <QIODevice>
#include <cmath>
#include <algorithm>

namespace paleo::inv {
QString curveEvidenceHash(const std::vector<double>& axis,const std::vector<float>& values){
 QByteArray bytes;QDataStream data(&bytes,QIODevice::WriteOnly);data.setVersion(QDataStream::Qt_6_0);
 data<<quint64(axis.size())<<quint64(values.size());for(double t:axis)data<<t;for(float v:values)data<<v;
 return QString::fromLatin1(QCryptographicHash::hash(bytes,QCryptographicHash::Sha256).toHex());
}
QVariantMap trajectoryEvidence(const std::vector<seismic::WellTrajSample>& path,const QString& source,const QString& reference){
 QByteArray bytes;QDataStream data(&bytes,QIODevice::WriteOnly);data.setVersion(QDataStream::Qt_6_0);
 for(const auto& p:path)data<<p.md<<p.x<<p.y<<p.twtMs;
 return {{QStringLiteral("trajectorySource"),source},{QStringLiteral("timeReference"),reference},
  {QStringLiteral("trajectoryPoints"),int(path.size())},
  {QStringLiteral("trajectorySha256"),QString::fromLatin1(QCryptographicHash::hash(bytes,QCryptographicHash::Sha256).toHex())}};
}
bool computeRegisteredWavelet(InversionWorkflow::WaveletJob* job,
    const std::function<bool()>& cancelled,const std::function<void(double)>& progress){
 const auto fail=[&](const QString& message){job->error=message;job->ok=false;return false;};
 SeismicDataAccess source;QString reason;std::string error;
 if(!source.openProjectSource(job->req.projectDirectory,job->req.seismicPath,&reason))return fail(reason);
 SeismicWellTrace along;
 if(!source.wellTrace(job->req.trajectory,job->req.timeReference,along,error,cancelled))return fail(QString::fromStdString(error));
 if(cancelled&&cancelled())return fail(QStringLiteral("已取消"));
 if(progress)progress(.7);
 const int length=std::max(3,int(std::lround(job->req.waveletLengthMs/along.sampleIntervalMs))|1);
 const auto result=paleo::inversion::extractWavelet(along.amplitudes.data(),int(along.amplitudes.size()),
   along.firstTimeMs,along.sampleIntervalMs,job->spikes.data(),int(job->spikes.size()),
   -.5*(length-1)*along.sampleIntervalMs,length,paleo::inversion::WaveletExtractOptions{});
 if(!result.ok)return fail(QStringLiteral("沿井子波提取失败：%1").arg(QString::fromStdString(result.reason)));
 job->wavelet=result.wavelet;job->fitCorrelation=result.fitCorrelation;
 job->spatialEvidence=trajectoryEvidence(job->req.trajectory,job->req.trajectorySource,job->req.timeReference);
 job->spatialEvidence.insert(QStringLiteral("sampling"),QStringLiteral("DataService_nearest_native_trace_linear_time_along_path"));
 job->spatialEvidence.insert(QStringLiteral("coordinateSystem"),along.coordinateSystem);
 job->spatialEvidence.insert(QStringLiteral("validSamples"),along.finiteSamples);
 job->spatialEvidence.insert(QStringLiteral("missingSamples"),along.missingSamples);
 job->spatialEvidence.insert(QStringLiteral("acSha256"),curveEvidenceHash(job->req.acDepthsM,job->req.acUsPerM));
 job->spatialEvidence.insert(QStringLiteral("denSha256"),curveEvidenceHash(job->req.denDepthsM,job->req.denValues));
 job->spatialEvidence.insert(QStringLiteral("fitCorrelation"),job->fitCorrelation);
 job->spatialEvidence.insert(QStringLiteral("dominantFreqHz"),job->wavelet.dominantFreqHz());
 job->nearestTraceNote=QStringLiteral("DataService 沿井逐样点取道 · %1 有效 / %2 缺测 · %3 路径列")
   .arg(along.finiteSamples).arg(along.missingSamples).arg(along.sourceColumns);
 job->ok=true;if(progress)progress(1);return true;
}
}
