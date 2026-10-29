// 层：视图
#include "stratigraphypage.h"
#include "../../workflow/stratigraphyworkflow.h"
#include <QDialog>
#include <QFormLayout>
#include <QDialogButtonBox>
#include <QLineEdit>
#include <QCheckBox>
#include <QInputDialog>
#include <QListWidget>
void StratigraphyPage::showConventions() {
 const auto current=m_workflow->conventions();QDialog dialog(this);dialog.setWindowTitle(tr("地层对比项目约定"));
 auto *form=new QFormLayout(&dialog);
 auto *depthType=new QLineEdit(current.depthType,&dialog);depthType->setPlaceholderText(tr("未知留空；例如 MD"));
 auto *depthUnit=new QLineEdit(current.depthUnit,&dialog);depthUnit->setPlaceholderText(tr("未知留空；例如 m"));
 auto *timeUnit=new QLineEdit(current.timeUnit,&dialog);timeUnit->setPlaceholderText(tr("未知留空；例如 ms"));
 auto *evidence=new QLineEdit(current.evidence,&dialog);
 auto *provisional=new QCheckBox(tr("允许生成临时对齐候选，仍须人工复核"),&dialog);provisional->setChecked(current.provisional);
 form->addRow(tr("已核实深度类型"),depthType);form->addRow(tr("已核实深度单位"),depthUnit);form->addRow(tr("已核实时间单位"),timeUnit);
 form->addRow(tr("依据"),evidence);form->addRow(provisional);
 auto *buttons=new QDialogButtonBox(QDialogButtonBox::Ok|QDialogButtonBox::Cancel,&dialog);form->addRow(buttons);
 connect(buttons,&QDialogButtonBox::accepted,&dialog,&QDialog::accept);connect(buttons,&QDialogButtonBox::rejected,&dialog,&QDialog::reject);
 if(dialog.exec()==QDialog::Accepted)m_workflow->setConventions({depthType->text().trimmed(),depthUnit->text().trimmed(),timeUnit->text().trimmed(),evidence->text().trimmed(),provisional->isChecked()});
}
void StratigraphyPage::chooseResource() {
 const QStringList labels={tr("GR 曲线"),tr("时深关系"),tr("井轨迹"),tr("人工四级旋回"),tr("人工三级旋回"),tr("地层道"),tr("T70 层位面"),tr("T72 层位面"),tr("T73 层位面"),tr("T80 层位面")};
 const QStringList kinds={"curves","time_depth","trajectories","manual4","manual3","formation","T70","T72","T73","T80"};
 bool ok=false;const QString label=QInputDialog::getItem(this,tr("选择输入资料"),tr("资料类型"),labels,0,false,&ok);if(!ok)return;
 const int index=labels.indexOf(label);const auto *well=m_wells->currentItem();
 if(index<6&&!well)return;
 const auto wellId=well?well->data(Qt::UserRole).toLongLong():0;
 const QString kind=index>=6?QStringLiteral("horizons"):index>=3?QStringLiteral("intervals"):kinds[index];
 const auto resources=m_workflow->resources(index>=6?0:wellId,kind);QStringList choices{tr("自动优选")};
 for(const auto &resource:resources)choices.append(QString::number(resource.id)+" · "+resource.name);
 const QString selection=QInputDialog::getItem(this,tr("选择输入资料"),tr("明确指定本次使用的资料"),choices,0,false,&ok);if(!ok)return;
 const int selected=choices.indexOf(selection);if(selected>=0)m_workflow->chooseResource(wellId,kinds[index],selected?resources[selected-1].id:0);
}
