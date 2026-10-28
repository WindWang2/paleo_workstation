// 层：功能
#include "mapbookqueue.h"

#include "workflowerrors_internal.h" // reportSnapshotWarnings：缺层警告统一入账

#include <QDate>
#include <QDir>
#include <QFile>
#include <QFileInfo>
#include <QJsonArray>
#include <QJsonDocument>
#include <QJsonObject>
#include <QObject>
#include <QPointer>
#include <QTemporaryDir>
#include <QThread>
#include <QTimer>
#include <QVariantMap>

#include <algorithm>

#include "../catalog/datacatalog.h"
#include "../io/atomicwrite.h"
#include "../qgis/layoutexport.h"
#include "../qgis/mapbooklayout.h"
#include "../services/paleotaskservice.h"
#include "mapexport.h" // registerMapPdfAsset（既有产物登记管线，复用不自写）

#include <qgsmaplayer.h>
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
    // 原子整写（QSaveFile + commit）：旧实现 QFile+Truncate 原地覆写，写中途
    // 崩溃/磁盘满会留下截断或空的 manifest，而上一份完整 manifest 已被抹掉。
    const QByteArray bytes = QJsonDocument( root ).toJson( QJsonDocument::Indented );
    QString writeErr;
    if ( !paleo::writeFileAtomic( path, bytes, &writeErr ) )
    {
      if ( error )
        *error = QObject::tr( "无法写 manifest：%1（%2）" ).arg( path, writeErr );
      return false;
    }
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
  // 装配阶段失败（prepare/index/manifest：index<0 的整体项）单独点名——
  // 它们不占逐版计数，并入「失败」会虚增版级失败数。
  int assembly = 0;
  for ( const Failure &f : failures )
    if ( f.index < 0 )
      ++assembly;
  QString line = QObject::tr( "共 %1 版：成功 %2 · 失败 %3 · 取消 %4" )
    .arg( total ).arg( succeeded ).arg( failed ).arg( cancelled );
  if ( assembly > 0 )
    line += QObject::tr( " · 装配失败 %1" ).arg( assembly );
  return line;
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

// Worker owns its private project, catalogue and output files. The owner only
// applies the finished catalogue journal after checking the project generation.
struct Exporter::AsyncRun {
  Request request;
  RunState state;
  PaleoLayoutExport::ProjectSnapshot snapshot;
  QStringList layerIds;
  std::unique_ptr<QTemporaryDir> staging;
  std::unique_ptr<DataCatalog> catalog;
  QVector<CatalogOp> journal;
  QPointer<DataCatalog> ownerCatalog;
  QPointer<PaleoTask> task;
  QString error;
};

Exporter::Exporter(PaleoTaskService *tasks, QgsProject *project,
                   QObject *parent)
    : QObject(parent), m_tasks(tasks), m_project(project) {}
Exporter::~Exporter() {
  if (m_async && m_async->task)
    m_async->task->requestCancel();
}

PaleoTask *Exporter::start(const Request &request) {
  if (!m_tasks || m_async)
    return nullptr;
  auto job = std::make_shared<AsyncRun>();
  job->request = request;
  for (auto *layer : request.layers)
    if (layer)
      job->layerIds.append(layer->id());
  job->request.layers.clear();
  job->request.catalog = nullptr;
  if (!PaleoLayoutExport::captureProjectSnapshot(m_project, job->snapshot,
                                                 &job->error)) {
    // Isolation failure is an explicit failed job, never a synchronous
    // fallback.
  } else if (request.catalog && !request.projectDir.isEmpty()) {
    job->staging = std::make_unique<QTemporaryDir>(request.projectDir +
                                                   "/.mapbook-XXXXXX");
    if (!job->staging->isValid())
      job->error = tr("无法创建地图册暂存目录");
    else {
      job->catalog = request.catalog->createStagingCopy(job->staging->path());
      job->ownerCatalog = request.catalog;
    }
  }
  auto *task = m_tasks->start(
      tr("地图册批量导出：%1").arg(request.book),
      [job](PaleoTask *task) -> QString {
        if (!job->error.isEmpty()) {
          job->state.result.total = job->request.tiles.size();
          job->state.result.failed = job->state.result.total;
          job->state.result.failures.append({-1, "*", "prepare", job->error});
          return job->error;
        }
        QStringList snapshotWarnings;
        auto project = PaleoLayoutExport::rebuildProject(job->snapshot,
                                                         &job->error,
                                                         &snapshotWarnings);
        if (!project) {
          job->state.result.total = job->request.tiles.size();
          job->state.result.failed = job->state.result.total;
          job->state.result.failures.append({-1, "*", "prepare", job->error});
          return job->error;
        }
        // 快照层缺源不阻断地图册导出，但产物少层必须可见（warning 档入账）。
        paleo::workflow_detail::reportSnapshotWarnings(snapshotWarnings);
        Request work = job->request;
        for (const auto &id : job->layerIds)
          if (auto *layer = project->mapLayer(id))
            work.layers.append(layer);
        if (job->catalog) {
          work.catalog = job->catalog.get();
          work.projectDir = job->staging->path();
        }
        Exporter worker(nullptr, project.get());
        job->state.request = work;
        if (worker.prepareRun(job->state, task)) {
          for (int i = 0; i < work.tiles.size(); ++i)
            if (!worker.exportTileAt(job->state, i, task))
              break;
          worker.finishRun(job->state, task);
        }
        if (job->catalog)
          job->journal = job->catalog->journal();
        const auto &result = job->state.result;
        if (!result.succeeded && result.failed)
          job->error = result.failures.isEmpty()
                           ? tr("地图册导出失败")
                           : result.failures.first().error;
        return job->error;
      });
  job->task = task;
  m_async = job;
  connect(task, &PaleoTask::changed, this, [this, task] {
    emit progress(int(task->bytesDone()), int(task->bytesTotal()),
                  task->detailText());
  });
  connect(task, &PaleoTask::finished, this, [this, job, task] {
    // C-3（full-audit-20261021）：摘除 mutationSeq 陈旧判据（#480 P3-3 口径
    // ——地图册输入在提交时已冻结进 staging，窗口期内的无关 catalog 写不再
    // 整批判废长导出；真冲突由 applyJournal 事务失败如实上报）。
    const bool stale =
        task->session() != m_tasks->session() ||
        (job->catalog &&
         (!job->ownerCatalog ||
          job->ownerCatalog->projectDir() != job->request.projectDir));
    auto &result = job->state.result;
    QString error;
    QHash<QString, QString> idMap; // C-3：撞号重映射表
    if (stale) {
      task->discardPublication();
      error = tr("工程数据已变化，导出成果未登记，请重新导出");
    } else if (job->catalog && !publishMapExportJournal(job->ownerCatalog,
                                                        job->request.projectDir,
                                                        job->staging->path(),
                                                        job->journal, &error,
                                                        &idMap)) {
      task->failPublication(error);
    } else if (job->catalog) {
      // C-3：撞号重映射——registeredIds 换成发布实际入库的新号再解析路径。
      for (auto &id : result.registeredIds) {
        if (const auto it = idMap.constFind(id); it != idMap.constEnd())
          id = it.value();
      }
      result.registered.clear();
      for (const auto &id : result.registeredIds)
        result.registered.append(job->ownerCatalog->versionFilePath(
            job->ownerCatalog->currentVersion(id)));
    }
    if (!error.isEmpty()) {
      result.failures.append({-1, "*", "register", error});
      result.failed += result.succeeded;
      result.succeeded = 0;
      result.registered.clear();
      result.registeredIds.clear();
      // 登记失败 = 整册一版都没进 catalog：逐页文件与 tileFiles 一并清空，
      // 索引页同理不成立——否则 manifest 会按 tileFiles 逐页标 "ok"，与
      // succeeded=0 自相矛盾（#197）。
      result.files.clear();
      result.indexPage.clear();
      std::fill(job->state.tileFiles.begin(), job->state.tileFiles.end(), QString());
      writeManifest(job->request, result, job->state.tileFiles,
                    &result.manifest, nullptr);
    }
    m_last = result;
    m_async.reset();
    emit finished(result);
  });
  return task;
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
    return bail(
        QStringLiteral("prepare"),
        QObject::tr(
            "地图册导出必须在导出器所属线程执行（使用本线程的独立工程）"));
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
