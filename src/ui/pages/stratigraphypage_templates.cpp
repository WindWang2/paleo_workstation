// 层：视图
#include "stratigraphypage.h"
#include "../../workflow/stratigraphyworkflow.h"
#include <QDialog>
#include <QBoxLayout>
#include <QLabel>
#include <QTableWidget>
#include <QHeaderView>
#include <QComboBox>
#include <QDialogButtonBox>
#include <algorithm>
void StratigraphyPage::showAssignments() {
 const auto scope=m_scope->currentData().toString();const auto selected=m_template->currentData().toString();
 if(selected.isEmpty()){m_status->setText(tr("先提取或选择人工标准模板。"));return;}
 auto rows=m_workflow->prepareAssignments(scope,selected);if(rows.isEmpty())return;
 QDialog dialog(this);dialog.setObjectName(QStringLiteral("stratigraphyAssignments"));dialog.setWindowTitle(tr("确认各井、各段的标准模板"));dialog.resize(800,500);auto* layout=new QVBoxLayout(&dialog);
 auto* note=new QLabel(tr("优先推荐同组且具有该段人工解释的标准井。每段可采用不同模板，也可保留本段；标准井自身保留人工解释。"),&dialog);note->setWordWrap(true);layout->addWidget(note);
 auto* table=new QTableWidget(rows.size(),3,&dialog);table->setObjectName(QStringLiteral("templateAssignments"));table->setHorizontalHeaderLabels({tr("目标井"),tr("内部段"),tr("采用的人工模板")});table->horizontalHeader()->setSectionResizeMode(QHeaderView::Stretch);layout->addWidget(table);
 for(int i=0;i<rows.size();++i){const auto& row=rows[i];table->setItem(i,0,new QTableWidgetItem(row.name));table->setItem(i,1,new QTableWidgetItem(row.segment));auto* combo=new QComboBox(table);combo->addItem(tr("保留本段／暂不传播"),QString());
  for(const auto& t:row.choices)combo->addItem(t.name+(t.sameGroup?tr(" [同组]"):QString()),t.key);combo->setCurrentIndex(std::max(0,combo->findData(row.key)));table->setCellWidget(i,2,combo);
 }
 auto* buttons=new QDialogButtonBox(QDialogButtonBox::Ok|QDialogButtonBox::Cancel,&dialog);layout->addWidget(buttons);connect(buttons,&QDialogButtonBox::accepted,&dialog,&QDialog::accept);connect(buttons,&QDialogButtonBox::rejected,&dialog,&QDialog::reject);
 if(dialog.exec()!=QDialog::Accepted)return;for(int i=0;i<rows.size();++i)rows[i].key=qobject_cast<QComboBox*>(table->cellWidget(i,2))->currentData().toString();m_workflow->propagate(rows,scope);
}
