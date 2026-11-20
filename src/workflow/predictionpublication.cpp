// 层：功能
#include "predictionpublication.h"

#include <QDir>
#include <QFile>

namespace paleo::workflow
{

PredictionCommitEvidence queryPredictionCommit( DataCatalog *catalog, const DerivedStaging &staging )
{
  PredictionCommitEvidence evidence;
  if ( !staging.isValid() )
  {
    evidence.detail = QStringLiteral( "staging 落位信息不完整，无法核对提交事实" );
    return evidence;
  }
  if ( !catalog || !catalog->isOpen() )
  {
    evidence.detail = QStringLiteral( "catalog 未绑定或未打开，无法证明提交状态" );
    return evidence;
  }
  const CatalogVersion version = catalog->versionById( staging.versionId );
  if ( version.id.isEmpty() )
  {
    // catalog 可查而版本不存在：可证明的未提交（版本号是 stage 期预分配的，
    // 只有 commit 才会写入 catalog）。
    evidence.fact = PredictionCommitFact::NotCommitted;
    evidence.detail = QStringLiteral( "版本 %1 未在 catalog 登记" ).arg( staging.versionId );
    return evidence;
  }
  // 版本在册：还要文件级证据——受管文件在盘且字节与登记 sha 一致。
  // 矛盾（文件缺失/被改/指纹不符）说明存储层状态可疑，判 Unknown 交由
  // 调用方阻断，不当作可覆盖的未提交。
  const QString path = catalog->versionFilePath( version );
  if ( path.isEmpty() || !QFile::exists( path ) )
  {
    evidence.detail = QStringLiteral( "版本 %1 已登记但受管文件缺失（%2）" )
                          .arg( staging.versionId, path );
    return evidence;
  }
  QString shaError;
  const QString sha = DataCatalog::sha256FileHex( path, &shaError );
  if ( sha.isEmpty() )
  {
    evidence.detail = QStringLiteral( "版本 %1 文件摘要计算失败（%2）" ).arg( staging.versionId, shaError );
    return evidence;
  }
  if ( !version.sha256.isEmpty() && sha.compare( version.sha256, Qt::CaseInsensitive ) != 0 )
  {
    evidence.detail = QStringLiteral( "版本 %1 文件与登记 sha256 不符（登记 %2，实测 %3）" )
                          .arg( staging.versionId, version.sha256, sha );
    return evidence;
  }
  evidence.fact = PredictionCommitFact::Committed;
  evidence.version = version;
  evidence.detail = QStringLiteral( "版本 %1 已提交（sha256 %2）" ).arg( staging.versionId, sha );
  return evidence;
}

} // namespace paleo::workflow
