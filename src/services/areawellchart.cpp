// 层：数据
#include "areawellchart.h"
#include "areawellmetadata.h"
#include "imagelod.h"
#include "domain/wellcompositemodel.h"
#include "domain/welllogfacies.h"
#include "domain/wellnumeric.h"
#include "io/lasalias.h"
#include "io/areamodel.h"
#include <paleo_data/service.h>
#include <QCoreApplication>
#include <QDir>
#include <QHash>
#include <QSet>
#include <cmath>
#include <limits>
#include <algorithm>

namespace {
namespace data = paleo::data;
QString text(const std::string &s) { return QString::fromUtf8(s); }
double metreScale(const paleo::data::DepthInfo &depth) {
  if (depth.depthType != "MD") return qQNaN();
  return paleo::wellnumeric::depthScaleToMetres(text(depth.depthUnit)).value_or(qQNaN());
}

bool validCurveWindow(const AreaWellCurveWindow &window) {
  return !std::isinf(window.topMd) && !std::isinf(window.bottomMd) &&
         !(window.topMd > window.bottomMd) &&
         window.maxSamples > 0 && window.maxSamples <= 8000000 &&
         window.maxReadBytes > 0 && window.maxReadBytes <= 256LL*1024*1024;
}

bool hasDepthBounds(const AreaWellCurveWindow &window) {
  return std::isfinite(window.topMd) || std::isfinite(window.bottomMd);
}

bool explicitCurveWindow(const AreaWellCurveWindow &window) {
  const AreaWellCurveWindow defaults;
  return hasDepthBounds(window) || window.maxSamples!=defaults.maxSamples ||
         window.maxReadBytes!=defaults.maxReadBytes;
}

bool readCurvePayload(data::DataService &service, const data::DataEntry &entry,
                      const AreaWellCurveWindow &window, data::CurveReadOptions &budget,
                      data::CurveWindowResult &result, std::string &error) {
  data::CurveWindowQuery query;
  query.curveId=entry.id; query.includeAuxiliaryAxes=false;
  const bool bounded=hasDepthBounds(window);
  // Depth bounds are in metres. Obtain the declared native axis without samples.
  query.sampleCount=bounded?0:entry.count;
  if (!bounded && entry.count>budget.maxSamples) { error="Well curve sample budget exceeded"; return false; }
  data::CurveReadStats stats;
  if (!service.readCurveWindow(query,result,error,budget,&stats)) return false;
  if (bounded) {
    const double scale=metreScale(result.depthInfo);
    if (!std::isfinite(scale)) return true; // Report the unknown convention without reading payload.
    if (!service.readCurveSlice(entry.id,window.topMd/scale,window.bottomMd/scale,
                               true,result.curve,error,budget,&stats)) return false;
  }
  budget.maxReadBytes-=stats.payloadBytesRead;
  budget.maxSamples-=static_cast<long long>(result.curve.depths.size());
  return true;
}

struct ChartDepthExtent {
  double top, bottom, scale;
};

std::optional<ChartDepthExtent> sourceTrackExtent(data::DataService &service,
    const data::WellPlotData &model, const std::function<bool()> &cancelled) {
  const double wellScale=metreScale(model.defaultDepthInfo);
  const double total=model.well.totalDrilledDepth*wellScale;
  if (std::isfinite(total) && total>0)
    return ChartDepthExtent{0,total,wellScale};

  std::string detail;
  std::vector<data::DataEntry> entries;
  if ((cancelled && cancelled()) || !service.listData("curves",model.wellId,entries,detail))
    return std::nullopt;

  // Charge every payload read, including unsuccessful candidates. Each request
  // fetches one sample, with the SDK's small compatibility ceiling for legacy blobs.
  data::CurveReadOptions budget;
  budget.maxSamples=1;
  budget.maxReadBytes=2*1024*1024;
  budget.cancelled=cancelled;
  QSet<qlonglong> seenLogSets;
  for (const auto &entry:entries) {
    if ((cancelled && cancelled()) || budget.maxReadBytes<=0)
      break;
    if (entry.count<2 || entry.parentId<=0 || seenLogSets.contains(entry.parentId))
      continue;
    seenLogSets.insert(entry.parentId);
    data::SourceIndexEntry source;
    if (!service.readSourceIndexEntry(entry.sourceId,source,detail) || !source.active)
      continue;

    data::CurveWindowQuery query;
    query.curveId=entry.id;
    query.sampleCount=0;
    query.processed=false;
    query.includeAuxiliaryAxes=false;
    data::CurveWindowResult metadata;
    if (!service.readCurveWindow(query,metadata,detail,budget) || metadata.totalSamples<2)
      continue;
    const double scale=metreScale(metadata.depthInfo);
    if (!std::isfinite(scale))
      continue;
    const auto endpoint=[&](qlonglong index) {
      if (budget.maxReadBytes<=0) return qQNaN();
      query.firstSample=index;
      query.sampleCount=1;
      data::CurveWindowResult sample;
      data::CurveReadStats stats;
      const bool ok=service.readCurveWindow(query,sample,detail,budget,&stats);
      budget.maxReadBytes-=stats.payloadBytesRead;
      if (!ok || sample.totalSamples!=metadata.totalSamples || sample.curve.depths.size()!=1 ||
          metreScale(sample.depthInfo)!=scale)
        return qQNaN();
      return sample.curve.depths.front()*scale;
    };
    const double first=endpoint(0);
    if (!std::isfinite(first))
      continue;
    const double last=endpoint(metadata.totalSamples-1);
    if (std::isfinite(last) && last!=first && std::isfinite(last-first))
      return ChartDepthExtent{std::min(first,last),std::max(first,last),scale};
  }
  return std::nullopt;
}

bool readCurveWindows(data::DataService &service, qlonglong well, const QStringList &requested,
                      const AreaWellCurveWindow &window, const std::function<bool()> &cancelled,
                      bool stableSelection, QVector<WellComposite::CurveData> &out,
                      QStringList &warnings, std::string &error,
                      const QHash<QString,qlonglong> &exact={}, bool strictMd=false,
                      AreaWellCurveReadUsage *usage=nullptr, const QString &mdReference={}) {
  if (!validCurveWindow(window)) { error="Invalid MD window or well curve budget"; return false; }
  if (strictMd && mdReference.isEmpty()) {error="input_md_reference_unknown";return false;}
  std::vector<data::DataEntry> entries;
  if(strictMd) {
    if(!readBoundedAreaWellMetadata(service,"curves",well,4096,entries,error,cancelled))return false;
  } else if(!service.listData("curves",well,entries,error))return false;
  QHash<qlonglong,QString> exactNames;
  if (!exact.isEmpty()) {
    if (exact.size()!=requested.size() || requested.isEmpty()) {error="invalid_exact_curve_selection";return false;}
    QSet<qlonglong> ids;
    for (const auto &feature:requested) {
      const auto id=exact.value(feature,0);
      if(id<=0||ids.contains(id)){error="invalid_exact_curve_selection";return false;}
      ids.insert(id);exactNames.insert(id,feature);
    }
  }
  auto &aliases=LasAliasMap::shared();
  for(auto it=exactNames.begin();it!=exactNames.end();++it) {
    const auto entry=std::find_if(entries.begin(),entries.end(),[id=it.key()](const auto &candidate){return candidate.id==id;});
    if(entry==entries.end()||aliases.canonicalCurve(text(entry->name))!=aliases.canonicalCurve(it.value())) {
      error="invalid_exact_curve_selection";return false;
    }
  }
  QHash<QString,QString> mnemonics;
  for (const auto &name:requested) {
    const auto canonical=aliases.canonicalCurve(name);
    if(!mnemonics.contains(canonical))mnemonics.insert(canonical,name);
  }
  QHash<QString,const data::DataEntry *> selected;
  const bool fixed=stableSelection && (!mnemonics.isEmpty()||!exactNames.isEmpty());
  if (fixed && exactNames.isEmpty()) for (const auto &entry:entries) {
    const auto name=text(entry.name), canonical=aliases.canonicalCurve(name);
    const auto mnemonic=mnemonics.constFind(canonical);
    if(mnemonic==mnemonics.cend())continue;
    if (!exact.isEmpty() && exact.value(*mnemonic, 0) != entry.id) continue;
    const auto *previous=selected.value(canonical);
    const bool exact=name.compare(*mnemonic,Qt::CaseInsensitive)==0;
    const bool previousExact=previous && text(previous->name).compare(*mnemonic,Qt::CaseInsensitive)==0;
    // Candidate identity is determined before touching payload, independent of the requested depth.
    if(!previous || exact>previousExact || (exact==previousExact && entry.id<previous->id))
      selected.insert(canonical,&entry);
  }
  data::CurveReadOptions budget;
  budget.maxSamples=window.maxSamples; budget.maxReadBytes=window.maxReadBytes; budget.cancelled=cancelled;
  for (const auto &entry:entries) {
    if (cancelled && cancelled()) { error="Cancelled"; return false; }
    const auto name=text(entry.name), canonical=aliases.canonicalCurve(name);
    if(!exactNames.isEmpty()) {if(!exactNames.contains(entry.id))continue;}
    else if ((!mnemonics.isEmpty() && !mnemonics.contains(canonical)) || (fixed && selected.value(canonical)!=&entry)) continue;
    if(entry.count<=0) {
      if(fixed)warnings.append(QStringLiteral("%1：所选曲线没有样点").arg(name));
      continue;
    }
    data::CurveWindowResult source;
    if (!readCurvePayload(service,entry,window,budget,source,error)) return false;
    if (!std::isfinite(metreScale(source.depthInfo))) {
      if (strictMd) { error="input_depth_semantics_unknown"; return false; }
      warnings.append(QStringLiteral("%1：需要已声明的 MD 米/英尺深度").arg(name)); continue;
    }
    if (strictMd && (source.depthInfo.mdReference.empty() || text(source.depthInfo.mdReference)!=mdReference)) {
      error="input_md_reference_mismatch";return false;
    }
    const auto &axis=source.curve.depths, &values=source.curve.values;
    if (axis.empty() || (fixed && !strictMd && std::all_of(values.cbegin(),values.cend(),[](double v) {
          return paleo::wellnumeric::isNullCurveValue(float(v));
        }))) {
      if(fixed)warnings.append(QStringLiteral("%1：所选曲线在当前深度窗无有效样点").arg(name));
      continue;
    }
    WellComposite::CurveData curve;
    QString reason;
    QVector<double> preparedValues(values.begin(),values.end());
    if (strictMd) {
      if(source.curve.quality.size()!=values.size()){error="input_quality_unknown";return false;}
      for(qsizetype i=0;i<preparedValues.size();++i) {
        if(source.curve.quality[size_t(i)]&~7U){error="input_quality_unknown";return false;}
        if(source.curve.quality[size_t(i)]&3)preparedValues[i]=qQNaN();
      }
    }
    if (!normalizeDepthCurve(QVector<double>(axis.begin(),axis.end()),preparedValues,
                             text(source.depthInfo.depthUnit),&curve,&reason)) {
      warnings.append(reason); continue;
    }
    curve.name=!exactNames.isEmpty()?exactNames.value(entry.id):strictMd?mnemonics.value(canonical,name):name;
    curve.unit=text(source.curve.unit);
    curve.modelSourceId=entry.sourceId; curve.modelLogSetId=entry.parentId;
    out.append(std::move(curve));
  }
  if (usage) {
    usage->samples += window.maxSamples-budget.maxSamples;
    usage->payloadBytes += window.maxReadBytes-budget.maxReadBytes;
  }
  if (cancelled && cancelled()) { error="Cancelled"; return false; }
  return true;
}

bool readChartTrack(data::DataService &service, qlonglong well, const AreaWellChartQuery &query,
                    data::WellPlotIntervals &out, QString *error) {
  const auto &selection=*query.intervalTrack;
  const auto fail=[&](const QString &reason){if(error)*error=reason;return false;};
  if (selection.use.modelWellId!=well || selection.use.context.purpose!=paleo::document::RelationPurpose::WellColumn ||
      selection.object.kind!=paleo::document::ObjectKind::IntervalTrack ||
      (query.context && selection.use.context!=*query.context) ||
      QDir::cleanPath(selection.selected.modelDirectory)!=QDir::cleanPath(text(service.projectPath())))
    return fail(QStringLiteral("所选分层不属于当前井、工区或井柱用途"));
  const auto resolved=paleo::document::resolveDocumentRelation(selection);
  if (!resolved.ok) return fail(resolved.error);
  std::string detail;
  std::vector<data::DataEntry> entries;
  if (!service.listData("intervals",well,entries,detail)) return fail(text(detail));
  const auto found=std::find_if(entries.begin(),entries.end(),[&](const auto &entry){return entry.id==resolved.object.id;});
  if (found==entries.end() || found->count>100000)
    return fail(QStringLiteral("所选分层不存在或超过 100000 段读取预算"));
  out.entry=*found;
  if (!service.readIntervalTrackInfo(found->id,out.value,detail)) return fail(text(detail));
  const double scale=metreScale(out.value.depthInfo);
  if (!std::isfinite(scale)) return fail(QStringLiteral("所选分层需要明确声明 MD 与米或英尺单位"));
  std::vector<data::IntervalRecord> records;
  if (!service.readIntervals(found->id,query.curves.topMd/scale,query.curves.bottomMd/scale,records,detail))
    return fail(text(detail));
  for (auto &row:records) out.value.intervals.push_back(std::move(row.value));
  return true;
}

bool appendSchemeIntervals(data::DataService &service, qlonglong well, const AreaWellChartQuery &query,
                           WellComposite::ComprehensiveWellData &out, QString *error) {
  const auto &context=*query.context;
  data::StratigraphicScheme scheme; std::string detail;
  if (!service.readStratigraphicScheme(context.schemeId.toStdString(),context.schemeRevision.toStdString(),scheme,detail)) {
    if(error)*error=text(detail);return false;
  }
  const auto *parent=context.intervalId.isEmpty()?nullptr:data::framework::findInterval(scheme,{context.intervalId.toStdString()});
  const auto *child=context.subIntervalId.isEmpty()?nullptr:data::framework::findSubInterval(scheme,{context.subIntervalId.toStdString()});
  if ((!context.intervalId.isEmpty() && (!parent || parent->status!="valid")) ||
      (!context.subIntervalId.isEmpty() && (!child || !parent || child->interval!=parent->id || child->status!="valid"))) {
    if(error)*error=QStringLiteral("井柱所选层段不属于该方案修订");return false;
  }
  const auto append=[&](const data::StratigraphicInterval &interval, double top, double bottom, const QString &suffix) {
    if(!std::isfinite(top)||!std::isfinite(bottom)||top>bottom||bottom<query.curves.topMd||top>query.curves.bottomMd)return;
    const auto *first=data::framework::findBoundary(scheme,interval.top), *last=data::framework::findBoundary(scheme,interval.bottom);
    if(!first||!last)return;
    WellComposite::FormationInterval item;
    item.name=text(first->name)+QStringLiteral("–")+text(last->name)+suffix;
    item.unitType=text(scheme.name);item.code=child?text(child->id.value):text(interval.id.value);
    item.topDepth=float(top);item.bottomDepth=float(bottom);
    out.formationIntervals.append(std::move(item));
  };
  if (child) {
    const auto range=data::framework::wellRange(scheme,well,child->id);
    const double scale=range.depthType=="MD"?(range.depthUnit=="m"?1.:range.depthUnit=="ft"?.3048:qQNaN()):qQNaN();
    if(range.resolved)append(*parent,range.top*scale,range.bottom*scale,QStringLiteral("/S%1").arg(child->index));
    if(out.formationIntervals.isEmpty())
      out.dataWarnings.append(QStringLiteral("所选子层段在该井的 MD 深度窗内无可用分层"));
    return true;
  }
  for (const auto &occ:scheme.wellOccurrences) {
    if(occ.wellId!=well||occ.depthType!="MD"||occ.status=="pending"||(parent&&occ.interval!=parent->id))continue;
    const auto *interval=parent?parent:data::framework::findInterval(scheme,occ.interval);
    const double scale=occ.depthUnit=="m"?1.:occ.depthUnit=="ft"?.3048:qQNaN();
    if(interval&&interval->status=="valid")append(*interval,occ.top*scale,occ.bottom*scale,{});
  }
  if(out.formationIntervals.isEmpty())
    out.dataWarnings.append(QStringLiteral("所选分层方案在该井的 MD 深度窗内无可用分层"));
  return true;
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
    QVector<wellsection::Curve> &out,QString *error,const std::function<bool()> &cancelled,
    const AreaWellCurveWindow &window){
  out.clear();if(error)error->clear();paleo::data::DataService ds;std::string detail;
  QVector<WellComposite::CurveData> windows; QStringList problems;
  if(well<=0){if(error)*error=QStringLiteral("无效的井编号");return false;}
  if(!ds.openProjectReadOnly(paleo::area_model::directory(project).toUtf8().toStdString(),detail) ||
     !readCurveWindows(ds,well,requested,window,cancelled,explicitCurveWindow(window),windows,problems,detail)) {
    if(error)*error=text(detail);return false;
  }
  QVector<wellsection::Curve> available;
  for(auto &curve:windows) {
    available.append({curve.name,curve.name,curve.unit,std::move(curve.depths),std::move(curve.values)});
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
                      const std::function<bool()> &cancelled,const AreaWellChartQuery *query,
                      AreaWellCurveReadUsage *usage) {
  out={}; paleo::data::WellPlotData model; std::string detail;
  if (query && !validCurveWindow(query->curves)) {
    if(error)*error=QStringLiteral("井柱深度窗或曲线预算无效");return false;
  }
  if (query && query->context && (!query->context->valid() || query->context->modelStoreId!="area-model" ||
      query->context->purpose!=paleo::document::RelationPurpose::WellColumn)) {
    if(error)*error=QStringLiteral("井柱分层方案或用途无效");return false;
  }
  const bool selectedTrack=query && query->intervalTrack.has_value();
  const bool selectedScheme=query && query->context && !query->context->schemeId.isEmpty() && !selectedTrack;
  const bool depthWindow=query && hasDepthBounds(query->curves);
  const bool curveWindow=query && (!query->curveNames.isEmpty() || explicitCurveWindow(query->curves));
  data::WellPlotOptions options;
  // Whole-chart callers still receive the legacy log-set display classifications.
  options.includeLogs=!curveWindow;
  options.includeIntervals=!selectedTrack&&!selectedScheme&&!depthWindow&&
      (!query||query->exactIntervalTrackIds.isEmpty());
  if (depthWindow) {
    options.includeMarkers=false;options.includePhotos=false;
  }
  const bool allowSourceTrackFallback=query && query->allowSourceTrackFallback && !curveWindow;
  if (allowSourceTrackFallback) options.maxValues=query->maxDefaultPlotValues;
  bool sourceTrackFallback=false;
  if (!service.readWellPlotData(well,options,model,detail,cancelled)) {
    if (!allowSourceTrackFallback || detail!="Well plot exceeds value budget") {
      if(error)*error=text(detail);return false;
    }
    options.includeLogs=false;
    if (!service.readWellPlotData(well,options,model,detail,cancelled)) {
      if(error)*error=text(detail);return false;
    }
    sourceTrackFallback=true;
  }
  if (selectedTrack) {
    data::WellPlotIntervals selected;
    if (!readChartTrack(service,well,*query,selected,error)) return false;
    model.intervals.push_back(std::move(selected));
  }
  if (query && !query->exactIntervalTrackIds.isEmpty()) {
    if (query->exactIntervalTrackIds.size()>32) { if(error)*error="interval_track_budget_exceeded";return false; }
    std::vector<data::DataEntry> entries;
    if(!readBoundedAreaWellMetadata(service,"intervals",well,128,entries,detail,cancelled)) {
      if(error)*error=text(detail);return false;
    }
    QSet<qlonglong> seen;
    for (const auto id:query->exactIntervalTrackIds) {
      const auto found=std::find_if(entries.begin(),entries.end(),[id](const auto &entry){return entry.id==id;});
      if (seen.contains(id)||found==entries.end()||found->count>100000) {if(error)*error="invalid_interval_track_ref";return false;}
      seen.insert(id);data::WellPlotIntervals selected;selected.entry=*found;
      if (!service.readIntervalTrackInfo(id,selected.value,detail)) {if(error)*error=text(detail);return false;}
      const double scale=metreScale(selected.value.depthInfo);
      if (!std::isfinite(scale)) {if(error)*error="input_depth_semantics_unknown";return false;}
      if (query->requireDeclaredMd && (query->mdReference.isEmpty() ||
          text(selected.value.depthInfo.mdReference)!=query->mdReference)) {
        if(error)*error="input_md_reference_mismatch";return false;
      }
      std::vector<data::IntervalRecord> records;
      if (!service.readIntervals(id,query->curves.topMd/scale,query->curves.bottomMd/scale,records,detail)) {
        if(error)*error=text(detail);return false;
      }
      for (auto &record:records)selected.value.intervals.push_back(std::move(record.value));
      model.intervals.push_back(std::move(selected));
    }
  }
  WellComposite::ComprehensiveWellData next;
  next.modelWellId=well; next.wellName=text(model.well.wellName);
  next.x=model.well.wellheadX; next.y=model.well.wellheadY;
  next.coordinateSystem=text(model.well.coordinateSystem); next.coordinateUnit=text(model.well.coordinateUnit);
  next.projectCoordinateSystem=text(model.projectCoordinates.coordinateSystem);
  next.projectCoordinateUnit=text(model.projectCoordinates.coordinateUnit);
  double lo=std::numeric_limits<double>::infinity(), hi=-lo;
  const auto depth=[&](double d){if(std::isfinite(d)){lo=qMin(lo,d);hi=qMax(hi,d);}};
  double wellScale=qQNaN();
  if (sourceTrackFallback) {
    wellScale=metreScale(model.defaultDepthInfo);
    if (const auto extent=sourceTrackExtent(service,model,cancelled)) {
      depth(extent->top);depth(extent->bottom);
      if (!std::isfinite(wellScale)) wellScale=extent->scale;
    }
    if (cancelled && cancelled()) { if(error)*error=QStringLiteral("Cancelled");return false; }
    next.dataWarnings << QCoreApplication::translate("AreaWellChart", "整井曲线超出读取预算。可从来源选择曲线，按深度窗加载。");
  }
  if (selectedScheme) {
    if(!appendSchemeIntervals(service,well,*query,next,error))return false;
    for(const auto &interval:next.formationIntervals){depth(interval.topDepth);depth(interval.bottomDepth);}
  }
  if (curveWindow) {
    QVector<WellComposite::CurveData> windows;
    if (!readCurveWindows(service,well,query->curveNames,query->curves,cancelled,true,windows,next.dataWarnings,detail,
          query->exactCurveIds,query->requireDeclaredMd,usage,query->mdReference)) {
      if(error)*error=text(detail);return false;
    }
    wellScale=metreScale(model.defaultDepthInfo);
    for (auto &curve:windows) {
      float min=std::numeric_limits<float>::infinity(), max=-min;
      for (float d:curve.depths) depth(d);
      for (float v:curve.values) if(std::isfinite(v)){min=qMin(min,v);max=qMax(max,v);}
      if(std::isfinite(min)){curve.minScale=min;curve.maxScale=max>min?max:min+1;}
      next.continuousCurves.append(std::move(curve));
    }
  }
  for (const auto &group : model.logs) {
    const double scale=metreScale(group.value.depthInfo);
    if (std::isfinite(scale)) { wellScale=scale; break; }
  }
  const auto scaleFor=[&](const paleo::data::DepthInfo &info, const QString &name, bool useWellDepth){
    const double scale=metreScale(info);
    if (std::isfinite(scale)) return scale;
    const bool undeclared=info.depthType.empty() && text(info.depthUnit).trimmed().isEmpty();
    if (useWellDepth && undeclared && std::isfinite(wellScale) && (!query || !query->requireDeclaredMd)) return wellScale;
    next.dataWarnings << QStringLiteral("%1：深度类型或单位未确认为 MD/m 或 MD/ft，未绘制").arg(name);
    return qQNaN();
  };
  for (const auto &group : model.logs) {
    if(cancelled && cancelled()){if(error)*error=QStringLiteral("Cancelled");return false;}
    const auto &logs=group.value; const double scale=scaleFor(logs.depthInfo,text(logs.logSetName),false);
    if(!std::isfinite(scale)) {
      if(query&&query->requireDeclaredMd){if(error)*error="input_depth_semantics_unknown";return false;}
      continue;
    }
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
    if(!std::isfinite(scale)) {
      if(query&&query->requireDeclaredMd){if(error)*error="input_depth_semantics_unknown";return false;}
      continue;
    }
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
  const bool displayable=!next.isEmpty() || (sourceTrackFallback && lo<hi && std::isfinite(hi-lo));
  if(error)*error=displayable?QString():(next.dataWarnings.isEmpty()?QStringLiteral("No drawable MD well data in model"):next.dataWarnings.join('\n'));
  out=std::move(next);return displayable;
}
