// 层：QGIS 封装
#include "qgisprojectservice.h"

#include "manifestprojection.h"
#include "projectmapreference.h"
#include "../metadata/atomicfile.h"
#include "../metadata/paleoprojectfile.h"
#include "../metadata/projectlock.h"

#include <QDir>
#include <QFile>
#include <QFileInfo>
#include <QJsonDocument>

#include <qgsproject.h>

#include <cstdio>

namespace
{
  // georeference 的 .qgz 内嵌副本（清单权威、副本兜底）：与 ManifestProjection
  // 同款「自定义属性字符串」通道，形状 = project.paleo 的 georeference 节。
  constexpr auto kGeoScope = "paleo";
  constexpr auto kGeoKey = "georeference";

  void embedGeoreferenceProperty(QgsProject *project,
                                 const PaleoGeoreference &g)
  {
    project->writeEntry(
        kGeoScope, kGeoKey,
        QString::fromUtf8(QJsonDocument(paleoGeoreferenceToJson(g))
                              .toJson(QJsonDocument::Compact)));
  }
} // namespace

// P0 spine service — owns a QgsProject per service instance. The project is
// created with `new QgsProject()` (service owns its open/save lifecycle,
// §37 manifest projections included), then registered via
// QgsProject::setInstance(): vendored QGIS internals hardcode the singleton
// in places that cannot be wired — e.g. QgsLayerTreeModel::dropMimeData
// deserializes dragged nodes with QgsProject::instance(), so an unregistered
// singleton leaves InternalMove layer references dangling (rows render, but
// layer() is null → dragged layers/groups draw blank on canvas). Owning the
// singleton also makes every `project ? project : QgsProject::instance()`
// fallback across src/ resolve to the real project instead of a phantom.
//
// Write contract (§41.2): never write the .qgz in place. writeProject() writes
// a sibling temp file then renames it over the target, so a crash mid-write
// can never leave a truncated/corrupt .qgz at the authoritative path. Ordering
// relative to the .gpkg commit and the .qgz.bak backup is NOT done here — that
// sequencing belongs to PaleoProjectStore::saveAll(), which calls this via the
// writeQgz callback.

QgisProjectService::QgisProjectService( QObject *parent )
  : QObject( parent )
  , m_project( new QgsProject( this ) )
{
  QgsProject::setInstance( m_project );
}

QgisProjectService::~QgisProjectService()
{
  cancelOpen();
  m_openFuture.waitForFinished(); // provider/runtime 析构前排空独立读取器
}

QgsProject *QgisProjectService::project() const
{
  return m_project;
}

bool QgisProjectService::runGate( const QString &projectDir, bool creating )
{
  if ( !m_openGate )
    return true;
  QString err;
  bool cancelled = false;
  if ( m_openGate( projectDir, creating, &err, &cancelled ) )
    return true;
  m_lastOpenCancelled = cancelled;
  m_errors << ( err.isEmpty() ? ( cancelled ? tr( "打开已取消" )
                                            : tr( "工程被打开闸门拒绝: %1" ).arg( projectDir ) )
                              : err );
  return false;
}

void QgisProjectService::notifyAboutToClose()
{
  if ( !m_path.isEmpty() )
    emit projectAboutToClose();
}

// 已发 projectAboutToClose 后 read() 失败：QgsProject::read() 先 clear()，
// 旧工程内容已不在——把服务落到「无工程」而不是保留旧路径（否则保存会
// 把空工程写回旧 .qgz，同 #152 的覆盖路径）。无论之前是否有工程都发
// projectClosed：闸门可能已为新目录取了锁，接线方据此释放。
void QgisProjectService::failAfterClose()
{
  if ( !m_path.isEmpty() )
  {
    m_path.clear();
    ++m_sessionId;
  }
  emit projectClosed();
}

void QgisProjectService::closeProject()
{
  cancelOpen();
  if ( m_path.isEmpty() )
    return;
  emit projectAboutToClose();
  m_project->clear();
  m_path.clear();
  m_georeference.reset();
  m_mapConfiguration = {};
  ++m_sessionId;
  emit projectClosed();
}

bool QgisProjectService::openProject( const QString &qgzPath )
{
  if (m_opening || m_openFuture.isRunning())
    return false;
  m_errors.clear();
  m_lastOpenCancelled = false;

  if ( qgzPath.isEmpty() || !QFile::exists( qgzPath ) )
  {
    m_errors << tr( "工程文件不存在：%1" ).arg( qgzPath );
    return false;
  }

  // project.paleo 清单入口（PROJECT_FILE_DESIGN）：.paleo → 解析出 qgz 成员
  // 再开；qgz 成员缺席 = 束损坏，拒开。其他成员缺失如实报 lastErrors 仍开。
  m_georeference.reset();
  m_mapConfiguration = {};
  QString qgzFile = qgzPath;
  if ( qgzPath.endsWith( QLatin1String( ".paleo" ) ) )
  {
    bool ok = false;
    QString perr;
    const PaleoProjectFile pf = readProjectFile( qgzPath, &ok, &perr );
    if ( !ok )
    {
      m_errors << ( perr.isEmpty() ? tr( "无法读取工程文件 %1" ).arg( qgzPath )
                                   : perr );
      return false;
    }
    m_georeference = pf.georeference;
    m_mapConfiguration = pf;
    if ( !pf.georeferenceError.isEmpty() )
      m_errors << tr( "配准节无效：%1（按无配准继续）" ).arg( pf.georeferenceError );
    const QString dir = QFileInfo( qgzPath ).absolutePath();
    if ( pf.qgz.isEmpty() ||
         !QFile::exists( QDir( dir ).filePath( pf.qgz ) ) )
    {
      m_errors << tr( "工程包已损坏：缺少 qgz 成员（%1）" )
                      .arg( pf.qgz.isEmpty() ? QStringLiteral( "not declared" )
                                             : pf.qgz );
      return false;
    }
    qgzFile = QDir( dir ).filePath( pf.qgz );
    for ( const QString &m : missingMembers( dir, pf ) )
      m_errors << tr( "工程包缺少成员：%1" ).arg( m ); // 如实报，不拦开
  }

  // #152：锁/只读决策在任何落盘（清单收养）与 read() 之前——拒绝或用户
  // 取消时当前工程、路径、锁全部原样保留。
  if ( !runGate( QFileInfo( qgzFile ).absolutePath(), false ) )
    return false;

  if ( !qgzPath.endsWith( QLatin1String( ".paleo" ) ) )
  {
    // .qgz 直开：旁有 .paleo → 校验束成员；旁无 → 收养（写一份清单），
    // 老工程静默升级。校验失败只进 lastErrors——束检查不拦可用工程。
    const QString dir = QFileInfo( qgzFile ).absolutePath();
    const QString paleoPath = paleoProjectFilePath( dir );
    if ( QFile::exists( paleoPath ) )
    {
      bool ok = false;
      QString perr;
      const PaleoProjectFile pf = readProjectFile( paleoPath, &ok, &perr );
      if ( ok )
      {
        m_georeference = pf.georeference;
        m_mapConfiguration = pf;
        if ( !pf.georeferenceError.isEmpty() )
          m_errors << tr( "配准节无效：%1（按无配准继续）" ).arg( pf.georeferenceError );
        for ( const QString &m : missingMembers( dir, pf ) )
          m_errors << tr( "工程包缺少成员：%1" ).arg( m );
      }
      else
        m_errors << tr( "工程清单无法读取：%1" ).arg( perr );
    }
  }

  notifyAboutToClose();
  if ( !m_project->read( qgzFile ) )
  {
    const QString err = m_project->error();
    m_errors << ( err.isEmpty() ? tr( "读取工程失败：%1" ).arg( qgzFile ) : err );
    failAfterClose();
    return false;
  }

  m_path = qgzFile;

  // 配准兜底链：清单（已取）→ .qgz 内嵌副本。取到后回写属性，保证
  // read() 清空过的 QgsProject 里副本与清单一致。
  if ( !m_georeference && m_mapConfiguration.name.isEmpty() )
  {
    bool propOk = false;
    const QString json =
        m_project->readEntry( kGeoScope, kGeoKey, QString(), &propOk );
    if ( propOk && !json.isEmpty() )
    {
      QJsonParseError pe;
      const QJsonDocument doc = QJsonDocument::fromJson( json.toUtf8(), &pe );
      PaleoGeoreference g;
      QString gerr;
      if ( pe.error == QJsonParseError::NoError && doc.isObject() &&
           paleoGeoreferenceFromJson( doc.object(), &g, &gerr ) )
        m_georeference = g;
    }
  }
  if ( m_georeference )
    embedGeoreferenceProperty( m_project, *m_georeference );
  else
    m_project->removeEntry(kGeoScope, kGeoKey);

  applyMapConfiguration();

  // 成功读完后才收养裸 QGZ，并将其已恢复的配准/底图一起写入权威清单。
  const QString directory = QFileInfo(m_path).absolutePath();
  if (!QFile::exists(paleoProjectFilePath(directory))) {
    auto adopted = projectFileForQgz(m_path);
    adopted.georeference = m_georeference;
    adopted.mapCrs = m_mapConfiguration.mapCrs;
    adopted.basemapEnabled = m_mapConfiguration.basemapEnabled;
    adopted.basemapTopo = m_mapConfiguration.basemapTopo;
    adopted.basemapHillshade = m_mapConfiguration.basemapHillshade;
    QString error;
    if (!writeProjectFile(directory, adopted, &error))
      m_errors << tr("无法接管工程清单：%1").arg(error);
    else m_mapConfiguration = adopted;
  }

  ++m_sessionId;
  emit projectOpened( m_path );
  return true;
}

bool QgisProjectService::createProject( const QString &qgzPath )
{
  if (m_opening || m_openFuture.isRunning())
  {
    m_errors = {tr("正在读取工程，请等待完成或取消后再新建")};
    return false;
  }
  m_errors.clear();
  m_lastOpenCancelled = false;

  if ( qgzPath.isEmpty() )
  {
    m_errors << tr( "工程路径为空，无法创建" );
    return false;
  }

  const QString projectDir = QFileInfo( qgzPath ).absolutePath();
  if ( m_openGate )
  {
    // 闸门（AppContext）负责取锁并在 projectOpened 时接管——不在这里
    // 取锁再放，避免「检查-释放-再取」之间被别的实例抢走。
    if ( !runGate( projectDir, true ) )
      return false;
  }
  else
  {
    ProjectDirLock lockCheck( projectDir );
    QString lockErr;
    if ( !lockCheck.tryLock( &lockErr ) )
    {
      m_errors << tr( "工程目录已被另一个实例锁定（%1），创建被拒绝" ).arg( lockErr );
      return false;
    }
    lockCheck.unlock();
  }

  notifyAboutToClose();
  m_project->clear();
  m_path = qgzPath;
  m_georeference.reset();
  m_mapConfiguration = {};
  applyMapConfiguration();

  // Materialize the file immediately so the path is authoritative from t=0 and
  // later saveAll() cycles always have an existing .qgz to back up.
  if ( !writeProject() )
  {
    const QStringList errs = m_errors;
    failAfterClose();
    m_errors = errs;
    return false;
  }

  // 工程清单随新建落盘（PROJECT_FILE_DESIGN）：.qgz + project.paleo 双件。
  // 清单写失败不拦工程创建——如实进 lastErrors。
  {
    QString werr;
    if ( !writeProjectFile( QFileInfo( qgzPath ).absolutePath(),
                            projectFileForQgz( qgzPath ), &werr ) )
      m_errors << tr( "写入工程清单失败：%1" ).arg( werr );
  }

  ++m_sessionId;
  emit projectOpened( m_path );
  return true;
}

bool QgisProjectService::writeProject()
{
  m_errors.clear();
  if (m_opening) {
    m_errors << tr("工程正在打开，请等待接管完成后保存");
    return false;
  }

  if ( m_path.isEmpty() )
  {
    m_errors << tr( "尚未设置工程路径，请先打开或创建工程" );
    return false;
  }

  // §37: mirror the manifest's declared set into project custom properties so
  // the .qgz is a projection of ALL declarations — QgsProject::write() only
  // persists instantiated layers. An embed failure fails the write: saving
  // without the declared set would silently drop uninstantiated layers.
  if ( m_declarationProvider )
  {
    QVector<LayerDeclaration> decls;
    QString providerError;
    if ( !m_declarationProvider( &decls, &providerError ) )
    {
      m_errors << tr( "读取清单声明失败：%1" ).arg( providerError );
      return false;
    }
    QString embedError;
    if ( !ManifestProjection::embedDeclarations( m_project, decls, &embedError ) )
    {
      m_errors << tr( "写入清单声明失败：%1" ).arg( embedError );
      return false;
    }
  }

  // Temp file lives in the same directory (required for atomic rename across
  // filesystems) and MUST keep the same suffix: QgsProject::write() picks the
  // zip (.qgz) vs xml (.qgs) storage backend from the filename extension.
  if ( m_georeference )
    embedGeoreferenceProperty( m_project, *m_georeference );

  const QFileInfo fi( m_path );
  const QString tmpPath = fi.dir().filePath(
    fi.completeBaseName() + QStringLiteral( ".tmp.%1" ).arg( fi.suffix() ) );

  QFile::remove( tmpPath );
  if ( !m_project->write( tmpPath ) )
  {
    const QString err = m_project->error();
    m_errors << ( err.isEmpty() ? tr( "写入工程失败：%1" ).arg( tmpPath ) : err );
    QFile::remove( tmpPath );
    return false;
  }

  // write(filename) retargets fileName() to the temp path — restore the
  // authoritative path now that the content is safely on disk.
  m_project->setFileName( m_path );

  // QFile::rename refuses to overwrite an existing destination (Qt 6.11,
  // including Linux). POSIX rename(2) replaces it atomically. Never unlink
  // the live .qgz first: a crash in that window deletes the project.
  if ( !paleoReplaceFile( tmpPath, m_path ) )
  {
    m_errors << tr( "无法用 %2 替换工程文件 %1" ).arg( m_path, tmpPath );
    return false;
  }

  emit projectWritten( m_path );
  return true;
}

QString QgisProjectService::projectPath() const
{
  return m_path;
}

void QgisProjectService::setDeclarationProvider( const std::function<bool( QVector<LayerDeclaration> *, QString * )> &provider )
{
  m_declarationProvider = provider;
}
