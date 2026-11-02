// 层：视图
#include "paleomainwindow.h"
#include "workflow/sectionworkbench.h"
#include "seismicsection/timedepthalignmentdialog.h"
#include <QApplication>
#include <QStatusBar>

void PaleoMainWindow::showTimeDepthAlignment(const QStringList &ids){
  if(!m_sectionWorkbench)return;
  QString error;const auto options=m_sectionWorkbench->timeDepthAlignmentOptions(ids,&error);
  if(!error.isEmpty()){statusBar()->showMessage(error,15000);return;}
  auto *parent=QApplication::activeModalWidget();
  TimeDepthAlignmentDialog dialog(options,parent?parent:this);
  connect(&dialog,&TimeDepthAlignmentDialog::saveRequested,this,[this,&dialog](const QVariantMap &selection){
    QString detail;if(!m_sectionWorkbench->saveTimeDepthAlignment(selection,&detail)){dialog.setMessage(detail);return;}
    dialog.accept();statusBar()->showMessage(tr("时深来源与地震时间基准已保存，正在刷新连井数据"),15000);
  });
  dialog.exec();
}
