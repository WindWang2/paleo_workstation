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
#include "../qgis/mapbookrender.h" // P5-W6：建版/渲染/销毁 + 图层摘要全走 facade
#include "../services/paleotaskservice.h"
#include "mapexport.h" // registerMapPdfAsset（既有产物登记管线，复用不自写）

namespace PaleoMapBookQueue
{

namespace
{
  QString defaultTitlePattern() { return QStringLiteral( "%{book} · %{tile_label}" ); }
  QString defaultFooterPattern()
  {
    return QStringLiteral( "%{tile_label} · 中心 %{center_x}, %{center_y} · %{extent} · %{crs}" );
  }

  // P5-W3：owner/调用线程上冻结输入说明（图层顺序 = Request.layers 顺序）。
  // managed 口径 = 图层带可用的 paleoVersionId 声明（记录的是图层声明身份）；
  // catalog 缺席或解析不到对应版本时 sha 如实留空，不伪造内容摘要。
  FrozenInputs captureInputs( const QList<QgsMapLayer *> &layers, QgsProject *project,
                              const DataCatalog *catalog )
  {
    FrozenInputs inputs;
    inputs.projectCrs = PaleoMapBookRender::projectCrsAuthid( project );
    inputs.qgisRuntime = PaleoLayoutExport::qgisVersion();
    for ( QgsMapLayer *layer : layers )
    {
      const PaleoMapBookRender::LayerSummary summary = PaleoMapBookRender::summarizeLayer( layer );
      LayerInputRecord record;
      record.qgisId = summary.id;
      record.paleoId = summary.paleoId;
      record.name = summary.name;
      record.type = summary.type;
      record.crs = summary.crs;
      record.assetId = summary.assetId;
      record.versionId = summary.versionId;
      record.source = summary.source;
      record.managed = !record.versionId.isEmpty();
      if ( record.managed && catalog && !record.assetId.isEmpty() )
      {
        const CatalogVersion version = catalog->currentVersion( record.assetId );
        if ( !version.id.isEmpty() && version.id == record.versionId )
          record.sha256 = version.sha256;
      }
      inputs.layers << record;
    }
    return inputs;
  }

  QJsonObject pageRecordJson( const Request &request, const PageRecord &page )
  {
    QJsonObject entry;
    entry.insert( QStringLiteral( "index" ), page.index );
    entry.insert( QStringLiteral( "name" ), page.tile );
    // 兼容旧字段：row/col/extent 从格定义来（状态以 PageRecord 为准）。
    for ( const PaleoMapBook::Tile &tile : request.tiles )
    {
      if ( tile.index == page.index )
      {
        entry.insert( QStringLiteral( "row" ), tile.row );
        entry.insert( QStringLiteral( "col" ), tile.col );
        QJsonArray extent;
        extent << tile.extent.xMin << tile.extent.yMin << tile.extent.xMax << tile.extent.yMax;
        entry.insert( QStringLiteral( "extent" ), extent );
        break;
      }
    }
    const QString file = page.file.isEmpty()
                             ? QString()
                             : QDir( bookRoot( request ) ).relativeFilePath( page.file );
    entry.insert( QStringLiteral( "file" ), file );
    entry.insert( QStringLiteral( "status" ), pageStatusToString( page.status ) );
    entry.insert( QStringLiteral( "bytes" ), double( page.bytes ) );
    if ( !page.sha256.isEmpty() )
      entry.insert( QStringLiteral( "sha256" ), page.sha256 );
    if ( !page.reason.isEmpty() )
      entry.insert( QStringLiteral( "reason" ), page.reason );
    return entry;
  }

  QJsonObject layerInputJson( const LayerInputRecord &record )
  {
    QJsonObject entry;
    entry.insert( QStringLiteral( "qgisId" ), record.qgisId );
    if ( !record.paleoId.isEmpty() )
      entry.insert( QStringLiteral( "paleoId" ), record.paleoId );
    entry.insert( QStringLiteral( "name" ), record.name );
    entry.insert( QStringLiteral( "type" ), record.type );
    entry.insert( QStringLiteral( "crs" ), record.crs.isEmpty()
                                                ? QStringLiteral( "unknown" )
                                                : record.crs );
    if ( !record.assetId.isEmpty() )
      entry.insert( QStringLiteral( "assetId" ), record.assetId );
    if ( !record.versionId.isEmpty() )
      entry.insert( QStringLiteral( "versionId" ), record.versionId );
    if ( !record.sha256.isEmpty() )
      entry.insert( QStringLiteral( "sha256" ), record.sha256 );
    entry.insert( QStringLiteral( "managed" ), record.managed );
    if ( !record.source.isEmpty() )
      entry.insert( QStringLiteral( "source" ), record.source );
    return entry;
  }

  bool writeManifest( const Request &request, const Result &result,
                      QString *pathOut, QString *error )
  {
    // P5-W4：页状态以 Result::pages 为单一事实源（不再从文件列表反推——
    // 取消页曾被写成 failed）。计数从页账本汇总并与 Result 计数交叉复核，
    // 对不上宁可写明（root 记账以页账本为准 + mismatch 警示行）也不静默出账。
    QJsonArray pages;
    int okCount = 0, failedCount = 0, cancelledCount = 0;
    for ( const PageRecord &page : result.pages )
    {
      pages << pageRecordJson( request, page );
      switch ( page.status )
      {
        case PageStatus::Ok: ++okCount; break;
        case PageStatus::Failed: ++failedCount; break;
        case PageStatus::Cancelled: ++cancelledCount; break;
      }
    }
    QStringList accountingMismatches;
    if ( okCount != result.succeeded )
      accountingMismatches << QObject::tr( "页账本 ok=%1 与 succeeded=%2 不符" )
                               .arg( okCount ).arg( result.succeeded );
    if ( failedCount != result.failed )
      accountingMismatches << QObject::tr( "页账本 failed=%1 与 failed=%2 不符" )
                               .arg( failedCount ).arg( result.failed );
    if ( cancelledCount != result.cancelled )
      accountingMismatches << QObject::tr( "页账本 cancelled=%1 与 cancelled=%2 不符" )
                               .arg( cancelledCount ).arg( result.cancelled );

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

    // 装配段：索引页状态（P5-W4 单列；请求没要 → skipped，不冒充成功）。
    QString indexFailure;
    for ( const Failure &f : result.failures )
      if ( f.stage == QLatin1String( "index" ) )
      {
        indexFailure = f.error;
        break;
      }
    QJsonObject index;
    if ( !result.indexFiles.isEmpty() )
      index.insert( QStringLiteral( "status" ), QStringLiteral( "ok" ) );
    else if ( !indexFailure.isEmpty() )
      index.insert( QStringLiteral( "status" ), QStringLiteral( "failed" ) );
    else
      index.insert( QStringLiteral( "status" ), QStringLiteral( "skipped" ) );
    if ( !indexFailure.isEmpty() )
      index.insert( QStringLiteral( "reason" ), indexFailure );
    QJsonArray indexFiles;
    for ( const QString &file : result.indexFiles )
    {
      QJsonObject entry;
      entry.insert( QStringLiteral( "file" ),
                    QDir( bookRoot( request ) ).relativeFilePath( file ) );
      entry.insert( QStringLiteral( "bytes" ), double( QFileInfo( file ).size() ) );
      indexFiles << entry;
    }
    index.insert( QStringLiteral( "files" ), indexFiles );

    // P5-W3：冻结输入说明（提交时刻的普通值对象；工程随后换版本不影响本册
    // 的对应关系——数据/布局可追溯，不承诺 PDF 字节逐次相同）。
    QJsonObject inputs;
    QJsonArray inputLayers;
    for ( const LayerInputRecord &record : request.inputs.layers )
      inputLayers << layerInputJson( record );
    inputs.insert( QStringLiteral( "layers" ), inputLayers );
    inputs.insert( QStringLiteral( "projectCrs" ), request.inputs.projectCrs.isEmpty()
                                                      ? QStringLiteral( "unknown" )
                                                      : request.inputs.projectCrs );
    if ( !request.horizon.isEmpty() )
    {
      inputs.insert( QStringLiteral( "horizon" ), request.horizon );
      inputs.insert( QStringLiteral( "analysis" ), request.horizon );
    }
    QJsonObject layout;
    layout.insert( QStringLiteral( "template" ), QStringLiteral( "builtin:A4" ) );
    layout.insert( QStringLiteral( "orientation" ),
                   request.landscape ? QStringLiteral( "landscape" )
                                     : QStringLiteral( "portrait" ) );
    layout.insert( QStringLiteral( "dpi" ), request.dpi );
    layout.insert( QStringLiteral( "titlePattern" ), request.titlePattern );
    layout.insert( QStringLiteral( "footerPattern" ), request.footerPattern );
    layout.insert( QStringLiteral( "withIndexPage" ), request.withIndexPage );
    layout.insert( QStringLiteral( "previewLimit" ), request.previewLimit );
    inputs.insert( QStringLiteral( "layout" ), layout );
    QJsonObject runtime;
    runtime.insert( QStringLiteral( "qgis" ), request.inputs.qgisRuntime );
    inputs.insert( QStringLiteral( "runtime" ), runtime );

    QJsonObject root;
    root.insert( QStringLiteral( "book" ), request.book );
    root.insert( QStringLiteral( "generated" ), request.date.isEmpty()
                                                  ? QDate::currentDate().toString( Qt::ISODate )
                                                  : request.date );
    root.insert( QStringLiteral( "format" ), extensionFor( request.format ) );
    root.insert( QStringLiteral( "dpi" ), request.dpi );
    root.insert( QStringLiteral( "tiles" ), request.tiles.size() );
    // 计数以页账本汇总为准（单一事实源），与 Result 计数不符时 warnings 钉明。
    root.insert( QStringLiteral( "succeeded" ), okCount );
    root.insert( QStringLiteral( "failed" ), failedCount );
    root.insert( QStringLiteral( "cancelled" ), cancelledCount );
    root.insert( QStringLiteral( "pages" ), pages );
    root.insert( QStringLiteral( "failures" ), failures );
    // 兼容旧字段：indexPage 仍是主索引文件相对路径（未出为空串）。
    root.insert( QStringLiteral( "indexPage" ),
                 result.indexPage.isEmpty()
                     ? QString()
                     : QDir( bookRoot( request ) ).relativeFilePath( result.indexPage ) );
    root.insert( QStringLiteral( "index" ), index );
    QJsonObject manifestStatus;
    manifestStatus.insert( QStringLiteral( "status" ), QStringLiteral( "ok" ) );
    root.insert( QStringLiteral( "manifest" ), manifestStatus );
    root.insert( QStringLiteral( "inputs" ), inputs );
    {
      QJsonArray warnings;
      for ( const QString &warning : result.warnings )
        warnings << warning;
      for ( const QString &mismatch : accountingMismatches )
        warnings << mismatch;
      if ( !warnings.isEmpty() )
        root.insert( QStringLiteral( "warnings" ), warnings );
    }

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

QString pageStatusToString( PageStatus status )
{
  switch ( status )
  {
    case PageStatus::Ok: return QStringLiteral( "ok" );
    case PageStatus::Failed: return QStringLiteral( "failed" );
    case PageStatus::Cancelled: return QStringLiteral( "cancelled" );
  }
  return QStringLiteral( "failed" );
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

QString Result::deliverySummary() const
{
  // P5-W7：交付视角一行——部分可用显式呈现（页成功 ≠ 索引/清单成功）。
  // 清单状态以 manifest 是否真写出来为准（prepare 阶段失败的册从未写过
  // manifest，failures 里只有 prepare 项——不能反推出「已写」）。
  QString line = summary();
  bool indexFailed = false, manifestFailed = false;
  for ( const Failure &f : failures )
  {
    if ( f.stage == QLatin1String( "index" ) )
      indexFailed = true;
    if ( f.stage == QLatin1String( "manifest" ) )
      manifestFailed = true;
  }
  if ( !indexFiles.isEmpty() )
    line += QObject::tr( " · 目录 %1 文件" ).arg( indexFiles.size() );
  else if ( indexFailed )
    line += QObject::tr( " · 目录失败" );
  if ( !manifest.isEmpty() )
    line += QObject::tr( " · 清单已写" );
  else
    line += manifestFailed ? QObject::tr( " · 清单失败" ) : QObject::tr( " · 清单未写" );
  if ( !warnings.isEmpty() )
    line += QObject::tr( " · 警告 %1" ).arg( warnings.size() );
  return line;
}

struct Exporter::RunState
{
  Request request;
  Result result;
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
      job->layerIds.append( PaleoMapBookRender::layerStableId( layer ) );
  // P5-W3：输入冻结说明必须在 owner 线程、layers/catalog 被清空前捕获——
  // 受管身份/CRS/摘要都是「提交时刻」的事实，worker 重建的私有工程与
  // owner catalog 不再回读。
  job->request.inputs = captureInputs(request.layers, m_project, request.catalog);
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
        const auto failWholeRun = [&job]( const QString &message ) {
          // 与 prepareRun 的 bail 同口径：整批 failed + 页账本同步（M3）。
          auto &result = job->state.result;
          result.total = job->request.tiles.size();
          result.failed = result.total;
          result.failures.append( { -1, QStringLiteral( "*" ),
                                    QStringLiteral( "prepare" ), message } );
          result.pages.resize( job->request.tiles.size() );
          for ( int i = 0; i < job->request.tiles.size(); ++i )
          {
            PageRecord &page = result.pages[i];
            page.index = job->request.tiles.at( i ).index;
            page.tile = job->request.tiles.at( i ).name;
            page.status = PageStatus::Failed;
            page.reason = message;
          }
        };
        if (!job->error.isEmpty()) {
          failWholeRun( job->error );
          return job->error;
        }
        QStringList snapshotWarnings;
        // #531 句柄口径：私有 QgsProject 的所有权关在封装层（ProjectHandle），
        // 本层只拿裸指针传给 facade——unique_ptr<QgsProject> 在此 TU 无法析构。
        auto projectHandle = PaleoLayoutExport::rebuildProjectHandle(
            job->snapshot, &job->error, &snapshotWarnings);
        if (!projectHandle) {
          failWholeRun( job->error );
          return job->error;
        }
        // 快照层缺源不阻断地图册导出，但产物少层必须可见（warning 档入账
        // ——P5-W3 起同时进 Result.warnings/manifest，不只进任务 detail）。
        job->state.result.warnings = snapshotWarnings;
        paleo::workflow_detail::reportSnapshotWarnings(snapshotWarnings);
        Request work = job->request;
        for (const auto &id : job->layerIds)
          if (auto *layer = PaleoMapBookRender::projectLayerById(projectHandle->project(), id))
            work.layers.append(layer);
        if (job->catalog) {
          work.catalog = job->catalog.get();
          work.projectDir = job->staging->path();
        }
        Exporter worker(nullptr, projectHandle->project());
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
      // 已渲染的目录页随整册作废：manifest 的 index 段必须显式 failed，
      // 不能落回 skipped（复审 L：降级语义如实）。
      if (!result.indexFiles.isEmpty())
        result.failures.append({-1, "*", "index",
                                tr("登记失败，目录页随整册作废")});
      result.failed += result.succeeded;
      result.succeeded = 0;
      result.registered.clear();
      result.registeredIds.clear();
      // 登记失败 = 整册一版都没进 catalog：逐页文件与页账本一并作废，
      // 索引页同理不成立——否则 manifest 会把逐页标 "ok"，与 succeeded=0
      // 自相矛盾（#197；P5-W4 起页账本同步降级，不只清文件列表）。
      result.files.clear();
      result.indexPage.clear();
      result.indexFiles.clear();
      result.manifest.clear();
      for (PageRecord &page : result.pages) {
        if (page.status == PageStatus::Ok) {
          page.status = PageStatus::Failed;
          page.file.clear();
          page.bytes = 0;
          page.sha256.clear();
          page.reason = error;
        }
      }
      // 降级后重写账本；重写失败也要入账——磁盘残留旧账（页标 ok）与新
      // Result 矛盾，不能静默（复审 L）。
      QString rewriteError;
      if (!writeManifest(job->request, result, &result.manifest, &rewriteError))
        result.failures.append({-1, "*", "manifest", rewriteError});
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

  // P5-W4：逐页账本先立（任何失败路径也逐页有账，与 tiles 同序同量）。
  result.pages.resize( request.tiles.size() );
  for ( int i = 0; i < request.tiles.size(); ++i )
  {
    PageRecord &page = result.pages[i];
    page.index = request.tiles.at( i ).index;
    page.tile = request.tiles.at( i ).name;
    page.status = PageStatus::Cancelled;
    page.reason = QObject::tr( "未执行" );
  }

  // bail：整批失败——failed=total 且页账本同步置 Failed（reason 带原因），
  // 不留「计数全 failed、页账本全 cancelled」的自相矛盾（#P5 复审 M3）。
  const auto bail = [&result, task]( const QString &stage, const QString &message ) {
    result.failed = result.total;
    result.failures.append( Failure{ -1, QStringLiteral( "*" ), stage, message } );
    for ( PageRecord &page : result.pages )
    {
      page.status = PageStatus::Failed;
      page.reason = message;
    }
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

  // P5-W3：同步入口没带冻结说明时，在调用线程按同一规则补齐（与 start()
  // 的 owner 线程捕获同一函数，同一语义）。
  if ( state.request.inputs.layers.isEmpty() )
    state.request.inputs = captureInputs( request.layers, m_project, request.catalog );

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

  const QDir rootDir( bookRoot( state.request ) );
  if ( !rootDir.exists() && !rootDir.mkpath( QStringLiteral( "." ) ) )
    return bail( QStringLiteral( "prepare" ),
                 QObject::tr( "无法创建图册目录：%1" ).arg( rootDir.absolutePath() ) );
  if ( !QDir().mkpath( pagesDir( state.request ) ) )
    return bail( QStringLiteral( "prepare" ),
                 QObject::tr( "无法创建版面目录：%1" ).arg( pagesDir( state.request ) ) );
  if ( state.request.withIndexPage &&
       !QDir().mkpath( QFileInfo( indexPath( state.request ) ).absolutePath() ) )
    return bail( QStringLiteral( "prepare" ),
                 QObject::tr( "无法创建索引目录：%1" ).arg( indexPath( state.request ) ) );

  state.qgisFormat = request.format == Format::Png ? PaleoLayoutExport::Format::Png
                                                   : PaleoLayoutExport::Format::Pdf;
  return true;
}

bool Exporter::exportTileAt( RunState &state, int i, PaleoTask *task )
{
  const Request &request = state.request;
  Result &result = state.result;

  // 协作式取消 + 预演上限：两者都只停未出的版，已出的保留；剩余版逐页记
  // cancelled（P5-W4：状态入页账本，manifest 与 Result 同源）。
  const auto stopRemaining = [&result, i]( const QString &reason ) {
    for ( int k = i; k < result.pages.size(); ++k )
    {
      result.pages[k].status = PageStatus::Cancelled;
      result.pages[k].reason = reason;
    }
    result.cancelled += result.total - i;
  };
  if ( task && task->cancelRequested() )
  {
    stopRemaining( QObject::tr( "用户取消（已出的版保留）" ) );
    return false;
  }
  if ( request.previewLimit >= 0 && i >= request.previewLimit )
  {
    stopRemaining( QObject::tr( "预演限额（前 %1 版）" ).arg( request.previewLimit ) );
    return false;
  }

  const PaleoMapBook::Tile &tile = request.tiles.at( i );
  PageRecord &page = result.pages[i];
  page.status = PageStatus::Failed; // 出错前先按失败立账，成功路径覆写
  const auto failTile = [&page, &result, &tile]( const QString &stage,
                                                 const QString &message ) {
    result.failed += 1;
    result.failures.append( Failure{ tile.index, tile.name, stage, message } );
    page.reason = message;
  };

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
    failTile( QStringLiteral( "template" ), templateError );
    return true;
  }

  // P5-W6：建版/渲染/销毁全在 qgis/mapbookrender facade 内完成——本层只
  // 描述任务、收 RenderOutcome 记账。
  PaleoMapBookRender::TileJob job;
  job.title = title;
  job.footer = footer;
  job.xMin = tile.extent.xMin;
  job.yMin = tile.extent.yMin;
  job.xMax = tile.extent.xMax;
  job.yMax = tile.extent.yMax;
  job.layers = request.layers;
  job.landscape = request.landscape;
  const auto rendered = PaleoMapBookRender::renderTile( m_project, job,
                                                         pagePath( request, tile ),
                                                         state.qgisFormat, request.dpi );
  if ( !rendered.ok )
  {
    // stage 语义保留：建版失败 layout、渲染/校验失败 export（facade 已标注）。
    failTile( rendered.stage.isEmpty() ? QStringLiteral( "export" ) : rendered.stage,
              rendered.error );
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
      failTile( QStringLiteral( "register" ),
                registerError.isEmpty() ? QObject::tr( "产物登记失败" ) : registerError );
      return true;
    }
    result.registered << managed;
    result.registeredIds << assetId;
  }

  // P5-W4：成功页入账（文件/字节/内容摘要）——摘要 best-effort，算不出留空
  //（文件已在 renderTile 内回读校验非空合法）。
  page.status = PageStatus::Ok;
  page.file = rendered.path;
  page.bytes = rendered.bytes;
  page.reason.clear();
  QString shaError;
  page.sha256 = DataCatalog::sha256FileHex( rendered.path, &shaError );
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

  // 报告装配：目录索引页（分页列全册条目，P5-W5 不再截断），失败只记账
  // 不回退已成版面。
  if ( request.withIndexPage && result.succeeded > 0 && !( task && task->cancelRequested() ) )
  {
    QStringList entries;
    for ( const PageRecord &page : result.pages )
    {
      if ( page.status != PageStatus::Ok )
        continue;
      for ( const PaleoMapBook::Tile &tile : request.tiles )
      {
        if ( tile.index != page.index )
          continue;
        entries << QStringLiteral( "%1  %2  %3" )
                     .arg( tile.name, tile.extent.extentText(),
                           QFileInfo( page.file ).fileName() );
        break;
      }
    }
    PaleoMapBookRender::IndexJob job;
    job.title = QObject::tr( "%1 目录（%2 版）" ).arg( request.book ).arg( entries.size() );
    job.entries = entries;
    job.landscape = false; // 目录页默认竖版（IndexSpec 同口径）
    const auto rendered = PaleoMapBookRender::renderIndex( m_project, job,
                                                           indexPath( request ),
                                                           state.qgisFormat, request.dpi );
    if ( rendered.ok )
    {
      result.indexPage = rendered.path;
      result.indexFiles = rendered.files;
    }
    else
    {
      result.failures.append( Failure{ -1, QStringLiteral( "*" ),
                                       QStringLiteral( "index" ), rendered.error } );
    }
  }

  QString manifestError;
  if ( !writeManifest( request, result, &result.manifest, &manifestError ) )
    result.failures.append( Failure{ -1, QStringLiteral( "*" ), QStringLiteral( "manifest" ),
                                     manifestError } );
}

} // namespace PaleoMapBookQueue
