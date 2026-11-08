// 层：视图
#include "paleomainwindow.h"
#include "paleotheme.h"
#include "common/screenfit.h"
#include "workflow/documentsession.h"
#include "workflow/sectionworkbench.h"
#include "seismicsection/timedepthalignmentdialog.h"
#include <QApplication>
#include <QComboBox>
#include <QDialogButtonBox>
#include <QFormLayout>
#include <QGroupBox>
#include <QLabel>
#include <QLineEdit>
#include <QPushButton>
#include <QStatusBar>
#include <QUuid>
#include <QVBoxLayout>

void PaleoMainWindow::showTimeDepthAlignment(const QStringList &ids){
  if(!m_sectionWorkbench)return;
  QString error;auto options=m_sectionWorkbench->timeDepthAlignmentOptions(ids,&error);
  const bool interpretationAvailable=m_documentSession && !m_documentSession->readOnly();
  if(!error.isEmpty() && !interpretationAvailable){statusBar()->showMessage(error,15000);return;}
  options.insert("interpretationAvailable",interpretationAvailable);
  auto *parent=QApplication::activeModalWidget();
  TimeDepthAlignmentDialog dialog(options,parent?parent:this);
  if(!error.isEmpty())dialog.setMessage(error);
  connect(&dialog,&TimeDepthAlignmentDialog::interpretationRequested,this,[this,&dialog,ids] {
    dialog.reject();showInterpretationSelection(ids);
  });
  connect(&dialog,&TimeDepthAlignmentDialog::saveRequested,this,[this,&dialog](const QVariantMap &selection){
    QString detail;if(!m_sectionWorkbench->saveTimeDepthAlignment(selection,&detail)){dialog.setMessage(detail);return;}
    dialog.accept();
    statusBar()->showMessage(detail.isEmpty()?tr("时深来源与地震时间基准已保存，正在刷新连井数据"):detail,15000);
  });
  dialog.exec();
}

namespace {
using namespace paleo::document;
void addChoices(QComboBox *box,const QVariantList &rows,const QString &dataKey) {
  for(const auto &value:rows) {
    const auto row=value.toMap();
    box->addItem(row.value("label").toString(),dataKey.isEmpty()?value:row.value(dataKey));
  }
}
bool parseObject(const QVariant &value,ModelObjectRef &ref,QString *error) {
  return modelObjectRefFromJson(QJsonObject::fromVariantMap(value.toMap()),ref,error);
}
void setPinnedChoice(QComboBox *box,const QVariant &value,const QString &missing) {
  int index=box->findData(value);
  if(index<0){index=box->count();box->addItem(missing,value);}
  box->setCurrentIndex(index);
}
struct ExactTimeDepthFields {
  QGroupBox *box;
  QLineEdit *mdReference,*timeReference;
  QComboBox *unit,*type,*sign;
  explicit ExactTimeDepthFields(QWidget *parent)
      :box(new QGroupBox(QObject::tr("确认所选时深关系的物理口径"),parent)),
       mdReference(new QLineEdit(box)),timeReference(new QLineEdit(box)),
       unit(new QComboBox(box)),type(new QComboBox(box)),sign(new QComboBox(box)) {
    box->setCheckable(true);box->setChecked(false);
    auto *form=new QFormLayout(box);
    unit->addItem(QObject::tr("请选择"));unit->addItem("ms","ms");unit->addItem("s","s");
    type->addItem(QObject::tr("请选择"));type->addItem("TWT","TWT");type->addItem("OWT","OWT");
    sign->addItem(QObject::tr("请选择"));sign->addItem(QObject::tr("原值向下为正"),false);
    sign->addItem(QObject::tr("原值取反"),true);
    form->addRow(QObject::tr("MD 起算面"),mdReference);
    form->addRow(QObject::tr("时间单位"),unit);
    form->addRow(QObject::tr("时间类型"),type);
    form->addRow(QObject::tr("时间零点"),timeReference);
    form->addRow(QObject::tr("正方向"),sign);
  }
  QVariantMap values() const {
    if(box->isHidden() || !box->isChecked())return {};
    return {{"mdReference",mdReference->text().trimmed()},{"unit",unit->currentData()},
            {"type",type->currentData()},{"reference",timeReference->text().trimmed()},
            {"negate",sign->currentData()}};
  }
};
}

void PaleoMainWindow::showInterpretationSelection(const QStringList &ids) {
  const QPointer<DocumentSession> document=m_documentSession;
  if(!document || document->readOnly())return;
  QString error;
  const auto options=document->interpretationOptions(ids,&error);
  if(!error.isEmpty()){statusBar()->showMessage(error,15000);return;}
  const auto epoch=document->epoch();
  const auto active=document->interpretationSelection();
  QDialog dialog(QApplication::activeModalWidget()?QApplication::activeModalWidget():this);
  dialog.setWindowTitle(tr("文档解释上下文"));
  dialog.setObjectName("documentInterpretationDialog");
  paleo::ui::resizeWithinScreen(&dialog,680,460);
  auto *layout=new QVBoxLayout(&dialog);
  layout->setSpacing(PaleoTheme::tokens().spacingSm);
  auto *note=new QLabel(tr("井道图、连井剖面和编图共享此方案修订。可为每口井保存所选时深关系或分层道。"),&dialog);
  note->setWordWrap(true);layout->addWidget(note);
  auto *form=new QFormLayout;layout->addLayout(form);
  auto *scheme=new QComboBox(&dialog),*volume=new QComboBox(&dialog),*well=new QComboBox(&dialog),*relation=new QComboBox(&dialog);
  scheme->setObjectName("interpretationScheme");volume->setObjectName("interpretationVolume");
  well->setObjectName("interpretationWell");relation->setObjectName("interpretationRelation");
  scheme->addItem(tr("未固定分层方案"));
  volume->addItem(tr("未固定目标地震体"));
  addChoices(scheme,options.value("schemes").toList(),{});
  addChoices(volume,options.value("volumes").toList(),"object");
  addChoices(well,options.value("wells").toList(),"id");
  if(active.context) {
    if(!active.context->schemeId.isEmpty()) {
      for(int i=1;i<scheme->count();++i) {
        const auto row=scheme->itemData(i).toMap();
        if(row.value("id").toString()==active.context->schemeId && row.value("revision").toString()==active.context->schemeRevision) {
          scheme->setCurrentIndex(i);break;
        }
      }
      if(scheme->currentIndex()==0)setPinnedChoice(scheme,QVariantMap{{"id",active.context->schemeId},{"revision",active.context->schemeRevision}},
          tr("原方案修订已失效"));
    }
    if(active.context->targetVolume)setPinnedChoice(volume,modelObjectRefJson(*active.context->targetVolume).toVariantMap(),tr("原目标地震体修订已失效"));
  }
  form->addRow(tr("分层方案修订"),scheme);form->addRow(tr("目标地震体修订"),volume);
  form->addRow(tr("井"),well);form->addRow(tr("井关系对象"),relation);
  const ExactTimeDepthFields semantics(&dialog);layout->addWidget(semantics.box);
  auto *message=new QLabel(&dialog);message->setWordWrap(true);layout->addWidget(message);
  const auto fillRelations=[document,well,relation,message] {
    relation->clear();relation->addItem(QObject::tr("只保存上下文"));
    QString detail;
    if(document) {
      const auto rows=document->interpretationRelationOptions(well->currentData().toString(),&detail);
      for(const auto &value:rows) {
        const auto row=value.toMap();
        relation->addItem((row.value("timeDepth").toBool()?QObject::tr("时深 · "):QObject::tr("分层 · "))+row.value("label").toString(),
                          row.value("object"));
      }
    }
    message->setText(detail);
  };
  connect(relation,&QComboBox::currentIndexChanged,&dialog,[relation,semantics] {
    ModelObjectRef ref;
    semantics.box->setVisible(parseObject(relation->currentData(),ref,nullptr) && ref.kind==ObjectKind::TimeDepth);
  });
  connect(well,&QComboBox::currentIndexChanged,&dialog,fillRelations);
  fillRelations();
  auto *buttons=new QDialogButtonBox(QDialogButtonBox::Save|QDialogButtonBox::Cancel,&dialog);
  layout->addWidget(buttons);
  if(active.context || !active.error.isEmpty()) {
    auto *reset=buttons->addButton(tr("使用工程默认解释"),QDialogButtonBox::ResetRole);
    connect(reset,&QPushButton::clicked,&dialog,[&] {
      QString detail;
      if(document && document->epoch()==epoch && document->clearInterpretation(&detail))dialog.accept();
      else message->setText(detail);
    });
  }
  connect(buttons,&QDialogButtonBox::rejected,&dialog,&QDialog::reject);
  QString pendingId;
  connect(document,&DocumentSession::commandFinished,&dialog,[&](const SourceCommandResult &result) {
    if(pendingId.isEmpty() || pendingId!=result.operationId)return;
    pendingId.clear();buttons->setEnabled(true);
    if(result.ok)dialog.accept();
    else message->setText(result.error);
  });
  connect(document,&DocumentSession::sessionChanged,&dialog,&QDialog::reject);
  connect(buttons,&QDialogButtonBox::accepted,&dialog,[&] {
    if(!document || document->epoch()!=epoch)return;
    QString detail;
    auto context=active.context.value_or(InterpretationContext{});
    context.documentId=document->documentId();context.modelStoreId="area-model";context.purpose=RelationPurpose::WellTie;
    const auto row=scheme->currentData().toMap();
    if(context.schemeId!=row.value("id").toString() || context.schemeRevision!=row.value("revision").toString()) {
      context.intervalId.clear();context.subIntervalId.clear();context.aggregate=false;
    }
    context.schemeId=row.value("id").toString();context.schemeRevision=row.value("revision").toString();
    context.targetVolume.reset();
    ModelObjectRef ref;
    if(volume->currentIndex()>0) {
      if(!parseObject(volume->currentData(),ref,&detail)){message->setText(detail);return;}
      context.targetVolume=ref;
    }
    if(relation->currentIndex()<=0) {
      if(document->selectInterpretation(context,&detail))dialog.accept();
      else message->setText(detail);
      return;
    }
    if(!parseObject(relation->currentData(),ref,&detail)){message->setText(detail);return;}
    auto command=document->chooseRelationCommand(context,well->currentData().toString(),ref,semantics.values(),&detail);
    if(!detail.isEmpty()){message->setText(detail);return;}
    command.operationId=QUuid::createUuid().toString(QUuid::WithoutBraces);pendingId=command.operationId;
    const auto result=document->execute(command);
    if(result.pending){buttons->setEnabled(false);message->setText(tr("正在核验所选关系与对象修订…"));}
    else {pendingId.clear();message->setText(result.error);}
  });
  if(dialog.exec()==QDialog::Accepted)statusBar()->showMessage(tr("文档解释选择已保存"),15000);
}
