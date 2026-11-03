// 层：视图
#include "seismicsectioncanvas.h"
#include "seismicsectiondockwidget.h"
#include "sectionhorizoncontrols.h"
#include "sectionhorizonrender.h"
#include <QHBoxLayout>
#include <QTimer>

namespace seismic {
void SeismicSectionCanvas::setHorizonOverlays(const SectionHorizons& horizons){
  m_horizons=horizons;update();emit horizonOverlaysChanged();
}
void SeismicSectionCanvas::setHorizonOverlaysVisible(bool visible){m_horizonsVisible=visible;update();}
void SeismicSectionCanvas::paintHorizonOverlays(QPainter& painter,const QRect& viewport){
  if(!m_horizonsVisible||!hasData()||m_orientation!=SectionOrientation::Vertical)return;
  if(m_majorHorizonsOnly){
    auto major=m_horizons;
    std::erase_if(major.curves,[](const auto &c){return c.internalEdge!=0;});
    std::erase_if(major.bands,[](const auto &b){return b.subdivision!=0;});
    paintSectionHorizons(painter,viewport,major,[this](int col,double t){return QPointF(traceToPixelX(col),timeToPixelY(t));});return;
  }
  paintSectionHorizons(painter,viewport,m_horizons,[this](int column,double time){return QPointF(traceToPixelX(column),timeToPixelY(time));});
}
void SeismicSectionDockWidget::showMajorHorizonConnections(){
  m_majorHorizonConnectionsNext=true;
  m_canvas->setMajorHorizonsOnly(true);m_canvas->setHorizonOverlaysVisible(true);
  if(auto *button=findChild<QToolButton *>("sectionHorizonToggle"))button->setChecked(true);
}
void SeismicSectionDockWidget::setupHorizonOverlayControl(QWidget *toolbar){
  auto *button=new SectionHorizonButton(toolbar);toolbar->layout()->addWidget(button);
  connect(button,&QToolButton::toggled,this,[this](bool on){if(m_canvas)m_canvas->setHorizonOverlaysVisible(on);});
  // setupUi constructs the canvas after this toolbar. Defer only connection setup.
  QTimer::singleShot(0,this,[this,button]{
    if(!m_canvas)return;
    connect(m_canvas,&SeismicSectionCanvas::horizonOverlaysChanged,button,[this,button]{
      button->setOverlayStatus(m_canvas->horizonOverlays());button->setEnabled(m_canvas->orientation()==SectionOrientation::Vertical);
    });
    button->setOverlayStatus(m_canvas->horizonOverlays());
  });
}
}
