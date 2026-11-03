// 层：视图
#include "timedepthalignmentdialog.h"
#include "ui/paleotheme.h"
#include "ui/common/screenfit.h"
#include <QComboBox>
#include <QDialogButtonBox>
#include <QFormLayout>
#include <QLabel>
#include <QLineEdit>
#include <QListWidget>
#include <QPushButton>
#include <QVBoxLayout>

TimeDepthAlignmentDialog::TimeDepthAlignmentDialog(const QVariantMap &options,QWidget *parent):QDialog(parent){
  setObjectName("timeDepthAlignmentDialog");setWindowTitle(tr("时深来源与地震时间基准"));
  paleo::ui::resizeWithinScreen(this,720,540);m_writable=options.value("writable").toBool();
  auto *layout=new QVBoxLayout(this);layout->setSpacing(PaleoTheme::tokens().spacingSm);
  auto *note=new QLabel(tr("按来源保存，影响同一导出中的全部井。保留原始点值；取反仅用于读取换算。地震体共用所填时间零点，采样间隔和振幅不变。"),this);
  note->setWordWrap(true);layout->addWidget(note);
  m_sources=new QListWidget(this);m_sources->setObjectName("timeDepthSources");layout->addWidget(m_sources,1);
  const auto sources=options.value("sources").toList();
  for(const auto &value:sources){const auto row=value.toMap();
    auto *item=new QListWidgetItem(tr("%1 · %2 份井时深关系 · 示例 %3 点，时间 %4～%5（负值 %6 点）")
      .arg(row.value("name").toString()).arg(row.value("relations").toInt()).arg(row.value("samples").toInt())
      .arg(row.value("minTime").toDouble()).arg(row.value("maxTime").toDouble()).arg(row.value("negative").toInt()),m_sources);
    item->setData(Qt::UserRole,row.value("id"));item->setToolTip(row.value("path").toString());item->setCheckState(Qt::Checked);
  }
  auto *form=new QFormLayout;layout->addLayout(form);
  m_unit=new QComboBox(this);m_unit->setObjectName("timeDepthUnit");m_unit->addItem(tr("选择时间单位"));m_unit->addItem("ms","ms");m_unit->addItem("s","s");form->addRow(tr("时深时间单位"),m_unit);
  m_type=new QComboBox(this);m_type->setObjectName("timeDepthType");m_type->addItem(tr("选择单／双程"));m_type->addItem(tr("双程 TWT"),"TWT");m_type->addItem(tr("单程 OWT"),"OWT");form->addRow(tr("时深时间类型"),m_type);
  m_sign=new QComboBox(this);m_sign->setObjectName("timeDepthSign");m_sign->addItem(tr("选择时间正方向"));m_sign->addItem(tr("原值已向下为正"),false);m_sign->addItem(tr("原值取反，转为向下为正"),true);form->addRow(tr("时深符号换算"),m_sign);
  m_reference=new QLineEdit(this);m_reference->setObjectName("timeDepthReference");m_reference->setPlaceholderText(tr("填写共同时间零点名称，如工区地震时间零点"));form->addRow(tr("共同时间基准"),m_reference);
  m_volume=new QComboBox(this);m_volume->setObjectName("timeDepthVolume");m_volume->addItem(tr("仅保存时深来源，不修改地震体"),0);
  for(const auto &value:options.value("volumes").toList()){const auto row=value.toMap();m_volume->addItem(row.value("name").toString(),row.value("id"));}
  form->addRow(tr("同时关联 TWT 地震体"),m_volume);
  if(!sources.isEmpty()){
    const auto row=sources.first().toMap();
    auto common=[&](const char *key){for(const auto &value:sources)if(value.toMap().value(key)!=row.value(key))return false;return true;};
    if(common("unit"))m_unit->setCurrentIndex(qMax(0,m_unit->findData(row.value("unit"))));
    if(common("type"))m_type->setCurrentIndex(qMax(0,m_type->findData(row.value("type"))));
    if(common("reference"))m_reference->setText(row.value("reference").toString());
    if(!row.value("reference").toString().isEmpty()&&common("negate"))m_sign->setCurrentIndex(row.value("negate").toBool()?2:1);
  }
  m_message=new QLabel(this);m_message->setWordWrap(true);m_message->setObjectName("timeDepthMessage");layout->addWidget(m_message);
  if(sources.isEmpty())setMessage(tr("所选井没有数据服务时深来源"));else if(!m_writable)setMessage(tr("工程为只读，不能保存"));
  auto *buttons=new QDialogButtonBox(QDialogButtonBox::Save|QDialogButtonBox::Cancel,this);layout->addWidget(buttons);
  m_save=buttons->button(QDialogButtonBox::Save);m_save->setObjectName("saveTimeDepthSemantics");m_save->setText(tr("保存来源与时间基准"));
  buttons->button(QDialogButtonBox::Cancel)->setText(tr("取消"));connect(buttons,&QDialogButtonBox::rejected,this,&QDialog::reject);
  connect(buttons,&QDialogButtonBox::accepted,this,[this]{
    QVariantList ids;for(int i=0;i<m_sources->count();++i)if(m_sources->item(i)->checkState()==Qt::Checked)ids<<m_sources->item(i)->data(Qt::UserRole);
    emit saveRequested({{"sources",ids},{"unit",m_unit->currentData()},{"type",m_type->currentData()},
      {"reference",m_reference->text().trimmed()},{"negate",m_sign->currentData()},{"volume",m_volume->currentData()}});
  });
  for(auto *combo:{m_unit,m_type,m_sign})connect(combo,&QComboBox::currentIndexChanged,this,&TimeDepthAlignmentDialog::updateSave);
  connect(m_reference,&QLineEdit::textChanged,this,&TimeDepthAlignmentDialog::updateSave);
  connect(m_sources,&QListWidget::itemChanged,this,&TimeDepthAlignmentDialog::updateSave);updateSave();
}
void TimeDepthAlignmentDialog::setMessage(const QString &message){m_message->setText(message);}
void TimeDepthAlignmentDialog::updateSave(){
  bool checked=false;for(int i=0;i<m_sources->count();++i)checked|=m_sources->item(i)->checkState()==Qt::Checked;
  m_save->setEnabled(m_writable&&checked&&m_unit->currentIndex()>0&&m_type->currentIndex()>0&&m_sign->currentIndex()>0&&!m_reference->text().trimmed().isEmpty());
}
