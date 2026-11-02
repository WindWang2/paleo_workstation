// 层：功能
#include "mappingworkbench.h"
#include "services/seismicunitinput.h"
#include "services/futuretask.h"
#include "services/paleotaskservice.h"
#include <QFutureWatcher>
#include <QtConcurrent>

bool MappingWorkbench::startUnitSeismicPrediction(const RemotePredictionRequest &request,const QString &source){
  m_request=request;m_predictionParameters={{"unitSelection",request.unitSelection},{"input_attribute","layer_window_rms"}};
  beginPredictionTask(tr("地震相预测 · %1").arg(paleo::strat::fromMap(request.unitSelection).displayName));
  emit predictionBusyChanged(true);emit predictionDetailChanged(tr("按当前地震层段读取逐道窗口"));
  auto cancelled=std::make_shared<std::atomic_bool>(false);m_prepareCancelled=cancelled;
  auto *watcher=new QFutureWatcher<paleo::strat::SeismicUnitInput>(this);
  connect(watcher,&QFutureWatcherBase::finished,this,[this,watcher,request,cancelled]{
    const auto input=watcher->result();watcher->deleteLater();
    if(m_request.id!=request.id||cancelled->load())return;
    m_prepareCancelled.reset();
    const auto failure=input.error.isEmpty()&&!m_remote?tr("预测服务已断开"):input.error;
    if(!failure.isEmpty()){
      m_request={};endPredictionTask(failure);emit predictionBusyChanged(false);emit errorOccurred(failure);return;
    }
    m_request.samples=input.samples;m_request.extent=input.extent;m_request.rows=input.rows;m_request.columns=input.columns;
    m_request.crsWkt=input.crs;m_predictionParameters.insert("valid_cells",input.validCells);
    m_predictionParameters.insert("seismic_source",input.sourcePath);
    emit predictionDetailChanged(tr("层段输入已准备，提交预测服务"));m_remote->start(m_request);
  });
  const auto unit=paleo::strat::fromMap(request.unitSelection);const auto project=m_dir;
  watcher->setFuture(QtConcurrent::run(paleo::tasks::pool(m_taskSvc),[project,unit,source,cancelled]{
    return paleo::strat::prepareSeismicUnitInput(project,unit,source,[cancelled]{return cancelled->load();});
  }));
  return true;
}
