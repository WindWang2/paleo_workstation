// 层：视图
#include "stratigraphyresultplot.h"
#include "../paleotheme.h"
#include <QPainter>
#include <QPainterPath>
#include <algorithm>
#include <cmath>
void StratigraphyResultPlot::paintEvent(QPaintEvent*) {
 QPainter p(this);const auto& t=PaleoTheme::tokens();p.fillRect(rect(),t.surface);p.setRenderHint(QPainter::Antialiasing);
 if(!series)return;const auto& s=*series;const size_t n=std::min(s.axis.size(),s.values.size());
 double xmin=INFINITY,xmax=-INFINITY,ymin=INFINITY,ymax=-INFINITY;
 for(size_t i=0;i<n;++i)if(std::isfinite(s.axis[i])&&std::isfinite(s.values[i])){xmin=std::min(xmin,s.axis[i]);xmax=std::max(xmax,s.axis[i]);ymin=std::min(ymin,s.values[i]);ymax=std::max(ymax,s.values[i]);}
 if(!std::isfinite(xmin))return;if(xmax<=xmin)xmax=xmin+1;if(ymax<=ymin)ymax=ymin+1;
 const QRectF plot(75,30,std::max(1,width()-100),std::max(1,height()-100));p.setPen(t.border);p.drawRect(plot);p.setPen(t.textMuted);
 p.drawText(QRectF(5,2,width()-10,22),Qt::AlignCenter,QString::fromStdString(s.name+" ["+s.status+"]"));
 p.drawText(QRectF(75,height()-30,width()-100,25),Qt::AlignCenter,QString::fromStdString(s.axisKind+" / "+s.axisUnit));
 for(int i=0;i<=4;++i){const double y=plot.bottom()-plot.height()*i/4;p.setPen(t.border);p.drawLine(QPointF(plot.left(),y),QPointF(plot.right(),y));p.setPen(t.textMuted);
  p.drawText(QRectF(0,y-10,68,20),Qt::AlignRight,QString::number(ymin+(ymax-ymin)*i/4,'g',5));const double x=plot.left()+plot.width()*i/4;
  p.drawText(QRectF(x-35,plot.bottom()+6,70,20),Qt::AlignCenter,QString::number(xmin+(xmax-xmin)*i/4,'g',6));
 }
 p.save();p.setClipRect(plot);p.setPen(QPen(s.status=="stale"?t.warning:t.text,1.6));QPainterPath path;bool start=true;
 for(size_t i=0;i<n;++i){if(!std::isfinite(s.axis[i])||!std::isfinite(s.values[i])||(i<s.quality.size()&&(s.quality[i]&1))){start=true;continue;}
  const QPointF point(plot.left()+(s.axis[i]-xmin)/(xmax-xmin)*plot.width(),plot.bottom()-(s.values[i]-ymin)/(ymax-ymin)*plot.height());
  if(start){path.moveTo(point);start=false;}else path.lineTo(point);
 }p.drawPath(path);p.restore();
}
