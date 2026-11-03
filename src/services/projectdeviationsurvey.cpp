// 层：数据
#include "projectdeviationsurvey.h"
#include "io/areamodel.h"
#include <paleo_data/service.h>
#include <QFile>
#include <QObject>
#include <algorithm>

ProjectDeviationRead readProjectDeviation(const QString& project, qlonglong wellId, qlonglong sourceId,
    std::optional<paleo::WellDeviationSurvey>& result, QString* error)
{
  result.reset();if(error)error->clear();
  const auto root=paleo::area_model::directory(project);
  if(wellId<=0||!QFile::exists(root+"/data.sqlite"))return ProjectDeviationRead::Unbound;
  const auto fail=[&](const QString& why){if(error)*error=why;return ProjectDeviationRead::Failed;};
  paleo::data::DataService service;std::string detail;std::vector<paleo::data::DataEntry> entries;
  if(!service.openProjectReadOnly(root.toStdString(),detail)||!service.listData("trajectories",wellId,entries,detail))
    return fail(QString::fromStdString(detail));
  if(sourceId>0)entries.erase(std::remove_if(entries.begin(),entries.end(),[&](const auto& e){return e.sourceId!=sourceId;}),entries.end());
  if(entries.empty())return ProjectDeviationRead::Ready;
  if(entries.size()!=1)return fail(QObject::tr("本井有多份测斜资料，请明确采用的 Survey 表"));
  paleo::data::TrajectorySurvey survey;
  if(!service.readTrajectory(entries.front().id,survey,detail))return fail(QString::fromStdString(detail));
  const auto reference=QString::fromStdString(survey.azimuthReference).trimmed().toLower();
  if(reference!=QLatin1String("grid north"))return fail(QObject::tr("测斜方位未声明为网格北，不能直接投射到地震坐标"));
  paleo::data::WellPlotOptions options;options.includeLogs=options.includeIntervals=options.includeMarkers=options.includePhotos=false;
  paleo::data::WellPlotData metadata;
  if(!service.readWellPlotData(wellId,options,metadata,detail))return fail(QString::fromStdString(detail));
  const auto unit=metadata.defaultDepthInfo.depthUnit;
  if(unit!="m"&&unit!="ft")return fail(QObject::tr("测斜 MD 缺少米或英尺单位"));
  QVector<paleo::DeviationStation> stations;
  for(const auto& p:survey.surveyPoints)stations.append({p.measuredDepth*(unit=="ft"?.3048:1.),p.inclinationDegrees,p.azimuthDegrees});
  QString reason;result=paleo::WellDeviationSurvey::fromStations(stations,&reason);
  if(!result)return fail(reason);
  return ProjectDeviationRead::Ready;
}
