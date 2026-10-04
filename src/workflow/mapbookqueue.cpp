// 层：功能
#include "mapbookqueue.h"

#include <QDate>
#include <QDir>
#include <QFile>
#include <QFileInfo>
#include <QJsonArray>
#include <QJsonDocument>
#include <QJsonObject>
#include <QObject>
#include <QPointer>
#include <QSemaphore>
#include <QThread>
#include <QTimer>
#include <QVariantMap>

#include "../catalog/datacatalog.h"
#include "../qgis/layoutexport.h"
#include "../qgis/mapbooklayout.h"
#include "../services/paleotaskservice.h"
#include "mapexport.h" // registerMapPdfAsset（既有产物登记管线，复用不自写）

#include <qgsprintlayout.h>
#include <qgsproject.h>
#include <qgsrectangle.h>

namespace PaleoMapBookQueue
{

namespace
{
  QString defaultTitlePattern() { return QStringLiteral( "%{book} · %{tile_label}" ); }
  QString defaultFooterPattern()
  {
    return QStringLiteral( "%{tile_label} · 中心 %{center_x}, %{center_y} · %{extent} · %{crs}" );
  }

  bool writeManifest( const Request &request, const Result &result,
                      const QVector<QString> &tileFiles, QString *pathOut, QString *error )
  {
    QJsonArray pages;
    for ( int i = 0; i < request.tiles.size(); ++i )
    {
      const PaleoMapBook::Tile &tile = request.tiles.at( i );
      QJsonObject entry;
      entry.insert( QStringLiteral( "index" ), tile.index );
      entry.insert( QStringLiteral( "name" ), tile.name );
      entry.insert( QStringLiteral( "row" ), tile.row );
      entry.insert( QStringLiteral( "col" ), tile.col );
      QJsonArray extent;
      extent << tile.extent.xMin << tile.extent.yMin << tile.extent.xMax << tile.extent.yMax;
      entry.insert( QStringLiteral( "extent" ), extent );
      const QString file = ( i < tileFiles.size() && !tileFiles.at( i ).isEmpty() )
                             ? QDir( bookRoot( request ) ).relativeFilePath( tileFiles.at( i ) )
                             : QString();
      entry.insert( QStringLiteral( "file" ), file );
      entry.insert( QStringLiteral( "status" ), file.isEmpty() ? QStringLiteral( "failed" )
                                                               : QStringLiteral( "ok" ) );
      pages << entry;
    }

    QJsonArray failures;
    for ( const Failure &f : result.failures )
    {
      QJsonObject entry;
      entry.insert( QStringLiteral( "index" ), f.index );
      entry.insert( QStringLiteral( "tile" ), f.tile );
      entry.insert( QStringLiteral( "stage" ), f.stage );
      entry.insert( QStringLiteral( "error" ), f.error );
      failures << entry;
    }

    QJsonObject root;
    root.insert( QStringLiteral( "book" ), request.book );
    root.insert( QStringLiteral( "generated" ), request.date.isEmpty()
                                                  ? QDate::currentDate().toString( Qt::ISODate )
                                                  : request.date );
    root.insert( QStringLiteral( "format" ), extensionFor( request.format ) );
    root.insert( QStringLiteral( "dpi" ), request.dpi );
    root.insert( QStringLiteral( "tiles" ), request.tiles.size() );
    root.insert( QStringLiteral( "succeeded" ), result.succeeded );
    root.insert( QStringLiteral( "failed" ), result.failed );
    root.insert( QStringLiteral( "cancelled" ), result.cancelled );
    root.insert( QStringLiteral( "pages" ), pages );
    root.insert( QStringLiteral( "failures" ), failures );
    root.insert( QStringLiteral( "indexPage" ), result.indexPage );

    const QString path = manifestPath( request );
    QFile file( path );
    if ( !file.open( QIODevice::WriteOnly | QIODevice::Truncate ) )
    {
      if ( error )
        *error = QObject::tr( "无法写 manifest：%1" ).arg( path );
      return false;
    }
    const QByteArray bytes = QJsonDocument( root ).toJson( QJsonDocument::Indented );
    if ( file.write( bytes ) != bytes.size() )
    {
      if ( error )
        *error = QObject::tr( "manifest 写入不完整：%1" ).arg( path );
      return false;
    }
    file.close();
    if ( pathOut )
      *pathOut = path;
    return true;
  }
} // namespace

QString extensionFor( Format format )
{
  return format == Format::Png ? QStringLiteral( "png" ) : QStringLiteral( "pdf" );
}

QString bookRoot( const Request &request )
{
  if ( request.outputDir.isEmpty() || request.book.isEmpty() )
    return QString();
  return QDir( request.outputDir ).filePath( request.book );
}

QString pagesDir( const Request &request )
{
  const QString root = bookRoot( request );
  return root.isEmpty() ? QString() : QDir( root ).filePath( QStringLiteral( "pages" ) );
}

QString pagePath( const Request &request, const PaleoMapBook::Tile &tile )
{
  const QString dir = pagesDir( request );
  if ( dir.isEmpty() || tile.name.isEmpty() )
    return QString();
  return QDir( dir ).filePath( tile.name + QLatin1Char( '.' ) + extensionFor( request.format ) );
}

QString indexPath( const Request &request )
{
  const QString root = bookRoot( request );
  if ( root.isEmpty() )
    return QString();
  return QDir( root ).filePath( QStringLiteral( "index/" ) + request.book +
                                QStringLiteral( "_index." ) + extensionFor( request.format ) );
}

QString manifestPath( const Request &request )
{
  const QString root = bookRoot( request );
  return root.isEmpty() ? QString() : QDir( root ).filePath( QStringLiteral( "manifest.json" ) );
}

QString Result::summary() const
{
  return QObject::tr( "共 %1 版：成功 %2 · 失败 %3 · 取消 %4" )
    .arg( total ).arg( succeeded ).arg( failed ).arg( cancelled );
}

struct Exporter::RunState
{
  Request request;
  Result result;
  QVector<QString> tileFiles;
  QString titlePattern;
  QString footerPattern;
  PaleoLayoutExport::Format qgisFormat = PaleoLayoutExport::Format::Png;
};

// 异步一册的共享账：state 只在所属线程读写；done/stopError 是与监视任务
// （worker）之间唯一的交接面（信号量提供 happens-before）。
struct Exporter::AsyncRun
{
  RunState state;
  QPointer<PaleoTask> task;
  int next = 0;
  bool prepared = false;
  QSemaphore done;              // 所属线程收尾时 release(1)
  QString monitorError;         // release 前写好，监视任务 acquire 后读
};

Exporter::Exporter( PaleoTaskService *tasks, QgsProject *project, QObject *parent )
  : QObject( parent ), m_tasks( tasks ), m_project( project )
{
}

Exporter::~Exporter()
{
  // 析构时还在跑：放开监视任务（它持有 shared_ptr，不会悬空）。
  if ( m_async )
  {
    m_async->monitorError = QObject::tr( "地图册导出器已销毁，导出中止" );
    m_async->done.release( 1 );
    m_async.reset();
  }
}

PaleoTask *Exporter::start( const Request &request )
{
  if ( !m_tasks || m_async )
    return nullptr;
  auto async = std::make_shared<AsyncRun>();
  async->state.request = request;

  // 监视任务：worker 上只等待所属线程的完成信号；取消后立即收尾（主线程会在
  // 下一版边界看到同一个取消标志并停下）——不碰 QGIS/catalog，不会死锁。
  PaleoTask *task = m_tasks->start(
    QObject::tr( "地图册批量导出：%1" ).arg( request.book ),
    [async]( PaleoTask *t ) -> QString {
      while ( !async->done.tryAcquire( 1, 50 ) )
      {
        if ( t->cancelRequested() )
          return QObject::tr( "已取消（已出的版保留，明细见 manifest）" );
      }
      return async->monitorError;
    },
    QString(), false );
  async->task = task;
  m_async = async;

  // prepare 失败也走异步收尾，保证 finished 在 start() 返回后才发出。
  async->prepared = prepareRun( async->state, task );
  QTimer::singleShot( 0, this, &Exporter::asyncStep );
  return task;
}

void Exporter::asyncStep()
{
  const std::shared_ptr<AsyncRun> async = m_async;
  if ( !async )
    return;
  RunState &state = async->state;
  PaleoTask *task = async->task.data();
  const bool prepared = async->prepared;
  const int total = state.request.tiles.size();
  if ( prepared && async->next < total )
  {
    // 任务行被清走（QPointer 置空）视同取消。
    bool keepGoing = true;
    if ( !async->task )
    {
      state.result.cancelled += total - async->next;
      keepGoing = false;
    }
    else
    {
      keepGoing = exportTileAt( state, async->next, task );
    }
    ++async->next;
    if ( keepGoing && async->next < total )
    {
      QTimer::singleShot( 0, this, &Exporter::asyncStep ); // 让出事件循环：界面不冻结
      return;
    }
  }

  if ( prepared )
    finishRun( state, task );
  const Result result = state.result;
  m_last = result;
  m_async.reset();
  if ( task )
    task->reportDetail( result.summary() );
  // 任务终态口径：一版都没出才算任务失败；部分失败不算（失败明细在
  // Result/manifest 里留账，不谎报也不拖死整队）。
  if ( !prepared )
    async->monitorError = result.failures.isEmpty() ? QObject::tr( "地图册导出准备失败" )
                                                    : result.failures.constFirst().error;
  else if ( result.succeeded == 0 && result.total > 0 && result.cancelled < result.total )
    async->monitorError = result.failures.isEmpty()
                            ? QObject::tr( "全部 %1 版导出失败（明细见 manifest）" ).arg( result.total )
                            : QObject::tr( "全部 %1 版导出失败：%2" )
                                .arg( result.total )
                                .arg( result.failures.constFirst().error );
  async->done.release( 1 );
  emit finished( result );
}

Result Exporter::run( PaleoTask *task, const Request &request )
{
  RunState state;
  state.request = request;
  if ( !prepareRun( state, task ) )
    return state.result;
  for ( int i = 0; i < request.tiles.size(); ++i )
    if ( !exportTileAt( state, i, task ) )
      break;
  finishRun( state, task );
  return state.result;
}

bool Exporter::prepareRun( RunState &state, PaleoTask *task )
{
  const Request &request = state.request;
  Result &result = state.result;
  result = Result();
  result.total = request.tiles.size();
  state.tileFiles = QVector<QString>( request.tiles.size(), QString() );

  const auto bail = [&result, task]( const QString &stage, const QString &message ) {
    result.failed = result.total;
    result.failures.append( Failure{ -1, QStringLiteral( "*" ), stage, message } );
    if ( task )
      task->reportDetail( message );
    return false;
  };

  // #148：版面/渲染用主线程 QgsProject，catalog 只收所属线程写入——跨线程直接拒绝。
  if ( QThread::currentThread() != thread() )
    return bail( QStringLiteral( "prepare" ),
                 QObject::tr( "地图册导出必须在导出器所属线程（GUI 线程）执行" ) );
  if ( !m_project )
    return bail( QStringLiteral( "prepare" ), QObject::tr( "未绑定 QgsProject（版面需要工程上下文）" ) );
  if ( request.book.isEmpty() || !DataCatalog::isSafePathSegment( request.book ) )
    return bail( QStringLiteral( "prepare" ),
                 QObject::tr( "图册名不是合法路径段：%1" ).arg( request.book ) );
  if ( request.outputDir.isEmpty() )
    return bail( QStringLiteral( "prepare" ), QObject::tr( "未指定输出目录" ) );
  if ( request.tiles.isEmpty() )
    return bail( QStringLiteral( "prepare" ), QObject::tr( "格序列为空（先生成 AOI 网格）" ) );

  // 模板变量预检：引用了变量表以外的名字就整批拒绝，避免跑到第 N 版才发现
  // 标题印不出来（workflow/mapbook 的严格替换语义，这里只提前一步）。
  const QStringList known = PaleoMapBook::variableNames();
  state.titlePattern = request.titlePattern.isEmpty() ? defaultTitlePattern()
                                                      : request.titlePattern;
  state.footerPattern = request.footerPattern.isEmpty() ? defaultFooterPattern()
                                                        : request.footerPattern;
  for ( const QString &pattern : { state.titlePattern, state.footerPattern } )
  {
    QString refsError;
    const QStringList refs = PaleoMapBook::referencedVariables( pattern, &refsError );
    if ( !refsError.isEmpty() )
      return bail( QStringLiteral( "prepare" ), refsError );
    for ( const QString &name : refs )
      if ( !known.contains( name ) )
        return bail( QStringLiteral( "prepare" ),
                     QObject::tr( "模板变量 %1 未定义（可用变量：%2）" )
                       .arg( name, known.join( QStringLiteral( ", " ) ) ) );
  }

  const QDir rootDir( bookRoot( request ) );
  if ( !rootDir.exists() && !rootDir.mkpath( QStringLiteral( "." ) ) )
    return bail( QStringLiteral( "prepare" ),
                 QObject::tr( "无法创建图册目录：%1" ).arg( rootDir.absolutePath() ) );
  if ( !QDir().mkpath( pagesDir( request ) ) )
    return bail( QStringLiteral( "prepare" ),
                 QObject::tr( "无法创建版面目录：%1" ).arg( pagesDir( request ) ) );
  if ( request.withIndexPage && !QDir().mkpath( QFileInfo( indexPath( request ) ).absolutePath() ) )
    return bail( QStringLiteral( "prepare" ),
                 QObject::tr( "无法创建索引目录：%1" ).arg( indexPath( request ) ) );

  state.qgisFormat = request.format == Format::Png ? PaleoLayoutExport::Format::Png
                                                   : PaleoLayoutExport::Format::Pdf;
  return true;
}

bool Exporter::exportTileAt( RunState &state, int i, PaleoTask *task )
{
  const Request &request = state.request;
  Result &result = state.result;

  // 协作式取消 + 预演上限：两者都只停未出的版，已出的保留。
  if ( task && task->cancelRequested() )
  {
    result.cancelled += request.tiles.size() - i;
    return false;
  }
  if ( request.previewLimit >= 0 && i >= request.previewLimit )
  {
    result.cancelled += request.tiles.size() - i;
    return false;
  }

  const PaleoMapBook::Tile &tile = request.tiles.at( i );
  const QVariantMap vars = PaleoMapBook::tileVariables(
    PaleoMapBook::TileContext{ tile, request.book, request.horizon, request.crs, request.date } );

  QString templateError;
  const QString title = PaleoMapBook::applyVariables( state.titlePattern, vars, &templateError );
  QString footer;
  if ( templateError.isEmpty() )
    footer = PaleoMapBook::applyVariables( state.footerPattern, vars, &templateError );
  if ( !templateError.isEmpty() )
  {
    // 严格替换失败（引用了未定义变量）：留账后继续下一版。
    result.failed += 1;
    result.failures.append(
      Failure{ tile.index, tile.name, QStringLiteral( "template" ), templateError } );
    return true;
  }

  PaleoMapBookLayout::TileSpec spec;
  spec.title = title;
  spec.footer = footer;
  spec.extent = QgsRectangle( tile.extent.xMin, tile.extent.yMin,
                              tile.extent.xMax, tile.extent.yMax );
  spec.layers = request.layers;
  spec.landscape = request.landscape;

  QString buildError;
  QgsPrintLayout *layout = PaleoMapBookLayout::buildTileLayout( m_project, spec, &buildError );
  if ( !layout )
  {
    result.failed += 1;
    result.failures.append( Failure{ tile.index, tile.name, QStringLiteral( "layout" ),
                                     buildError.isEmpty() ? QObject::tr( "版面构建失败" )
                                                          : buildError } );
    return true;
  }

  const auto rendered = PaleoMapBookLayout::renderLayout( layout, pagePath( request, tile ),
                                                           state.qgisFormat, request.dpi );
  delete layout;
  if ( !rendered.ok )
  {
    result.failed += 1;
    result.failures.append( Failure{ tile.index, tile.name, QStringLiteral( "export" ),
                                     rendered.error } );
    return true;
  }

  // 产物登记（可选）：受管副本进 catalog，登记失败同版失败留账。
  if ( request.catalog && !request.projectDir.isEmpty() )
  {
    QString sha, managed, registerError;
    // 格式必须如实入库（registerMapPdfAsset 缺省会把 format 印成 pdf）。
    const QString assetId = registerMapPdfAsset( request.catalog, request.projectDir,
                                                 rendered.path, &sha, &managed, &registerError,
                                                 extensionFor( request.format ) );
    if ( assetId.isEmpty() )
    {
      result.failed += 1;
      result.failures.append( Failure{ tile.index, tile.name, QStringLiteral( "register" ),
                                       registerError.isEmpty() ? QObject::tr( "产物登记失败" )
                                                               : registerError } );
      return true;
    }
    result.registered << managed;
    result.registeredIds << assetId;
  }

  state.tileFiles[i] = rendered.path;
  result.files << rendered.path;
  result.succeeded += 1;

  if ( task )
    task->reportBytes( result.succeeded + result.failed, request.tiles.size() );
  emit progress( result.succeeded + result.failed, request.tiles.size(), tile.name );
  return true;
}

void Exporter::finishRun( RunState &state, PaleoTask *task )
{
  const Request &request = state.request;
  Result &result = state.result;

  // 报告装配最小面：目录索引页（一页列全册条目），失败只记账不回退已成版面。
  if ( request.withIndexPage && result.succeeded > 0 && !( task && task->cancelRequested() ) )
  {
    QStringList entries;
    for ( int i = 0; i < request.tiles.size(); ++i )
    {
      const PaleoMapBook::Tile &tile = request.tiles.at( i );
      const QString file = state.tileFiles.at( i );
      if ( file.isEmpty() )
        continue;
      entries << QStringLiteral( "%1  %2  %3" )
                   .arg( tile.name, tile.extent.extentText(), QFileInfo( file ).fileName() );
    }
    PaleoMapBookLayout::IndexSpec spec;
    spec.title = QObject::tr( "%1 目录（%2 版）" ).arg( request.book ).arg( entries.size() );
    spec.entries = entries;
    QString indexError;
    QgsPrintLayout *indexLayout = PaleoMapBookLayout::buildIndexLayout( m_project, spec, &indexError );
    if ( indexLayout )
    {
      const auto rendered = PaleoMapBookLayout::renderLayout( indexLayout, indexPath( request ),
                                                              state.qgisFormat, request.dpi );
      delete indexLayout;
      if ( rendered.ok )
        result.indexPage = rendered.path;
      else
        result.failures.append( Failure{ -1, QStringLiteral( "*" ), QStringLiteral( "index" ),
                                         rendered.error } );
    }
    else
    {
      result.failures.append( Failure{ -1, QStringLiteral( "*" ), QStringLiteral( "index" ),
                                       indexError } );
    }
  }

  QString manifestError;
  if ( !writeManifest( request, result, state.tileFiles, &result.manifest, &manifestError ) )
    result.failures.append( Failure{ -1, QStringLiteral( "*" ), QStringLiteral( "manifest" ),
                                     manifestError } );
}

} // namespace PaleoMapBookQueue
