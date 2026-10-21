// 层：功能
#include "mapexport.h"
#include "workflowerrors_internal.h"

#include "../catalog/datacatalog.h"
#include "../metadata/layermanifest.h"
#include "../metadata/atomicfile.h"
#include "../qgis/qgislayerservice.h"
#include "../qgis/qgisprojectservice.h"
#include "../qgis/layoutexport.h" // PaleoLayoutExport::exportLayout（QGIS 封装层导出核心）

#include "../services/paleotaskservice.h"
#include <QCryptographicHash>
#include <QDir>
#include <QFile>
#include <QFileInfo>
#include <QImage>
#include <QPointer>
#include <QTemporaryDir>
#include <qgslayout.h>
#include <qgsmessagelog.h>
#include <qgsproject.h>
#include <qgsreadwritecontext.h>

#include <qgsprintlayout.h>

namespace
{
using paleo::workflow_detail::setError;

} // namespace

QgsPrintLayout *buildHorizonMapLayout( QgisLayerService *layers, QgisProjectService *projectSvc,
                                       const QString &horizon, QString *error )
{
  return PaleoLayoutExport::buildHorizonMapLayout( layers, projectSvc, horizon, error );
}

QString exportHorizonMapPdf( QgisLayerService *layers, QgisProjectService *projectSvc,
                             const QString &horizon, const QString &outPath, QString *error )
{
  const auto fail = [error]( const QString &msg ) {
    if ( error )
      *error = msg;
    return QString();
  };
  if ( outPath.isEmpty() )
    return fail( QObject::tr( "导出路径为空" ) );

  QgsPrintLayout *layout = buildHorizonMapLayout( layers, projectSvc, horizon, error );
  if ( !layout )
    return QString();

  const auto outcome = PaleoLayoutExport::exportLayout(
      layout, outPath, PaleoLayoutExport::Format::Pdf, 300.0,
      PaleoLayoutExport::PageRange() );
  delete layout;
  if ( !outcome.ok )
    return fail( outcome.error );
  return outcome.files.value( 0 );
}

QString registerMapPdfAsset( DataCatalog *catalog, const QString &projectDir,
                             const QString &pdfPath, QString *sha256Out,
                             QString *managedPathOut, QString *error,
                             const QString &assetFormat, const QString &horizon )
{
  const QString format = assetFormat.isEmpty() ? QStringLiteral( "pdf" ) : assetFormat;
  const auto fail = [error]( const QString &msg ) {
    if ( error )
      *error = msg;
    return QString();
  };
  if ( !catalog )
    return fail( QObject::tr( "未绑定数据目录" ) );
  if ( projectDir.isEmpty() )
    return fail( QObject::tr( "工程目录未设置" ) );
  if ( pdfPath.isEmpty() || !QFile::exists( pdfPath ) )
    return fail( QObject::tr( "找不到图件文件: %1" ).arg( pdfPath ) );

  // 文件摘要先算 —— dedup 与登记共用同一份 SHA-256。
  QString shaErr;
  const QString sha = DataCatalog::sha256FileHex( pdfPath, &shaErr );
  if ( sha.isEmpty() )
    return fail( shaErr.isEmpty() ? QObject::tr( "无法计算 SHA-256: %1" ).arg( pdfPath )
                                  : shaErr );

  // §3 dedup：同 SHA-256 且同层位域（含空↔空）的 OUTPUT 版本 → 复用既有
  // 版本的资产，不新增。层位域必须严格同域：空层位导出不得复用带层位登记
  // 的既有版本（否则调用侧按本层位回写 extra 会把既有版本的层位归属清空，
  // C-01）；带层位导出同样不得复用工区级版本。
  CatalogVersion existing;
  for (const auto &candidate : catalog->versions())
    if (candidate.sha256.compare(sha, Qt::CaseInsensitive) == 0 && candidate.stage == "OUTPUT" &&
        candidate.extra.value("horizon").toString() == horizon && catalog->assetById(candidate.assetId).format == format) {
      // 在盘复核（R2-1）：陈旧 OUTPUT 版本（sha 在册、受管文件已删）不得
      // 复用——旧 versionBySha256 路径有此复核，改线性扫描后须保留，否则
      // dedup 返回悬空 managed 路径，批量导出到发布期才失败。
      const QString onDisk = catalog->versionFilePath(candidate);
      QString verifyError;
      if (QFile::exists(onDisk) &&
          DataCatalog::sha256FileHex(onDisk, &verifyError).compare(sha, Qt::CaseInsensitive) == 0) {
        existing = candidate; break;
      }
    }
  if ( !existing.id.isEmpty() && existing.stage == "OUTPUT" &&
       catalog->assetById(existing.assetId).format == format &&
       existing.extra.value("horizon").toString() == horizon )
  {
    if ( sha256Out )
      *sha256Out = sha;
    if ( managedPathOut )
      *managedPathOut = catalog->versionFilePath(existing);
    return existing.assetId;
  }

  const QString fileName = QFileInfo( pdfPath ).fileName();
  if ( !DataCatalog::isSafePathSegment( fileName ) )
    return fail( QObject::tr( "文件名不是合法路径段: %1" ).arg( fileName ) );

  const QString assetId = catalog->nextAssetId();
  const QString versionId = catalog->nextVersionId();
  const QString relDir =
      DataCatalog::managedPath( QStringLiteral( "output" ), assetId, versionId, fileName );
  if ( relDir.isEmpty() )
    return fail( QObject::tr( "受管路径段不合法（output/%1/%2）" ).arg( assetId, versionId ) );
  const QString relPath = QStringLiteral( "artifacts/" ) + relDir;
  CatalogVersion pending;
  pending.managed = true;
  pending.path = relPath;
  const QString dst = DataCatalog::resolvedVersionPath( projectDir, pending );
  if ( dst.isEmpty() )
    return fail( QObject::tr( "unsafe managed destination: %1" ).arg( relPath ) );
  const QDir dir = QFileInfo( dst ).absoluteDir();
  if ( !dir.exists() && !dir.mkpath( QStringLiteral( "." ) ) )
    return fail( QObject::tr( "cannot create directory %1" ).arg( dir.absolutePath() ) );

  // 受管副本：partial + rename 原子落位，成功后置只读（与 RAW 入库同纪律）。
  const QString partial = dst + QStringLiteral( ".partial" );
  if ( !QFile::copy( pdfPath, partial ) )
    return fail( QObject::tr( "cannot copy %1 → %2" ).arg( pdfPath, partial ) );
  if ( !paleoReplaceFile( partial, dst ) )
  {
    QFile::remove( partial );
    return fail( QObject::tr( "cannot place %1" ).arg( dst ) );
  }
  QFile::setPermissions( dst, QFileDevice::ReadOwner | QFileDevice::ReadUser |
                                  QFileDevice::ReadGroup | QFileDevice::ReadOther );

  CatalogAsset asset;
  asset.id = assetId;
  asset.type = QStringLiteral( "document" ); // 图件（PDF/PNG）归 document 一类
  asset.format = format;
  asset.displayName = fileName;
  if ( !catalog->addAsset( asset, error ) )
  {
    // 受管副本已落位但资产没入库——不留孤儿字节。dst 刚被置只读，Windows
    // 删只读文件会失败，先恢复可写再删。
    QFile::setPermissions( dst, QFileDevice::ReadOwner | QFileDevice::WriteOwner | QFileDevice::ReadUser );
    QFile::remove( dst );
    return fail( error ? *error : QStringLiteral( "catalog addAsset failed" ) );
  }

  CatalogVersion ver;
  ver.id = versionId;
  ver.assetId = assetId;
  ver.stage = QStringLiteral( "OUTPUT" );
  ver.versionNumber = 1;
  ver.managed = true;
  ver.path = relPath;
  ver.sourceUri = QFileInfo( pdfPath ).absoluteFilePath();
  ver.sha256 = sha;
  ver.fileName = fileName;
  if (!horizon.isEmpty()) ver.extra.insert("horizon", horizon);
  if ( !catalog->addVersion( ver, error ) )
  {
    // 版本没入库：回滚刚加的资产并清掉受管副本——不留「资产在、零版本」
    // 幽灵 + 孤儿字节（零版本资产无血缘引用，removeAsset 不会被拦）。
    QString removeErr;
    catalog->removeAsset( assetId, &removeErr );
    QFile::setPermissions( dst, QFileDevice::ReadOwner | QFileDevice::WriteOwner | QFileDevice::ReadUser );
    QFile::remove( dst );
    return fail( error && !error->isEmpty() ? *error : QStringLiteral( "catalog addVersion failed" ) );
  }

  if ( sha256Out )
    *sha256Out = sha;
  if ( managedPathOut )
    *managedPathOut = dst;
  return assetId;
}

namespace {
// C-3（full-audit-20261021）：staging 侧分配的 ast-N/ver-N 序号取自任务启动
// 时的 catalog 快照——窗口期内任何无关写（导入、预测落版本、另一导出发布）
// 都会占用同号 id，重放 addAsset/addVersion 即撞「duplicate id」。发布前把
// 撞号 id 重映射为 live 新号，并同步改写后续引用与受管路径段；无撞号时
// 原样返回。#480 P3-3 同口径：输入在提交时已冻结，无关写不再整批判废长
// 任务；真冲突仍由 applyJournal 的事务快照失败如实上报。
QVector<CatalogOp> remapCollidingJournalIds(DataCatalog *live,
                                            const QVector<CatalogOp> &ops,
                                            QHash<QString, QString> &idMap)
{
  // R2-MF-1（full-audit-20261021 独立复审）：新号必须同时避让 live 已占
  // 与「同批 journal 自有 id」——只查 live 会把撞号 ast-N 映射到本批下一
  // 个 ast-(N+1)（live 无、journal 有），重放 addAsset 同 id 两次 → 整批
  // 失败（恰在批量导出 + 一笔无关写的目标场景）。
  QSet<QString> journalAssetIds, journalVersionIds;
  for (const CatalogOp &op : ops)
  {
    if (op.kind == CatalogOp::Kind::AddAsset)
      journalAssetIds.insert(op.asset.id);
    else if (op.kind == CatalogOp::Kind::AddVersion)
      journalVersionIds.insert(op.version.id);
  }
  QHash<QString, QString> assetMap, versionMap;
  for (const CatalogOp &op : ops)
  {
    if (op.kind == CatalogOp::Kind::AddAsset &&
        !live->assetById(op.asset.id).id.isEmpty())
    {
      QString next = live->nextAssetId();
      while (journalAssetIds.contains(next))
        next = live->nextAssetId();
      assetMap.insert(op.asset.id, next);
    }
    else if (op.kind == CatalogOp::Kind::AddVersion &&
             !live->versionById(op.version.id).id.isEmpty())
    {
      QString next = live->nextVersionId();
      while (journalVersionIds.contains(next))
        next = live->nextVersionId();
      versionMap.insert(op.version.id, next);
    }
  }
  if (assetMap.isEmpty() && versionMap.isEmpty())
    return ops;
  const auto rewritePathSegments = [&](QString &path) {
    if (path.isEmpty())
      return;
    QStringList segs = path.split(QLatin1Char('/'));
    bool changed = false;
    for (QString &seg : segs)
    {
      if (const auto it = assetMap.constFind(seg); it != assetMap.constEnd())
      {
        seg = it.value();
        changed = true;
      }
      else if (const auto it = versionMap.constFind(seg); it != versionMap.constEnd())
      {
        seg = it.value();
        changed = true;
      }
    }
    if (changed)
      path = segs.join(QLatin1Char('/'));
  };
  QVector<CatalogOp> out = ops;
  for (CatalogOp &op : out)
  {
    switch (op.kind)
    {
      case CatalogOp::Kind::AddAsset:
        if (const auto it = assetMap.constFind(op.asset.id); it != assetMap.constEnd())
          op.asset.id = it.value();
        break;
      case CatalogOp::Kind::AddVersion:
        if (const auto it = assetMap.constFind(op.version.assetId); it != assetMap.constEnd())
          op.version.assetId = it.value();
        if (const auto it = versionMap.constFind(op.version.id); it != versionMap.constEnd())
          op.version.id = it.value();
        rewritePathSegments(op.version.path);
        break;
      case CatalogOp::Kind::AddLink:
        if (const auto it = assetMap.constFind(op.link.assetId); it != assetMap.constEnd())
          op.link.assetId = it.value();
        break;
      case CatalogOp::Kind::UpdateVersionExtra:
        if (const auto it = versionMap.constFind(op.id); it != versionMap.constEnd())
          op.id = it.value();
        break;
      default: // AddEntity/UpdateLink 等位置或实体引用 op：导出链不产生，
        break; // 重放语义见 applyJournal 的事务快照
    }
  }
  for (auto it = assetMap.cbegin(); it != assetMap.cend(); ++it)
    idMap.insert(it.key(), it.value());
  for (auto it = versionMap.cbegin(); it != versionMap.cend(); ++it)
    idMap.insert(it.key(), it.value());
  return out;
}
} // namespace

bool publishMapExportJournal(DataCatalog *catalog, const QString &projectDir,
                             const QString &stagingRoot,
                             const QVector<CatalogOp> &ops, QString *error,
                             QHash<QString, QString> *idMap) {
  QStringList moved;
  const auto fail = [&](const QString &why) {
    for (const auto &path : moved) {
      QFile::setPermissions(path, QFileDevice::ReadOwner | QFileDevice::WriteOwner);
      QFile::remove(path);
    }
    if (error)
      *error = why;
    return false;
  };
  QHash<QString, QString> remapped;
  const QVector<CatalogOp> replay = remapCollidingJournalIds(catalog, ops, remapped);
  for (int i = 0; i < ops.size(); ++i) {
    if (ops.at(i).kind != CatalogOp::Kind::AddVersion || !ops.at(i).version.managed)
      continue;
    // 源路径按原 id（staging 侧文件已按原 id 落盘），目标按重映射后的 id。
    const auto source =
        DataCatalog::resolvedVersionPath(stagingRoot, ops.at(i).version);
    const auto target =
        DataCatalog::resolvedVersionPath(projectDir, replay.at(i).version);
    if (source.isEmpty() || target.isEmpty() || QFile::exists(target))
      return fail(QObject::tr("导出成果路径不安全或已存在"));
    if (!QDir().mkpath(QFileInfo(target).absolutePath()) ||
        !QFile::rename(source, target))
      return fail(QObject::tr("无法提交导出成果：%1").arg(target));
    moved.append(target);
  }
  QString why;
  if (!catalog || !catalog->applyJournal(replay, &why))
    return fail(why);
  if (idMap)
    *idMap = remapped;
  return true;
}

namespace {
using MapProducer = std::function<bool(const QString &, QString *)>;
PaleoTask *startMapProductExport(MapProducer produce, PaleoTaskService *tasks,
                                 DataCatalog *catalog, const QString &directory,
                                 const QString &name,
                                 const QString &destination,
                                 const QString &format, QObject *receiver,
                                 MapProductExportDone done, QString *error) {
  if (!tasks || !catalog || directory.isEmpty() ||
      !DataCatalog::isSafePathSegment(name)) {
    setError(error, QObject::tr("未绑定工程/任务中心或导出文件名不合法"));
    return nullptr;
  }
  const auto title = QObject::tr("导出图件：%1").arg(name);
  for (auto *running : tasks->tasks()) {
    if (running->running() && running->title() == title) {
      setError(error, QObject::tr("该图件已有导出任务，请等待完成或取消"));
      return nullptr;
    }
  }
  struct Job {
    std::unique_ptr<QTemporaryDir> staging;
    std::unique_ptr<DataCatalog> catalog;
    QVector<CatalogOp> journal;
    MapProductExportOutcome outcome;
  };
  auto job = std::make_shared<Job>();
  job->staging =
      std::make_unique<QTemporaryDir>(directory + "/.map-export-XXXXXX");
  if (!job->staging->isValid()) {
    setError(error, QObject::tr("无法创建导出暂存目录"));
    return nullptr;
  }
  job->catalog = catalog->createStagingCopy(job->staging->path());
  QPointer<DataCatalog> owner(catalog);
  auto *task = tasks->start(title, [job, produce, name, destination,
                                    format](PaleoTask *task) {
    const auto path = job->staging->filePath(name);
    task->reportStage("build", 5);
    if (task->cancelRequested())
      return QString();
    if (!produce(path, &job->outcome.error))
      return job->outcome.error;
    if (task->cancelRequested())
      return QString();
    task->reportStage("hash", 80);
    job->outcome.assetId = registerMapPdfAsset(
        job->catalog.get(), job->staging->path(), path, &job->outcome.sha256,
        &job->outcome.managedPath, &job->outcome.error, format);
    if (job->outcome.assetId.isEmpty())
      return job->outcome.error;
    if (!destination.isEmpty()) {
      const auto partial = destination + ".partial";
      if (!QFile::copy(path, partial) ||
          !paleoReplaceFile(partial, destination)) {
        QFile::remove(partial);
        return job->outcome.error =
                   QObject::tr("无法写入导出目标：%1").arg(destination);
      }
    }
    job->journal = job->catalog->journal();
    return job->outcome.error;
  });
  // Publication belongs to the service: closing a panel must not strand a
  // completed export. UI notification uses a separately guarded receiver.
  QPointer<QObject> target(receiver ? receiver : tasks);
  QObject::connect(
      task, &PaleoTask::finished, tasks,
      [job, task, tasks, owner, directory, target, done] {
        // C-3（full-audit-20261021）：摘除「开始时 mutationSeq」陈旧判据
        // （#480 P3-3 口径——导出输入在提交时已冻结进 staging，窗口期内的
        // 无关 catalog 写不再整批判废导出；真冲突由 applyJournal 事务失败
        // 如实上报）。session/工程目录守卫保留。
        const bool stale = task->session() != tasks->session() || !owner ||
                           owner->projectDir() != directory;
        job->outcome.cancelled = task->cancelRequested() || stale;
        if (task->state() == PaleoTask::State::Failed &&
            job->outcome.error.isEmpty())
          job->outcome.error = task->errorText();
        if (stale)
          task->discardPublication();
        if (!job->outcome.cancelled &&
            task->state() == PaleoTask::State::Succeeded) {
          QHash<QString, QString> idMap;
          if (!publishMapExportJournal(owner, directory, job->staging->path(),
                                       job->journal, &job->outcome.error, &idMap))
            task->failPublication(job->outcome.error);
          else {
            if (const auto it = idMap.constFind(job->outcome.assetId);
                it != idMap.constEnd())
              job->outcome.assetId = it.value();
            job->outcome.managedPath = owner->versionFilePath(
                owner->currentVersion(job->outcome.assetId));
          }
        }
        if (job->outcome.cancelled || !job->outcome.error.isEmpty()) {
          job->outcome.assetId.clear();
          job->outcome.managedPath.clear();
        }
        if (target && done)
          done(job->outcome);
      });
  return task;
}
} // namespace

PaleoTask *startMapLayoutExport(QgsLayout *layout, PaleoTaskService *tasks,
                                DataCatalog *catalog, const QString &directory,
                                const QString &pdfPath, QObject *receiver,
                                MapProductExportDone done, QString *error) {
  if (!layout) {
    setError(error, QObject::tr("未提供版面"));
    return nullptr;
  }
  PaleoLayoutExport::ProjectSnapshot snapshot;
  if (!PaleoLayoutExport::captureProjectSnapshot(layout->project(), snapshot,
                                                 error))
    return nullptr;
  QDomDocument xml;
  QgsReadWriteContext context;
  xml.appendChild(layout->writeXml(xml, context));
  return startMapProductExport(
      [snapshot, xml](const QString &path, QString *error) {
        QStringList warnings;
        auto project =
            PaleoLayoutExport::rebuildProject(snapshot, error, &warnings);
        if (!project)
          return false;
        // 快照层缺源不阻断导出，但产物少层必须可见（warning 档统一入账，
        // worker 线程直报 ErrorHub，未安装时降级 stderr）。
        paleo::workflow_detail::reportSnapshotWarnings(warnings);
        QgsPrintLayout rebuilt(project.get());
        QgsReadWriteContext context;
        if (!rebuilt.readXml(xml.documentElement(), xml, context)) {
          setError(error, QObject::tr("无法重建版面快照"));
          return false;
        }
        const auto output = PaleoLayoutExport::exportLayout(
            &rebuilt, path, PaleoLayoutExport::Format::Pdf, 300,
            PaleoLayoutExport::PageRange());
        setError(error, output.error);
        return output.ok;
      },
      tasks, catalog, directory, QFileInfo(pdfPath).fileName(), pdfPath, "pdf",
      receiver, std::move(done), error);
}
PaleoTask *startMapFrameExport(const QImage &image, PaleoTaskService *tasks,
                               DataCatalog *catalog, const QString &directory,
                               const QString &name, QObject *receiver,
                               MapProductExportDone done, QString *error) {
  return startMapProductExport(
      [image](const QString &path, QString *error) {
        if (image.isNull() || !image.save(path, "PNG")) {
          setError(error, QObject::tr("演化帧编码失败"));
          return false;
        }
        return true;
      },
      tasks, catalog, directory, name, {}, "png", receiver, std::move(done),
      error);
}
