// 层：功能
#include "atomicjob.h"

#include "workflowerrors_internal.h"

namespace paleo
{

bool runProcessingAndPublish( ProcessingAlgorithmAdapter &adapter, const AlgorithmSpec &spec,
                              const QVariantMap &params, const AtomicProcessingDest &dest,
                              QString *error,
                              const QgisProcessingService::ProcessingHooks &hooks )
{
  using paleo::workflow_detail::setError;
  if ( !dest.registrar )
  {
    setError( error, QStringLiteral( "AtomicJob：未绑定 DerivedAssetRegistrar" ) );
    return false;
  }
  const AlgorithmRunResult runResult = adapter.run( spec, params, error, hooks );
  if ( !runResult.ok )
    return false;
  QString commitErr;
  if ( !publishArtifacts( *dest.registrar, dest.staging, runResult, dest.parentVersionIds,
                          dest.sourceUri, dest.extra, &commitErr, dest.fingerprint ) )
  {
    setError( error, commitErr );
    return false;
  }
  return true;
}

bool runProcessingAndPublishById( ProcessingAlgorithmAdapter &adapter, const QString &algorithmId,
                                  const QVariantMap &params, const AtomicProcessingDest &dest,
                                  QString *error,
                                  const QgisProcessingService::ProcessingHooks &hooks )
{
  AlgorithmSpec spec = AlgorithmSpecRegistry::spec( algorithmId );
  if ( spec.id.isEmpty() )
  {
    spec.id = algorithmId;
    spec.engine = QStringLiteral( "processing" );
    spec.complete = false;
    AlgoPort o;
    o.id = QStringLiteral( "OUTPUT" );
    o.kind = QStringLiteral( "raster" );
    spec.outputs = { o };
  }
  return runProcessingAndPublish( adapter, spec, params, dest, error, hooks );
}

} // namespace paleo
