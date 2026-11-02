// 层：视图
#pragma once
#include "domain/seismic/sectionhorizons.h"
#include "ui/paleotheme.h"
#include <QPainter>
#include <QPainterPath>
#include <algorithm>
#include <cmath>
#include <functional>

namespace seismic {
// Pure drawing: time/column conversion comes from the owning view's existing axes.
inline void paintSectionHorizons(QPainter& painter,const QRectF& clip,const SectionHorizons& overlay,
                                 const std::function<QPointF(int,double)>& pixelAt){
  painter.save();painter.setClipRect(clip,Qt::IntersectClip);painter.setRenderHint(QPainter::Antialiasing);
  const auto& ink=PaleoTheme::tokens(PaleoTheme::Theme::Light);
  auto font=PaleoTheme::bodyFont();font.setPointSize(PaleoTheme::tokens().labelPt);painter.setFont(font);
  QVector<QRectF> labels;
  for(const auto& curve:overlay.curves){
    QPainterPath path;bool connected=false,haveLabel=false;QPointF anchor;
    for(size_t col=0;col<curve.timesMs.size();++col){
      const double time=curve.timesMs[col];
      if(!std::isfinite(time)){connected=false;continue;}
      const auto point=pixelAt(int(col),time);
      if(!std::isfinite(point.x())||!std::isfinite(point.y())){connected=false;continue;}
      if(connected)path.lineTo(point);else path.moveTo(point);
      connected=true;
      if(!haveLabel&&clip.contains(point)){anchor=point;haveLabel=true;}
    }
    QPen line(ink.text,curve.internalEdge?1.1:1.8);line.setCosmetic(true);
    if(curve.provisional)line.setStyle(Qt::DashLine);
    auto halo=line;halo.setColor(ink.onPrimary);halo.setWidthF(line.widthF()+2.5);
    painter.strokePath(path,halo);painter.strokePath(path,line);
    if(!haveLabel)continue;
    const auto text=painter.fontMetrics().elidedText(curve.name,Qt::ElideRight,std::max(1,int(clip.width()/2)));
    const double width=painter.fontMetrics().horizontalAdvance(text)+6,height=painter.fontMetrics().height()+2;
    const double left=std::clamp(anchor.x()+4,clip.left(),std::max(clip.left(),clip.right()-width));
    QRectF badge(left,std::clamp(anchor.y()-height,clip.top(),std::max(clip.top(),clip.bottom()-height)),width,height);
    if(std::any_of(labels.begin(),labels.end(),[&](const auto& other){return other.intersects(badge);}))continue;
    labels.append(badge);painter.fillRect(badge,ink.surface);painter.setPen(ink.text);
    painter.drawText(badge,Qt::AlignCenter,text);
  }
  painter.restore();
}
}
