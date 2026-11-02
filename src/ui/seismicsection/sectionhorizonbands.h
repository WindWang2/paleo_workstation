// 层：视图
#pragma once
#include "domain/seismic/sectionhorizons.h"
#include "ui/paleotheme.h"
#include <QPainter>
#include <QPainterPath>
#include <QHash>
#include <QVector>
#include <algorithm>
#include <functional>
#include <cmath>

namespace seismic {
using HorizonPixelAt=std::function<QPointF(int,double)>;
struct HorizonBandLabel {QRectF rect;QString text;QColor color;};
inline bool horizonPointIsFinite(const QPointF& point){return std::isfinite(point.x())&&std::isfinite(point.y());}
// Explicit scheme relationships only. Missing or inverted pairs break the fill.
inline void paintSectionHorizonBands(QPainter& painter,const QRectF& clip,const SectionHorizons& overlay,
                                     const HorizonPixelAt& pixelAt,QVector<HorizonBandLabel>& labels){
  QHash<QString,const SectionHorizonCurve *> curves;
  QHash<QString,QString> parent;
  for(const auto& curve:overlay.curves)curves.insert(curve.id,&curve);
  for(const auto& band:overlay.bands)if(!band.subdivision)parent.insert(band.id,band.name);
  for(const auto& band:overlay.bands){
    if(!band.subdivision&&std::any_of(overlay.bands.begin(),overlay.bands.end(),[&](const auto& other){
      return other.subdivision&&other.id.startsWith(band.id+QStringLiteral(":S"));}))continue;
    const auto *top=curves.value(band.topCurveId),*base=curves.value(band.bottomCurveId);
    if(!top||!base)continue;
    const auto parentId=band.id.left(band.id.lastIndexOf(QStringLiteral(":S")));
    auto color=PaleoTheme::sectionHorizonColor(parent.value(parentId,band.name));
    if(band.subdivision)color=color.lighter(100+12*(band.subdivision-1));
    auto fill=color;fill.setAlphaF(.20f);
    QVector<QPointF> upper,lower;QRectF bestLabel;
    const double w=painter.fontMetrics().horizontalAdvance(band.name)+8,h=painter.fontMetrics().height()+4;
    const auto flush=[&]{
      if(upper.size()>1){
        QPainterPath path;path.moveTo(upper.front());
        for(int i=1;i<upper.size();++i)path.lineTo(upper[i]);
        for(auto i=lower.crbegin();i!=lower.crend();++i)path.lineTo(*i);
        path.closeSubpath();painter.fillPath(path,fill);
      }
      upper.clear();lower.clear();
    };
    for(size_t col=0;col<std::min(top->timesMs.size(),base->timesMs.size());++col){
      const auto t=top->timesMs[col],b=base->timesMs[col];
      if(!std::isfinite(t)||!std::isfinite(b)||b<=t){flush();continue;}
      const auto a=pixelAt(int(col),t),z=pixelAt(int(col),b);
      if(!horizonPointIsFinite(a)||!horizonPointIsFinite(z)){flush();continue;}
      upper.append(a);lower.append(z);
      QRectF candidate(a.x()-w/2,(a.y()+z.y()-h)/2,w,h);
      if(upper.size()>1&&std::abs(z.y()-a.y())>=h+6&&clip.contains(candidate)&&
         (bestLabel.isEmpty()||std::abs(candidate.center().x()-clip.center().x())<std::abs(bestLabel.center().x()-clip.center().x())))bestLabel=candidate;
    }
    flush();
    if(!bestLabel.isEmpty())labels.append({bestLabel,band.name,color});
  }
}
inline void paintSectionHorizonBandLabels(QPainter& painter,const QVector<HorizonBandLabel>& candidates,QVector<QRectF>& occupied){
  auto paper=PaleoTheme::tokens(PaleoTheme::Theme::Light).surface;paper.setAlphaF(.85f);
  for(const auto& label:candidates){
    if(std::any_of(occupied.begin(),occupied.end(),[&](const auto& r){return r.intersects(label.rect);}))continue;
    occupied.append(label.rect);painter.fillRect(label.rect,paper);painter.setPen(label.color);
    painter.drawText(label.rect,Qt::AlignCenter,label.text);
  }
}
}
