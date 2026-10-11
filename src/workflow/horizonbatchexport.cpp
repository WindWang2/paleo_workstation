// 层：功能
#include "horizonbatchexport.h"

#include "mapexport.h" // registerMapPdfAsset：图件 → catalog OUTPUT 登记

#include "../catalog/datacatalog.h"
#include "../metadata/layermanifest.h"
#include "../qgis/qgislayerservice.h"

#include <QDate>
#include <QDir>
#include <QFile>
#include <QFileInfo>
#include <QTemporaryDir>
#include <QCoreApplication>
#include <QPointer>
#include <QTimer>
#include <memory>
#include "../services/paleotaskservice.h"

#include <qgslayout.h>
#include <qgslayoutitem.h>
#include <qgslayoutitemlabel.h>
#include <qgslayoutitemmap.h>
#include <qgslayoutundostack.h>
#include <qgsmaplayer.h>
#include <qgsprintlayout.h>
#include <qgsrectangle.h>
#include <qgscoordinatetransform.h>
#include <qgsproject.h>

namespace
{
  // 文件名段消毒：路径非法字符与空白替换为 '_'，空段回 "untitled"。
  QString safeSegment( const QString &raw )
  {
    QString s = raw;
    for ( const QChar ch : { QLatin1Char( '/' ), QLatin1Char( '\\' ), QLatin1Char( ':' ),
                             QLatin1Char( '*' ), QLatin1Char( '?' ), QLatin1Char( '"' ),
                             QLatin1Char( '<' ), QLatin1Char( '>' ), QLatin1Char( '|' ),
                             QLatin1Char( ' ' ), QLatin1Char( '\t' ) } )
      s.replace( ch, QLatin1Char( '_' ) );
    s.remove( QLatin1Char( '\n' ) ).remove( QLatin1Char( '\r' ) ).simplified();
    return s.isEmpty() ? QStringLiteral( "untitled" ) : s;
  }

  QString extensionFor( PaleoHorizonBatchExport::Format format )
  {
    switch ( format )
    {
      case PaleoHorizonBatchExport::Format::Png: return QStringLiteral( "png" );
      case PaleoHorizonBatchExport::Format::Pdf: return QStringLiteral( "pdf" );
      case PaleoHorizonBatchExport::Format::Svg: return QStringLiteral( "svg" );
    }
    return QStringLiteral( "png" );
  }

  QgsRectangle combinedExtent( const QList<QgsMapLayer *> &layers, QgsLayoutItemMap *map )
  {
    QgsRectangle extent;
    for ( QgsMapLayer *layer : layers )
    {
      if ( !layer )
        continue;
      if (layer->customProperty("paleoBasemap").toBool()) continue;
      const auto bounds = QgsCoordinateTransform(layer->crs(), map->crs(), map->layout()->project()).transformBoundingBox(layer->extent());
      if ( extent.isNull() )
        extent = bounds;
      else
        extent.combineExtentWith( bounds );
    }
    return extent;
  }
} // namespace

namespace PaleoHorizonBatchExport
{

QMap<QString, QList<QgsMapLayer *>> resolveHorizonLayers( QgisLayerService *layers,
                                                          QStringList *horizonsOut )
{
  QMap<QString, QList<QgsMapLayer *>> out;
  if ( horizonsOut )
    horizonsOut->clear();
  if ( !layers )
    return out;

  QVector<LayerDeclaration> declared;
  QString err;
  if ( !layers->tryDeclared( &declared, &err ) )
    return out;

  for ( const LayerDeclaration &d : declared )
  {
    if ( d.horizon.isEmpty() )
      continue; // 与层位无关的图层不进批量出图
    QgsMapLayer *layer = layers->instantiate( d.layerId );
    if ( !layer )
      continue; // 实例化失败如实少一层；整层位空集由 run 入账
    out[d.horizon].append( layer );
    if ( horizonsOut && !horizonsOut->contains( d.horizon ) )
      horizonsOut->append( d.horizon );
  }
  return out;
}

QString Result::summary() const
{
  return QObject::tr( "批量出图：%1 层位，成功 %2、失败 %3" ).arg( total ).arg( succeeded ).arg( failed );
}

// ---- 逐层位步进器：同步 run() 与后台 startAsync() 共用一套语义 ---------
struct Batch
{
  Request request;
  QPointer<QgsPrintLayout> layout;
  QPointer<DataCatalog> catalog;
  Result result;
  QStringList horizons;
  // 图层以 QPointer 守护：后台出图期间图层被移除 → 该层位少一层而非悬垂。
  QMap<QString, QList<QPointer<QgsMapLayer>>> guarded;
  int next = 0;
  struct MapState
  {
    QPointer<QgsLayoutItemMap> map;
    QList<QPointer<QgsMapLayer>> layers;
    QgsRectangle extent;
    bool followPreset = false;
  };
  QVector<MapState> states;
  QPointer<QgsLayoutItemLabel> title;
  QString titleBackup;
  QString project, date, ext;
  std::unique_ptr<QTemporaryDir> scratch;
  bool begun = false;

  void fail( const QString &horizon, const QString &why )
  {
    HorizonOutcome outcome;
    outcome.horizon = horizon;
    outcome.error = why;
    result.horizons.append( outcome );
    ++result.failed;
  }
  void failRemaining( const QString &why )
  {
    for ( ; next < horizons.size(); ++next )
      fail( horizons.at( next ), why );
  }
  bool done() const { return next >= horizons.size(); }

  // 前置校验 + 版面快照；false = 整体失败（已入账）。
  bool begin()
  {
    result.total = request.horizonLayers.size();
    horizons = request.horizonLayers.keys();
    layout = request.layout;
    catalog = request.catalog;
    for ( auto it = request.horizonLayers.cbegin(); it != request.horizonLayers.cend(); ++it )
      for ( QgsMapLayer *layer : it.value() )
        guarded[it.key()].append( layer );
    if ( !request.layout )
    {
      failRemaining( QObject::tr( "未提供版面" ) );
      return false;
    }
    if ( !request.catalog || request.projectDir.isEmpty() )
    {
      failRemaining( QObject::tr( "未绑定 catalog 受管区（图件纪律：不产游离文件）" ) );
      return false;
    }
    if ( horizons.isEmpty() )
      return true; // total=0：没有层位组，不是错误
    // 批处理是版面骨架的原子操作：不进 undo 历史。
    if ( layout->undoStack() )
      layout->undoStack()->blockCommands( true );
    QList<QgsLayoutItemMap *> maps;
    layout->layoutItems( maps );
    for ( QgsLayoutItemMap *map : maps )
    {
      QList<QPointer<QgsMapLayer>> original;
      for ( QgsMapLayer *layer : map->layers() )
        original.append( layer );
      states.append( { map, original, map->extent(), map->followVisibilityPreset() } );
    }
    title = qobject_cast<QgsLayoutItemLabel *>( layout->itemById( QStringLiteral( "title" ) ) );
    titleBackup = title ? title->text() : QString();
    project = safeSegment( request.projectName.isEmpty() ? QStringLiteral( "project" ) : request.projectName );
    date = request.date.isEmpty() ? QDate::currentDate().toString( QStringLiteral( "yyyyMMdd" ) )
                                  : request.date;
    ext = extensionFor( request.format );
    scratch = std::make_unique<QTemporaryDir>();
    begun = true;
    return true;
  }

  // 导出并登记一个层位（next 前进一格）。
  void step()
  {
    const QString horizon = horizons.at( next++ );
    if ( !layout || !catalog )
    {
      fail( horizon, QObject::tr( "版面或工程已关闭" ) );
      return;
    }
    HorizonOutcome outcome;
    outcome.horizon = horizon;
    QList<QgsMapLayer *> layers;
    for ( const QPointer<QgsMapLayer> &layer : guarded.value( horizon ) )
      if ( layer )
        layers.append( layer.data() );
    if ( layers.isEmpty() )
    {
      fail( horizon, QObject::tr( "层位 %1 没有可出图的图层" ).arg( horizon ) );
      return;
    }
    if ( states.isEmpty() )
    {
      fail( horizon, QObject::tr( "版面中没有地图项" ) );
      return;
    }
    // ---- 换内容：层集快照 + 全幅范围 + 标题 -------------------------------
    for ( const MapState &state : std::as_const( states ) )
    {
      QgsLayoutItemMap *map = state.map;
      if ( !map )
        continue;
      map->setFollowVisibilityPreset( false ); // 批量出图即快照语义
      map->setLayers( layers );
      try
      {
        const auto extent = combinedExtent( layers, map );
        if ( !extent.isNull() && !extent.isEmpty() )
          map->setExtent( extent );
      }
      catch ( const QgsCsException &ex )
      {
        fail( horizon, ex.what() );
        return;
      }
    }
    if ( title )
      title->setText( QObject::tr( "%1 · %2" ).arg( request.projectName.isEmpty()
                                                       ? QStringLiteral( "project" )
                                                       : request.projectName,
                                                   horizon ) );
    // ---- 导出到临时文件（文件名即最终受管文件名）--------------------------
    const QString baseName = QStringLiteral( "%1_%2_%3" ).arg( project, safeSegment( horizon ), date );
    const QString tmpPath = scratch && scratch->isValid()
                                ? scratch->filePath( baseName + QLatin1Char( '.' ) + ext )
                                : QDir::temp().filePath( baseName + QLatin1Char( '.' ) + ext );
    const auto exported = PaleoLayoutExport::exportLayout( layout, tmpPath, request.format,
                                                           request.dpi, PaleoLayoutExport::PageRange() );
    if ( !exported.ok )
    {
      fail( horizon, exported.error );
      return;
    }
    // ---- 登记 catalog（OUTPUT；受管副本即成品，不留游离文件）----------------
    QString sha, managed, err;
    const QString assetId = registerMapPdfAsset( catalog, request.projectDir, exported.files.value( 0 ),
                                                 &sha, &managed, &err, ext );
    if ( assetId.isEmpty() )
    {
      fail( horizon, err.isEmpty() ? QObject::tr( "catalog 登记失败：%1" ).arg( baseName ) : err );
      return;
    }
    outcome.ok = true;
    outcome.file = managed;
    outcome.assetId = assetId;
    outcome.sha256 = sha;
    result.horizons.append( outcome );
    ++result.succeeded;
  }

  // ---- 版面复原（幂等）-----------------------------------------------------
  void finish()
  {
    if ( !begun )
      return;
    begun = false;
    for ( const MapState &state : std::as_const( states ) )
    {
      if ( !state.map )
        continue;
      state.map->setFollowVisibilityPreset( state.followPreset );
      QList<QgsMapLayer *> original;
      for ( const QPointer<QgsMapLayer> &layer : state.layers )
        if ( layer )
          original.append( layer.data() );
      state.map->setLayers( original );
      state.map->setExtent( state.extent );
    }
    if ( title )
      title->setText( titleBackup );
    if ( layout && layout->undoStack() )
      layout->undoStack()->blockCommands( false );
  }
};

Result run( const Request &request )
{
  Batch batch;
  batch.request = request;
  if ( !batch.begin() )
    return batch.result;
  while ( !batch.done() )
    batch.step();
  batch.finish();
  return batch.result;
}

PaleoTask *startAsync( const Request &request, PaleoTaskService *tasks,
                       std::function<void( const Result &, bool cancelled )> done, QString *error )
{
  if ( !tasks )
  {
    if ( error )
      *error = QObject::tr( "任务中心不可用" );
    return nullptr;
  }
  auto batch = std::make_shared<Batch>();
  batch->request = request;
  const QString titleText = QObject::tr( "按层位批量出图 · %1 个层位" ).arg( request.horizonLayers.size() );
  QPointer<PaleoTask> task = tasks->beginExternal( titleText );
  if ( !task )
  {
    if ( error )
      *error = QObject::tr( "无法创建任务" );
    return nullptr;
  }
  const quint64 session = task->session();
  QPointer<PaleoTaskService> service( tasks );
  if ( !batch->begin() )
  {
    task->finishExternal( batch->result.horizons.isEmpty() ? QObject::tr( "未提供版面或工程" )
                                                           : batch->result.horizons.value( 0 ).error );
    if ( done )
      done( batch->result, false );
    return task;
  }
  task->reportBytes( 0, std::max<qint64>( 1, batch->horizons.size() ) );
  // UIS-03：版面渲染绑定 GUI 线程（QGraphicsScene 线程亲和），不能整体搬到
  // worker；改为每个事件循环回合只出一个层位——界面在层位之间保持响应，
  // 任务中心可见进度并可取消；会话切换（工程关闭/切换）即中止并复原版面。
  auto tick = std::make_shared<std::function<void()>>();
  *tick = [batch, task, service, session, done, tick]() {
    const bool stale = !service || service->session() != session;
    const bool cancelled = !task || task->cancelRequested() || stale;
    if ( cancelled || batch->done() )
    {
      if ( cancelled )
        batch->failRemaining( stale ? QObject::tr( "工程已切换，批量出图已中止" )
                                    : QObject::tr( "已取消" ) );
      batch->finish();
      if ( task )
        task->finishExternal( batch->result.failed > 0 && !cancelled
                                  ? batch->result.summary()
                                  : QString() );
      if ( done )
        done( batch->result, cancelled );
      *tick = nullptr; // 断开自引用
      return;
    }
    if ( task )
      task->reportDetail( QObject::tr( "导出层位 %1" ).arg( batch->horizons.at( batch->next ) ) );
    batch->step();
    if ( task )
      task->reportBytes( batch->next, std::max<qint64>( 1, batch->horizons.size() ) );
    QTimer::singleShot( 0, service ? static_cast<QObject *>( service.data() ) : qApp, *tick );
  };
  QTimer::singleShot( 0, tasks, *tick );
  return task;
}

} // namespace PaleoHorizonBatchExport
