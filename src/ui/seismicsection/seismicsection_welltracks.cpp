// 层：视图
#include "seismicsectioncanvas.h"
#include "domain/wellnumeric.h"
#include "ui/paleotheme.h"
#include <QPainter>
#include <QPolygonF>
#include <algorithm>
#include <cmath>

namespace seismic {
void SeismicSectionCanvas::paintWellTracks(QPainter& painter,const SectionWellInfo& well,const QRect& viewport,double x){
  const auto &tokens=PaleoTheme::tokens();
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
    const double width=36,left=x+4+width>viewport.right()?x-4-width:x+4;
    QPolygonF path;painter.setPen(QPen(curve.color.isValid()?curve.color:tokens.text,1.5));painter.setBrush(Qt::NoBrush);
    const auto flush=[&]{if(path.size()>1)painter.drawPolyline(path);path.clear();};
    for(size_t i=0;i<curve.values.size();++i){
      const double value=curve.values[i],time=curve.twtMs[i];
      if(paleo::wellnumeric::isNullCurveValue(value)||!std::isfinite(time)){flush();continue;}
      const double y=timeToPixelY(time);
      if(y<viewport.top()-10||y>viewport.bottom()+10)continue;
      path.append(QPointF(left+std::clamp((value-minimum)/span,0.,1.)*width,y));
    }
    flush();
  }
  if(m_showTops)for(const auto &top:well.tops){
    if(!std::isfinite(top.twtMs))continue;
    const double y=timeToPixelY(top.twtMs);if(y<viewport.top()||y>viewport.bottom())continue;
    const auto color=well.mdTimeDepth?PaleoTheme::sectionHorizonColor(top.topName):top.color;
    painter.setPen(QPen(tokens.onPrimary,4));painter.drawLine(QPointF(x-8,y),QPointF(x+8,y));
    painter.setPen(QPen(color.isValid()?color:tokens.textMuted,2));painter.drawLine(QPointF(x-8,y),QPointF(x+8,y));
    painter.setFont(PaleoTheme::monoFont());const int width=painter.fontMetrics().horizontalAdvance(top.topName)+8;
    const double left=x+48+width>viewport.right()?x-48-width:x+48;
    const QRectF label(left,y-8,width,16);
    painter.setBrush(tokens.surfaceAltRaised);painter.setPen(tokens.border);
    painter.drawRoundedRect(label,tokens.radiusSm,tokens.radiusSm);
    painter.setPen(tokens.text);painter.drawText(label,Qt::AlignCenter,top.topName);
  }
}
}
