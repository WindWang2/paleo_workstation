// 层：数据
#include "frameworkstore.h"

#include "metadata/atomicfile.h"

#include <QDir>
#include <QFile>
#include <QFileInfo>
#include <QJsonDocument>
#include <QJsonObject>

namespace SequenceFramework
{

QString FrameworkStore::boundaryEntityId( const QString &horizon )
{
  return QStringLiteral( "sb-%1" ).arg( horizon.trimmed().toUpper() );
}

FrameworkStore::FrameworkStore( DataCatalog *catalog, const QString &projectDir )
  : m_catalog( catalog ), m_projectDir( projectDir )
{
}

QString FrameworkStore::assetId() const
{
  if ( m_catalog == nullptr )
    return QString();
  for ( const CatalogAsset &a : m_catalog->assets() )
    if ( a.type == assetType() )
      return a.id;
  return QString();
}

int FrameworkStore::versionCount() const
{
  const QString id = assetId();
  if ( id.isEmpty() )
    return 0;
  return m_catalog->versionsForAsset( id ).size();
}

bool FrameworkStore::load( Framework *out, QString *error ) const
{
  if ( out == nullptr )
    return false;
  if ( m_catalog == nullptr )
  {
    if ( error )
      *error = QStringLiteral( "catalog 未接线" );
    return false;
  }
  const QString aid = assetId();
  if ( aid.isEmpty() )
  {
    *out = Framework();
    if ( error )
      error->clear();
    return true; // 工程还没建格架——不是错误
  }
  const CatalogVersion ver = m_catalog->currentVersion( aid );
  if ( ver.id.isEmpty() )
  {
    *out = Framework();
    if ( error )
      error->clear();
    return true;
  }
  const QString abs = DataCatalog::resolvedVersionPath( m_projectDir, ver );
  if ( abs.isEmpty() )
  {
    if ( error )
      *error = QStringLiteral( "格架版本路径不合法：%1" ).arg( ver.path );
    return false;
  }
  QFile f( abs );
  if ( !f.open( QIODevice::ReadOnly ) )
  {
    if ( error )
      *error = QStringLiteral( "格架文件打不开：%1" ).arg( abs );
    return false;
  }
  const QByteArray bytes = f.readAll();
  QJsonParseError pe;
  const QJsonDocument doc = QJsonDocument::fromJson( bytes, &pe );
  if ( pe.error != QJsonParseError::NoError || !doc.isObject() )
  {
    if ( error )
      *error = QStringLiteral( "格架 JSON 解析失败：%1" ).arg( pe.errorString() );
    return false;
  }
  QString perr;
  const Framework fw = fromJson( doc.object(), &perr );
  if ( !perr.isEmpty() )
  {
    if ( error )
      *error = perr;
    return false;
  }
  *out = fw;
  if ( error )
    error->clear();
  return true;
}

bool FrameworkStore::save( const Framework &fw, QString *error )
{
  if ( m_catalog == nullptr )
  {
    if ( error )
      *error = QStringLiteral( "catalog 未接线" );
    return false;
  }
  if ( m_catalog->refusesWrites() )
  {
    if ( error )
      *error = QStringLiteral( "catalog 处于拒绝写入态，格架未保存" );
    return false;
  }
  if ( m_projectDir.isEmpty() )
  {
    if ( error )
      *error = QStringLiteral( "工程目录为空，格架未保存" );
    return false;
  }

  DataCatalog::BatchSave batch( m_catalog );

  QString aid = assetId();
  if ( aid.isEmpty() )
  {
    CatalogAsset asset;
    asset.id = m_catalog->nextAssetId();
    asset.type = assetType();
    asset.format = QStringLiteral( "json" );
    asset.displayName = QStringLiteral( "层序地层格架" );
    QString aerr;
    if ( !m_catalog->addAsset( asset, &aerr ) )
    {
      if ( error )
        *error = aerr;
      return false;
    }
    aid = asset.id;
  }

  const QString versionId = m_catalog->nextVersionId();
  const QString rel = DataCatalog::managedPath( QStringLiteral( "OUTPUT" ), aid, versionId,
                                               payloadFileName() );
  if ( rel.isEmpty() )
  {
    if ( error )
      *error = QStringLiteral( "格架受管路径不合法" );
    return false;
  }
  const QString abs = m_projectDir + QStringLiteral( "/" ) + rel;
  {
    const QFileInfo info( abs );
    if ( !info.dir().mkpath( QStringLiteral( "." ) ) )
    {
      if ( error )
        *error = QStringLiteral( "格架目录创建失败：%1" ).arg( info.dir().absolutePath() );
      return false;
    }
  }
  const QByteArray bytes = toJsonBytes( fw );
  {
    // #233：tmp 同胞 + 原子替换——短写/崩溃不留截断 ver-N 孤儿。
    QString writeErr;
    if ( !paleoWriteFileAtomic( abs, bytes, &writeErr ) )
    {
      if ( error )
        *error = QStringLiteral( "格架文件写入失败：%1（%2）" ).arg( abs, writeErr );
      return false;
    }
  }

  const CatalogVersion prev = m_catalog->currentVersion( aid );
  CatalogVersion ver;
  ver.id = versionId;
  ver.assetId = aid;
  ver.stage = QStringLiteral( "OUTPUT" );
  ver.versionNumber = prev.id.isEmpty() ? 1 : prev.versionNumber + 1;
  ver.managed = true;
  ver.path = rel;
  ver.sourceUri = QStringLiteral( "paleo://sequence-framework" );
  ver.fileName = payloadFileName();
  ver.sha256 = DataCatalog::sha256FileHex( abs );
  ver.parentVersionIds = prev.id.isEmpty() ? QStringList() : QStringList{ prev.id };
  if ( ver.sha256.isEmpty() )
  {
    if ( error )
      *error = QStringLiteral( "格架文件校验和计算失败：%1" ).arg( abs );
    return false;
  }
  QString verr;
  if ( !m_catalog->addVersion( ver, &verr ) )
  {
    // 版本没入库 → 刚写的受管文件成了孤儿字节，就地删掉（produce-then-
    // commit：不留「字节在、元数据不在」的半截状态）。
    QFile::remove( abs );
    if ( error )
      *error = verr;
    return false;
  }

  // 归属链接：格架单元引用的层序界面实体 → 本格架资产。只补不建——实体
  // 不存在（该层位还没导入）时如实跳过，不新建 sequence_boundary。
  for ( const FrameworkUnit &u : fw.units )
  {
    QStringList bounds;
    if ( !u.topBoundary.isEmpty() )
      bounds.append( u.topBoundary );
    if ( !u.baseBoundary.isEmpty() )
      bounds.append( u.baseBoundary );
    for ( const QString &h : bounds )
    {
      const QString eid = boundaryEntityId( h );
      if ( !m_catalog->hasEntity( eid ) )
        continue;
      bool exists = false;
      for ( const EntityAssetLink &l : m_catalog->linksForEntity( eid ) )
        if ( l.assetId == aid && l.role == unitRole() )
        {
          exists = true;
          break;
        }
      if ( exists )
        continue;
      EntityAssetLink link;
      link.entityType = QStringLiteral( "sequence_boundary" );
      link.entityId = eid;
      link.assetId = aid;
      link.role = unitRole();
      link.isPrimary = true;
      QString lerr;
      if ( !m_catalog->addLink( link, &lerr ) )
      {
        if ( error )
          *error = lerr;
        return false;
      }
    }
  }

  QString ferr;
  if ( !batch.flush( &ferr ) )
  {
    if ( error )
      *error = ferr;
    return false;
  }
  if ( error )
    error->clear();
  return true;
}

} // namespace SequenceFramework
