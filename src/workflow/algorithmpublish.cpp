// 层：功能
#include "algorithmpublish.h"

#include "workflowerrors_internal.h"
#include "workflows_internal.h"

namespace paleo
{

AlgorithmRunResult runResultFromProcessingOutput( const QVariantMap &results,
                                                  const QString &portId )
{
  AlgorithmRunResult r;
  if ( results.isEmpty() )
  {
    r.ok = false;
    return r;
  }
  const QString path = paleo::workflow_detail::outputPathOf( results );
  if ( path.isEmpty() )
  {
    r.ok = false;
    return r;
  }
  AlgorithmOutputArtifact art;
  art.portId = portId;
  art.absolutePath = path;
  r.artifacts.append( art );
  r.ok = true;
  // 透传引擎诊断键（receipts 等），供调用方写入 extra；publish 本身不解读。
  r.diagnostics = results;
  return r;
}


AlgorithmRunResult runResultFromOutputPath( const QString &absolutePath,
                                            const QString &portId )
{
  AlgorithmRunResult r;
  if ( absolutePath.isEmpty() )
  {
    r.ok = false;
    return r;
  }
  AlgorithmOutputArtifact art;
  art.portId = portId;
  art.absolutePath = absolutePath;
  r.artifacts.append( art );
  r.ok = true;
  return r;
}

bool publishArtifacts( DerivedAssetRegistrar &reg, const DerivedStaging &staging,
                       const AlgorithmRunResult &result,
                       const QStringList &parentVersionIds, const QString &sourceUri,
                       const QVariantMap &extra, QString *error,
                       const DerivedFileFingerprint *fingerprint )
{
  using paleo::workflow_detail::setError;

  if ( !result.ok )
  {
    setError( error, result.error.isEmpty()
                         ? QStringLiteral( "算法运行失败，无法发布派生产物" )
                         : result.error );
    return false;
  }
  if ( !staging.isValid() )
  {
    setError( error, QStringLiteral( "派生产物 staging 无效，无法发布" ) );
    return false;
  }
  if ( !reg.isBound() )
  {
    setError( error, QStringLiteral( "派生产物登记未绑定 catalog" ) );
    return false;
  }

  // 选主工件：优先 OUTPUT；否则第一个带绝对路径且非纯诊断的端口。
  const AlgorithmOutputArtifact *chosen = nullptr;
  for ( const AlgorithmOutputArtifact &a : result.artifacts )
  {
    if ( a.absolutePath.isEmpty() )
      continue;
    if ( a.portId == QStringLiteral( "OUTPUT" ) )
    {
      chosen = &a;
      break;
    }
    if ( !chosen )
      chosen = &a;
  }
  if ( !chosen )
  {
    setError( error, QStringLiteral( "算法结果无带路径的产出工件，无法发布" ) );
    return false;
  }

  QVariantMap merged = extra;
  for ( auto it = chosen->extra.constBegin(); it != chosen->extra.constEnd(); ++it )
    merged.insert( it.key(), it.value() );

  return reg.commitExternal( staging, chosen->absolutePath, parentVersionIds, sourceUri,
                             merged, error, fingerprint );
}

} // namespace paleo
