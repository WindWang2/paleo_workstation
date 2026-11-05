// 层：数据
#include "analysisunitcontext.h"
#include "subintervalfactors.h"
#include "stratigraphicsubintervals.h"
#include "io/areamodel.h"
#include <paleo_data/service.h>
#include <QJsonObject>
#include <cmath>
#include <algorithm>
#include "domain/wellcompositemodel.h"

namespace paleo::strat {
namespace fw=paleo::data::framework;
QString analysisProjectCrs(const QString &project,QString *error){
  paleo::data::DataService ds;paleo::data::ProjectCoordinates coordinates;std::string detail;
  if(!ds.openProjectReadOnly(area_model::directory(project).toStdString(),detail)||!ds.readProjectCoordinates(coordinates,detail)||coordinates.coordinateUnit!="m"){
    if(error)*error=QString::fromStdString(detail.empty()?"分析需要工区米制坐标系":detail);return {};
  }
  return QString::fromStdString(coordinates.coordinateSystem);
}
bool freezeAnalysisUnit(const QString &project,const UnitSelection &requested,UnitSelection &frozen,
                        QString *error,bool allowAggregate){
  frozen={};if(error)error->clear();
  const auto fail=[&](const QString &reason){if(error)*error=reason;return false;};
  if(!requested.isValid()||(!allowAggregate&&requested.aggregate))
    return fail(QStringLiteral("请先选择地震层位单元及具体子层段 S1–S4"));
  paleo::data::DataService ds;std::string detail;paleo::data::StratigraphicScheme scheme;
  if(!ds.openProjectReadOnly(area_model::directory(project).toStdString(),detail)||
     !ds.readStratigraphicScheme(requested.schemeId.toStdString(),requested.revision.toStdString(),scheme,detail))
    return fail(QString::fromStdString(detail));
  const auto *interval=fw::findInterval(scheme,{requested.intervalId.toStdString()});
  if(!interval||interval->status!="valid")return fail(QStringLiteral("所选地震层位单元不存在或顶底关系无效"));
  int index=0;
  if(!requested.aggregate){
    const auto *sub=fw::findSubInterval(scheme,{requested.subIntervalId.toStdString()});
    if(!sub||sub->interval!=interval->id)return fail(QStringLiteral("子层段与所选地震层位单元不一致"));
    index=sub->index;
  }
  frozen=makeSelection(scheme,requested.intervalId,index);
  return frozen.isValid()&&!frozen.revision.isEmpty();
}
bool analysisUnitGeometryCurrent(const QString &project,const UnitSelection &unit,QString *error){
  return analysisUnitGeometryCurrentFor(project,unit,AnalysisGeometryDependency::LegacyGlobal,0,error);
}
bool analysisUnitGeometryCurrentFor(const QString &project,const UnitSelection &unit,
    AnalysisGeometryDependency dependency,qlonglong wellId,QString *error){
  paleo::data::DataService ds;std::string detail;paleo::data::StratigraphicScheme old,head;
  if(!unit.isValid()||unit.revision.isEmpty()||
     !ds.openProjectReadOnly(area_model::directory(project).toStdString(),detail)||
     !ds.readStratigraphicScheme(unit.schemeId.toStdString(),unit.revision.toStdString(),old,detail)||
     !ds.readStratigraphicScheme(unit.schemeId.toStdString(),{},head,detail)){
    if(error)*error=QString::fromStdString(detail.empty()?"分析层段身份不完整":detail);return false;
  }
  const auto *interval=fw::findInterval(old,{unit.intervalId.toStdString()});
  const auto *sub=fw::findSubInterval(old,{unit.subIntervalId.toStdString()});
  if(!interval || (!unit.aggregate && (!sub || sub->interval!=interval->id)) ||
     (dependency==AnalysisGeometryDependency::WellRange && wellId<=0)) {
    if(error)*error=QStringLiteral("分析依赖缺少有效单元或明确井编号");return false;
  }
  paleo::data::SubIntervalResultBinding binding;
  binding.interval={unit.intervalId.toStdString()};binding.subInterval={unit.subIntervalId.toStdString()};
  binding.aggregate=unit.aggregate;binding.wellId=wellId;
  if(dependency==AnalysisGeometryDependency::WellRange)binding.kind="well_facies";
  else if(dependency==AnalysisGeometryDependency::SeismicSurface)binding.kind="seismic_facies";
  const auto expected=fw::resultGeometryFingerprint(old,binding);
  const bool current=!expected.empty() && expected==fw::resultGeometryFingerprint(head,binding);
  if(!current && error)*error=QStringLiteral("分析实际依赖的层段或地震面在计算期间已修改，请重新获取或预测");
  return current;
}
paleo::data::WellSubIntervalRange analysisWellRange(const paleo::data::StratigraphicScheme &scheme,
    const UnitSelection &unit,const QString &entityId,qlonglong modelWellId){
  const bool native=modelWellId>0&&std::any_of(scheme.wellOccurrences.begin(),scheme.wellOccurrences.end(),[&](const auto &o){
    return o.wellId==modelWellId&&o.interval.value==unit.intervalId.toStdString();});
  const auto id=native||entityId.isEmpty()?modelWellId:stableWellKey(entityId);
  if(!unit.aggregate)return fw::wellRange(scheme,id,{unit.subIntervalId.toStdString()});
  paleo::data::WellSubIntervalRange combined;combined.resolved=true;combined.provisional=false;combined.wellId=id;
  for(const auto *sub:fw::subIntervalsOf(scheme,{unit.intervalId.toStdString()})){
    const auto r=fw::wellRange(scheme,id,sub->id);if(!r.resolved)return r;
    if(!std::isfinite(combined.top)||r.top<combined.top)combined.top=r.top;
    if(!std::isfinite(combined.bottom)||r.bottom>combined.bottom)combined.bottom=r.bottom;
    combined.depthType=r.depthType;combined.depthUnit=r.depthUnit;combined.provisional|=r.provisional;
  }
  combined.index=4;combined.resolved=std::isfinite(combined.top)&&combined.bottom>combined.top;return combined;
}
WellComposite::ComprehensiveWellData scopeWellData(const WellComposite::ComprehensiveWellData &data,
    const paleo::data::WellSubIntervalRange &range){
  auto scoped=data;scoped.minDepth=range.top;scoped.maxDepth=range.bottom;
  const bool valid=range.resolved&&range.depthType=="MD"&&range.depthUnit=="m"&&std::isfinite(range.top)&&std::isfinite(range.bottom)&&range.bottom>range.top;
  for(auto &curve:scoped.continuousCurves){
    const auto original=curve;curve.depths.clear();curve.values.clear();
    for(qsizetype i=0;i<original.depths.size();++i){
      const double depth=original.depths[i];
      if(!valid||!std::isfinite(depth)||depth<range.top||depth>range.bottom||(depth==range.bottom&&range.index!=4))continue;
      curve.depths.append(original.depths[i]);if(i<original.values.size())curve.values.append(original.values[i]);
    }
  }
  const auto clip=[&](auto &intervals){
    intervals.erase(std::remove_if(intervals.begin(),intervals.end(),[&](const auto &row){return !valid||!std::isfinite(row.topDepth)||!std::isfinite(row.bottomDepth)||row.bottomDepth<=row.topDepth||row.bottomDepth<=range.top||row.topDepth>=range.bottom;}),intervals.end());
    for(auto &row:intervals){
      row.topDepth=std::max(double(row.topDepth),range.top);
      // S4 includes its last sample; retain an existing category covering that endpoint.
      if(range.index!=4)row.bottomDepth=std::min(double(row.bottomDepth),range.bottom);
    }
  };
  clip(scoped.formationIntervals);clip(scoped.stratigraphyIntervals);clip(scoped.lithologyIntervals);
  clip(scoped.seismicIntervals);clip(scoped.seismicSubIntervals);return scoped;
}
QJsonArray scopeWellRows(const QJsonArray &rows,const paleo::data::WellSubIntervalRange &range){
  QJsonArray scoped;
  if(!range.resolved||range.depthType!="MD"||range.depthUnit!="m")return scoped;
  for(const auto &row:rows){
    const double d=row.toObject().value(QStringLiteral("深度")).toDouble(NAN);
    if(std::isfinite(d)&&d>=range.top&&(d<range.bottom||(range.index==4&&d==range.bottom)))scoped.append(row);
  }
  return scoped;
}
}
