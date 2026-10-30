// 层：视图
#include "stratigraphypage.h"
#include "../../workflow/stratigraphyworkflow.h"
#include "../wellcomposite/multiwellview.h"
#include "../paleotheme.h"
#include <paleo_data/interpretation.h>
#include <QComboBox>
#include <QDoubleSpinBox>
#include <QLabel>
#include <QLineEdit>
#include <QJsonDocument>
#include <QTimer>
#include <algorithm>

void StratigraphyPage::connectPreview(WellComposite::WellCompositePanel *panel, qlonglong well) {
 using WellComposite::WellCompositeCanvas;
 connect(panel->canvas(),&WellCompositeCanvas::depthClicked,this,[this,well](double md){
  clearSelection();m_selectedWell=well;m_depth->setValue(md);m_name->clear();
  m_source->setText(tr("已拾取 MD %1 m；填写名称后新增内部界线。区域段须有明确的顶底界面。").arg(md,0,'f',3));
 });
 const auto select=[this,well](const QString& label){selectObject(well,m_markerIds.value(QString::number(well)+QLatin1Char('/')+label));};
 connect(panel->canvas(),&WellCompositeCanvas::markerSelected,this,select);
 connect(panel->canvas(),&WellCompositeCanvas::markerContextMenuRequested,this,[select](const QString& label,double){select(label);});
 connect(panel,&WellComposite::WellCompositePanel::boundaryDepthEdited,this,[this,well](const QString& label,double md){
  if(m_refreshing)return;
  const auto id=m_markerIds.value(QString::number(well)+QLatin1Char('/')+label);if(id.isEmpty())return;
  m_workflow->moveBoundary(well,id,md);
  refreshPreview(); // 拒绝越界等非法编辑时也立即回显工作流中的合法位置。
 });
}

void StratigraphyPage::refreshPreview() {
 using namespace WellComposite;
 const bool replaced=m_reloadPreview;const auto previousRange=m_section->displayDepthRange();
 m_section->setLinkScroll(false);
 if(replaced) {
  m_reloadPreview=false;m_section->clearPanels();int slot=0;
  for(const auto& well:m_workflow->document().wells) {
   ComprehensiveWellData data;QString error;
   if(!m_workflow->previewWellData(well.wellId,&data,&error)){m_status->setText(tr("井道预览装载失败：%1").arg(error));continue;}
   auto* panel=new WellCompositePanel(m_section);panel->setProperty("wellId",qlonglong(well.wellId));
   panel->setObjectName(QStringLiteral("stratigraphyWell_%1").arg(well.wellId));
   panel->loadWellData(data);panel->setMinimumWidth(qRound(panel->canvas()->totalTracksWidth())+PaleoTheme::tokens().spacingSm);
   connectPreview(panel,well.wellId);m_section->setPanel(slot++,panel);
  }
 }
 m_markerIds.clear();const auto rows=m_workflow->rows();const auto tracks=m_workflow->tracks();
 for(int i=0;i<m_section->slotCount();++i)if(auto* panel=m_section->panelAt(i)) {
  const auto well=panel->property("wellId").toLongLong();QVector<DepthTools::MarkerLine> markers;QHash<QString,int> names;
  for(const auto& row:rows)if(row.wellId==well&&!row.cycle)++names[row.name];
  for(const auto& row:rows)if(row.wellId==well&&!row.cycle) {
   const QString label=(row.role=="T70"||row.role=="T72"||row.role=="T73"||row.role=="T80")?row.role:(names[row.name]>1?row.name+QStringLiteral(" [%1]").arg(row.id):row.name);
   markers.append({row.top,label});m_markerIds[QString::number(well)+QLatin1Char('/')+label]=row.id;
  }
  panel->canvas()->setMarkerLines(markers);panel->setBoundaryAnnotationMode(true,m_workflow->canEdit());
  // 只更新已有曲线的显示数据，人工改线时保留预览的道顺序、宽度和显隐状态。
  for(const auto& base:panel->canvas()->tracks())if(auto curve=std::dynamic_pointer_cast<CurveTrack>(base)) {
   auto values=curve->curves();
   for(auto& value:values)for(const auto& track:tracks)if(track.wellId==well&&
       (value.name==track.mnemonic||value.name+" *"==track.mnemonic||track.mnemonic+" *"==value.name)) {
    value.name=track.mnemonic;value.depths=track.depths;value.values=track.values;break;
   }
   curve->setCurves(values);
  }
 }
 m_section->setDatumMarker(m_flatten->currentData().toString());
 m_section->setLinkScroll(true);
 if(replaced) {
  QTimer::singleShot(0,this,[this]{
   const auto view=QJsonDocument::fromJson(QByteArray::fromStdString(m_workflow->document().viewStateJson)).object();
   if(view["top"].isDouble()&&view["bottom"].toDouble()>view["top"].toDouble())m_section->setDisplayDepthRange(view["top"].toDouble(),view["bottom"].toDouble());else fitFormation();
  });
 }else m_section->setDisplayDepthRange(previousRange.first,previousRange.second);
 const auto range=m_section->displayDepthRange();m_rangeTop->setValue(range.first);m_rangeBottom->setValue(range.second);
}
