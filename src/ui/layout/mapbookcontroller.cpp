// 层：视图
#include "mapbookcontroller.h"

#include <QDate>

#include "mapbookpanel.h"
#include "../../services/paleotaskservice.h"

PaleoMapBookController::PaleoMapBookController( PaleoMapBookPanel *panel, PaleoTaskService *tasks,
                                                QObject *parent )
  : QObject( parent ), m_panel( panel ), m_tasks( tasks )
{
  if ( panel )
  {
    connect( panel, &PaleoMapBookPanel::batchRequested, this, [this] { startBatch(); } );
    connect( panel, &PaleoMapBookPanel::cancelRequested, this, &PaleoMapBookController::cancelBatch );
  }
}

void PaleoMapBookController::setProjectProvider( const std::function<QgsProject *()> &provider )
{
  m_projectProvider = provider;
}

void PaleoMapBookController::setLayersProvider( const std::function<QList<QgsMapLayer *>()> &provider )
{
  m_layersProvider = provider;
}

void PaleoMapBookController::setCatalog( DataCatalog *catalog ) { m_catalog = catalog; }

void PaleoMapBookController::setProjectDirProvider( const std::function<QString()> &provider )
{
  m_projectDirProvider = provider;
}

void PaleoMapBookController::setCrsTextProvider( const std::function<QString()> &provider )
{
  m_crsProvider = provider;
}

void PaleoMapBookController::setHorizonProvider( const std::function<QString()> &provider )
{
  m_horizonProvider = provider;
}

bool PaleoMapBookController::startBatch()
{
  auto log = [this]( const QString &line ) {
    if ( m_panel )
      m_panel->appendLog( line );
    emit statusMessage( line );
  };
  if ( !m_panel )
    return false;
  if ( busy() )
  {
    log( tr( "已有一册地图册在导出，请等待完成或先取消" ) );
    return false;
  }
  if ( !m_tasks )
  {
    log( tr( "任务服务未接入——无法批量导出地图册" ) );
    return false;
  }
  QgsProject *project = m_projectProvider ? m_projectProvider() : nullptr;
  const QString projectDir = m_projectDirProvider ? m_projectDirProvider() : QString();
  if ( !project || projectDir.isEmpty() )
  {
    log( tr( "无打开工程——无法批量导出地图册" ) );
    return false;
  }

  QString gridError;
  PaleoMapBookQueue::Request request;
  request.tiles = m_panel->previewTiles( &gridError );
  if ( request.tiles.isEmpty() )
  {
    log( tr( "参数不可用：%1" ).arg( gridError.isEmpty() ? tr( "格序列为空" ) : gridError ) );
    return false;
  }
  request.layers = m_layersProvider ? m_layersProvider() : QList<QgsMapLayer *>();
  if ( request.layers.isEmpty() )
  {
    log( tr( "画布没有可出图的图层——先在画布上显示要出图的图层" ) );
    return false;
  }
  request.book = m_panel->bookName();
  request.outputDir = m_panel->outputDir();
  request.titlePattern = m_panel->titlePattern();
  request.footerPattern = m_panel->footerPattern();
  request.dpi = m_panel->dpi();
  request.format = m_panel->format();
  request.withIndexPage = m_panel->withIndexPage();
  request.previewLimit = m_panel->previewLimit();
  request.catalog = m_catalog;
  request.projectDir = m_catalog ? projectDir : QString();
  request.horizon = m_horizonProvider ? m_horizonProvider() : QString();
  request.crs = m_crsProvider ? m_crsProvider() : QString();
  request.date = QDate::currentDate().toString( Qt::ISODate );

  // 每册一个 Exporter（Exporter 一次只跑一册）；工程指针在此刻现取，
  // 不跨工程复用。
  auto *exporter = new PaleoMapBookQueue::Exporter( m_tasks, project, this );
  const quint64 generation = m_generation;
  connect( exporter, &PaleoMapBookQueue::Exporter::progress, this,
           [this, generation]( int done, int total, const QString & ) {
             if ( generation == m_generation && m_panel )
               m_panel->setProgress( done, total );
           } );
  connect( exporter, &PaleoMapBookQueue::Exporter::finished, this,
           [this, generation, exporter]( const PaleoMapBookQueue::Result &result ) {
             onExporterFinished( generation, exporter, result );
           } );
  PaleoTask *task = exporter->start( request );
  if ( !task )
  {
    delete exporter;
    log( tr( "地图册导出未能启动" ) );
    return false;
  }
  m_exporter = exporter;
  m_task = task;
  m_panel->setBusy( true ); // 会清日志：起跑行写在其后
  m_panel->setProgress( 0, request.tiles.size() );
  m_panel->appendLog( tr( "开始导出地图册「%1」：%2 版" ).arg( request.book ).arg( request.tiles.size() ) );
  return true;
}

void PaleoMapBookController::cancelBatch()
{
  if ( m_task )
    m_task->requestCancel();
  if ( m_panel && busy() )
    m_panel->appendLog( tr( "已请求取消：当前版结束后停止，已出的版保留" ) );
}

void PaleoMapBookController::resetProject()
{
  ++m_generation; // 迟到的 progress/finished 一律作废
  if ( m_task )
    m_task->requestCancel();
  m_task.clear();
  // 在途的 Exporter 保留到它自己 finished（下一版边界看到取消即收尾，不再
  // 碰旧工程图层），回包里 deleteLater；这里只断开与面板的关系。
  m_exporter.clear();
  if ( m_panel )
  {
    m_panel->setBusy( false );
    m_panel->clearLog();
  }
}

void PaleoMapBookController::onExporterFinished( quint64 generation,
                                                 PaleoMapBookQueue::Exporter *exporter,
                                                 const PaleoMapBookQueue::Result &result )
{
  if ( exporter )
    exporter->deleteLater(); // 不在其 emit 栈内直接 delete
  if ( generation != m_generation )
    return; // 旧工程的一册：结果作废
  m_exporter.clear();
  m_task.clear();
  if ( !m_panel )
    return;
  m_panel->setBusy( false );
  m_panel->appendLog( result.summary() );
  for ( const PaleoMapBookQueue::Failure &failure : result.failures )
    m_panel->appendLog( tr( "失败 [%1/%2] %3" ).arg( failure.tile, failure.stage, failure.error ) );
  if ( !result.manifest.isEmpty() )
    m_panel->appendLog( tr( "清单：%1" ).arg( result.manifest ) );
  if ( !result.indexPage.isEmpty() )
    m_panel->appendLog( tr( "目录页：%1" ).arg( result.indexPage ) );
  emit statusMessage( tr( "地图册导出结束：%1" ).arg( result.summary() ) );
  emit batchFinished( result );
}
