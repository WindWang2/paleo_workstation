// 层：视图
#include "wellsectionpanel.h"
#include "wellsectionscene.h"
#include "ui/seismicsection/sectionhorizoncontrols.h"
#include "ui/seismicsection/sectionhorizonrender.h"
#include <QHBoxLayout>
#include <QGraphicsScene>
#include <cmath>

void WellSectionPanel::setupHorizonOverlayControl(QWidget *toolbar){
  auto *button=new seismic::SectionHorizonButton(toolbar);toolbar->layout()->addWidget(button);
  connect(button,&QToolButton::toggled,this,&WellSectionPanel::setSeismicHorizonsEnabled);
}
void WellSectionPanel::setSeismicHorizonsEnabled(bool on){
  m_st.seismicHorizonsOn=on;
  if(auto *button=findChild<QToolButton *>("sectionHorizonToggle"))button->setChecked(on);
  m_scene->update();
}
int WellSectionPanel::linkCount()const{
  int count=0;
  for(int i=0;i+1<m_st.wells.size();++i)for(const auto &link:m_st.linksForGap(i))
    if(wellsection::linkConnected(m_linkOverrides,m_st.wells[i].id,m_st.wells[i+1].id,link.name))++count;
  return count;
}
void WellSectionPanel::updateHorizonOverlayStatus(){
  seismic::SectionHorizons merged;size_t count=0;
  for(const auto& gap:m_st.strip.gaps){
    count+=gap.horizons.curves.size();
    merged.warnings.append(gap.horizons.warnings);
  }
  merged.curves.resize(count);
  merged.warnings.removeDuplicates();
  if(auto *button=findChild<QToolButton *>("sectionHorizonToggle"))static_cast<seismic::SectionHorizonButton *>(button)->setOverlayStatus(merged);
}
namespace wellsectionui {
std::optional<QPainterPath> GapItem::seismicLinkPath(const QString &name)const{
  if(m_index>=m_st->strip.gaps.size())return {};
  const auto &gap=m_st->strip.gaps[m_index];
  for(const auto &curve:gap.horizons.curves){
    if(curve.internalEdge||curve.name!=name)continue;
    QPainterPath path;bool connected=false;
    const auto &a=m_st->wells[m_index],&b=m_st->wells[m_index+1];
    for(size_t col=0;col<curve.timesMs.size();++col){
      double f=curve.timesMs.size()>1?double(col)/(curve.timesMs.size()-1):0.;
      if(gap.columnDistancesM.size()==curve.timesMs.size()&&gap.columnDistancesM.back()>gap.columnDistancesM.front())
        f=(gap.columnDistancesM[col]-gap.columnDistancesM.front())/(gap.columnDistancesM.back()-gap.columnDistancesM.front());
      const double depth=wellsection::gapDisplayDepthAtTwt(a,m_st->offsets.value(m_index),b,m_st->offsets.value(m_index+1),
        f,curve.timesMs[col],m_st->window.top,m_st->window.base,m_st->domain);
      if(!std::isfinite(depth)){connected=false;continue;}
      const QPointF point(m_st->columnRight(m_index)+f*m_st->gapWidth(m_index),m_st->yForDisplay(depth));
      if(connected)path.lineTo(point);else path.moveTo(point);connected=true;
    }
    return path;
  }
  return {};
}
void GapItem::paintSeismicHorizons(QPainter *p,const QRectF& exposed,const wellsection::SeismicGap& gap){
  if(!m_st->seismicHorizonsOn||gap.horizons.curves.empty())return;
  const auto& a=m_st->wells[m_index];const auto& b=m_st->wells[m_index+1];
  const double x0=m_st->columnRight(m_index),width=m_st->gapWidth(m_index);
  seismic::paintSectionHorizons(*p,exposed,gap.horizons,[&](int col,double time){
    double f=gap.columns>1?double(col)/(gap.columns-1):0.;
    if(gap.columnDistancesM.size()==size_t(gap.columns)&&gap.columnDistancesM.back()>gap.columnDistancesM.front())
      f=(gap.columnDistancesM[col]-gap.columnDistancesM.front())/(gap.columnDistancesM.back()-gap.columnDistancesM.front());
    const double depth=wellsection::gapDisplayDepthAtTwt(a,m_st->offsets.value(m_index),b,m_st->offsets.value(m_index+1),
      f,time,m_st->window.top,m_st->window.base,m_st->domain);
    return QPointF(x0+f*width,std::isfinite(depth)?m_st->yForDisplay(depth):qQNaN());
  });
}
}
