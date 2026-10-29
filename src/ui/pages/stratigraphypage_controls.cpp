// 层：视图
#include "stratigraphypage.h"
#include "../../workflow/stratigraphyworkflow.h"
#include "../wellcomposite/multiwellview.h"
#include <paleo_data/interpretation.h>
#include <QBoxLayout>
#include <QFormLayout>
#include <QPushButton>
#include <QComboBox>
#include <QLineEdit>
#include <QDoubleSpinBox>
#include <QCheckBox>
#include <QLabel>
#include <QMessageBox>
#include <QInputDialog>
#include <QSignalBlocker>
void StratigraphyPage::buildActions(QBoxLayout* layout) {
 auto* row=new QHBoxLayout;layout->addLayout(row);
 const auto button=[this,row](const QString& text,const char* name,const auto& fn){auto* b=new QPushButton(text,this);b->setObjectName(QLatin1String(name));row->addWidget(b);connect(b,&QPushButton::clicked,this,fn);return b;};
 auto* load=button(tr("加载井道图"),"loadCorrelationWells",[this]{if(confirmReplace())m_workflow->loadWells(selectedWells());});
 auto* horizons=button(tr("计算区域界面"),"autoResolveBoundaries",[this]{startHorizons();});
 auto* divide=button(tr("按统一模板划分"),"autoDivideCycles",[this]{m_workflow->calculate(selectedWells(),true,m_scope->currentData().toString());});
 auto* save=button(tr("保存分层版本"),"saveInterpretation",[this]{m_workflow->setViewOptions({{"flatten",m_flatten->currentData().toString()},{"segmentScope",m_scope->currentData().toString()}});m_workflow->save();});
 auto* undo=button(tr("撤销"),"undoInterpretation",[this]{m_workflow->undo();});
 auto* redo=button(tr("重做"),"redoInterpretation",[this]{m_workflow->redo();});
 auto* advanced=button(tr("更多设置与历史"),"stratigraphyAdvancedToggle",[this](bool checked){if(auto* w=findChild<QWidget*>(QStringLiteral("stratigraphyAdvanced")))w->setVisible(checked);});advanced->setCheckable(true);
 button(tr("计算结果…"),"stratigraphyResultsButton",[this]{showResults();});
 auto* cancel=button(tr("取消计算"),"cancelStratigraphy",[this]{m_workflow->cancel();});cancel->hide();
 row->addStretch();
 const auto update=[this,load,horizons,divide,save,undo,redo]{const bool enabled=m_workflow->canEdit();for(auto* b:{load,horizons,divide,save})b->setEnabled(enabled);undo->setEnabled(m_workflow->canUndo());redo->setEnabled(m_workflow->canRedo());};
 connect(m_workflow,&StratigraphyWorkflow::changed,this,update);
 connect(m_workflow,&StratigraphyWorkflow::busyChanged,this,[update,cancel](bool busy){update();cancel->setVisible(busy);});update();
}
void StratigraphyPage::buildEditor(QBoxLayout* layout) {
 auto* editor=new QWidget(this);editor->setObjectName(QStringLiteral("stratigraphyEditor"));
 auto* form=new QFormLayout(editor);m_name=new QLineEdit(editor);m_name->setObjectName(QStringLiteral("boundaryName"));
 m_depth=new QDoubleSpinBox(editor);m_depth->setObjectName(QStringLiteral("boundaryMd"));m_depth->setRange(-100000,100000);m_depth->setDecimals(3);m_depth->setSuffix(tr(" m"));
 m_lock=new QCheckBox(tr("人工保护"),editor);m_lock->setObjectName(QStringLiteral("protectBoundary"));
 m_level=new QComboBox(editor);m_level->setObjectName(QStringLiteral("cycleLevel"));m_level->addItem(tr("三级旋回"),3);m_level->addItem(tr("四级旋回"),4);
 m_direction=new QComboBox(editor);m_direction->setObjectName(QStringLiteral("cycleDirection"));m_direction->addItem(tr("方向待定"),0);m_direction->addItem(tr("正旋回"),1);m_direction->addItem(tr("反旋回"),-1);
 form->addRow(tr("界线／旋回名称"),m_name);form->addRow(tr("界线 MD"),m_depth);form->addRow(m_lock);
 const auto button=[this,editor,form](const QString& text,const char* name,const auto& fn){auto* b=new QPushButton(text,editor);b->setObjectName(QLatin1String(name));form->addRow(b);connect(b,&QPushButton::clicked,this,fn);return b;};
 button(tr("应用界线属性"),"applyBoundary",[this]{if(!m_selectedCycle&&!m_selectedObject.isEmpty())m_workflow->editBoundary(m_selectedWell,m_selectedObject,m_depth->value(),m_name->text(),m_lock->isChecked());});
 button(tr("新增内部界线"),"addBoundary",[this]{m_workflow->addBoundary(m_selectedWell,m_depth->value(),m_name->text());});
 button(tr("确认／采用候选"),"confirmBoundary",[this]{if(!m_selectedCycle&&!m_selectedObject.isEmpty())m_workflow->confirmBoundary(m_selectedWell,m_selectedObject);});
 button(tr("删除界线并合并相邻旋回"),"removeBoundary",[this]{
  if(m_selectedCycle||m_selectedObject.isEmpty())return;
  if(QMessageBox::question(this,tr("合并相邻旋回"),tr("删除此内部界面并合并相邻两个四级旋回？其他旋回和三级边界保持不变。"))==QMessageBox::Yes)m_workflow->removeBoundary(m_selectedWell,m_selectedObject);
 });
 form->addRow(tr("旋回级别"),m_level);form->addRow(tr("旋回方向"),m_direction);
 button(tr("应用旋回属性"),"applyCycle",[this]{if(m_selectedCycle)m_workflow->editCycle(m_selectedWell,m_selectedObject,m_name->text(),m_level->currentData().toInt(),m_direction->currentData().toInt());});
 button(tr("编辑三级组合…"),"editHierarchy",[this]{showHierarchy();});
 m_source=new QLabel(editor);m_source->setObjectName(QStringLiteral("selectionSource"));m_source->setWordWrap(true);m_source->setTextFormat(Qt::PlainText);m_source->setTextInteractionFlags(Qt::TextSelectableByMouse);form->addRow(m_source);
 connect(m_workflow,&StratigraphyWorkflow::changed,editor,[this,editor]{editor->setEnabled(m_workflow->canEdit());});
 connect(m_workflow,&StratigraphyWorkflow::busyChanged,editor,[this,editor](bool){editor->setEnabled(m_workflow->canEdit());});editor->setEnabled(m_workflow->canEdit());
 layout->addWidget(editor,1);
}
void StratigraphyPage::buildAdvanced(QBoxLayout* layout) {
 auto* widget=new QWidget(this);widget->setObjectName(QStringLiteral("stratigraphyAdvanced"));auto* box=new QVBoxLayout(widget);
 auto* row=new QHBoxLayout;box->addLayout(row);
 const auto button=[this,row](const QString& text,const char* name,const auto& fn){auto* b=new QPushButton(text,this);b->setObjectName(QLatin1String(name));row->addWidget(b);connect(b,&QPushButton::clicked,this,fn);return b;};
 button(tr("项目约定…"),"stratigraphyConventions",[this]{showConventions();});button(tr("选择输入资料…"),"stratigraphyInputs",[this]{chooseResource();});
 button(tr("坐标与时深…"),"stratigraphyGeometry",[this]{showGeometry();});
 button(tr("算法参数…"),"algorithmParameters",[this]{showAlgorithms();});button(tr("资料与计算诊断…"),"stratigraphyDiagnostics",[this]{showDiagnostics();});
 button(tr("井序…"),"stratigraphyWellOrder",[this]{showWellOrder();});
 button(tr("井群分组…"),"stratigraphyClusters",[this]{showClustering();});
 button(tr("应用分层到工程"),"activateStratigraphy",[this]{m_workflow->activateClassification(m_selectedWell);});
 m_flatten=new QComboBox(widget);m_flatten->setObjectName(QStringLiteral("flattenDatum"));m_flatten->addItem(tr("真实 MD"),QString());
 for(const char* r:{"T70","T72","T73","T80"})m_flatten->addItem(tr("拉平 %1").arg(QLatin1String(r)),QLatin1String(r));row->addWidget(m_flatten);
 connect(m_flatten,&QComboBox::currentIndexChanged,this,[this]{if(m_section&&!m_reloadPreview){m_section->setDatumMarker(m_flatten->currentData().toString());fitFormation();}m_workflow->setViewOptions({{"flatten",m_flatten->currentData().toString()}});});
 auto* second=new QHBoxLayout;box->addLayout(second);m_scope=new QComboBox(widget);m_scope->setObjectName(QStringLiteral("segmentScope"));m_scope->addItem(tr("全部段"),QString());
 for(const char* r:{"E12","E3","E4"})m_scope->addItem(QLatin1String(r),QLatin1String(r));second->addWidget(m_scope);
 connect(m_scope,&QComboBox::currentIndexChanged,this,[this]{m_workflow->setViewOptions({{"segmentScope",m_scope->currentData().toString()}});});
 auto* derive=new QPushButton(tr("重算所选段曲线"),widget);derive->setObjectName(QStringLiteral("calculateCurves"));second->addWidget(derive);connect(derive,&QPushButton::clicked,this,[this]{m_workflow->derive(m_scope->currentData().toString());});
 m_template=new QComboBox(widget);m_template->setObjectName(QStringLiteral("referenceTemplate"));second->addWidget(m_template,1);
 auto* propagate=new QPushButton(tr("各井各段模板传播…"),widget);second->addWidget(propagate);connect(propagate,&QPushButton::clicked,this,[this]{showAssignments();});
 auto* extract=new QPushButton(tr("保存人工标准模板…"),widget);second->addWidget(extract);connect(extract,&QPushButton::clicked,this,[this]{
  bool ok=false;const auto name=QInputDialog::getText(this,tr("人工标准模板"),tr("模板名称"),QLineEdit::Normal,{},&ok);if(!ok||name.trimmed().isEmpty())return;
  const double tolerance=QInputDialog::getDouble(this,tr("人工解释端点协调"),tr("模板提取容差 / m（0 为严格一致，不移动原井端点）"),0,0,20,2,&ok);if(ok)m_workflow->makeTemplate(m_selectedWell,name,tolerance);
 });
 auto* history=new QHBoxLayout;box->addLayout(history);m_saved=new QComboBox(widget);m_saved->setObjectName(QStringLiteral("savedInterpretation"));m_revision=new QComboBox(widget);m_revision->setObjectName(QStringLiteral("interpretationRevision"));history->addWidget(m_saved,1);history->addWidget(m_revision,1);
 auto* read=new QPushButton(tr("读取所选版本"),widget);read->setObjectName(QStringLiteral("readInterpretation"));history->addWidget(read);
 connect(m_saved,&QComboBox::currentIndexChanged,this,&StratigraphyPage::refreshRevisions);
 connect(read,&QPushButton::clicked,this,[this]{if(confirmReplace())m_workflow->loadVersion(m_saved->currentData().toString(),m_revision->currentData().toString());});
 layout->addWidget(widget);widget->hide();
}
void StratigraphyPage::refreshRevisions() {
 QSignalBlocker blocker(m_revision);m_revision->clear();const auto id=m_saved->currentData().toString();
 for(const auto& v:m_workflow->versions(id))m_revision->addItem(v.name+QStringLiteral(" · ")+v.id.left(8),v.id);
 const auto& doc=m_workflow->document();const QString revision=QString::fromStdString(doc.id)==id?QString::fromStdString(doc.revision):m_saved->currentData(Qt::UserRole+1).toString();
 const int at=m_revision->findData(revision);if(at>=0)m_revision->setCurrentIndex(at);
}
void StratigraphyPage::refreshVersions() {
 QSignalBlocker blocker(m_saved);const auto selected=m_saved->currentData();m_saved->clear();
 for(const auto& v:m_workflow->versions()){m_saved->addItem(v.name,v.id);m_saved->setItemData(m_saved->count()-1,v.head,Qt::UserRole+1);}
 const auto& doc=m_workflow->document();int at=m_saved->findData(QString::fromStdString(doc.id));if(at<0)at=m_saved->findData(selected);if(at>=0)m_saved->setCurrentIndex(at);refreshRevisions();
 QSignalBlocker templates(m_template);const auto key=m_template->currentData();m_template->clear();
 for(const auto& t:m_workflow->templates())m_template->addItem(t.name,t.id);
 at=m_template->findData(key);if(at>=0)m_template->setCurrentIndex(at);
}
