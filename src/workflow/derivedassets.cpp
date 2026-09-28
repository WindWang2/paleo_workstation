// 层：功能
#include "derivedassets.h"

#include "../catalog/datacatalog.h"

#include <QDir>
#include <QFile>
#include <QFileInfo>

namespace
{
  void setError( QString *error, const QString &text )
  {
    if ( error )
      *error = text;
  }
} // namespace

DerivedAssetRegistrar::DerivedAssetRegistrar( DataCatalog *catalog, const QString &projectDir )
  : m_catalog( catalog )
  , m_projectDir( projectDir )
{
}

DerivedStaging DerivedAssetRegistrar::stage( const QString &assetType, const QString &displayName,
                                             const QString &fileName, QString *error )
{
  DerivedStaging st;
  if ( !m_catalog || !m_catalog->isOpen() )
  {
    setError( error, QStringLiteral( "派生产物登记未绑定 catalog——无法把输出落到工程目录" ) );
    return st;
  }
  if ( !DataCatalog::isSafePathSegment( fileName ) )
  {
    setError( error, QStringLiteral( "派生文件名不是合法路径段: %1" ).arg( fileName ) );
    return st;
  }

  // 资产按 (type, displayName) 复用：同一产物的重算进同一资产的版本历史。
  QString assetId;
  for ( const CatalogAsset &a : m_catalog->assets() )
  {
    if ( a.type == assetType && a.displayName == displayName )
    {
      assetId = a.id;
      break;
    }
  }
  if ( assetId.isEmpty() )
  {
    CatalogAsset a;
    a.id = m_catalog->nextAssetId();
    a.type = assetType;
    a.format = QFileInfo( fileName ).suffix().toLower();
    a.displayName = displayName;
    if ( !m_catalog->addAsset( a, error ) )
      return st;
    assetId = a.id;
  }

  st.assetId = assetId;
  st.versionId = m_catalog->nextVersionId();
  // CatalogVersion::versionNumber 缺省是 1（空结果不是 0）——新资产从 1 起，
  // 已有版本资产的下一版 = 当前最大 +1；本注册器里已 stage 未 commit 的版本
  // 也占号（同资产连续 stage 不重号）。
  const CatalogVersion current = m_catalog->currentVersion( assetId );
  const int catalogMax = current.id.isEmpty() ? 0 : current.versionNumber;
  st.versionNumber = qMax( catalogMax, m_pending.value( assetId, 0 ) ) + 1;
  m_pending.insert( assetId, st.versionNumber );
  st.relativePath = QStringLiteral( "artifacts/" ) +
                    DataCatalog::managedPath( QStringLiteral( "derived" ), assetId,
                                              st.versionId, fileName );
  if ( st.relativePath.isEmpty() || st.relativePath.endsWith( QLatin1Char( '/' ) ) )
  {
    setError( error, QStringLiteral( "派生受管路径计算失败: %1" ).arg( fileName ) );
    return DerivedStaging();
  }
  st.absolutePath = QDir( m_projectDir ).absoluteFilePath( st.relativePath );
  const QDir dir = QFileInfo( st.absolutePath ).absoluteDir();
  if ( !dir.exists() && !dir.mkpath( QStringLiteral( "." ) ) )
  {
    setError( error, QStringLiteral( "无法创建派生产物目录 %1" ).arg( dir.absolutePath() ) );
    return DerivedStaging();
  }
  return st;
}

bool DerivedAssetRegistrar::commit( const DerivedStaging &st, const QStringList &parentVersionIds,
                                    const QString &sourceUri, const QVariantMap &extra,
                                    QString *error )
{
  if ( !st.isValid() )
  {
    setError( error, QStringLiteral( "派生产物登记缺少落位信息" ) );
    return false;
  }
  if ( !QFile::exists( st.absolutePath ) )
  {
    setError( error, QStringLiteral( "派生产物尚未落盘：%1" ).arg( st.absolutePath ) );
    return false;
  }
  QString shaErr;
  const QString sha = DataCatalog::sha256FileHex( st.absolutePath, &shaErr );
  if ( sha.isEmpty() )
  {
    setError( error, QStringLiteral( "派生产物 sha256 计算失败：%1（%2）" )
                            .arg( st.absolutePath, shaErr ) );
    return false;
  }

  CatalogVersion v;
  v.id = st.versionId;
  v.assetId = st.assetId;
  v.stage = QStringLiteral( "DERIVED" );
  v.versionNumber = st.versionNumber;
  v.managed = true;
  v.path = st.relativePath;
  v.sourceUri = sourceUri;
  v.sha256 = sha;
  v.fileName = QFileInfo( st.absolutePath ).fileName();
  v.parentVersionIds = parentVersionIds;
  v.extra = extra;
  if ( !m_catalog->addVersion( v, error ) )
    return false;
  m_pending.remove( st.assetId );

  QFile::setPermissions( st.absolutePath, QFileDevice::ReadOwner | QFileDevice::ReadUser |
                                              QFileDevice::ReadGroup | QFileDevice::ReadOther );
  return true;
}

bool DerivedAssetRegistrar::commitExternal( const DerivedStaging &st, const QString &actualPath,
                                            const QStringList &parentVersionIds,
                                            const QString &sourceUri, const QVariantMap &extra,
                                            QString *error )
{
  if ( actualPath.isEmpty() )
  {
    setError( error, QStringLiteral( "算法产物路径为空，无法登记" ) );
    return false;
  }
  if ( QDir::cleanPath( actualPath ) == QDir::cleanPath( st.absolutePath ) )
    return commit( st, parentVersionIds, sourceUri, extra, error );

  if ( !QFile::exists( actualPath ) )
  {
    setError( error, QStringLiteral( "算法产物不存在：%1" ).arg( actualPath ) );
    return false;
  }
  if ( QFile::exists( st.absolutePath ) )
    QFile::remove( st.absolutePath );
  if ( !QFile::copy( actualPath, st.absolutePath ) )
  {
    setError( error, QStringLiteral( "无法把算法产物收进受管路径 %1 ← %2" )
                             .arg( st.absolutePath, actualPath ) );
    return false;
  }
  return commit( st, parentVersionIds, sourceUri, extra, error );
}

QStringList DerivedAssetRegistrar::parentVersionIdsFor( const QStringList &paths ) const
{
  QStringList out;
  if ( !m_catalog || paths.isEmpty() )
    return out;
  const QDir base( m_projectDir );
  for ( const QString &p : paths )
  {
    if ( p.isEmpty() )
      continue;
    const QString canonical = QDir::cleanPath(
        QDir::isAbsolutePath( p ) ? p : base.absoluteFilePath( p ) );
    bool found = false;
    for ( const CatalogAsset &a : m_catalog->assets() )
    {
      for ( const CatalogVersion &v : m_catalog->versionsForAsset( a.id ) )
      {
        if ( v.path.isEmpty() )
          continue;
        const QString vp = QDir::cleanPath(
            QDir::isAbsolutePath( v.path ) ? v.path : base.absoluteFilePath( v.path ) );
        if ( vp == canonical )
        {
          out.append( v.id );
          found = true;
          break;
        }
      }
      if ( found )
        break;
    }
  }
  return out;
}
