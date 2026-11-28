// 层：视图
#include "seismicsectioncanvas.h"
#include "domain/wellnumeric.h"
#include "ui/paleotheme.h"
#include <QPainter>
#include <QPolygonF>
#include <algorithm>
#include <cmath>

namespace seismic {
namespace {
bool tracksOnLeft(double wellheadX,const QRect &viewport) {
  return wellheadX-viewport.left()>viewport.right()-wellheadX;
}
bool onSection(const WellTrajSample &point, double lastTrace, double buffer) {
  return point.tracePosition >= 0 && point.tracePosition <= lastTrace &&
         std::abs(point.offsetDistanceM) <= buffer && std::isfinite(point.twtMs) && point.twtMs >= 0;
}
bool crossesPathGap(const SectionWellInfo &well, double fromMd, double toMd, double lastTrace, double buffer) {
  if (!std::isfinite(fromMd) || well.trajectory.empty()) return false;
  const auto limits=std::minmax(fromMd,toMd);
  auto station=std::upper_bound(well.trajectory.begin(),well.trajectory.end(),limits.first,
      [](double md,const WellTrajSample &sample){return md<sample.md;});
  for (; station!=well.trajectory.end() && station->md<limits.second; ++station)
    if (!onSection(*station,lastTrace,buffer)) return true;
  return false;
}

void paintSpatialCurve(QPainter &painter,const SeismicSectionCanvas &canvas,const SectionWellInfo &well,
                       const WellCurveItem &curve,bool left,double minimum,double span) {
  if(curve.depthsM.size()!=curve.values.size() || curve.values.size()<2)return;
  QPolygonF path;
  const auto flush=[&]{if(path.size()>1)painter.drawPolyline(path);path.clear();};
  const bool reversed=curve.depthsM.front()>curve.depthsM.back();
  WellTrajSample previous;
  double previousValue=0,previousMd=qQNaN();
  for(size_t sample=0;sample<curve.values.size();++sample){
    const auto i=reversed?curve.values.size()-1-sample:sample;
    const double value=curve.values[i],md=curve.depthsM[i],time=curve.twtMs[i];
    if(!std::isfinite(md)||!std::isfinite(time)||time<0||paleo::wellnumeric::isNullCurveValue(value)){
      flush();previousMd=qQNaN();continue;
    }
    auto current=wellPathAtMd(well,md);
    if(std::isfinite(current.twtMs))current.twtMs=time;
    if(md>previousMd){
      const auto addSegment=[&](const WellTrajSample &a,const WellTrajSample &b){
        const auto segment=clipWellPathSegment(a,b,canvas.traceCount()-1,canvas.bufferDistanceM());
        if(!segment){flush();return;}
        constexpr double width=36.;
        const auto pixel=[&](const WellTrajSample &s){
          const double f=(s.md-previousMd)/(md-previousMd);
          const double logValue=previousValue+(value-previousValue)*f;
          const double x=canvas.traceToPixelX(s.tracePosition)+(left?-4-width:4.);
          return QPointF(x+std::clamp((logValue-minimum)/span,0.,1.)*width,canvas.timeToPixelY(s.twtMs));
        };
        const auto first=pixel(segment->first);
        if(!path.isEmpty() && path.back()!=first)flush();
        if(path.isEmpty())path.append(first);
        path.append(pixel(segment->second));
      };
      const WellTrajSample *start=&previous;
      auto station=std::upper_bound(well.trajectory.begin(),well.trajectory.end(),previousMd,
                                    [](double depth,const auto &s){return depth<s.md;});
      for(;station!=well.trajectory.end()&&station->md<md;++station){
        addSegment(*start,*station);start=&*station;
      }
      addSegment(*start,current);
    }else flush();
    previous=current;previousMd=md;previousValue=value;
  }
  flush();
}
}
void SeismicSectionCanvas::paintWellPath(QPainter& painter,const SectionWellInfo& well,const QRect& viewport,double x){
  const auto &tokens=PaleoTheme::tokens();
  if(well.requiresSpatialPath&&well.trajectory.empty())return;
  QPolygonF path;
  const auto flush=[&]{if(path.size()>1){
    painter.setPen(QPen(tokens.onPrimary,4));painter.drawPolyline(path);
    painter.setPen(QPen(tokens.text,2));painter.drawPolyline(path);
  }path.clear();};
  for(size_t i=1;i<well.trajectory.size();++i){
    const auto segment=clipWellPathSegment(well.trajectory[i-1],well.trajectory[i],m_traces-1,m_bufferDistanceM);
    if(!segment){flush();continue;}
    const auto pixel=[this](const WellTrajSample &s){return QPointF(traceToPixelX(s.tracePosition),timeToPixelY(s.twtMs));};
    const auto first=pixel(segment->first);
    if(!path.isEmpty() && path.back()!=first)flush();
    if(path.isEmpty())path.append(first);
    path.append(pixel(segment->second));
  }
  flush();
  // Compatibility for old explicitly supplied projections; native wells never
  // acquire a fictitious two-point/vertical line when spatial samples are absent.
  if(well.trajectory.empty()&&!well.requiresSpatialPath){
    double bottom=x;const WellTrajectory *projected=nullptr;
    for(const auto &t:m_wellTrajectories)if(t.wellId==well.wellId){projected=&t;break;}
    if(projected){bottom=traceToPixelX(projected->bottomTracePos);
      for(const auto &v:projected->vertices){
        if(!std::isfinite(v.tracePos)||!std::isfinite(v.twtMs)||v.twtMs<0){flush();continue;}
        path.append(QPointF(traceToPixelX(v.tracePos),timeToPixelY(v.twtMs)));
      }
    }
    if(!projected||projected->vertices.size()<2){
      const double t=well.calibrated?well.bottomTwtMs:m_tdModel.DepthToTwtMs(well.totalDepth);
      const double top=well.mdTimeDepth&&!well.mdTimeDepth->points().empty()?well.mdTimeDepth->points().front().timeMs+well.timeShiftMs:m_t0Ms;
      if(std::isfinite(t))path<<QPointF(x,timeToPixelY(top))<<QPointF(bottom,timeToPixelY(t));
    }
    flush();
  }
  for(const auto &syn:m_syntheticOverlays){
    if(syn.wellId!=well.wellId||!syn.ok||syn.twtMs.empty())continue;
    painter.setPen(QPen(tokens.error,1.4));painter.setBrush(Qt::NoBrush);
    QPolygonF wave;const auto flushWave=[&]{if(wave.size()>1)painter.drawPolyline(wave);wave.clear();};
    const double trackOffset=tracksOnLeft(x,viewport)?-44.:44.;
    double previousMd=qQNaN();
    for(size_t i=0;i<std::min(syn.twtMs.size(),syn.amplitude.size());++i){
      const auto point=wellPathAtTwt(well,syn.twtMs[i]);
      const double px=well.trajectory.empty()?x:traceToPixelX(point.tracePosition);
      if(!std::isfinite(px)||!std::isfinite(syn.amplitude[i])||(!well.trajectory.empty()&&!onSection(point,m_traces-1,m_bufferDistanceM))){flushWave();continue;}
      if(!wave.isEmpty()&&crossesPathGap(well,previousMd,point.md,m_traces-1,m_bufferDistanceM))flushWave();
      previousMd=point.md;
      wave.append(QPointF(px+trackOffset+syn.amplitude[i]*16,timeToPixelY(syn.twtMs[i])));
    }
    flushWave();
  }
}
void SeismicSectionCanvas::paintWellTracks(QPainter& painter,const SectionWellInfo& well,const QRect& viewport,double x){
  const auto &tokens=PaleoTheme::tokens();
  if(well.requiresSpatialPath&&well.trajectory.empty())return;
  const bool nativeTracks=well.requiresSpatialPath||well.mdTimeDepth.has_value();
  const WellCurveItem *primary=nullptr;size_t best=0;
  if(nativeTracks&&m_showCurves){
    for(const auto &curve:well.curves){
      if(curve.values.size()!=curve.twtMs.size()||(!well.trajectory.empty()&&curve.depthsM.size()!=curve.values.size()))continue;
      size_t usable=0;
      for(size_t i=0;i<std::min(curve.values.size(),curve.twtMs.size());++i)
        if(!paleo::wellnumeric::isNullCurveValue(curve.values[i])&&std::isfinite(curve.twtMs[i]))++usable;
      const bool gr=baseCurveMnemonic(curve.curveName).compare("GR",Qt::CaseInsensitive)==0;
      const bool priorGr=primary&&baseCurveMnemonic(primary->curveName).compare("GR",Qt::CaseInsensitive)==0;
      if(usable>1&&(!primary||(gr&&!priorGr)||(gr==priorGr&&usable>best))){primary=&curve;best=usable;}
    }
  }
  const bool drawLeft=tracksOnLeft(x,viewport);
  if(m_showCurves)for(const auto &curve:well.curves){
    if(nativeTracks && &curve!=primary)continue;
    if(curve.values.empty()||curve.values.size()!=curve.twtMs.size())continue;
    const bool gr=nativeTracks&&baseCurveMnemonic(curve.curveName).compare("GR",Qt::CaseInsensitive)==0;
    const double minimum=gr?20:curve.minVal,span=gr?130:std::max(1e-4f,curve.maxVal-curve.minVal);
    const double width=36;
    QPolygonF path;painter.setPen(QPen(curve.color.isValid()?curve.color:tokens.text,1.5));painter.setBrush(Qt::NoBrush);
    if(!well.trajectory.empty()){paintSpatialCurve(painter,*this,well,curve,drawLeft,minimum,span);continue;}
    const auto flush=[&]{if(path.size()>1)painter.drawPolyline(path);path.clear();};
    const double left=drawLeft?x-4-width:x+4;
    for(size_t i=0;i<curve.values.size();++i){
      const double value=curve.values[i],time=curve.twtMs[i];
      if(paleo::wellnumeric::isNullCurveValue(value)||!std::isfinite(time)){flush();continue;}
      const double y=timeToPixelY(time);
      path.append(QPointF(left+std::clamp((value-minimum)/span,0.,1.)*width,y));
    }
    flush();
  }
  if(m_showTops)for(const auto &top:well.tops){
    if(!std::isfinite(top.twtMs))continue;
    const double y=timeToPixelY(top.twtMs);if(y<viewport.top()||y>viewport.bottom())continue;
    const auto point=wellPathAtMd(well,top.md);
    const double mark=std::isfinite(top.tracePosition)?top.tracePosition:point.tracePosition;
    const double topX=well.trajectory.empty()?x:traceToPixelX(mark);
    if(!std::isfinite(topX)||(!well.trajectory.empty()&&!onSection(point,m_traces-1,m_bufferDistanceM)))continue;
    const auto color=nativeTracks?PaleoTheme::sectionHorizonColor(top.topName):top.color;
    painter.setPen(QPen(tokens.onPrimary,4));painter.drawLine(QPointF(topX-8,y),QPointF(topX+8,y));
    painter.setPen(QPen(color.isValid()?color:tokens.textMuted,2));painter.drawLine(QPointF(topX-8,y),QPointF(topX+8,y));
    painter.setFont(PaleoTheme::monoFont());const int width=painter.fontMetrics().horizontalAdvance(top.topName)+8;
    const double left=topX+48+width>viewport.right()?topX-48-width:topX+48;
    const QRectF label(left,y-8,width,16);
    painter.setBrush(tokens.surfaceAltRaised);painter.setPen(tokens.border);
    painter.drawRoundedRect(label,tokens.radiusSm,tokens.radiusSm);
    painter.setPen(tokens.text);painter.drawText(label,Qt::AlignCenter,top.topName);
  }
}
}
