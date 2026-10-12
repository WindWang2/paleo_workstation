// 层：功能
#include "derivedassets.h"
#include "workflowerrors_internal.h"

#include "../catalog/datacatalog.h"

#include <QDir>
#include <QFile>
#include <QFileInfo>

namespace
{
using paleo::workflow_detail::setError;

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
  // 新资产的入库延迟到 commit（见头注）：stage 只 find-or-reserve。
  const QString nameKey = assetType + QLatin1Char( '\x1f' ) + displayName;
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
    assetId = m_reservedByName.value( nameKey ); // 本注册器内同名预留复用
  if ( assetId.isEmpty() )
  {
    // 只预留 id，不 addAsset——计算失败/取消时零 catalog 副作用。
    assetId = m_catalog->nextAssetId();
    ReservedAsset r;
    r.type = assetType;
    r.displayName = displayName;
    r.format = QFileInfo( fileName ).suffix().toLower();
    m_reserved.insert( assetId, r );
    m_reservedByName.insert( nameKey, assetId );
  }
  if ( m_reserved.contains( assetId ) )
  {
    // 预留信息随本 staging 走：跨注册器实例的 commit（任务中心 prepare/
    // publish 分属不同 registrar——constraintfactorjobs_localdirection/
    // _geostat 先例）也要能结算，不能只看注册器私有成员。
    const ReservedAsset &r = m_reserved.value( assetId );
    st.reservedType = r.type;
    st.reservedDisplayName = r.displayName;
    st.reservedFormat = r.format;
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

bool DerivedAssetRegistrar::commit(const DerivedStaging &st,
                                   const QStringList &parentVersionIds,
                                   const QString &sourceUri,
                                   const QVariantMap &extra, QString *error,
                                   const DerivedFileFingerprint *fingerprint) {
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
  // 延迟建资产的 commit 期结算：预留资产此刻才入库；若 stage→commit 期间
  // 外部已建同名资产，则把成果文件迁到既有资产的受管路径下归并——同一
  // 产物的版本历史不因延迟而分叉成两个资产。结算放在文件存在性检查之后：
  // 缺文件的失败 commit 不得先建出资产。
  DerivedStaging eff = st;
  const DerivedFileFingerprint *fp = fingerprint;
  DerivedFileFingerprint translated;
  bool assetCreatedBySettlement = false;
  // 预留信息以 staging 本体为准（reservedType 非空 = 延迟预留）：任务中心
  // 作业 prepare/publish 分属不同 registrar 实例（constraintfactorjobs_
  // localdirection/_geostat 先例），注册器私有的 m_reserved 在 commit 侧
  // 看不到——只认它会把首算资产的结算整个跳过。
  if ( !st.reservedType.isEmpty() )
  {
    QString finalId = st.assetId;
    for ( const CatalogAsset &a : m_catalog->assets() )
      if ( a.type == st.reservedType && a.displayName == st.reservedDisplayName )
      {
        finalId = a.id;
        break;
      }
    if ( finalId != st.assetId )
    {
      const QString newRel = QStringLiteral( "artifacts/" ) +
                             DataCatalog::managedPath( QStringLiteral( "derived" ), finalId,
                                                       st.versionId,
                                                       QFileInfo( st.absolutePath ).fileName() );
      const QString newAbs = QDir( m_projectDir ).absoluteFilePath( newRel );
      const QDir newDir = QFileInfo( newAbs ).absoluteDir();
      if ( ( !newDir.exists() && !newDir.mkpath( QStringLiteral( "." ) ) ) ||
           !QFile::rename( st.absolutePath, newAbs ) )
      {
        setError( error, QStringLiteral( "归并到既有派生资产 %1 失败：%2 ← %3" )
                                 .arg( finalId, newAbs, st.absolutePath ) );
        return false;
      }
      QDir( QFileInfo( st.absolutePath ).absolutePath() )
          .rmdir( QStringLiteral( "." ) ); // 只收空目录，best-effort
      eff.assetId = finalId;
      eff.relativePath = newRel;
      eff.absolutePath = newAbs;
      const CatalogVersion cur = m_catalog->currentVersion( finalId );
      const int catalogMax = cur.id.isEmpty() ? 0 : cur.versionNumber;
      eff.versionNumber = qMax( catalogMax, m_pending.value( finalId, 0 ) ) + 1;
      if ( fingerprint )
      {
        // rename 不改内容元数据（size/mtime/sha）——指纹按新路径解释，防线不弱化。
        translated = *fingerprint;
        translated.path = newAbs;
        fp = &translated;
      }
    }
    else
    {
      CatalogAsset a;
      a.id = st.assetId;
      a.type = st.reservedType;
      a.format = st.reservedFormat;
      a.displayName = st.reservedDisplayName;
      if ( !m_catalog->addAsset( a, error ) )
        return false;
      assetCreatedBySettlement = true;
    }
    m_reserved.remove( st.assetId );
    m_reservedByName.remove( st.reservedType + QLatin1Char( '\x1f' ) + st.reservedDisplayName );
  }
  QString shaErr;
  if (fp && !fp->matches(eff.absolutePath)) {
    setError(error, QStringLiteral("发布前成果文件已变化，请重新计算"));
    return false;
  }
  const QString sha =
      fp ? fp->sha256
         : DataCatalog::sha256FileHex(eff.absolutePath, &shaErr);
  if ( sha.isEmpty() )
  {
    setError( error, QStringLiteral( "派生产物 sha256 计算失败：%1（%2）" )
                            .arg( eff.absolutePath, shaErr ) );
    return false;
  }

  CatalogVersion v;
  v.id = st.versionId;
  v.assetId = eff.assetId;
  v.stage = QStringLiteral( "DERIVED" );
  v.versionNumber = eff.versionNumber;
  v.managed = true;
  v.path = eff.relativePath;
  v.sourceUri = sourceUri;
  v.sha256 = sha;
  v.fileName = QFileInfo( eff.absolutePath ).fileName();
  v.parentVersionIds = parentVersionIds;
  v.extra = extra;
  if ( !m_catalog->addVersion( v, error ) )
  {
    // 结算刚建的资产不因落盘失败留成零版本幽灵（P2 窗口收口）；归并分支
    // 则把文件迁回原预留路径，staging 保持可重试。均 best-effort——失败
    // 原因仍以 addVersion 的 error 为准上报。
    if ( assetCreatedBySettlement )
    {
      QString removeErr;
      m_catalog->removeAsset( eff.assetId, &removeErr );
      QFile::setPermissions( eff.absolutePath,
                             QFileDevice::ReadOwner | QFileDevice::WriteOwner | QFileDevice::ReadUser );
      QFile::remove( eff.absolutePath );
      QDir( QFileInfo( eff.absolutePath ).absolutePath() ).rmdir( QStringLiteral( "." ) );
    }
    else if ( eff.absolutePath != st.absolutePath )
    {
      QFile::rename( eff.absolutePath, st.absolutePath );
      QDir( QFileInfo( eff.absolutePath ).absolutePath() ).rmdir( QStringLiteral( "." ) );
    }
    return false;
  }
  m_pending.remove( st.assetId );
  m_pending.remove( eff.assetId );

  QFile::setPermissions( eff.absolutePath, QFileDevice::ReadOwner | QFileDevice::ReadUser |
                                              QFileDevice::ReadGroup | QFileDevice::ReadOther );
  return true;
}

bool DerivedAssetRegistrar::commitExternal(
    const DerivedStaging &st, const QString &actualPath,
    const QStringList &parentVersionIds, const QString &sourceUri,
    const QVariantMap &extra, QString *error,
    const DerivedFileFingerprint *fingerprint) {
  if ( actualPath.isEmpty() )
  {
    setError( error, QStringLiteral( "算法产物路径为空，无法登记" ) );
    return false;
  }
  if ( QDir::cleanPath( actualPath ) == QDir::cleanPath( st.absolutePath ) )
    return commit(st, parentVersionIds, sourceUri, extra, error, fingerprint);

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
        const QString vp = QDir::cleanPath(m_catalog->versionFilePath(v));
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

DerivedFileFingerprint
DerivedFileFingerprint::read(const QString &path,
                             const std::function<bool()> &cancelled,
                             QString *error) {
  DerivedFileFingerprint result;
  const QFileInfo before(path);
  result.path = before.absoluteFilePath();
  result.size = before.size();
  result.modified = before.lastModified();
  result.sha256 = DataCatalog::sha256FileHex(path, error, cancelled);
  if (result.sha256.isEmpty() || !result.matches(path)) {
    if (error && error->isEmpty())
      *error = QStringLiteral("计算成果在校验期间已变化");
    return {};
  }
  return result;
}
bool DerivedFileFingerprint::matches(const QString &path) const {
  const QFileInfo file(path);
  return sha256.size() == 64 && file.isFile() && !file.isSymLink() &&
         file.absoluteFilePath() == this->path && file.size() == size &&
         file.lastModified() == modified;
}
