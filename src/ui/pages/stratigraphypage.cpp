// 层：视图
#include "stratigraphypage.h"
#include "../../workflow/stratigraphyworkflow.h"
#include "../paleotheme.h"
#include "../correlationpanel.h"
#include "../correlation/horizonmarkers.h"
#include "../correlation/curvebrowser.h"
#include <QBoxLayout>
#include <QListWidget>
#include <QTableWidget>
#include <QHeaderView>
#include <QLabel>
#include <QSplitter>
#include <QShowEvent>
#include <QSignalBlocker>
#include <QComboBox>
#include <QLineEdit>
#include <QDoubleSpinBox>
#include <QCheckBox>
#include <QMessageBox>
#include <QGraphicsView>
#include <QPushButton>
#include <QTimer>
#include <QJsonDocument>
#include <paleo_data/interpretation.h>
#include <algorithm>
StratigraphyPage::StratigraphyPage(StratigraphyWorkflow* workflow,QWidget* parent)
 :QWidget(parent),m_workflow(workflow) {
 setObjectName(QStringLiteral("stratigraphyPage"));
 auto* layout=new QVBoxLayout(this);const auto& t=PaleoTheme::tokens();
 layout->setContentsMargins(t.spacingSm,t.spacingSm,t.spacingSm,t.spacingSm);layout->setSpacing(t.spacingSm);
 buildActions(layout);buildAdvanced(layout);
 m_status=new QLabel(tr("加载井道图，计算区域界面并划分旋回；双击井道拾取深度，选中界线可拖动或编辑。"),this);
 m_status->setObjectName(QStringLiteral("stratigraphyStatus"));m_status->setWordWrap(true);
 m_status->setTextFormat(Qt::PlainText);m_status->setTextInteractionFlags(Qt::TextSelectableByMouse);layout->addWidget(m_status);
 buildRangeControls(layout);
 auto* body=new QHBoxLayout;m_wells=new QListWidget(this);
 m_wells->setObjectName(QStringLiteral("stratigraphyWells"));m_wells->setAccessibleName(tr("参与对比的井"));body->addWidget(m_wells,1);
 auto* splitter=new QSplitter(Qt::Vertical,this);
 m_section=new WellCorrelationPanel(nullptr,splitter);m_section->setObjectName(QStringLiteral("stratigraphySection"));
 m_section->setToolTip(tr("双击井道拾取深度，拖动界线修改 MD。带 * 的派生曲线为历史结果，需重算相应段；原始 GR 保持不变。"));
 m_section->curveBrowser()->hide();m_section->markers()->setCommitOnRelease(true);splitter->addWidget(m_section);
 m_results=new QTableWidget(0,7,splitter);m_results->setObjectName(QStringLiteral("stratigraphyResults"));
 m_results->setAccessibleName(tr("地层划分结果"));m_results->setSelectionBehavior(QAbstractItemView::SelectRows);
 m_results->setHorizontalHeaderLabels({tr("井"),tr("界面／旋回"),tr("级别"),tr("顶深 MD（m）"),tr("底深 MD（m）"),tr("来源／方向"),tr("状态")});
 m_results->horizontalHeader()->setSectionResizeMode(QHeaderView::ResizeToContents);m_results->horizontalHeader()->setStretchLastSection(true);
 m_results->setEditTriggers(QAbstractItemView::NoEditTriggers);splitter->addWidget(m_results);splitter->setStretchFactor(0,3);splitter->setStretchFactor(1,1);
 body->addWidget(splitter,5);buildEditor(body);layout->addLayout(body,1);
 connect(workflow,&StratigraphyWorkflow::documentReplaced,this,[this]{clearSelection();const auto view=QJsonDocument::fromJson(QByteArray::fromStdString(m_workflow->document().viewStateJson)).object();m_flatten->setCurrentIndex(std::max(0,m_flatten->findData(view["flatten"].toString())));m_scope->setCurrentIndex(std::max(0,m_scope->findData(view["segmentScope"].toString())));});
 connect(workflow,&StratigraphyWorkflow::changed,this,&StratigraphyPage::refresh);
 connect(workflow,&StratigraphyWorkflow::statusChanged,m_status,&QLabel::setText);
 connect(workflow,&StratigraphyWorkflow::busyChanged,this,[this](bool busy){m_wells->setEnabled(!busy);m_section->markers()->setEditable(m_workflow->canEdit());});
 connect(workflow,&StratigraphyWorkflow::boundarySelected,this,[this](qlonglong well,const QString& id){selectObject(well,id);});
 connect(m_wells,&QListWidget::currentItemChanged,this,[this](QListWidgetItem* item){if(item){m_selectedWell=item->data(Qt::UserRole).toLongLong();m_selectedObject.clear();}});
 connect(m_wells,&QListWidget::itemChanged,this,[this]{updateAvailability();});
 connect(m_scope,&QComboBox::currentIndexChanged,this,[this]{updateAvailability();});
 connect(m_section->markers(),&HorizonMarkerSet::markerSelected,this,[this](const QString& key,const QString& well){
  selectObject(well.toLongLong(),m_markerIds.value(well+QLatin1Char('/')+key));
 });
 connect(m_section->markers(),&HorizonMarkerSet::markerDepthChanged,this,[this](const QString& key,const QString& well,float depth){
  if(m_refreshing)return;const auto id=m_markerIds.value(well+QLatin1Char('/')+key);if(id.isEmpty())return;
  m_markerMove=true;m_workflow->moveBoundary(well.toLongLong(),id,depth);m_markerMove=false;
  for(const auto& row:m_workflow->rows())if(!row.cycle&&row.wellId==well.toLongLong()&&row.id==id&&float(row.top)!=depth){
   m_refreshing=true;m_section->markers()->setWellDepth(key,well,float(row.top));m_refreshing=false;
  }
 });
 connect(m_section,&WellCorrelationPanel::wellDepthPicked,this,[this](const QString& well,double md){
  clearSelection();m_selectedWell=well.toLongLong();m_depth->setValue(md);m_name->clear();
  m_source->setText(tr("已拾取 MD %1 m；填写名称后新增内部界线。区域段须有明确的顶底界面。").arg(md,0,'f',3));
 });
 connect(m_results,&QTableWidget::cellClicked,this,[this](int row,int){const auto rows=m_workflow->rows();if(row<rows.size())selectObject(rows[row].wellId,rows[row].id,rows[row].cycle);});
 refresh();
}
QList<qlonglong> StratigraphyPage::selectedWells() const {
 if(!m_wells)return {};
 QList<qlonglong> ids;for(int i=0;i<m_wells->count();++i)if(m_wells->item(i)->checkState()==Qt::Checked)ids.append(m_wells->item(i)->data(Qt::UserRole).toLongLong());return ids;
}
bool StratigraphyPage::confirmReplace(){return !m_workflow->hasUnsavedChanges()||QMessageBox::question(this,tr("未保存的解释"),tr("当前方案有未保存修改，是否放弃并加载所选井或版本？"),QMessageBox::Discard|QMessageBox::Cancel,QMessageBox::Cancel)==QMessageBox::Discard;}
void StratigraphyPage::clearSelection(){m_selectedWell=0;m_selectedObject.clear();m_selectedCycle=false;if(m_name)m_name->clear();if(m_depth)m_depth->setValue(0);if(m_lock)m_lock->setChecked(false);if(m_source){m_source->clear();m_source->setToolTip({});}highlightSelection();}
void StratigraphyPage::selectObject(qlonglong well,const QString& id,bool cycle) {
 clearSelection();m_selectedWell=well;m_selectedObject=id;m_selectedCycle=cycle;
 for(const auto& row:m_workflow->rows())if(row.wellId==well&&row.id==id&&row.cycle==cycle) {
  m_name->setText(row.name);m_depth->setValue(row.top);m_lock->setChecked(row.locked);
  if(cycle){m_level->setCurrentIndex(m_level->findData(row.level.toInt()));m_direction->setCurrentIndex(m_direction->findData(row.direction));}
  m_source->setText(tr("%1 / %2\n来源：%3 · %4\n初始深度：%5 m").arg(row.well,row.name,row.source,row.status).arg(row.original,0,'f',3));m_source->setToolTip(row.evidence);break;
 }
 highlightSelection();
}
void StratigraphyPage::refresh() {
 m_refreshing=true;const auto wells=m_workflow->wells();
 QList<qlonglong> previous;for(int i=0;i<m_wells->count();++i)previous.append(m_wells->item(i)->data(Qt::UserRole).toLongLong());
 QList<qlonglong> current;for(const auto& w:wells)current.append(w.id);
 if(current!=previous){m_wells->clear();for(const auto& well:wells){auto* item=new QListWidgetItem(well.name,m_wells);item->setData(Qt::UserRole,well.id);item->setCheckState(Qt::Checked);}}
 const auto rows=m_workflow->rows();QSignalBlocker tableBlock(m_results);m_results->setRowCount(rows.size());
 for(int i=0;i<rows.size();++i){const auto& r=rows[i];const QString direction=r.direction==1?tr("正旋回"):r.direction==-1?tr("反旋回"):tr("方向待定");
  const QStringList cells={r.well,r.name,r.level,QString::number(r.top,'f',3),QString::number(r.bottom,'f',3),r.cycle?direction:r.source,r.status+(r.locked?tr(" · 保护"):QString())};
  for(int c=0;c<cells.size();++c)m_results->setItem(i,c,new QTableWidgetItem(cells[c]));
 }
 refreshVersions();updateAvailability();m_section->markers()->setEditable(m_workflow->canEdit());
 if(m_markerMove){m_refreshing=false;return;}
 QList<QPair<QString,QString>> columns;const auto tracks=m_workflow->tracks();
 for(const auto& track:tracks){const auto id=QString::number(track.wellId);if(std::none_of(columns.begin(),columns.end(),[&](const auto& w){return w.first==id;}))columns.append({id,track.name});}
 const QString flatten=m_flatten->currentData().toString();m_markerIds.clear();
 const auto previousRange=m_section->depthRange();const QString identity=QString::fromStdString(m_workflow->document().id);const bool replaced=identity!=m_loadedIdentity;
 m_section->beginBatch();m_section->setWells(columns);m_section->clearWellCurves();m_section->markers()->setManifestHorizons({});
 for(const auto& track:tracks)m_section->addWellTrack(QString::number(track.wellId),track.mnemonic,{},track.depths,track.values);
 for(const auto& row:rows)if(!row.cycle){const auto well=QString::number(row.wellId);
  const QString key=(row.role=="T70"||row.role=="T72"||row.role=="T73"||row.role=="T80")?row.role:row.id;
  m_section->markers()->addMarker(key);m_section->markers()->setMarkerLabel(key,row.name);m_section->markers()->setWellDepth(key,well,float(row.top));m_markerIds[well+QLatin1Char('/')+key]=row.id;
 }
 m_section->setFlattenMarker(flatten);m_section->endBatch();m_refreshing=false;
 if(replaced){m_loadedIdentity=identity;const auto view=QJsonDocument::fromJson(QByteArray::fromStdString(m_workflow->document().viewStateJson)).object();
  if(view["top"].isDouble()&&view["bottom"].toDouble()>view["top"].toDouble())m_section->setDepthRange(float(view["top"].toDouble()),float(view["bottom"].toDouble()));else fitFormation();
 }else if(!columns.isEmpty())m_section->setDepthRange(previousRange.first,previousRange.second);
 const auto range=m_section->depthRange();m_rangeTop->setValue(range.first);m_rangeBottom->setValue(range.second);
 if(!m_selectedObject.isEmpty()){const auto id=m_selectedObject;const auto well=m_selectedWell;const bool cycle=m_selectedCycle;selectObject(well,id,cycle);}
}
void StratigraphyPage::showEvent(QShowEvent* event){QWidget::showEvent(event);m_workflow->refreshWells();}
void StratigraphyPage::updateAvailability(){if(auto* b=findChild<QPushButton*>(QStringLiteral("autoDivideCycles")))b->setEnabled(m_workflow->canSubdivide(selectedWells(),m_scope->currentData().toString()));}
void StratigraphyPage::highlightSelection(){
 // Deferred past the graphics item's event handler so selection cannot reap
 // a line while its mousePressEvent is still on the stack.
 QTimer::singleShot(0,this,[this]{const auto well=QString::number(m_selectedWell);QString key;
  if(!m_selectedCycle)for(auto it=m_markerIds.begin();it!=m_markerIds.end();++it)if(it.key().startsWith(well+QLatin1Char('/'))&&it.value()==m_selectedObject){key=it.key().mid(well.size()+1);break;}
  m_section->highlightMarker(key,well);
 });
}
