// 层：视图
#include "stratigraphypage.h"
#include "../../workflow/stratigraphyworkflow.h"
#include "../paleotheme.h"
#include "../wellcomposite/multiwellview.h"
#include <QScrollArea>
#include <QBoxLayout>
#include <QListWidget>
#include <QTableWidget>
#include <QHeaderView>
#include <QLabel>
#include <QSplitter>
#include <QShowEvent>
#include <QSignalBlocker>
#include <QSet>
#include <QComboBox>
#include <QLineEdit>
#include <QDoubleSpinBox>
#include <QCheckBox>
#include "../notifications/paleonotify.h"
#include <QGraphicsView>
#include <QPushButton>
#include <QTimer>
#include <QJsonDocument>
#include <paleo_data/interpretation.h>
#include <algorithm>
namespace {
// QSplitter 首次布局按「sizeHint×拉伸系数」分配初始高度。剖面滚动区的 sizeHint 仅为内容提示
// （~22px），3:1 拉伸会落成剖面 1/4；给它与结果表默认提示（192px）同量级的高度提示，
// 3:1 拉伸才落成剖面约 3/4（D9.1 禁程序化 setSizes，宽高只由拉伸系数与用户拖动决定）。
class SectionScrollArea final : public QScrollArea {
public:
 using QScrollArea::QScrollArea;
 QSize sizeHint() const override { const QSize base=QScrollArea::sizeHint(); return {base.width(),std::max(base.height(),192)}; }
};
}
StratigraphyPage::StratigraphyPage(StratigraphyWorkflow* workflow,QWidget* parent)
 :QWidget(parent),m_workflow(workflow) {
 setObjectName(QStringLiteral("stratigraphyPage"));
 auto* layout=new QVBoxLayout(this);const auto& t=PaleoTheme::tokens();
 layout->setContentsMargins(t.spacingSm,t.spacingSm,t.spacingSm,t.spacingSm);layout->setSpacing(t.spacingSm);
 buildActions(layout);buildAdvanced(layout);
 m_status=new QLabel(tr("加载井道图，计算区域界面并划分旋回；单击井道拾取深度，选中界线可拖动或编辑。"),this);
 m_status->setObjectName(QStringLiteral("stratigraphyStatus"));m_status->setWordWrap(true);
 m_status->setTextFormat(Qt::PlainText);m_status->setTextInteractionFlags(Qt::TextSelectableByMouse);layout->addWidget(m_status);
 buildRangeControls(layout);
 auto* body=new QHBoxLayout;m_wells=new QListWidget(this);
 m_wells->setObjectName(QStringLiteral("stratigraphyWells"));m_wells->setAccessibleName(tr("参与对比的井"));body->addWidget(m_wells,1);
 auto* splitter=new QSplitter(Qt::Vertical,this);
 splitter->setObjectName(QStringLiteral("stratigraphySplit"));
 auto* scroll=new SectionScrollArea(splitter);scroll->setWidgetResizable(true);
 m_section=new WellComposite::MultiWellView(scroll);m_section->setObjectName(QStringLiteral("stratigraphySection"));
 m_section->setLayoutMode(WellComposite::MultiWellView::LayoutMode::Row);
 m_section->setToolTip(tr("单击预览井道拾取 MD，拖动界线修改解释；带 * 的派生曲线需重算相应段。"));
 scroll->setWidget(m_section);splitter->addWidget(scroll);
 m_results=new QTableWidget(0,7,splitter);m_results->setObjectName(QStringLiteral("stratigraphyResults"));
 m_results->setAccessibleName(tr("地层划分结果"));m_results->setSelectionBehavior(QAbstractItemView::SelectRows);
 m_results->setHorizontalHeaderLabels({tr("井"),tr("界面／旋回"),tr("级别"),tr("顶深 MD（m）"),tr("底深 MD（m）"),tr("来源／方向"),tr("状态")});
 m_results->horizontalHeader()->setSectionResizeMode(QHeaderView::ResizeToContents);m_results->horizontalHeader()->setStretchLastSection(true);
 m_results->setEditTriggers(QAbstractItemView::NoEditTriggers);splitter->addWidget(m_results);splitter->setStretchFactor(0,3);splitter->setStretchFactor(1,1);
 body->addWidget(splitter,5);buildEditor(body);layout->addLayout(body,1);
 connect(workflow,&StratigraphyWorkflow::documentReplaced,this,[this]{m_reloadPreview=true;clearSelection();const auto view=QJsonDocument::fromJson(QByteArray::fromStdString(m_workflow->document().viewStateJson)).object();m_flatten->setCurrentIndex(std::max(0,m_flatten->findData(view["flatten"].toString())));m_scope->setCurrentIndex(std::max(0,m_scope->findData(view["segmentScope"].toString())));});
 connect(workflow,&StratigraphyWorkflow::changed,this,&StratigraphyPage::refresh);
 connect(workflow,&StratigraphyWorkflow::statusChanged,m_status,&QLabel::setText);
 connect(workflow,&StratigraphyWorkflow::busyChanged,this,[this](bool busy){m_wells->setEnabled(!busy);for(auto* panel:m_section->findChildren<WellComposite::WellCompositePanel*>())panel->setBoundaryAnnotationMode(true,m_workflow->canEdit());});
 connect(workflow,&StratigraphyWorkflow::boundarySelected,this,[this](qlonglong well,const QString& id){selectObject(well,id);});
 connect(m_wells,&QListWidget::currentItemChanged,this,[this](QListWidgetItem* item){if(item){m_selectedWell=item->data(Qt::UserRole).toLongLong();m_selectedObject.clear();}});
 connect(m_wells,&QListWidget::itemChanged,this,[this]{updateAvailability();});
 connect(m_scope,&QComboBox::currentIndexChanged,this,[this]{updateAvailability();});
 connect(m_results,&QTableWidget::cellClicked,this,[this](int row,int){const auto rows=m_workflow->rows();if(row<rows.size())selectObject(rows[row].wellId,rows[row].id,rows[row].cycle);});
 refresh();
}
QList<qlonglong> StratigraphyPage::selectedWells() const {
 if(!m_wells)return {};
 QList<qlonglong> ids;for(int i=0;i<m_wells->count();++i)if(m_wells->item(i)->checkState()==Qt::Checked)ids.append(m_wells->item(i)->data(Qt::UserRole).toLongLong());return ids;
}
bool StratigraphyPage::confirmReplace(){return !m_workflow->hasUnsavedChanges()||PaleoNotify::ask(this,tr("未保存的解释"),tr("当前方案有未保存修改，是否放弃并加载所选井或版本？"),PaleoNotify::AskButtons::DiscardCancel,PaleoNotify::AskDefault::Reject);} // #868
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
 QList<qlonglong> previous;QSet<qlonglong> checked;for(int i=0;i<m_wells->count();++i){auto* item=m_wells->item(i);const auto id=item->data(Qt::UserRole).toLongLong();previous.append(id);if(item->checkState()==Qt::Checked)checked.insert(id);}
 QList<qlonglong> current;for(const auto& w:wells)current.append(w.id);
 if(current!=previous){QSignalBlocker blocker(m_wells);m_wells->clear();for(const auto& well:wells){auto* item=new QListWidgetItem(well.name,m_wells);item->setData(Qt::UserRole,well.id);item->setCheckState(checked.contains(well.id)?Qt::Checked:Qt::Unchecked);}}
 const auto rows=m_workflow->rows();QSignalBlocker tableBlock(m_results);m_results->setRowCount(rows.size());
 for(int i=0;i<rows.size();++i){const auto& r=rows[i];const QString direction=r.direction==1?tr("正旋回"):r.direction==-1?tr("反旋回"):tr("方向待定");
  const QStringList cells={r.well,r.name,r.level,QString::number(r.top,'f',3),QString::number(r.bottom,'f',3),r.cycle?direction:r.source,r.status+(r.locked?tr(" · 保护"):QString())};
  for(int c=0;c<cells.size();++c)m_results->setItem(i,c,new QTableWidgetItem(cells[c]));
 }
 refreshVersions();updateAvailability();refreshPreview();m_refreshing=false;
 if(!m_selectedObject.isEmpty()){const auto id=m_selectedObject;const auto well=m_selectedWell;const bool cycle=m_selectedCycle;selectObject(well,id,cycle);}
}
void StratigraphyPage::showEvent(QShowEvent* event){
 QWidget::showEvent(event);m_workflow->refreshWells();
 if(!property("initialSplitApplied").toBool()) {
  setProperty("initialSplitApplied",true);
  QTimer::singleShot(0,this,[this]{
   const auto view=QJsonDocument::fromJson(QByteArray::fromStdString(m_workflow->document().viewStateJson)).object();
   if(view["top"].isDouble()&&view["bottom"].toDouble()>view["top"].toDouble())m_section->setDisplayDepthRange(view["top"].toDouble(),view["bottom"].toDouble());else fitFormation();
  });
 }
}
void StratigraphyPage::updateAvailability(){if(auto* b=findChild<QPushButton*>(QStringLiteral("autoDivideCycles")))b->setEnabled(m_workflow->canSubdivide(selectedWells(),m_scope->currentData().toString()));}
void StratigraphyPage::highlightSelection(){
 if(!m_section)return;
 for(int i=0;i<m_section->slotCount();++i)if(auto* panel=m_section->panelAt(i)) {
  double depth=qQNaN();
  if(panel->property("wellId").toLongLong()==m_selectedWell)
   for(const auto& row:m_workflow->rows())if(row.wellId==m_selectedWell&&row.id==m_selectedObject&&row.cycle==m_selectedCycle){depth=row.top;break;}
  panel->canvas()->setHoverDepth(depth);
 }
}
