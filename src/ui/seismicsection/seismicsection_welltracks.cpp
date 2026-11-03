// 层：视图
#include "seismicsectioncanvas.h"
#include "domain/wellnumeric.h"
#include "ui/paleotheme.h"
#include <QPainter>
#include <QPolygonF>
#include <algorithm>
#include <cmath>

namespace seismic {
void SeismicSectionCanvas::paintWellPath(QPainter& painter,const SectionWellInfo& well,const QRect& viewport,double x){
  const auto &tokens=PaleoTheme::tokens();
  if(well.requiresSpatialPath&&well.trajectory.empty())return;
  QPolygonF path;bool observed=false;
  const auto flush=[&]{if(path.size()>1){
    painter.setPen(QPen(tokens.onPrimary,4));painter.drawPolyline(path);
    painter.setPen(QPen(tokens.text,2));painter.drawPolyline(path);
  }path.clear();};
  for(const auto &point:well.trajectory){
    observed=true;
    if(!std::isfinite(point.tracePosition)||!std::isfinite(point.twtMs)||point.twtMs<0||
       !std::isfinite(point.offsetDistanceM)||std::abs(point.offsetDistanceM)>m_bufferDistanceM){flush();continue;}
    path.append(QPointF(traceToPixelX(point.tracePosition),timeToPixelY(point.twtMs)));
  }
  flush();
  // Compatibility for old explicitly supplied projections; native wells never
  // acquire a fictitious two-point/vertical line when spatial samples are absent.
  if(!observed&&!well.requiresSpatialPath){
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
    if(syn.wellId!=well.wellId||syn.twtMs.empty())continue;
    if(!syn.ok){
      painter.setFont(PaleoTheme::bodyFont(tokens.labelPt));painter.setPen(tokens.warning);
      painter.drawText(QRectF(x+8,viewport.top()+4,150,30),Qt::AlignLeft|Qt::TextWordWrap,tr("合成记录不可用：%1").arg(syn.reason));
      continue;
    }
    painter.setPen(QPen(tokens.error,1.4));painter.setBrush(Qt::NoBrush);
    QPolygonF wave;const auto flushWave=[&]{if(wave.size()>1)painter.drawPolyline(wave);wave.clear();};
    for(size_t i=0;i<std::min(syn.twtMs.size(),syn.amplitude.size());++i){
      const auto point=wellPathAtTwt(well,syn.twtMs[i]);
      const double px=well.trajectory.empty()?x:traceToPixelX(point.tracePosition);
      if(!std::isfinite(px)||!std::isfinite(syn.amplitude[i])||(!well.trajectory.empty()&&std::abs(point.offsetDistanceM)>m_bufferDistanceM)){flushWave();continue;}
      wave.append(QPointF(px+44+syn.amplitude[i]*16,timeToPixelY(syn.twtMs[i])));
    }
    flushWave();
  }
}
void SeismicSectionCanvas::paintWellTracks(QPainter& painter,const SectionWellInfo& well,const QRect& viewport,double x){
  const auto &tokens=PaleoTheme::tokens();
  if(well.requiresSpatialPath&&well.trajectory.empty())return;
  const WellCurveItem *primary=nullptr;size_t best=0;
  if(well.mdTimeDepth){
    for(const auto &curve:well.curves){
      size_t usable=0;
      for(size_t i=0;i<std::min(curve.values.size(),curve.twtMs.size());++i)
        if(!paleo::wellnumeric::isNullCurveValue(curve.values[i])&&std::isfinite(curve.twtMs[i]))++usable;
      const bool gr=baseCurveMnemonic(curve.curveName).compare("GR",Qt::CaseInsensitive)==0;
      const bool priorGr=primary&&baseCurveMnemonic(primary->curveName).compare("GR",Qt::CaseInsensitive)==0;
      if(usable>1&&(!primary||(gr&&!priorGr)||(gr==priorGr&&usable>best))){primary=&curve;best=usable;}
    }
  }
  if(m_showCurves)for(const auto &curve:well.curves){
    if(well.mdTimeDepth && &curve!=primary)continue;
    if(curve.values.empty()||curve.values.size()!=curve.twtMs.size())continue;
    const bool gr=well.mdTimeDepth&&baseCurveMnemonic(curve.curveName).compare("GR",Qt::CaseInsensitive)==0;
    const double minimum=gr?20:curve.minVal,span=gr?130:std::max(1e-4f,curve.maxVal-curve.minVal);
    const double width=36;
    QPolygonF path;painter.setPen(QPen(curve.color.isValid()?curve.color:tokens.text,1.5));painter.setBrush(Qt::NoBrush);
    const auto flush=[&]{if(path.size()>1)painter.drawPolyline(path);path.clear();};
    for(size_t i=0;i<curve.values.size();++i){
      const double value=curve.values[i],time=curve.twtMs[i];
      if(paleo::wellnumeric::isNullCurveValue(value)||!std::isfinite(time)){flush();continue;}
      const auto point=wellPathAtMd(well,i<curve.depthsM.size()?curve.depthsM[i]:qQNaN());
      const double px=well.trajectory.empty()?x:traceToPixelX(point.tracePosition);
      if(!std::isfinite(px)||(!well.trajectory.empty()&&std::abs(point.offsetDistanceM)>m_bufferDistanceM)){flush();continue;}
      const double left=px+4+width>viewport.right()?px-4-width:px+4;
      const double y=timeToPixelY(time);
      if(y<viewport.top()-10||y>viewport.bottom()+10)continue;
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
    if(!std::isfinite(topX)||(!well.trajectory.empty()&&std::abs(point.offsetDistanceM)>m_bufferDistanceM))continue;
    const auto color=well.mdTimeDepth?PaleoTheme::sectionHorizonColor(top.topName):top.color;
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
