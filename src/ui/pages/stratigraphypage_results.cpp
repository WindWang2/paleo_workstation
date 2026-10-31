// 层：视图
#include "stratigraphypage.h"
#include "../windowsizing_internal.h"
#include "stratigraphyresultplot.h"
#include "../../workflow/stratigraphyworkflow.h"
#include <QDialog>
#include <QBoxLayout>
#include <QComboBox>
#include <QLabel>
#include <QSplitter>
#include <QTableWidget>
#include <QHeaderView>
#include <QSpinBox>
#include <QPushButton>
#include <QPlainTextEdit>
#include <QJsonDocument>
#include <QFileDialog>
#include <QSignalBlocker>
#include <algorithm>
#include <cmath>
void StratigraphyPage::showResults() {
 const auto document=m_workflow->document();std::vector<const paleo::data::InterpretationSeries*> all;
 QDialog dialog(this);dialog.setWindowTitle(tr("计算结果 · 数值与曲线"));paleo::ui_detail::resizeWithinScreen(&dialog,QSize(1100,720));auto* layout=new QVBoxLayout(&dialog);auto* combo=new QComboBox(&dialog);combo->setObjectName(QStringLiteral("stratigraphyResultSeries"));
 for(const auto& w:document.wells)for(const auto& s:w.series){all.push_back(&s);combo->addItem(QString::fromStdString(w.name+" / "+s.segmentId+" / "+s.kind+" / "+s.name));}
 if(all.empty()){m_status->setText(tr("请先计算曲线或完成层序划分。"));return;}layout->addWidget(combo);
 auto* note=new QLabel(&dialog);note->setWordWrap(true);layout->addWidget(note);auto* split=new QSplitter(&dialog);auto* plot=new StratigraphyResultPlot(split);plot->setMinimumWidth(450);split->addWidget(plot);
 auto* table=new QTableWidget(0,3,split);table->horizontalHeader()->setSectionResizeMode(QHeaderView::Stretch);table->setEditTriggers(QAbstractItemView::NoEditTriggers);split->addWidget(table);split->setStretchFactor(0,13);split->setStretchFactor(1,7);layout->addWidget(split,1);
 auto* bottom=new QHBoxLayout;bottom->addWidget(new QLabel(tr("表格起始行（每页 500 行）"),&dialog));auto* offset=new QSpinBox(&dialog);bottom->addWidget(offset);bottom->addStretch();
 auto* info=new QPushButton(tr("参数与来源"),&dialog);auto* csv=new QPushButton(tr("导出完整 CSV"),&dialog);auto* close=new QPushButton(tr("关闭"),&dialog);for(auto* b:{info,csv,close})bottom->addWidget(b);layout->addLayout(bottom);
 const auto refresh=[&]{const auto& s=*all[size_t(combo->currentIndex())];plot->series=&s;plot->update();const size_t n=std::min(s.axis.size(),s.values.size());
  QSignalBlocker block(offset);offset->setMaximum(n?int(n-1):0);const size_t start=size_t(offset->value());const int count=int(std::min(size_t(500),n-start));
  table->setHorizontalHeaderLabels({QString::fromStdString(s.axisKind+" / "+s.axisUnit),tr("数值 / %1").arg(QString::fromStdString(s.unit)),tr("质量标记")});table->setRowCount(count);
  for(int i=0;i<count;++i){const size_t k=start+size_t(i);table->setItem(i,0,new QTableWidgetItem(QString::number(s.axis[k],'g',15)));table->setItem(i,1,new QTableWidgetItem(std::isfinite(s.values[k])?QString::number(s.values[k],'g',15):tr("缺失")));table->setItem(i,2,new QTableWidgetItem(QString::number(k<s.quality.size()?s.quality[k]:0)));}
  note->setText(tr("%1 个样点 · 状态 %2 · 运行 %3\n%4").arg(n).arg(QString::fromStdString(s.status),QString::fromStdString(s.runId),s.kind=="FISCHER_CDMT"?tr("费舍尔 CDMT：横轴为旋回序号，纵轴为旋回厚度偏离平均值的累计量（米）。"):tr("显示保存数值；质量标记 1 为缺失，2 为插值，4 为边缘不稳定。")));
 };
 connect(combo,&QComboBox::currentIndexChanged,&dialog,[&]{QSignalBlocker block(offset);offset->setValue(0);refresh();});connect(offset,&QSpinBox::valueChanged,&dialog,[&]{refresh();});connect(close,&QPushButton::clicked,&dialog,&QDialog::accept);
 connect(info,&QPushButton::clicked,&dialog,[&]{const auto& s=*all[size_t(combo->currentIndex())];QDialog details(&dialog);details.setWindowTitle(tr("计算参数与来源"));paleo::ui_detail::resizeWithinScreen(&details,QSize(750,500));auto* l=new QVBoxLayout(&details);auto* text=new QPlainTextEdit(&details);text->setReadOnly(true);
  text->setPlainText(tr("曲线：%1\n运行：%2\n输入摘要：%3\n段：%4\n状态：%5\n\n").arg(QString::fromStdString(s.id),QString::fromStdString(s.runId),QString::fromStdString(s.inputFingerprint),QString::fromStdString(s.segmentId),QString::fromStdString(s.status))+QString::fromUtf8(QJsonDocument::fromJson(QByteArray::fromStdString(s.parametersJson)).toJson(QJsonDocument::Indented)));l->addWidget(text);details.exec();
 });
 connect(csv,&QPushButton::clicked,&dialog,[&]{const auto path=QFileDialog::getSaveFileName(&dialog,tr("导出计算曲线"),{},tr("CSV 文件 (*.csv)"));if(!path.isEmpty())m_workflow->exportCurve(*all[size_t(combo->currentIndex())],path);});refresh();dialog.exec();
}
