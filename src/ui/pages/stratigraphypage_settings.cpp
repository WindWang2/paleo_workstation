// 层：视图
#include "stratigraphypage.h"
#include "../../workflow/stratigraphyworkflow.h"
#include <paleo_data/interpretation.h>
#include <QDialog>
#include <QDialogButtonBox>
#include <QBoxLayout>
#include <QFormLayout>
#include <QScrollArea>
#include <QLineEdit>
#include <QDoubleSpinBox>
#include <QCheckBox>
#include <QComboBox>
#include <QTableWidget>
#include <QHeaderView>
#include <QInputDialog>
#include <QLabel>
#include <QJsonObject>
#include <algorithm>
namespace {
void buttons(QDialog& d,QVBoxLayout* l) {
 auto* b=new QDialogButtonBox(QDialogButtonBox::Ok|QDialogButtonBox::Cancel,&d);l->addWidget(b);
 QObject::connect(b,&QDialogButtonBox::accepted,&d,&QDialog::accept);QObject::connect(b,&QDialogButtonBox::rejected,&d,&QDialog::reject);
}
}
void StratigraphyPage::showGeometry() {
 if(!m_workflow->canEdit())return;auto settings=m_workflow->geometrySettings();
 QDialog dialog(this);dialog.setWindowTitle(tr("确认坐标与时深约定"));dialog.resize(700,700);auto* layout=new QVBoxLayout(&dialog);
 auto* note=new QLabel(tr("仅按资料来源填写。未知项留空会阻止相关自动转换；已声明冲突不会被覆盖。外推只使用时深端部趋势，原始 GR 与时深采样保持不变。"),&dialog);note->setWordWrap(true);layout->addWidget(note);
 auto* scroll=new QScrollArea(&dialog);scroll->setWidgetResizable(true);auto* content=new QWidget(scroll);auto* form=new QFormLayout(content);scroll->setWidget(content);layout->addWidget(scroll);
 QList<QPair<QString,QLineEdit*>> strings;QList<QPair<QString,QDoubleSpinBox*>> numbers;QList<QPair<QString,QCheckBox*>> flags;
 const QList<QPair<QString,QString>> textFields={{"coordinateSystem",tr("共同 XY 坐标系")},{"coordinateUnit",tr("XY 单位")},{"timeReference",tr("共同时间基准")},{"mdReference",tr("MD 基准")},{"verticalDepthReference",tr("垂向深度基准")},{"depthUnit",tr("深度单位")},{"logDepthType",tr("测井深度类型")},{"depthType",tr("时深表深度类型")},{"wellTimeUnit",tr("井时间单位")},{"horizonTimeUnit",tr("层位时间单位")},{"wellTimeType",tr("井时间类型 TWT／OWT")},{"horizonTimeType",tr("层位时间类型 TWT／OWT")},{"policyEvidence",tr("约定依据")}};
 for(const auto& [key,label]:textFields){auto* edit=new QLineEdit(settings[key].toString(),content);form->addRow(label,edit);strings.append({key,edit});}
 const QList<QPair<QString,QString>> numericFields={{"wellTimeSign",tr("井时间符号 ±1")},{"horizonTimeSign",tr("层位时间符号 ±1")},{"wellOffsetMs",tr("井时间偏移 / ms")},{"horizonOffsetMs",tr("层位时间偏移 / ms")},{"maximumSurfaceEdge",tr("层位最大边长（0 不限制）")},{"maximumTopExtrapolationM",tr("顶部时深外推上限 / m（0 不另限）")},{"topExtrapolationFitWindowM",tr("顶部拟合窗 / m")},{"maximumBottomExtrapolationM",tr("底部时深外推上限 / m（0 不另限）")},{"bottomExtrapolationFitWindowM",tr("底部拟合窗 / m")},{"maximumLogExtrapolationM",tr("GR 两端搜索余量 / m")}};
 for(const auto& [key,label]:numericFields){auto* edit=new QDoubleSpinBox(content);edit->setRange(key.contains("Offset")?-100000:0,100000);if(key.endsWith("Sign")){edit->setRange(-1,1);edit->setSingleStep(2);}edit->setDecimals(3);edit->setValue(settings[key].toDouble());form->addRow(label,edit);numbers.append({key,edit});}
 const QList<QPair<QString,QString>> boolFields={{"resolveFromSources",tr("按来源约定自动解析")},{"allowProvisionalAlignment",tr("允许临时对齐候选（待复核）")},{"allowOneWay",tr("允许明确 OWT 转为 TWT")},{"allowIrregularSurface",tr("允许不规则层位面")},{"allowTopExtrapolation",tr("启用顶部时深趋势外推")},{"allowBottomExtrapolation",tr("启用底部时深趋势外推")}};
 for(const auto& [key,label]:boolFields){auto* edit=new QCheckBox(label,content);edit->setChecked(settings[key].toBool());form->addRow(edit);flags.append({key,edit});}
 buttons(dialog,layout);if(dialog.exec()!=QDialog::Accepted)return;
 for(const auto& [key,value]:strings)settings[key]=value->text().trimmed();for(const auto& [key,value]:numbers)settings[key]=value->value();for(const auto& [key,value]:flags)settings[key]=value->isChecked();
 m_workflow->setGeometrySettings(settings);
}
void StratigraphyPage::showClustering() {
 if(!m_workflow->canEdit())return;const int size=int(m_workflow->document().wells.size());if(size<2){m_status->setText(tr("至少加载两口井后进行分组"));return;}
 bool ok=false;const int count=QInputDialog::getInt(this,tr("曲线形态分组"),tr("分组数量"),std::min(3,size),1,std::min(8,size),1,&ok);if(!ok)return;
 auto groups=m_workflow->previewClustering(count);if(groups.members.empty())return;
 QDialog dialog(this);dialog.setWindowTitle(tr("井群分组与人工修订"));auto* layout=new QVBoxLayout(&dialog);auto* note=new QLabel(tr("分组用于推荐同类参考井；具有人工解释的井才能作为标准井。可修订分组，原始计算与人工修订标记同时保留。"),&dialog);note->setWordWrap(true);layout->addWidget(note);
 auto* table=new QTableWidget(int(groups.members.size()),2,&dialog);table->setHorizontalHeaderLabels({tr("井"),tr("分组")});table->horizontalHeader()->setSectionResizeMode(QHeaderView::Stretch);layout->addWidget(table);
 for(size_t i=0;i<groups.members.size();++i){QString name;for(const auto& w:m_workflow->document().wells)if(w.wellId==groups.members[i].wellId)name=QString::fromStdString(w.name);table->setItem(int(i),0,new QTableWidgetItem(name));auto* combo=new QComboBox(table);for(int g=0;g<count;++g)combo->addItem(tr("组 %1").arg(g+1),g);combo->setCurrentIndex(groups.members[i].group);table->setCellWidget(int(i),1,combo);}
 buttons(dialog,layout);if(dialog.exec()!=QDialog::Accepted)return;
 for(size_t i=0;i<groups.members.size();++i){const int n=qobject_cast<QComboBox*>(table->cellWidget(int(i),1))->currentData().toInt();groups.members[i].manual=n!=groups.members[i].group;groups.members[i].group=n;}m_workflow->saveClustering(groups);
}
void StratigraphyPage::startHorizons() {
 const auto ids=selectedWells();if(!m_workflow->selectionMatches(ids)&&!confirmReplace())return;
 const auto issues=m_workflow->inputIssues(ids);if(issues.isEmpty()){m_workflow->calculate(ids,false);return;}
 QDialog dialog(this);dialog.setWindowTitle(tr("仅处理未能自动匹配的资料"));dialog.resize(850,420);auto* layout=new QVBoxLayout(&dialog);
 auto* note=new QLabel(tr("唯一来源已自动选择。缺失资料请先在数据管理中补齐；多个候选仅需明确选择一个来源。"),&dialog);note->setWordWrap(true);layout->addWidget(note);
 auto* table=new QTableWidget(issues.size(),3,&dialog);table->setHorizontalHeaderLabels({tr("井／界面"),tr("待确认来源"),tr("说明")});table->horizontalHeader()->setSectionResizeMode(QHeaderView::Stretch);layout->addWidget(table);
 for(int i=0;i<issues.size();++i){const auto& issue=issues[i];QString name;for(const auto& w:m_workflow->wells())if(w.id==issue.well)name=w.name;table->setItem(i,0,new QTableWidgetItem(name+" · "+issue.field));table->setItem(i,2,new QTableWidgetItem(issue.message));auto* combo=new QComboBox(table);combo->addItem(tr("请选择一个来源"),0);
  const QString category=issue.field=="curve"?QStringLiteral("curves"):issue.field.startsWith("horizon:")?QStringLiteral("horizons"):QStringLiteral("intervals");
  for(const auto& resource:m_workflow->resources(issue.field.startsWith("horizon:")?0:issue.well,category))if(issue.candidates.contains(resource.id))combo->addItem(resource.name,resource.id);table->setCellWidget(i,1,combo);
 }
 buttons(dialog,layout);if(dialog.exec()!=QDialog::Accepted)return;
 for(int i=0;i<issues.size();++i){const auto id=qobject_cast<QComboBox*>(table->cellWidget(i,1))->currentData().toLongLong();if(!id)continue;QString kind=issues[i].field;if(kind=="curve")kind="curves";else if(kind.startsWith("horizon:"))kind=kind.mid(8);m_workflow->chooseResource(issues[i].well,kind,id);}
 if(!m_workflow->inputIssues(ids).isEmpty()){m_status->setText(tr("仍有资料缺失或来源待确认，请补齐后再计算。"));return;}m_workflow->calculate(ids,false);
}
