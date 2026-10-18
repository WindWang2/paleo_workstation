// 层：功能
#include "horizonbatchexport.h"

#include "workflowerrors_internal.h" // reportSnapshotWarnings：缺层警告统一入账

#include "mapexport.h" // registerMapPdfAsset：图件 → catalog OUTPUT 登记

#include "../catalog/datacatalog.h"
#include "../metadata/layermanifest.h"
#include "../qgis/qgislayerservice.h"
#include "../services/dataexport.h"

#include <QDate>
#include <QHash>
#include <QJsonDocument>
#include <QSaveFile>
#include <QDir>
#include <QFile>
#include <QFileInfo>
#include <QTemporaryDir>
#include <QCoreApplication>
#include <QPointer>
#include <QTimer>
#include <memory>
#include "../services/paleotaskservice.h"

#include <qgscoordinatetransform.h>
#include <qgslayout.h>
#include <qgslayoutitem.h>
#include <qgslayoutitemlabel.h>
#include <qgslayoutitemmap.h>
#include <qgslayoutundostack.h>
#include <qgsmaplayer.h>
#include <qgsprintlayout.h>
#include <qgsproject.h>
#include <qgsreadwritecontext.h>
#include <qgsrectangle.h>

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
      case PaleoHorizonBatchExport::Format::Tiff: return QStringLiteral( "tif" );
    }
    return QStringLiteral( "png" );
  }

  // 批处理文件名规则（#537）：`<工程>_<层位>_<日期>_<序号>`，序号 = 层位在批次内
  // 的次序（0 起）——消毒后同形的层位名（"K1/q" 与 "K1 q" 的 safeSegment 都是
  // "K1_q"）也拿到不同文件。step() 的暂存导出名与 startAsync() 的外部交付名
  // 共用这条规则，避免两处命名漂移。
  QString batchBaseName( const QString &project, const QString &horizon,
                         const QString &date, int index )
  {
    return QStringLiteral( "%1_%2_%3_%4" )
        .arg( project, safeSegment( horizon ), date, QString::number( index + 1 ) );
  }

  // 批次命名上下文：工程段 / 日期段 / 扩展名。Batch::step()（暂存导出名）与
  // startAsync() 的交付循环（外部目录落盘名）必须同源。
  struct BatchNaming
  {
    QString project;
    QString date;
    QString ext;
  };

  BatchNaming batchNaming( const PaleoHorizonBatchExport::Request &request )
  {
    BatchNaming naming;
    naming.project = safeSegment( request.projectName.isEmpty()
                                      ? QStringLiteral( "project" )
                                      : request.projectName );
    naming.date = request.date.isEmpty()
                      ? QDate::currentDate().toString( QStringLiteral( "yyyyMMdd" ) )
                      : request.date;
    naming.ext = extensionFor( request.format );
    return naming;
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
  // 已占用的交付文件名 → 层位（#537）：safeSegment 把 '/'、' '、'\t' 等一律折叠
  // 成 '_'（"K1/q" 与 "K1 q"、"K1_q" 三名同形），Windows 大小写不敏感同样同
  // 文件；平面命名下 QFile::copy 目标已存在即失败，且失败发生在全部渲染之后。
  QHash<QString, QString> usedNames;
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
    const auto naming = batchNaming( request );
    project = naming.project;
    date = naming.date;
    ext = naming.ext;
    scratch = std::make_unique<QTemporaryDir>();
    begun = true;
    return true;
  }

  // 导出并登记一个层位（next 前进一格）。
  void step()
  {
    const int index = next; // 层位序号（0 起）：交付文件名的唯一性来源
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
          map->zoomToExtent( extent ); // preserve the designed page frame for every horizon
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
    // 层位序号进文件名（#537）：消毒后同形的层位名不再撞同一交付文件；万一仍
    // 撞名（如同名层位），如实报「层位 %1 与 %2 导出文件名相同」而不是让
    // QFile::copy 在全部渲染完成后才失败。
    const QString baseName = batchBaseName( project, horizon, date, index );
    const auto claimed = usedNames.value( baseName );
    if ( !claimed.isEmpty() && claimed != horizon )
    {
      fail( horizon, QObject::tr( "层位 %1 与 %2 导出文件名相同" ).arg( claimed, horizon ) );
      return;
    }
    usedNames.insert( baseName, horizon );
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
    for (const auto &file : exported.files) {
      const auto assetId = registerMapPdfAsset(catalog, request.projectDir, file, &sha, &managed, &err, ext);
      if (assetId.isEmpty()) { fail(horizon, err); return; }
      outcome.files << managed; outcome.assetIds << assetId;
      if (outcome.assetId.isEmpty()) {
        outcome.file = managed; outcome.assetId = assetId; outcome.sha256 = sha;
      }
    }
    outcome.ok = true;
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

PaleoTask *startAsync(const Request &request, PaleoTaskService *tasks,
                      std::function<void(const Result &, bool)> done,
                      QString *error) {
  static const char *kBusy = "paleoBatchExportRunning";
  const auto reject = [&](const QString &why) -> PaleoTask * {
    if (error)
      *error = why;
    return nullptr;
  };
  if (!tasks)
    return reject(QObject::tr("任务中心不可用"));
  if (!request.layout || !request.catalog || request.projectDir.isEmpty())
    return reject(QObject::tr("未提供版面或工程数据目录"));
  if (request.layout->property(kBusy).toBool())
    return reject(
        QObject::tr("该版面已有批量出图在进行，请等待完成或在任务中心取消"));
  struct Job {
    Request request;
    PaleoLayoutExport::ProjectSnapshot project;
    QDomDocument layout;
    QMap<QString, QStringList> layerIds;
    QPointer<QgsPrintLayout> ownerLayout;
    QPointer<DataCatalog> ownerCatalog;
    quint64 sequence = 0;
    std::unique_ptr<QTemporaryDir> staging, external;
    std::unique_ptr<DataCatalog> catalog;
    QVector<CatalogOp> journal;
    Result result;
    QString error;
  };
  auto job = std::make_shared<Job>();
  job->request = request;
  job->request.layout = nullptr;
  job->request.catalog = nullptr;
  job->request.horizonLayers.clear();
  job->ownerLayout = request.layout;
  job->ownerCatalog = request.catalog;
  job->sequence = request.catalog->mutationSeq();
  for (auto it = request.horizonLayers.cbegin();
       it != request.horizonLayers.cend(); ++it) {
    job->layerIds[it.key()] = {};
    for (auto *layer : it.value())
      if (layer)
        job->layerIds[it.key()].append(layer->id());
  }
  if (!PaleoLayoutExport::captureProjectSnapshot(request.layout->project(),
                                                 job->project, &job->error))
    return reject(job->error);
  QgsReadWriteContext context;
  job->layout.appendChild(request.layout->writeXml(job->layout, context));
  // 崩溃残留的隐藏暂存根（.horizon-export-* / .paleo-batch-*）无清扫器（#540）：
  // 入口先扫一遍再建自己的暂存目录。
  paleo::exporting::sweepStaleStagingRoots(request.projectDir);
  if (!request.outputDirectory.isEmpty())
    paleo::exporting::sweepStaleStagingRoots(
        QFileInfo(request.outputDirectory).absoluteDir().path());
  job->staging = std::make_unique<QTemporaryDir>(request.projectDir +
                                                 "/.horizon-export-XXXXXX");
  if (!job->staging->isValid())
    return reject(QObject::tr("无法创建出图暂存目录"));
  if (!request.outputDirectory.isEmpty()) {
    const QFileInfo destination(request.outputDirectory);
    if (destination.exists() || !destination.absoluteDir().exists()) return reject(QObject::tr("批量导出目标必须是新目录"));
    job->external = std::make_unique<QTemporaryDir>(destination.absoluteDir().filePath(".paleo-batch-XXXXXX"));
    if (!job->external->isValid()) return reject(QObject::tr("无法创建批量导出目录"));
  }
  job->catalog = request.catalog->createStagingCopy(job->staging->path());
  job->result.total = job->layerIds.size();
  request.layout->setProperty(kBusy, true);
  auto *task = tasks->start(
      QObject::tr("按层位批量出图 · %1 个层位").arg(job->layerIds.size()),
      [job](PaleoTask *task) -> QString {
        QStringList snapshotWarnings;
        auto project = PaleoLayoutExport::rebuildProject(job->project,
                                                         &job->error,
                                                         &snapshotWarnings);
        if (!project)
          return job->error;
        // 快照层缺源不阻断批量出图，但产物少层必须可见（warning 档入账）。
        paleo::workflow_detail::reportSnapshotWarnings(snapshotWarnings);
        QgsPrintLayout layout(project.get());
        QgsReadWriteContext context;
        if (!layout.readXml(job->layout.documentElement(), job->layout,
                            context))
          return job->error = QObject::tr("版面快照重建失败");
        Batch batch;
        batch.request = job->request;
        batch.request.layout = &layout;
        batch.request.catalog = job->catalog.get();
        batch.request.projectDir = job->staging->path();
        for (auto it = job->layerIds.cbegin(); it != job->layerIds.cend();
             ++it) {
          batch.request.horizonLayers[it.key()] = {};
          for (const auto &id : it.value())
            if (auto *layer = project->mapLayer(id))
              batch.request.horizonLayers[it.key()].append(layer);
        }
        if (batch.begin()) {
          while (!batch.done()) {
            if (task->cancelRequested()) {
              batch.failRemaining(QObject::tr("已取消"));
              break;
            }
            task->reportDetail(
                QObject::tr("导出层位 %1").arg(batch.horizons.at(batch.next)));
            batch.step();
            task->reportBytes(batch.next,
                              std::max(1, int(batch.horizons.size())));
          }
          batch.finish();
        }
        job->result = batch.result;
        if (job->external) {
          // 交付名与 step() 的导出名同源（#537）。catalog §3 dedup（同 SHA-256
          // 复用既有资产，mapexport.cpp:89）会把渲染结果相同的层位并到同一资产，
          // 受管文件名只剩首个层位的——按受管名交付会让第二个层位的 QFile::copy
          // 目标已存在，整批在全部渲染完成后才失败（正是 #537 要消灭的失败模式）。
          // 故每层位按自己的导出名落盘；一页以上产物保留源文件名，避免自覆盖。
          const auto naming = batchNaming(job->request);
          const QStringList keys = job->layerIds.keys();
          QVariantList outcomes;
          for (const auto &outcome : job->result.horizons) {
            QStringList files;
            QString copyError;
            // 层位序号以该层位在批次键序中的位置为准（与 step() 的 index 同源）
            const int at = keys.indexOf(outcome.horizon);
            const auto base = batchBaseName(naming.project, outcome.horizon, naming.date,
                                            at < 0 ? 0 : at);
            for (const auto &path : outcome.files) {
              const auto name = outcome.files.size() == 1
                                    ? base + QLatin1Char('.') + naming.ext
                                    : QFileInfo(path).fileName();
              if (!QFile::copy(path, job->external->filePath(name))) {
                copyError = QObject::tr("无法写入批量导出文件：%1").arg(name);
                break;
              }
              QFile::setPermissions(job->external->filePath(name), QFileDevice::ReadOwner | QFileDevice::WriteOwner | QFileDevice::ReadUser | QFileDevice::WriteUser);
              files << name;
            }
            outcomes.append(QVariantMap{{"horizon", outcome.horizon}, {"ok", outcome.ok && copyError.isEmpty()}, {"files", files},
                {"assets", outcome.assetIds}, {"error", copyError.isEmpty() ? outcome.error : copyError}});
            if (!copyError.isEmpty()) { job->error = copyError; break; }
          }
          // 清单先于任何早退落盘（#541）：写入失败/取消的批次，暂存目录里同样
          // 留下带 error / cancelled 字段的清单——发布门只放行完整成功的批次。
          const auto bytes = QJsonDocument::fromVariant(QVariantMap{{"schema", "paleo_horizon_export_v1"},
              {"format", extensionFor(job->request.format)}, {"dpi", job->request.dpi},
              {"cancelled", task->cancelRequested()}, {"error", job->error}, {"horizons", outcomes}}).toJson();
          QSaveFile metadata(job->external->filePath("metadata.json"));
          if (!metadata.open(QIODevice::WriteOnly) || metadata.write(bytes) != bytes.size() || !metadata.commit())
            job->error = QObject::tr("无法写出批量图件清单");
        }
        job->journal = job->catalog->journal();
        if (job->error.isEmpty() && job->result.failed && !task->cancelRequested())
          job->error = job->result.summary();
        return job->error;
      });
  QObject::connect(task, &PaleoTask::finished, tasks, [job, task, tasks, done] {
    if (job->ownerLayout)
      job->ownerLayout->setProperty(kBusy, QVariant());
    const bool stale =
        task->session() != tasks->session() || !job->ownerCatalog ||
        job->ownerCatalog->projectDir() != job->request.projectDir ||
        job->ownerCatalog->mutationSeq() != job->sequence;
    QString why;
    if (stale) {
      task->discardPublication();
      why = QObject::tr("工程数据已变化，出图成果未登记，请重新导出");
    } else if (task->cancelRequested() || task->state() != PaleoTask::State::Succeeded) {
      // 发布门（#541，与 geologicalexport.cpp:214 / mapexport.cpp:287-288 对齐）：
      // worker 报错或取消的批次不发布外部目录、不登记 journal——整批视为未交付，
      // 已导出层位在结果里留痕（ok=false + 原因），不落幽灵路径。
      if (job->error.isEmpty())
        job->error = task->cancelRequested() ? QObject::tr("已取消，批量出图成果未登记")
                                            : task->errorText();
      why = job->error;
    } else {
      bool externalPlaced = false;
      if (job->external) {
        externalPlaced = QDir().rename(job->external->path(), job->request.outputDirectory);
        if (!externalPlaced) why = QObject::tr("无法提交批量图件目录");
      }
      if (why.isEmpty() && !publishMapExportJournal(job->ownerCatalog, job->request.projectDir,
                                                   job->staging->path(), job->journal, &why)) {
        if (externalPlaced) QDir(job->request.outputDirectory).removeRecursively();
      }
      if (!why.isEmpty()) task->failPublication(why);
      else if (externalPlaced) job->result.directory = job->request.outputDirectory;
    }
    if (job->result.horizons.isEmpty() && !job->error.isEmpty()) {
      for (const auto &horizon : job->layerIds.keys())
        job->result.horizons.append({horizon, false, {}, {}, {}, job->error});
      job->result.failed = job->result.total;
    }
    for (auto &outcome : job->result.horizons) {
      if (!outcome.ok)
        continue;
      if (!why.isEmpty()) {
        outcome.ok = false;
        outcome.error = why;
        outcome.file.clear();
        outcome.assetId.clear();
        outcome.sha256.clear();
        outcome.files.clear(); outcome.assetIds.clear();
        --job->result.succeeded;
        ++job->result.failed;
      } else {
        outcome.file = job->ownerCatalog->versionFilePath(
            job->ownerCatalog->currentVersion(outcome.assetId));
        outcome.files.clear();
        for (const auto &asset : outcome.assetIds)
          outcome.files << job->ownerCatalog->versionFilePath(job->ownerCatalog->currentVersion(asset));
      }
    }
    job->catalog.reset(); job->external.reset(); job->staging.reset();
    job->project = {}; job->layout.clear();
    if (done)
      done(job->result, task->cancelRequested() || stale);
  });
  return task;
}

} // namespace PaleoHorizonBatchExport
