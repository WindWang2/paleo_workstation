// 层：数据
#include "seismicunitinput.h"
#include "analysisunitcontext.h"
#include "io/areamodel.h"
#include <paleo_data/service.h>
#include <QFileInfo>
#include <algorithm>
#include <cmath>
#include <limits>
#include <ogr_spatialref.h>
#include <cpl_conv.h>

namespace paleo::strat {
namespace {
QString canonical(const QString &path){const QFileInfo file(path);return file.canonicalFilePath().isEmpty()?file.absoluteFilePath():file.canonicalFilePath();}
bool selectVolume(paleo::data::DataService &ds,const QString &source,long long &id,paleo::data::SeismicVolume &volume,std::string &error){
  std::vector<paleo::data::DataEntry> entries;if(!ds.listData("volumes",0,entries,error))return false;
  id=0;
#ifdef Q_OS_WIN
  constexpr auto pathCase=Qt::CaseInsensitive;
#else
  constexpr auto pathCase=Qt::CaseSensitive;
#endif
  for(const auto &entry:entries){
    paleo::data::SeismicVolume candidate;if(!ds.readVolume(entry.id,candidate,error))return false;
    if(!source.isEmpty()&&canonical(QString::fromStdString(candidate.dataFile)).compare(canonical(source),pathCase)!=0)continue;
    if(id){error="有多个匹配地震体，请明确选择一个地震来源";return false;}
    id=entry.id;volume=std::move(candidate);
  }
  if(!id){error="所选地震来源不在当前工区数据服务中";return false;}return true;
}
}
bool readSeismicAnalysisWindow(const QString &project,const UnitSelection &unit,const QString &source,
    paleo::data::SeismicHorizonWindowResult &out,QString *error,const std::function<bool()> &cancelled){
  out={};UnitSelection frozen;if(!freezeAnalysisUnit(project,unit,frozen,error))return false;
  paleo::data::DataService ds;std::string detail;long long volumeId=0;paleo::data::SeismicVolume volume;
  const auto fail=[&]{out={};if(error)*error=QString::fromStdString(detail);return false;};
  if(!ds.openProjectReadOnly(area_model::directory(project).toStdString(),detail)||!selectVolume(ds,source,volumeId,volume,detail))return fail();
  paleo::data::SeismicFileInfo info;if(!ds.inspectSeismic(volumeId,info,detail,cancelled))return fail();
  paleo::data::SeismicHorizonWindowQuery query;query.volumeId=volumeId;
  query.undeclaredTimeUnit="ms";query.undeclaredTimeType="TWT"; // current survey's explicit display convention
  query.target.schemeId=frozen.schemeId.toStdString();query.target.revision=frozen.revision.toStdString();
  query.target.intervalId.value=frozen.intervalId.toStdString();query.target.subIntervalId.value=frozen.subIntervalId.toStdString();
  query.inlineStride=std::max(1,(info.inlineCount+63)/64);query.crosslineStride=std::max(1,(info.crosslineCount+63)/64);
  query.resampledSampleCount=16;query.computeDepthRange=false;query.maxValues=1048576;
  const QFileInfo before(QString::fromStdString(volume.dataFile));const auto size=before.size();const auto modified=before.lastModified();
  if(!ds.readSeismicHorizonWindow(query,out,detail,cancelled))return fail();
  const QFileInfo after(QString::fromStdString(volume.dataFile));
  if(after.size()!=size||after.lastModified()!=modified){detail="地震来源在层段取数过程中已变更，请重新运行";return fail();}
  return true;
}
SeismicUnitInput prepareSeismicUnitInput(const QString &project,const UnitSelection &unit,const QString &source,
    const std::function<bool()> &cancelled){
  SeismicUnitInput input;input.sourcePath=source;
  if(!freezeAnalysisUnit(project,unit,input.unit,&input.error))return input;
  paleo::data::SeismicHorizonWindowResult window;
  if(!readSeismicAnalysisWindow(project,input.unit,source,window,&input.error,cancelled))return input;
  paleo::data::DataService ds;std::string detail;paleo::data::SeismicVolume volume;
  if(!ds.openProjectReadOnly(area_model::directory(project).toStdString(),detail)||!ds.readVolume(window.volumeId,volume,detail)){
    input.error=QString::fromStdString(detail);return input;
  }
  OGRSpatialReference crs;char *wkt=nullptr;
  if(crs.SetFromUserInput(volume.geometry.coordinateSystem.c_str())!=OGRERR_NONE||crs.exportToWkt(&wkt)!=OGRERR_NONE){
    input.error=QStringLiteral("地震体坐标系不可用于成果网格");return input;
  }
  input.crs=QString::fromUtf8(wkt);CPLFree(wkt);input.sourcePath=QString::fromStdString(volume.dataFile);
  double xmin=std::numeric_limits<double>::infinity(),xmax=-xmin,ymin=xmin,ymax=-xmin;
  // Include missing/review traces in the grid geometry. Their cells remain NaN.
  for(const auto &trace:window.traces)if(std::isfinite(trace.x)&&std::isfinite(trace.y)){
    xmin=std::min(xmin,trace.x);xmax=std::max(xmax,trace.x);ymin=std::min(ymin,trace.y);ymax=std::max(ymax,trace.y);
  }
  const double dx=(xmax-xmin)/(input.columns-1),dy=(ymax-ymin)/(input.rows-1);
  if(!std::isfinite(dx)||!std::isfinite(dy)||dx<=0||dy<=0){input.error=QStringLiteral("当前地震层段缺少有效二维坐标范围");return input;}
  input.extent=QRectF(xmin-dx/2,ymin-dy/2,dx*input.columns,dy*input.rows);
  input.samples.fill(std::numeric_limits<float>::quiet_NaN(),input.rows*input.columns);
  QVector<double> distances(input.samples.size(),std::numeric_limits<double>::infinity());
  for(const auto &trace:window.traces){
    if(cancelled&&cancelled()){input.error=QStringLiteral("已取消");input.samples.clear();return input;}
    if(!std::isfinite(trace.x)||!std::isfinite(trace.y))continue;
    const int col=std::clamp(int(std::lround((trace.x-xmin)/dx)),0,input.columns-1);
    const int row=std::clamp(int(std::lround((ymax-trace.y)/dy)),0,input.rows-1),index=row*input.columns+col;
    const double distance=std::hypot(trace.x-(xmin+col*dx),trace.y-(ymax-row*dy));
    if(distance>=distances[index])continue;
    distances[index]=distance;
    input.samples[index]=trace.status==1&&std::isfinite(trace.rmsAmplitude)?trace.rmsAmplitude:std::numeric_limits<float>::quiet_NaN();
  }
  input.validCells=int(std::count_if(input.samples.begin(),input.samples.end(),[](float v){return std::isfinite(v);}));
  if(!input.validCells)input.error=QStringLiteral("当前层位单元内没有有效地震样点");
  return input;
}
}
