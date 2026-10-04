// 层：功能
#include "layouttemplatelibrary.h"

#include "../catalog/datacatalog.h"
#include "../metadata/atomicfile.h"

#include <QDomDocument>
#include <QFile>
#include <QFileInfo>
#include <QTemporaryDir>

#include <qgslayout.h>
#include <qgsreadwritecontext.h>

namespace
{
  void setError( QString *error, const QString &text )
  {
    if ( error )
      *error = text;
  }
} // namespace

namespace PaleoLayoutTemplateLibrary
{

Library::Library( DataCatalog *catalog, LayoutTemplateStore *store, const QString &projectDir )
  : m_catalog( catalog )
  , m_store( store )
  , m_projectDir( projectDir )
{
}

QVector<LayoutTemplateInfo> Library::list() const
{
  return m_store ? m_store->templates() : QVector<LayoutTemplateInfo>();
}

LayoutTemplateInfo Library::byName( const QString &name ) const
{
  return m_store ? m_store->byName( name ) : LayoutTemplateInfo{};
}

bool Library::rename( const QString &templateId, const QString &newName, QString *error )
{
  if ( !m_store )
  {
    setError( error, QObject::tr( "未绑定模板库存储" ) );
    return false;
  }
  return m_store->rename( templateId, newName, error );
}

bool Library::remove( const QString &templateId, QString *error )
{
  if ( !m_store )
  {
    setError( error, QObject::tr( "未绑定模板库存储" ) );
    return false;
  }
  return m_store->remove( templateId, error );
}

// 序列化 → 受管字节落位 → catalog 版本登记。assetId 为空 = 新资产；否则挂
// 同资产的新版本（versionHint = 该资产当前最高版本号）。
SaveOutcome Library::registerContent( QgsLayout *layout, const QString &assetId, const QString &versionHint )
{
  SaveOutcome out;
  const auto fail = [&out]( const QString &msg ) {
    out.error = msg;
    return out;
  };
  if ( !layout )
    return fail( QObject::tr( "没有可保存的版面" ) );
  if ( !m_catalog )
    return fail( QObject::tr( "未绑定数据目录" ) );
  if ( m_projectDir.isEmpty() )
    return fail( QObject::tr( "工程目录未设置" ) );

  // 1. 序列化到临时 .qpt（QgsLayout::saveAsTemplate 只收文件路径）。
  QTemporaryDir dir;
  if ( !dir.isValid() )
    return fail( QObject::tr( "无法创建临时目录" ) );
  const QString tmpPath = dir.filePath( QStringLiteral( "template.qpt" ) );
  QgsReadWriteContext context;
  if ( !layout->saveAsTemplate( tmpPath, context ) )
    return fail( QObject::tr( "版面序列化失败" ) );
  QFile tmp( tmpPath );
  if ( !tmp.open( QIODevice::ReadOnly ) )
    return fail( QObject::tr( "无法读取序列化结果" ) );
  const QByteArray bytes = tmp.readAll();
  tmp.close();
  if ( bytes.isEmpty() )
    return fail( QObject::tr( "序列化结果为空" ) );

  // 2. SHA-256（版本摘要 + 字节未变判断）。
  const QString sha = DataCatalog::sha256FileHex( tmpPath );
  if ( sha.isEmpty() )
    return fail( QObject::tr( "无法计算 SHA-256" ) );

  const bool isNewAsset = assetId.isEmpty();
  const QString finalAssetId = isNewAsset ? m_catalog->nextAssetId() : assetId;
  const QString versionId = m_catalog->nextVersionId();

  // 3. 受管落位：intermediate/<asset>/<version>/template.qpt（partial+rename
  //    原子写，置只读——与 OUTPUT 图件同纪律）。
  const QString relDir =
      DataCatalog::managedPath( QStringLiteral( "intermediate" ), finalAssetId, versionId,
                                QStringLiteral( "template.qpt" ) );
  if ( relDir.isEmpty() )
    return fail( QObject::tr( "受管路径段不合法（intermediate/%1/%2）" ).arg( finalAssetId, versionId ) );
  const QString relPath = QStringLiteral( "artifacts/" ) + relDir;
  CatalogVersion pending;
  pending.managed = true;
  pending.path = relPath;
  const QString dst = DataCatalog::resolvedVersionPath( m_projectDir, pending );
  if ( dst.isEmpty() )
    return fail( QObject::tr( "受管目标不安全: %1" ).arg( relPath ) );
  const QDir parentDir = QFileInfo( dst ).absoluteDir();
  if ( !parentDir.exists() && !parentDir.mkpath( QStringLiteral( "." ) ) )
    return fail( QObject::tr( "无法创建目录 %1" ).arg( parentDir.absolutePath() ) );
  const QString partial = dst + QStringLiteral( ".partial" );
  if ( !QFile::copy( tmpPath, partial ) )
    return fail( QObject::tr( "无法复制 %1 → %2" ).arg( tmpPath, partial ) );
  if ( !paleoReplaceFile( partial, dst ) )
  {
    QFile::remove( partial );
    return fail( QObject::tr( "无法落位 %1" ).arg( dst ) );
  }
  QFile::setPermissions( dst, QFileDevice::ReadOwner | QFileDevice::ReadUser |
                                  QFileDevice::ReadGroup | QFileDevice::ReadOther );

  // 4. catalog 行（新资产才建资产行；版本行每次都建）。
  QString catalogErr;
  if ( isNewAsset )
  {
    CatalogAsset asset;
    asset.id = finalAssetId;
    asset.type = QStringLiteral( "layout_template" );
    asset.format = QStringLiteral( "qpt" );
    asset.displayName = QStringLiteral( "template-%1.qpt" ).arg( finalAssetId );
    if ( !m_catalog->addAsset( asset, &catalogErr ) )
      return fail( catalogErr.isEmpty() ? QObject::tr( "catalog addAsset 失败" ) : catalogErr );
  }
  CatalogVersion ver;
  ver.id = versionId;
  ver.assetId = finalAssetId;
  ver.stage = QStringLiteral( "INTERMEDIATE" );
  ver.versionNumber = versionHint.isEmpty() ? 1 : versionHint.toInt() + 1;
  ver.managed = true;
  ver.path = relPath;
  ver.sha256 = sha;
  ver.fileName = QStringLiteral( "template.qpt" );
  if ( !m_catalog->addVersion( ver, &catalogErr ) )
    return fail( catalogErr.isEmpty() ? QObject::tr( "catalog addVersion 失败" ) : catalogErr );

  out.ok = true;
  out.assetId = finalAssetId;
  out.versionId = versionId;
  out.sha256 = sha;
  out.managedPath = dst;
  return out;
}

SaveOutcome Library::saveAs( QgsLayout *layout, const QString &name, const QString &kind,
                             const QString &pageSize, bool landscape )
{
  SaveOutcome out;
  if ( !m_store )
  {
    out.error = QObject::tr( "未绑定模板库存储" );
    return out;
  }
  if ( name.isEmpty() )
  {
    out.error = QObject::tr( "模板名不能为空" );
    return out;
  }

  const SaveOutcome content = registerContent( layout, QString(), QString() );
  if ( !content.ok )
    return content;

  QString err;
  const QString id = m_store->create( name, kind, pageSize, landscape, content.assetId,
                                      content.versionId, content.sha256, &err );
  if ( id.isEmpty() )
  {
    out.error = err;
    return out;
  }
  out = content;
  out.templateId = id;
  return out;
}

SaveOutcome Library::save( QgsLayout *layout, const QString &templateId )
{
  SaveOutcome out;
  if ( !m_store )
  {
    out.error = QObject::tr( "未绑定模板库存储" );
    return out;
  }
  const LayoutTemplateInfo tpl = m_store->byId( templateId );
  if ( tpl.isNull() )
  {
    out.error = QObject::tr( "模板不存在: %1" ).arg( templateId );
    return out;
  }
  if ( !m_catalog )
  {
    out.error = QObject::tr( "未绑定数据目录" );
    return out;
  }

  // 每次保存 = 同资产新版本。不做「内容未变复用」探测：QGIS 的模板序列化
  // round-trip 非字节稳定（比例尺框重算等既有行为），字节比对会漏判；
  // append-only 账本如实记每次保存（同名同内容的新版本是诚实的重存）。
  const CatalogVersion current = m_catalog->currentVersion( tpl.assetId );
  const SaveOutcome content =
      registerContent( layout, tpl.assetId, QString::number( current.versionNumber ) );
  if ( !content.ok )
    return content;

  QString err;
  if ( !m_store->updateContent( templateId, content.versionId, content.sha256, &err ) )
  {
    out.error = err;
    return out;
  }
  out = content;
  out.templateId = templateId;
  return out;
}

QString Library::contentPath( const LayoutTemplateInfo &tpl ) const
{
  if ( tpl.versionId.isEmpty() )
    return QString();
  const CatalogVersion ver = m_catalog ? m_catalog->versionById( tpl.versionId ) : CatalogVersion();
  if ( ver.id.isEmpty() || !ver.managed )
    return ver.id.isEmpty() ? QString() : ver.path; // 外链版本按原路径
  return DataCatalog::resolvedVersionPath( m_projectDir, ver );
}

bool Library::apply( QgsLayout *layout, const LayoutTemplateInfo &tpl, QString *error )
{
  if ( !layout )
  {
    setError( error, QObject::tr( "没有可套用的版面" ) );
    return false;
  }
  const QString path = contentPath( tpl );
  if ( path.isEmpty() || !QFile::exists( path ) )
  {
    setError( error, QObject::tr( "模板内容文件不存在: %1" ).arg( path ) );
    return false;
  }
  QFile file( path );
  if ( !file.open( QIODevice::ReadOnly ) )
  {
    setError( error, QObject::tr( "无法读取模板文件: %1" ).arg( path ) );
    return false;
  }
  QDomDocument doc;
  if ( !doc.setContent( &file ) )
  {
    setError( error, QObject::tr( "模板文件不是合法 XML: %1" ).arg( path ) );
    return false;
  }
  file.close();
  bool loaded = false;
  layout->loadFromTemplate( doc, QgsReadWriteContext(), /*clearExisting=*/true, &loaded );
  if ( !loaded )
  {
    setError( error, QObject::tr( "模板套用失败: %1" ).arg( tpl.name ) );
    return false;
  }
  return true;
}

} // namespace PaleoLayoutTemplateLibrary
