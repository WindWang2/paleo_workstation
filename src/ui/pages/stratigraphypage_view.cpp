// 层：视图
#include "stratigraphypage.h"
#include "../../workflow/stratigraphyworkflow.h"
#include "../wellcomposite/multiwellview.h"
#include <QBoxLayout>
#include <QDoubleSpinBox>
#include <QPushButton>
#include <QLabel>
#include <QFileDialog>
#include <algorithm>
#include <cmath>
#include <limits>
void StratigraphyPage::buildRangeControls(QBoxLayout* layout) {
 auto* row=new QHBoxLayout;layout->addLayout(row);auto* fit=new QPushButton(tr("全图"),this);fit->setObjectName(QStringLiteral("fitFormation"));row->addWidget(fit);connect(fit,&QPushButton::clicked,this,&StratigraphyPage::fitFormation);
 row->addWidget(new QLabel(tr("显示深度范围（拉平后为相对深度）"),this));
 m_rangeTop=new QDoubleSpinBox(this);m_rangeTop->setObjectName(QStringLiteral("displayTop"));m_rangeBottom=new QDoubleSpinBox(this);m_rangeBottom->setObjectName(QStringLiteral("displayBottom"));
 for(auto* field:{m_rangeTop,m_rangeBottom}){field->setRange(-100000,100000);field->setDecimals(3);field->setSuffix(tr(" m"));row->addWidget(field);}
 auto* apply=new QPushButton(tr("应用范围"),this);apply->setObjectName(QStringLiteral("applyDisplayRange"));row->addWidget(apply);connect(apply,&QPushButton::clicked,this,[this]{
  const double top=m_rangeTop->value(),bottom=m_rangeBottom->value();if(bottom<=top){m_status->setText(tr("显示底界必须大于顶界。"));return;}
  m_section->setDisplayDepthRange(top,bottom);m_workflow->setViewOptions({{"top",top},{"bottom",bottom}});
 });
 auto* image=new QPushButton(tr("导出连井图…"),this);image->setObjectName(QStringLiteral("exportCorrelation"));row->addWidget(image);connect(image,&QPushButton::clicked,this,[this]{
  const auto path=QFileDialog::getSaveFileName(this,tr("导出当前连井图"),{},tr("PNG 图像 (*.png)"));if(path.isEmpty())return;
  m_status->setText(m_section->grab().save(path)?tr("连井图已导出（包含井组及深度尺）。"):tr("图像导出失败，请检查路径、可写性或缩小井组。"));
 });row->addStretch();
}
void StratigraphyPage::fitFormation() {
 if(!m_section)return;
 double lo=INFINITY,hi=-INFINITY;const auto rows=m_workflow->rows();const auto tracks=m_workflow->tracks();
 for(int i=0;i<m_section->slotCount();++i){auto* panel=m_section->panelAt(i);if(!panel)continue;const auto id=panel->property("wellId");double a=INFINITY,b=-INFINITY;
  for(const auto& row:rows)if(row.wellId==id.toLongLong()&&!row.cycle&&std::isfinite(row.top)){a=std::min(a,row.top);b=std::max(b,row.top);}
  if(!std::isfinite(a)||b<=a)for(const auto& track:tracks)if(track.wellId==id.toLongLong())for(float md:track.depths)if(std::isfinite(md)){a=std::min(a,double(md));b=std::max(b,double(md));}
  if(std::isfinite(a)&&std::isfinite(b)){const double offset=m_section->displayOffset(panel);lo=std::min(lo,a-offset);hi=std::max(hi,b-offset);}
 }
 if(std::isfinite(lo)&&hi>lo){const double padding=(hi-lo)*.04;const double top=lo-padding,bottom=hi+padding;
  m_section->setDisplayDepthRange(top,bottom);m_rangeTop->setValue(top);m_rangeBottom->setValue(bottom);m_workflow->setViewOptions({{"top",top},{"bottom",bottom}});
 }
}
