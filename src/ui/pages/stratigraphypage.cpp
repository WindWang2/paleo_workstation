// 层：视图
#include "stratigraphypage.h"
#include "../../workflow/stratigraphyworkflow.h"
#include "../paleotheme.h"
#include "../correlationpanel.h"
#include "../correlation/horizonmarkers.h"
#include "../correlation/curvebrowser.h"
#include <QHBoxLayout>
#include <QVBoxLayout>
#include <QListWidget>
#include <QTableWidget>
#include <QHeaderView>
#include <QPushButton>
#include <QLabel>
#include <QInputDialog>
#include <QSignalBlocker>
#include <QSplitter>
#include <QShowEvent>
#include <algorithm>
StratigraphyPage::StratigraphyPage(StratigraphyWorkflow *workflow,QWidget *parent)
 :QWidget(parent),m_workflow(workflow) {
 setObjectName(QStringLiteral("stratigraphyPage"));
 auto *layout=new QVBoxLayout(this);const auto &t=PaleoTheme::tokens();
 layout->setContentsMargins(t.spacingSm,t.spacingSm,t.spacingSm,t.spacingSm);layout->setSpacing(t.spacingSm);
 auto *actions=new QHBoxLayout;
 auto *options=new QPushButton(tr("项目约定…"),this);actions->addWidget(options);
 connect(options,&QPushButton::clicked,this,&StratigraphyPage::showConventions);
 auto *resources=new QPushButton(tr("选择输入资料…"),this);actions->addWidget(resources);
 connect(resources,&QPushButton::clicked,this,&StratigraphyPage::chooseResource);
 auto *horizons=new QPushButton(tr("计算区域界面"),this);
 auto *divide=new QPushButton(tr("按统一模板划分"),this);
 auto *save=new QPushButton(tr("保存分层版本"),this);
 auto *undo=new QPushButton(tr("撤销调整"),this);actions->addWidget(undo);
 auto *apply=new QPushButton(tr("应用分层到工程"),this);actions->addWidget(apply);
 connect(apply,&QPushButton::clicked,this,[this]{if(auto *well=m_wells->currentItem())m_workflow->activateClassification(well->data(Qt::UserRole).toLongLong());});
 connect(undo,&QPushButton::clicked,workflow,&StratigraphyWorkflow::undo);
 for(auto *button:{horizons,divide,save})actions->addWidget(button);
 const QList<QPushButton *> controls{options,resources,horizons,divide,save,undo,apply};
 const auto updateControls=[workflow,controls] {
  for(auto *button:controls){button->setEnabled(workflow->canEdit());button->setToolTip(workflow->canEdit()?QString():tr("请打开可写工程，并等待当前任务完成"));}
 };
 connect(workflow,&StratigraphyWorkflow::changed,this,updateControls);
 connect(workflow,&StratigraphyWorkflow::busyChanged,this,[updateControls](bool){updateControls();});updateControls();
 actions->addStretch();layout->addLayout(actions);
 m_status=new QLabel(tr("请先打开工程并导入井资料。选择井后计算界面，再按统一模板划分三级／四级旋回；双击深度可调整界面。"),this);
 m_status->setObjectName(QStringLiteral("stratigraphyStatus"));
 m_status->setWordWrap(true);m_status->setTextFormat(Qt::PlainText);
 m_status->setTextInteractionFlags(Qt::TextSelectableByMouse);layout->addWidget(m_status);
 auto *body=new QHBoxLayout;m_wells=new QListWidget(this);
 m_wells->setObjectName(QStringLiteral("stratigraphyWells"));
 m_wells->setAccessibleName(tr("参与对比的井"));body->addWidget(m_wells,1);
 m_results=new QTableWidget(0,6,this);m_results->setAccessibleName(tr("地层划分结果"));
 m_results->setHorizontalHeaderLabels({tr("井"),tr("界面／旋回"),tr("级别"),tr("顶深 MD（m）"),tr("底深 MD（m）"),tr("状态")});
 m_results->horizontalHeader()->setSectionResizeMode(QHeaderView::ResizeToContents);
 m_results->horizontalHeader()->setStretchLastSection(true);
 m_results->setEditTriggers(QAbstractItemView::NoEditTriggers);
 auto *splitter=new QSplitter(Qt::Vertical,this);
 m_section=new WellCorrelationPanel(nullptr,splitter);splitter->addWidget(m_section);splitter->addWidget(m_results);
 m_section->curveBrowser()->hide();
 body->addWidget(splitter,4);layout->addLayout(body,1);
 connect(m_section->markers(),&HorizonMarkerSet::markerDepthChanged,this,[this](const QString &marker,const QString &well,float depth){
  if(m_refreshing)return;
  for(const auto &row:m_workflow->rows())if(row.wellId==well.toLongLong()&&row.name==marker&&!row.id.isEmpty()) {
   m_markerMove=true;m_workflow->moveBoundary(row.wellId,row.id,depth);m_markerMove=false;
   for(const auto &updated:m_workflow->rows())if(updated.wellId==row.wellId&&updated.id==row.id&&float(updated.top)!=depth) {
    m_refreshing=true;m_section->markers()->setWellDepth(marker,well,float(updated.top));m_refreshing=false;
   }
   break;
  }
 });
 const auto calculate=[this](bool subdivide) {
  QList<qlonglong> ids;for(int i=0;i<m_wells->count();++i)if(m_wells->item(i)->checkState()==Qt::Checked)ids.append(m_wells->item(i)->data(Qt::UserRole).toLongLong());
  m_workflow->calculate(ids,subdivide);
 };
 connect(horizons,&QPushButton::clicked,this,[calculate]{calculate(false);});
 connect(divide,&QPushButton::clicked,this,[calculate]{calculate(true);});
 connect(save,&QPushButton::clicked,workflow,&StratigraphyWorkflow::save);
 connect(workflow,&StratigraphyWorkflow::changed,this,&StratigraphyPage::refresh);
 connect(workflow,&StratigraphyWorkflow::statusChanged,m_status,&QLabel::setText);
 connect(workflow,&StratigraphyWorkflow::busyChanged,this,[this](bool busy){m_wells->setEnabled(!busy);});
 connect(m_results,&QTableWidget::cellDoubleClicked,this,[this](int row,int column){
  const auto rows=m_workflow->rows();if(row>=rows.size()||column!=3||rows[row].id.isEmpty())return;
  bool accepted=false;const auto &value=rows[row];
  const double md=QInputDialog::getDouble(this,tr("调整共享界面"),tr("测深 MD（m）"),value.top,0,100000,3,&accepted);
  if(accepted)m_workflow->moveBoundary(value.wellId,value.id,md);
 });refresh();
}
void StratigraphyPage::refresh() {
 m_refreshing=true;
 const auto wells=m_workflow->wells();
 if(m_wells->count()!=wells.size() || (!wells.isEmpty() && m_wells->item(0)->data(Qt::UserRole).toLongLong()!=wells.first().id)) {
  m_wells->clear();for(const auto &well:wells){auto *item=new QListWidgetItem(well.name,m_wells);item->setData(Qt::UserRole,well.id);item->setCheckState(Qt::Checked);}
 }
 const auto rows=m_workflow->rows();m_results->setRowCount(rows.size());
 for(int i=0;i<rows.size();++i){const auto &r=rows[i];const QStringList cells={r.well,r.name,r.level,QString::number(r.top,'f',3),QString::number(r.bottom,'f',3),r.status};for(int c=0;c<cells.size();++c)m_results->setItem(i,c,new QTableWidgetItem(cells[c]));}
 if(m_markerMove){m_refreshing=false;return;}
 QList<QPair<QString,QString>> columns;
 const auto tracks=m_workflow->tracks();
 for(const auto &track:tracks){const auto id=QString::number(track.wellId);if(std::none_of(columns.begin(),columns.end(),[&](const auto &w){return w.first==id;}))columns.append({id,track.name});}
 m_section->beginBatch();m_section->setWells(columns);m_section->clearWellCurves();
 m_section->markers()->setManifestHorizons({});
 for(const auto &track:tracks)m_section->addWellTrack(QString::number(track.wellId),track.mnemonic,{},track.depths,track.values);
 for(const auto &row:rows)if(!row.id.isEmpty()) {m_section->markers()->addMarker(row.name);m_section->markers()->setWellDepth(row.name,QString::number(row.wellId),float(row.top));}
 m_section->endBatch();m_refreshing=false;
}
void StratigraphyPage::showEvent(QShowEvent *event) {
 QWidget::showEvent(event);m_workflow->refreshWells();
}
