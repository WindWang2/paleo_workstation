// 层：视图
#include "stratigraphypage.h"
#include "../windowsizing_internal.h"
#include "../../workflow/stratigraphyworkflow.h"
#include <paleo_data/interpretation.h>
#include <QBoxLayout>
#include <QFormLayout>
#include <QDialog>
#include <QDialogButtonBox>
#include <QPlainTextEdit>
#include <QTableWidget>
#include <QHeaderView>
#include <QComboBox>
#include <QSpinBox>
#include <QDoubleSpinBox>
#include <QCheckBox>
#include <QLineEdit>
#include <QLabel>
#include <QInputDialog>
#include <QListWidget>
#include <algorithm>
#include <map>
namespace {
QDialogButtonBox* acceptButtons(QDialog& dialog,QBoxLayout* layout) {
 auto* b=new QDialogButtonBox(QDialogButtonBox::Ok|QDialogButtonBox::Cancel,&dialog);layout->addWidget(b);
 QObject::connect(b,&QDialogButtonBox::accepted,&dialog,&QDialog::accept);QObject::connect(b,&QDialogButtonBox::rejected,&dialog,&QDialog::reject);return b;
}
}
void StratigraphyPage::showHierarchy() {
 if(!m_workflow->canEdit())return;
 QString segment=m_scope->currentData().toString();const auto rows=m_workflow->rows();
 for(const auto& r:rows)if(r.wellId==m_selectedWell&&r.id==m_selectedObject&&r.cycle)segment=r.segment;
 QList<StratigraphyRow> children;for(const auto& r:rows)if(r.cycle&&r.level=="4"&&r.wellId==m_selectedWell&&r.segment==segment)children.append(r);
 std::sort(children.begin(),children.end(),[](const auto& a,const auto& b){return a.top<b.top;});
 if(children.isEmpty()){m_status->setText(tr("请先选择一口井的旋回，或选择具体段；该段须有四级旋回。"));return;}
 QDialog dialog(this);dialog.setObjectName(QStringLiteral("stratigraphyHierarchyDialog"));dialog.setWindowTitle(tr("三级组合 · %1").arg(segment));paleo::ui_detail::resizeWithinScreen(&dialog,QSize(650,500));
 auto* layout=new QVBoxLayout(&dialog);auto* note=new QLabel(tr("连续四级旋回填写相同组号，即组成一个三级旋回。组号递增，0 表示不建立父级；同组方向须一致。共享端点与原深度保持不变。"),&dialog);note->setWordWrap(true);layout->addWidget(note);
 auto* table=new QTableWidget(children.size(),3,&dialog);table->setObjectName(QStringLiteral("hierarchyGroups"));table->setHorizontalHeaderLabels({tr("四级旋回"),tr("三级组号"),tr("三级方向")});table->horizontalHeader()->setSectionResizeMode(QHeaderView::Stretch);layout->addWidget(table);
 std::map<QString,int> groupIds;int count=0;
 for(int i=0;i<children.size();++i){const auto& c=children[i];table->setItem(i,0,new QTableWidgetItem(c.name));auto* group=new QSpinBox(table);group->setRange(0,children.size());int direction=0;
  if(!c.parent.isEmpty()){if(!groupIds.contains(c.parent))groupIds[c.parent]=++count;group->setValue(groupIds[c.parent]);for(const auto& p:rows)if(p.wellId==m_selectedWell&&p.id==c.parent&&p.cycle)direction=p.direction;}
  table->setCellWidget(i,1,group);auto* combo=new QComboBox(table);combo->addItem(tr("方向待定"),0);combo->addItem(tr("正旋回"),1);combo->addItem(tr("反旋回"),-1);combo->setCurrentIndex(combo->findData(direction));table->setCellWidget(i,2,combo);
 }
 acceptButtons(dialog,layout);if(dialog.exec()!=QDialog::Accepted)return;
 QList<StratigraphyGroup> groups;for(int i=0;i<children.size();++i)groups.append({children[i].id,qobject_cast<QSpinBox*>(table->cellWidget(i,1))->value(),qobject_cast<QComboBox*>(table->cellWidget(i,2))->currentData().toInt()});
 m_workflow->editHierarchy(m_selectedWell,segment,groups);
}
void StratigraphyPage::showAlgorithms() {
 if(!m_workflow->canEdit())return;auto profile=m_workflow->document().profile;
 QDialog dialog(this);dialog.setWindowTitle(tr("算法参数与输出约定"));auto* layout=new QVBoxLayout(&dialog);auto* form=new QFormLayout;layout->addLayout(form);
 auto* note=new QLabel(tr("参数随解释版本保存。AR 是样本阶数；余弦相位不是余弦相似度。显示与划分使用同一计算结果。"),&dialog);note->setWordWrap(true);form->addRow(note);
 const auto number=[&](const QString& label,double value,double lo,double hi){auto* s=new QDoubleSpinBox(&dialog);s->setRange(lo,hi);s->setDecimals(2);s->setValue(value);form->addRow(label,s);return s;};
 auto* step=number(tr("重采样步长 / m"),profile.sampleStep,.05,20);auto* gap=number(tr("长缺口阈值 / m"),profile.maxGap,.1,100);
 auto* padding=number(tr("计算余量 / m"),profile.padding,0,1000);auto* thickness=number(tr("最小旋回厚度 / m"),profile.minimumThickness,.1,500);
 auto* orders=new QLineEdit(&dialog);QStringList values;for(int n:profile.arOrders)values.append(QString::number(n));orders->setText(values.join(','));form->addRow(tr("AR 阶数，逗号分隔"),orders);
 const auto check=[&](const QString& label,bool value){auto* s=new QCheckBox(label,&dialog);s->setChecked(value);form->addRow(s);return s;};
 auto* resample=check(tr("等间隔重采样（关闭则保留原始采样）"),profile.resample);auto* invert=check(tr("−cos(φ)，示例相位符号约定"),profile.invertPhase);
 auto* pace=check(tr("PACE 参与划分"),profile.usePace);auto* residual=check(tr("GR 残差累计参与划分"),profile.useResidual);auto* phase=check(tr("相位参与划分"),profile.usePhase);
 auto* guidance=check(tr("允许模板定位候选（待校核）"),profile.allowTemplateGuidedCandidates);
 acceptButtons(dialog,layout);if(dialog.exec()!=QDialog::Accepted)return;
 std::vector<int> parsed;for(const auto& n:orders->text().split(',')){bool ok=false;int v=n.trimmed().toInt(&ok);if(!ok||v<2||v>512){m_status->setText(tr("AR 阶数需为 2–512 的整数"));return;}parsed.push_back(v);}
 if(parsed.empty()||parsed.size()>8){m_status->setText(tr("请选择 1–8 个 AR 阶数"));return;}
 profile.arOrders=parsed;profile.sampleStep=step->value();profile.maxGap=gap->value();profile.padding=padding->value();profile.minimumThickness=thickness->value();
 profile.resample=resample->isChecked();profile.invertPhase=invert->isChecked();profile.usePace=pace->isChecked();profile.useResidual=residual->isChecked();profile.usePhase=phase->isChecked();profile.allowTemplateGuidedCandidates=guidance->isChecked();m_workflow->setProfile(profile);
}
void StratigraphyPage::showDiagnostics() {
 const auto& doc=m_workflow->document();QStringList text;
 for(const auto& w:doc.wells){text.append(QString::fromStdString(w.name));for(const auto& warning:w.warnings)text.append(QString::fromStdString(warning));}
 for(const auto& run:doc.runs){text.append(QString::fromStdString(run.kind+" · "+run.status));text.append(QString::fromStdString(run.parametersJson));text.append(QString::fromStdString(run.diagnosticsJson));}
 QDialog dialog(this);dialog.setWindowTitle(tr("资料与计算诊断"));paleo::ui_detail::resizeWithinScreen(&dialog,QSize(850,600));auto* layout=new QVBoxLayout(&dialog);auto* view=new QPlainTextEdit(&dialog);view->setReadOnly(true);view->setPlainText(text.join('\n'));layout->addWidget(view);acceptButtons(dialog,layout);dialog.exec();
}
void StratigraphyPage::showWellOrder() {
 QDialog dialog(this);dialog.setWindowTitle(tr("拖动调整井序"));auto* layout=new QVBoxLayout(&dialog);auto* list=new QListWidget(&dialog);list->setDragDropMode(QAbstractItemView::InternalMove);
 for(const auto& well:m_workflow->document().wells){auto* item=new QListWidgetItem(QString::fromStdString(well.name),list);item->setData(Qt::UserRole,QVariant::fromValue<qlonglong>(well.wellId));}
 layout->addWidget(list);acceptButtons(dialog,layout);if(dialog.exec()!=QDialog::Accepted)return;QList<qlonglong> order;for(int i=0;i<list->count();++i)order.append(list->item(i)->data(Qt::UserRole).toLongLong());m_workflow->reorderWells(order);
}
