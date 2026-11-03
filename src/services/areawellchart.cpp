// 层：数据
#include "areawellchart.h"
#include "imagelod.h"
#include "domain/wellcompositemodel.h"
#include "domain/welllogfacies.h"
#include "domain/wellnumeric.h"
#include "io/lasalias.h"
#include "io/areamodel.h"
#include <paleo_data/service.h>
#include <cmath>
#include <limits>
#include <algorithm>

namespace {
QString text(const std::string &s) { return QString::fromUtf8(s); }
double metreScale(const paleo::data::DepthInfo &depth) {
  if (depth.depthType != "MD") return qQNaN();
  const auto unit = text(depth.depthUnit).trimmed().toLower();
  if (unit == "m" || unit == "metre" || unit == "meter") return 1;
  if (unit == "ft" || unit == "feet") return .3048;
  return qQNaN();
}

QString property(const paleo::data::GeologicalInterval &row, const QStringList &labels) {
  for (const auto &p : row.properties)
    if (labels.contains(text(p.measurementName))) return text(p.originalText);
  return {};
}
void appendInterval(const paleo::data::IntervalTrack &track, const paleo::data::GeologicalInterval &v,
                    double scale, WellComposite::ComprehensiveWellData &out) {
  const auto kind = text(track.trackType), name = text(track.trackName);
  const float top = float(v.topDepth * scale), bottom = float(v.bottomDepth * scale);
  if (name.contains(QStringLiteral("地层系统"))) {
    WellComposite::StratigraphyInterval item;item.topDepth=top;item.bottomDepth=bottom;
    item.system=property(v,{QStringLiteral("系")});item.series=property(v,{QStringLiteral("统")});
    item.formation=property(v,{QStringLiteral("组")});
    if(item.formation.isEmpty())item.formation=text(v.intervalName);
    out.stratigraphyIntervals.append(item);
  } else if (kind.contains(QStringLiteral("旋回"))) {
    WellComposite::SymbolItem item;item.topDepth=top;item.bottomDepth=bottom;item.label=text(v.intervalName);item.track=name;
    if(item.label.contains(QStringLiteral("反")))item.kind=WellComposite::SymbolKind::NegativeCycle;
    else if(item.label.contains(QStringLiteral("正")))item.kind=WellComposite::SymbolKind::PositiveCycle;
    else {WellComposite::TextInterval label;label.topDepth=top;label.bottomDepth=bottom;label.category=name;label.text=item.label;out.textIntervals.append(label);return;}
    out.symbolItems.append(item);
  } else if (kind.contains(QStringLiteral("岩性")) || name.contains(QStringLiteral("岩性"))) {
    WellComposite::LithologyInterval item; item.topDepth=top; item.bottomDepth=bottom;
    item.lithoName=text(v.intervalName); item.lithoCode=property(v,{QStringLiteral("岩性代码")});
    out.lithologyIntervals.append(item);
  } else if (kind.contains(QStringLiteral("沉积相")) || name == QStringLiteral("相")
             || name == QStringLiteral("亚相") || name == QStringLiteral("微相")) {
    WellComposite::FaciesInterval item; item.topDepth=top; item.bottomDepth=bottom;
    item.majorFacies=property(v,{QStringLiteral("相"),QStringLiteral("大相")});
    item.subFacies=property(v,{QStringLiteral("亚相")});
    item.microFacies=text(v.faciesType);
    const QString label = text(v.intervalName);
    if (name == QStringLiteral("相") || name.contains(QStringLiteral("大相"))) item.majorFacies = label;
    else if (name == QStringLiteral("亚相")) item.subFacies = label;
    else if (name == QStringLiteral("微相")) item.microFacies = label;
    else if (item.majorFacies.isEmpty()) item.majorFacies = label;
    out.faciesIntervals.append(item);
  } else if (kind.contains(QStringLiteral("取心"))) {
    WellComposite::CoreBarrel item; item.topDepth=top; item.bottomDepth=bottom; item.barrelNo=text(v.intervalName);
    for (const auto &p : v.properties) {
      const auto label=text(p.measurementName);
      if (label.contains(QStringLiteral("进尺"))) item.cutLength=float(p.numericValue*scale);
      else if (label.contains(QStringLiteral("心长"))) item.recoveredLength=float(p.numericValue*scale);
      else if (label.contains(QStringLiteral("收获率"))) item.recoveryRate=float(p.numericValue);
      else item.description+=text(p.originalText)+" ";
    }
    out.coreBarrels.append(item);
  } else if (kind.contains(QStringLiteral("地层")) || kind.contains(QStringLiteral("地震")) ||
             name.contains(QStringLiteral("地震层位")) || name.contains(QStringLiteral("地震小层")) ||
             name.contains(QStringLiteral("砂层")) || name.contains(QStringLiteral("层序"))) {
    WellComposite::FormationInterval item; item.topDepth=top; item.bottomDepth=bottom;
    item.name=text(v.intervalName); item.unitType=name;
    if (name.contains(QStringLiteral("砂层"))) out.sandIntervals.append(item);
    else if (name.contains(QStringLiteral("层序"))) out.sequenceIntervals.append(item);
    else {
      if (name.contains(QStringLiteral("地震小层"))) {
        item.color = QColor(QStringLiteral("#B3E5FC"));
        out.seismicSubIntervals.append(item);
      } else if (name.contains(QStringLiteral("地震层位"))) {
        item.color = QColor(QStringLiteral("#C8E6C9"));
        out.seismicIntervals.append(item);
      }
      out.formationIntervals.append(item);
    }
  } else {
    WellComposite::TextInterval item; item.topDepth=top; item.bottomDepth=bottom;
    item.category=name; item.text=text(v.intervalName);
    for (const auto &p : v.properties) item.text+=" "+text(p.originalText);
    out.textIntervals.append(item);
  }
}
}
QStringList areaWellCurveNames(const QString &project,const QVector<qlonglong> &wells){
  paleo::data::DataService ds;std::string error;QStringList names;
  if(!ds.openProjectReadOnly(paleo::area_model::directory(project).toStdString(),error))return names;
  for(const auto id:wells){std::vector<paleo::data::DataEntry> entries;
    if(ds.listData("curves",id,entries,error))for(const auto &entry:entries)
      if(entry.count>0&&!names.contains(QString::fromStdString(entry.name),Qt::CaseInsensitive))names.append(QString::fromStdString(entry.name));
  }
  return names;
}
bool readAreaWellSectionCurves(const QString &project,qlonglong well,const QStringList &requested,
    QVector<wellsection::Curve> &out,QString *error,const std::function<bool()> &cancelled){
  out.clear();if(error)error->clear();paleo::data::DataService ds;std::string detail;
  if(!ds.openProjectReadOnly(paleo::area_model::directory(project).toStdString(),detail)){if(error)*error=QString::fromStdString(detail);return false;}
  paleo::data::WellPlotOptions options;options.includeIntervals=false;options.includeMarkers=false;options.includePhotos=false;
  paleo::data::WellPlotData model;if(!ds.readWellPlotData(well,options,model,detail,cancelled)){if(error)*error=QString::fromStdString(detail);return false;}
  QVector<wellsection::Curve> available;QStringList problems;
  for(const auto &group:model.logs){
    if(cancelled&&cancelled())return false;
    const auto &logs=group.value;
    if(logs.depthInfo.depthType!="MD"||!std::isfinite(metreScale(logs.depthInfo))){problems.append(QStringLiteral("%1：需要已声明的 MD 米/英尺深度").arg(QString::fromStdString(logs.logSetName)));continue;}
    for(const auto &source:logs.logCurves){
      WellComposite::CurveData normalized;QVector<double> depths,values;QString reason;
      for(double value:logs.samplingAxis.depthValues)depths.append(value);for(double value:source.sampleValues)values.append(value);
      if(!normalizeDepthCurve(depths,values,QString::fromStdString(logs.depthInfo.depthUnit),&normalized,&reason)){problems.append(reason);continue;}
      wellsection::Curve curve;curve.mnemonic=QString::fromStdString(source.curveName);curve.sourceMnemonic=curve.mnemonic;
      curve.unit=QString::fromStdString(source.curveUnit);curve.depths=normalized.depths;curve.values=normalized.values;available.append(curve);
    }
  }
  if(requested.isEmpty())out=available;
  else for(const auto &name:requested){
    const wellsection::Curve *best=nullptr;qsizetype usable=-1;
    for(const auto &curve:available){
      if(LasAliasMap::shared().canonicalCurve(curve.mnemonic)!=LasAliasMap::shared().canonicalCurve(name))continue;
      const auto count=std::count_if(curve.values.cbegin(),curve.values.cend(),[](float v){return !paleo::wellnumeric::isNullCurveValue(v);});
      if(count>usable){best=&curve;usable=count;}
    }
    if(best){auto curve=*best;curve.mnemonic=name;out.append(curve);}
  }
  if(error)*error=problems.join('\n');return !out.isEmpty();
}
bool readAreaWellLithology(const QString &project, qlonglong well,
                          QVector<wellsection::LithoSegment> &out, QString *error) {
  out.clear(); if (error) error->clear();
  paleo::data::DataService ds; std::string detail;
  const auto fail = [&] { out.clear(); if (error) *error=text(detail); return false; };
  if (!ds.openProjectReadOnly(paleo::area_model::directory(project).toUtf8().toStdString(),detail)) return fail();
  paleo::data::WellPlotOptions options;
  options.includeLogs=false; options.includeMarkers=false; options.includePhotos=false;
  paleo::data::WellPlotData model;
  if (!ds.readWellPlotData(well,options,model,detail)) return fail();
  for (const auto &group : model.intervals) {
    const auto &track=group.value;
    if (!text(track.trackType).contains(QStringLiteral("岩性")) &&
        !text(track.trackName).contains(QStringLiteral("岩性"))) continue;
    double scale=metreScale(track.depthInfo);
    if (track.depthInfo.depthType.empty() && track.depthInfo.depthUnit.empty()) scale=metreScale(model.defaultDepthInfo);
    if (!std::isfinite(scale)) { detail="Lithology depth convention is not confirmed as MD/m or MD/ft"; return fail(); }
    for (const auto &v : track.intervals) {
      wellsection::LithoSegment segment;
      segment.topMd=v.topDepth*scale; segment.baseMd=v.bottomDepth*scale;
      segment.litho=text(v.intervalName); segment.source=wellsection::LithoSource::Interpreted;
      segment.provenance=QStringLiteral("DataService · %1 · track %2").arg(text(track.trackName)).arg(group.entry.id);
      out.append(segment);
    }
  }
  return true;
}
bool readAreaWellChart(paleo::data::DataService &service, qlonglong well,
                      WellComposite::ComprehensiveWellData &out, QString *error,
                      const std::function<bool()> &cancelled) {
  out={}; paleo::data::WellPlotData model; std::string detail;
  if (!service.readWellPlotData(well,{},model,detail,cancelled)) { if(error)*error=text(detail); return false; }
  WellComposite::ComprehensiveWellData next;
  next.modelWellId=well; next.wellName=text(model.well.wellName);
  next.x=model.well.wellheadX; next.y=model.well.wellheadY;
  next.coordinateSystem=text(model.well.coordinateSystem); next.coordinateUnit=text(model.well.coordinateUnit);
  next.projectCoordinateSystem=text(model.projectCoordinates.coordinateSystem);
  next.projectCoordinateUnit=text(model.projectCoordinates.coordinateUnit);
  double lo=std::numeric_limits<double>::infinity(), hi=-lo;
  const auto depth=[&](double d){if(std::isfinite(d)){lo=qMin(lo,d);hi=qMax(hi,d);}};
  double wellScale=qQNaN();
  for (const auto &group : model.logs) {
    const double scale=metreScale(group.value.depthInfo);
    if (std::isfinite(scale)) { wellScale=scale; break; }
  }
  const auto scaleFor=[&](const paleo::data::DepthInfo &info, const QString &name, bool useWellDepth){
    const double scale=metreScale(info);
    if (std::isfinite(scale)) return scale;
    const bool undeclared=info.depthType.empty() && text(info.depthUnit).trimmed().isEmpty();
    if (useWellDepth && undeclared && std::isfinite(wellScale)) return wellScale;
    next.dataWarnings << QStringLiteral("%1：深度类型或单位未确认为 MD/m 或 MD/ft，未绘制").arg(name);
    return qQNaN();
  };
  for (const auto &group : model.logs) {
    if(cancelled && cancelled()){if(error)*error=QStringLiteral("Cancelled");return false;}
    const auto &logs=group.value; const double scale=scaleFor(logs.depthInfo,text(logs.logSetName),false);
    if(!std::isfinite(scale))continue;
    for (const auto &curve : logs.logCurves) {
      WellComposite::CurveData value; value.name=text(curve.curveName); value.unit=text(curve.curveUnit);
      value.modelSourceId=group.entry.sourceId; value.modelLogSetId=group.entry.parentId;
      float min=std::numeric_limits<float>::infinity(),max=-min;
      // The canvas searches an ascending MD axis. Preserve paired values while
      // ordering the display copy; the stored sampling order remains unchanged.
      std::vector<size_t> order;
      for(size_t i=0;i<logs.samplingAxis.depthValues.size();++i)
        if(std::isfinite(logs.samplingAxis.depthValues[i]))order.push_back(i);
      std::stable_sort(order.begin(),order.end(),[&](size_t a,size_t b){return logs.samplingAxis.depthValues[a]<logs.samplingAxis.depthValues[b];});
      for (const auto i : order) {
        const double d=logs.samplingAxis.depthValues[i]*scale;
        if(!std::isfinite(d))continue;
        value.depths.append(float(d)); value.values.append(float(curve.sampleValues.at(i))); depth(d);
        if(std::isfinite(value.values.last())){min=qMin(min,value.values.last());max=qMax(max,value.values.last());}
      }
      if(std::isfinite(min)){value.minScale=min;value.maxScale=max>min?max:min+1;}
      if(text(logs.logSetName).contains(QStringLiteral("离散"))){value.mode=WellComposite::CurveDisplayMode::Discrete;next.discreteCurves.append(value);}
      else next.continuousCurves.append(value);
    }
  }
  for (const auto &group : model.intervals) {
    if(cancelled && cancelled()){if(error)*error=QStringLiteral("Cancelled");return false;}
    const auto &track=group.value; const double scale=scaleFor(track.depthInfo,text(track.trackName),true);
    if(!std::isfinite(scale))continue;
    for(const auto &row:track.intervals) {
      if(!std::isfinite(row.topDepth)||!std::isfinite(row.bottomDepth))continue;
      depth(row.topDepth*scale);depth(row.bottomDepth*scale);appendInterval(track,row,scale,next);
    }
  }
  for(const auto &group:model.markers) {
    const double scale=scaleFor(group.value.depthInfo,text(group.value.trackName),true);
    if(!std::isfinite(scale))continue;
    for(const auto &marker:group.value.markers)if(std::isfinite(marker.depthValue)) {
      depth(marker.depthValue*scale);next.standardHorizons.append({marker.depthValue*scale,text(marker.markerName)});
    }
  }
  if(!model.photos.empty()) {
    const double scale=scaleFor(model.defaultDepthInfo,QStringLiteral("岩心照片"),true);
    if(std::isfinite(scale))for(const auto &group:model.photos)for(const auto &photo:group.value.photos) {
      if(!std::isfinite(photo.topDepth)||!std::isfinite(photo.bottomDepth))continue;
      WellComposite::ImageDepthItem item;item.topDepth=float(photo.topDepth*scale);item.bottomDepth=float(photo.bottomDepth*scale);
      item.imagePath=text(photo.imagePath);item.caption=text(photo.description);
      item.thumbnail=paleo::imagelod::loadThumbnail(item.imagePath).thumbnail;next.images.append(item);
      depth(item.topDepth);depth(item.bottomDepth);
    }
  }
  if(std::isfinite(lo)){next.minDepth=lo;next.maxDepth=hi;}
  if(error)*error=next.isEmpty()?(next.dataWarnings.isEmpty()?QStringLiteral("No drawable MD well data in model"):next.dataWarnings.join('\n')):QString();
  out=std::move(next);return !out.isEmpty();
}
