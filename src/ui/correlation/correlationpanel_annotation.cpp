// 层：视图
#include "../correlationpanel.h"
#include <QGraphicsView>
#include "correlationpanel_scene_internal.h"
#include <QImage>
#include <cmath>
QPoint WellCorrelationPanel::positionForDepth(const QString& id,double md) const {
 for(const auto& g:m_lastGeoms)if(g.wellId==id)
  return m_view->mapFromScene(QPointF(g.rect.center().x(),yForDepth(m_markers->displayDepth(id,float(md)))));
 return QPoint(-1,-1);
}
void HorizonMarkerSet::setMarkerLabel(const QString& name,const QString& label) {
 for(auto& marker:m_markers)if(marker.name==name){marker.label=label;return;}
}
void WellCorrelationPanel::setDepthRange(float top,float bottom) {
 if(!std::isfinite(top)||!std::isfinite(bottom)||bottom<=top)return;
 m_fixedDepthRange=true;m_markers->setClipToColumns(true);m_axisMin=top;m_axisMax=bottom;rebuildScene();
}
bool WellCorrelationPanel::exportImage(const QString& path) const {
 if(!m_scene||m_wells.isEmpty())return false;
 CorrelationView copy(m_scene,nullptr);
 copy.ruler=m_ruler;copy.rulerEnabled=!m_wells.isEmpty();copy.rulerTop=depthBodyTop();
 copy.axisMin=m_axisMin;copy.axisMax=m_axisMax;copy.depthOfY=[this](qreal y){return depthAtY(y);};
 copy.setBackgroundBrush(m_view->backgroundBrush());copy.setFrameShape(QFrame::NoFrame);
 copy.setHorizontalScrollBarPolicy(Qt::ScrollBarAlwaysOff);copy.setVerticalScrollBarPolicy(Qt::ScrollBarAlwaysOff);
 copy.setAlignment(Qt::AlignLeft|Qt::AlignTop);copy.setSceneRect(m_scene->sceneRect());
 const QSize logical(int(std::ceil(m_scene->sceneRect().width()))+2,int(std::ceil(m_scene->sceneRect().height()))+2);
 const qreal dpr=devicePixelRatioF();const QSize pixels=logical*dpr;
 if(pixels.isEmpty()||qint64(pixels.width())*pixels.height()>64000000)return false;
 copy.resize(logical);QImage image(pixels,QImage::Format_ARGB32_Premultiplied);if(image.isNull())return false;
 image.setDevicePixelRatio(dpr);image.fill(PaleoTheme::tokens().surface);copy.QWidget::render(&image);return image.save(path);
}
void WellCorrelationPanel::highlightMarker(const QString& marker,const QString& well) {
 m_markers->setSelectedMarker(marker,well);relayoutMarkers();
}
